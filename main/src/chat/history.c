#include "chat/history.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "common/utils.h"
#include "server/websocket_server.h"
#include "storage/message_id_store.h"

static const char *TAG = "CHAT_HISTORY";

uint64_t chat_history_json_since_id(const cJSON *root)
{
    cJSON *since_id = cJSON_GetObjectItem(root, "since_id");
    if (!cJSON_IsNumber(since_id)) {
        since_id = cJSON_GetObjectItem(root, "last_seen_id");
    }

    if (!cJSON_IsNumber(since_id) || since_id->valuedouble <= 0) {
        return 0;
    }

    if (since_id->valuedouble > (double)CHAT_MESSAGE_MAX_SAFE_ID) {
        return CHAT_MESSAGE_MAX_SAFE_ID;
    }

    return (uint64_t)since_id->valuedouble;
}

void chat_history_fill_bounds_locked(app_context_t *ctx, history_bounds_t *bounds)
{
    if (ctx == NULL || bounds == NULL) {
        return;
    }

    memset(bounds, 0, sizeof(*bounds));
    bounds->boot_start_id = ctx->boot_start_id;
    bounds->current_id = ctx->message_id_counter;
    bounds->capacity = MAX_MESSAGES;

    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (ctx->message_buffer[i].payload == NULL) {
            continue;
        }

        uint64_t id = ctx->message_buffer[i].id;
        if (bounds->count == 0 || id < bounds->earliest_id) {
            bounds->earliest_id = id;
        }
        if (id > bounds->latest_id) {
            bounds->latest_id = id;
        }
        bounds->count++;
    }

    bounds->restore_before_id = bounds->count > 0 ? bounds->earliest_id : bounds->boot_start_id;
    bounds->has_more_before = bounds->restore_before_id > 1;
}

uint64_t chat_history_current_restore_before_id(app_context_t *ctx)
{
    uint64_t restore_before_id = ctx ? ctx->boot_start_id : 1;

    if (ctx != NULL && xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) == pdTRUE) {
        history_bounds_t bounds;
        chat_history_fill_bounds_locked(ctx, &bounds);
        restore_before_id = bounds.restore_before_id;
        xSemaphoreGive(ctx->message_mutex);
    }

    return restore_before_id;
}

char *chat_history_build_info_payload(app_context_t *ctx)
{
    cJSON *root = cJSON_CreateObject();
    if (ctx == NULL || root == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "historyInfo");
    cJSON_AddStringToObject(root, "from", "server");
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(NULL));

    cJSON *to = cJSON_AddObjectToObject(root, "to");
    cJSON *users = to ? cJSON_AddArrayToObject(to, "users") : NULL;
    if (to == NULL || users == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddBoolToObject(to, "all", true);

    if (xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(root);
        return NULL;
    }

    history_bounds_t bounds;
    chat_history_fill_bounds_locked(ctx, &bounds);
    xSemaphoreGive(ctx->message_mutex);

    cJSON_AddNumberToObject(root, "boot_start_id", (double)bounds.boot_start_id);
    cJSON_AddNumberToObject(root, "current_id", (double)bounds.current_id);
    cJSON_AddNumberToObject(root, "earliest_id", (double)bounds.earliest_id);
    cJSON_AddNumberToObject(root, "latest_id", (double)bounds.latest_id);
    cJSON_AddNumberToObject(root, "restore_before_id", (double)bounds.restore_before_id);
    cJSON_AddNumberToObject(root, "count", bounds.count);
    cJSON_AddNumberToObject(root, "capacity", bounds.capacity);
    cJSON_AddBoolToObject(root, "has_more_before", bounds.has_more_before);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

void chat_history_send_info_to_client(app_context_t *ctx, int fd)
{
    char *payload = chat_history_build_info_payload(ctx);
    if (payload == NULL) {
        chat_ws_send_error(ctx, fd, "server_busy", "History boundary is temporarily unavailable");
        return;
    }

    chat_ws_send_text(ctx, fd, payload);
    free(payload);
}

void chat_history_broadcast_info(app_context_t *ctx)
{
    char *payload = chat_history_build_info_payload(ctx);
    if (payload == NULL) {
        ESP_LOGW(TAG, "Failed to build history boundary payload");
        return;
    }

    chat_ws_broadcast(ctx, payload);
    free(payload);
}

void chat_history_send_to_client(app_context_t *ctx, int fd, uint64_t since_id)
{
    if (ctx == NULL || xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
        chat_ws_send_error(ctx, fd, "server_busy", "Message history is temporarily unavailable");
        return;
    }

    int sent = 0;
    int current_pos = ctx->message_buffer_head;
    for (int i = 0; i < MAX_MESSAGES; i++) {
        int index = (current_pos + i) % MAX_MESSAGES;
        if (ctx->message_buffer[index].payload == NULL || ctx->message_buffer[index].id <= since_id) {
            continue;
        }

        esp_err_t ret = chat_ws_send_text(ctx, fd, ctx->message_buffer[index].payload);
        if (ret == ESP_OK) {
            sent++;
        } else {
            ESP_LOGW(TAG, "History send failed for fd=%d: %s", fd, esp_err_to_name(ret));
            break;
        }
    }

    xSemaphoreGive(ctx->message_mutex);
    ESP_LOGI(TAG, "Sent %d history messages to fd=%d since_id=%" PRIu64, sent, fd, since_id);
}

esp_err_t chat_history_finalize_and_store_message(app_context_t *ctx, cJSON *root, char **payload_out)
{
    char *payload = NULL;
    esp_err_t ret = ESP_OK;

    if (ctx == NULL || payload_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *payload_out = NULL;

    if (xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (ctx->message_id_counter >= CHAT_MESSAGE_MAX_SAFE_ID) {
        ret = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    uint64_t id = ctx->message_id_counter + 1;
    int64_t timestamp = current_timestamp_s(root);

    cJSON_DeleteItemFromObjectCaseSensitive(root, "id");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "timestamp");
    if (cJSON_AddNumberToObject(root, "id", (double)id) == NULL ||
        cJSON_AddNumberToObject(root, "timestamp", (double)timestamp) == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }

    payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        ret = chat_message_ids_persist(id, ctx->boot_start_id);
        if (ret != ESP_OK) {
            free(payload);
            goto out;
        }

        if (ctx->message_buffer[ctx->message_buffer_head].payload != NULL) {
            free(ctx->message_buffer[ctx->message_buffer_head].payload);
        }

        ctx->message_id_counter = id;
        ctx->message_buffer[ctx->message_buffer_head].payload = payload;
        ctx->message_buffer[ctx->message_buffer_head].id = id;
        ctx->message_buffer_head = (ctx->message_buffer_head + 1) % MAX_MESSAGES;
        *payload_out = payload;
    } else {
        ret = ESP_ERR_NO_MEM;
    }

out:
    xSemaphoreGive(ctx->message_mutex);
    return ret;
}
