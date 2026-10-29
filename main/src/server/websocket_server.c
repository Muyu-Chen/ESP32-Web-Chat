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

static void mark_broken_session(app_context_t *ctx, int fd, const char *stage, esp_err_t ret, int socket_errno)
{
    bool was_joined = chat_sessions_mark_closing(ctx, fd);
    ESP_LOGW(TAG, "%s failed for fd=%d: %s, errno=%d", stage, fd, esp_err_to_name(ret), socket_errno);
    if (was_joined) {
        chat_sessions_broadcast_online_users(ctx);
    }
}

esp_err_t chat_ws_send_text(app_context_t *ctx, int fd, const char *payload)
{
    if (ctx == NULL || ctx->server == NULL || payload == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_ws_get_fd_info(ctx->server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return ESP_ERR_INVALID_STATE;
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

bool chat_ws_broadcast(app_context_t *ctx, const char *payload)
{
    int fds[MAX_CLIENTS];
    int fd_count = 0;
    bool closed_client = false;

    if (ctx == NULL || payload == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->client_slots[i].active || ctx->client_slots[i].closing || !ctx->client_slots[i].joined) {
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
            closed_client = true;
        }
    }

    return closed_client;
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
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(ctx));

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

    chat_sessions_mark_closing(ctx, fd);
    if (ctx->server == NULL) {
        chat_sessions_remove_by_fd(ctx, fd);
        return;
    }

    if (ctx->httpd_task_handle != NULL && xTaskGetCurrentTaskHandle() == ctx->httpd_task_handle) {
        if (shutdown(fd, SHUT_RDWR) != 0 && errno != ENOTCONN) {
            ESP_LOGW(TAG, "Failed to shut down fd=%d: errno=%d", fd, errno);
        }
        return;
    }

    esp_err_t ret = httpd_sess_trigger_close(ctx->server, fd);
    if (ret == ESP_ERR_NOT_FOUND) {
        chat_sessions_remove_by_fd(ctx, fd);
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue close for fd=%d: %s", fd, esp_err_to_name(ret));
        shutdown(fd, SHUT_RDWR);
    }
}

esp_err_t chat_ws_pre_handshake_handler(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_context_t *ctx = req->user_ctx ? (app_context_t *)req->user_ctx : &g_app_context;
    int fd = httpd_req_to_sockfd(req);
    if (ctx->httpd_task_handle == NULL) {
        ctx->httpd_task_handle = xTaskGetCurrentTaskHandle();
    }

    if (!chat_sessions_ensure_slot(ctx, fd)) {
        ESP_LOGW(TAG, "Max clients reached; rejecting WebSocket handshake for fd=%d", fd);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void chat_ws_session_close_handler(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    if (chat_sessions_remove_by_fd(&g_app_context, sockfd)) {
        ESP_LOGI(TAG, "Closed WebSocket client slot for fd=%d", sockfd);
        chat_sessions_broadcast_online_users(&g_app_context);
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
        int socket_errno = errno;
        // The HTTP server may already have consumed part of the frame header.
        // Retrying would interpret the remaining bytes as a new frame.
        mark_broken_session(ctx, fd, "Frame probe", ret, socket_errno);
        return ret;
    }

    if (ws_pkt.len == 0 && ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        if (!ws_pkt.final || !chat_sessions_is_joined(ctx, fd)) {
            chat_ws_close_client(ctx, fd);
        } else {
            chat_sessions_mark_alive(ctx, fd);
        }
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        bool is_text = ws_pkt.type == HTTPD_WS_TYPE_TEXT && ws_pkt.final;
        chat_ws_send_error(ctx, fd,
                           is_text ? "bad_json" : "bad_frame",
                           is_text ? "Invalid JSON object" : "Only text WebSocket frames are supported");
        if (!is_text || !chat_sessions_is_joined(ctx, fd)) {
            chat_ws_close_client(ctx, fd);
        }
        return ESP_OK;
    }

    if (ws_pkt.len > MAX_WS_PAYLOAD_BYTES) {
        ESP_LOGW(TAG, "Payload too large from fd=%d: %d bytes", fd, (int)ws_pkt.len);
        chat_ws_send_error(ctx, fd, "payload_too_large", "WebSocket payload is too large");
        if (chat_sessions_mark_closing(ctx, fd)) {
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
        int socket_errno = errno;
        free(buf);
        mark_broken_session(ctx, fd, "Frame receive", ret, socket_errno);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (chat_sessions_mark_closing(ctx, fd)) {
            chat_sessions_broadcast_online_users(ctx);
        }
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        if (!ws_pkt.final || !chat_sessions_is_joined(ctx, fd)) {
            chat_ws_close_client(ctx, fd);
        } else {
            chat_sessions_mark_alive(ctx, fd);
        }
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT || !ws_pkt.final) {
        chat_ws_send_error(ctx, fd, "bad_frame", "Only text WebSocket frames are supported");
        chat_ws_close_client(ctx, fd);
        free(buf);
        return ESP_OK;
    }

    cJSON *root = chat_json_parse_object((const char *)ws_pkt.payload, ws_pkt.len);
    if (root == NULL) {
        chat_ws_send_error(ctx, fd, "bad_json", "Invalid JSON object");
        if (!chat_sessions_is_joined(ctx, fd)) {
            chat_ws_close_client(ctx, fd);
        }
    } else {
        ret = chat_protocol_handle_json(ctx, fd, root);
    }

    cJSON_Delete(root);
    free(buf);
    return ret;
}
