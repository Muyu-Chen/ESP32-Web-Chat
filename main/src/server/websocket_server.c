#include "server/websocket_server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"

#include "chat/protocol.h"
#include "chat/sessions.h"
#include "common/utils.h"

static const char *TAG = "CHAT_WS";

esp_err_t chat_ws_send_text(app_context_t *ctx, int fd, const char *payload)
{
    if (ctx == NULL || ctx->server == NULL || payload == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)payload;
    ws_pkt.len = strlen(payload);

    if (ctx->httpd_task_handle != NULL && xTaskGetCurrentTaskHandle() != ctx->httpd_task_handle) {
        return httpd_ws_send_data(ctx->server, fd, &ws_pkt);
    }

    return httpd_ws_send_frame_async(ctx->server, fd, &ws_pkt);
}

void chat_ws_broadcast(app_context_t *ctx, const char *payload)
{
    int fds[MAX_CLIENTS];
    int fd_count = 0;

    if (ctx == NULL || payload == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->client_slots[i].active) {
            continue;
        }

        if (fd_count < MAX_CLIENTS) {
            fds[fd_count++] = ctx->client_slots[i].fd;
        }
    }

    xSemaphoreGive(ctx->client_mutex);

    for (int i = 0; i < fd_count; i++) {
        esp_err_t ret = chat_ws_send_text(ctx, fds[i], payload);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send to fd=%d: %s", fds[i], esp_err_to_name(ret));
            chat_ws_close_client(ctx, fds[i]);
        }
    }
}

esp_err_t chat_ws_send_error(app_context_t *ctx, int fd, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "from", "server");
    cJSON_AddStringToObject(root, "code", code ? code : "error");
    cJSON_AddStringToObject(root, "data", message ? message : "Request failed");
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(NULL));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = chat_ws_send_text(ctx, fd, payload);
    free(payload);
    return ret;
}

void chat_ws_close_client(app_context_t *ctx, int fd)
{
    if (ctx == NULL || fd < 0) {
        return;
    }

    chat_sessions_remove_by_fd(ctx, fd);
    if (ctx->server != NULL) {
        httpd_sess_trigger_close(ctx->server, fd);
    }
}

void chat_ws_session_close_handler(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    if (chat_sessions_remove_by_fd(&g_app_context, sockfd)) {
        ESP_LOGI(TAG, "Closed WebSocket client slot for fd=%d", sockfd);
    }

    if (sockfd >= 0) {
        close(sockfd);
    }
}

esp_err_t chat_ws_handler(httpd_req_t *req)
{
    app_context_t *ctx = req->user_ctx ? (app_context_t *)req->user_ctx : &g_app_context;
    int fd = httpd_req_to_sockfd(req);
    if (ctx->httpd_task_handle == NULL) {
        ctx->httpd_task_handle = xTaskGetCurrentTaskHandle();
    }

    if (!chat_sessions_ensure_slot(ctx, fd)) {
        ESP_LOGW(TAG, "Max clients reached; rejecting fd=%d", fd);
        return ESP_FAIL;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        int err = errno;
        if (err == ECONNRESET || err == ENOTCONN || err == EPIPE || err == ESHUTDOWN) {
            ESP_LOGI(TAG, "Client disconnected, fd=%d", fd);
            if (chat_sessions_remove_by_fd(ctx, fd)) {
                chat_sessions_broadcast_online_users(ctx);
            }
            return ret;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return ESP_OK;
        }

        if (chat_sessions_remove_by_fd(ctx, fd)) {
            chat_sessions_broadcast_online_users(ctx);
        }
        ESP_LOGW(TAG, "Frame probe failed for fd=%d ret=%d errno=%d", fd, ret, err);
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    if (ws_pkt.len > MAX_WS_PAYLOAD_BYTES) {
        ESP_LOGW(TAG, "Payload too large from fd=%d: %d bytes", fd, (int)ws_pkt.len);
        chat_ws_send_error(ctx, fd, "payload_too_large", "WebSocket payload is too large");
        if (chat_sessions_remove_by_fd(ctx, fd)) {
            chat_sessions_broadcast_online_users(ctx);
        }
        return ESP_FAIL;
    }

    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate WebSocket buffer");
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Frame receive failed for fd=%d: %s", fd, esp_err_to_name(ret));
        free(buf);
        if (chat_sessions_remove_by_fd(ctx, fd)) {
            chat_sessions_broadcast_online_users(ctx);
        }
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (chat_sessions_remove_by_fd(ctx, fd)) {
            chat_sessions_broadcast_online_users(ctx);
        }
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        chat_ws_send_error(ctx, fd, "bad_frame", "Only text WebSocket frames are supported");
        free(buf);
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)ws_pkt.payload, ws_pkt.len);
    if (root == NULL || !cJSON_IsObject(root)) {
        chat_ws_send_error(ctx, fd, "bad_json", "Invalid JSON object");
    } else {
        chat_protocol_handle_json(ctx, fd, root);
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}
