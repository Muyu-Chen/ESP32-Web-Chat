#include "chat/history.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "common/utils.h"
#include "server/websocket_server.h"
#include "storage/message_id_store.h"

static const char *TAG = "CHAT_HISTORY";

bool chat_history_parse_since_id(const cJSON *root, uint64_t *since_id_out)
{
    cJSON *since_id = cJSON_GetObjectItemCaseSensitive(root, "since_id");
    if (since_id == NULL) {
        since_id = cJSON_GetObjectItemCaseSensitive(root, "last_seen_id");
    }

    if (since_id_out == NULL) {
        return false;
    }
    *since_id_out = 0;

    if (since_id == NULL) {
        return true;
    }

    if (!cJSON_IsNumber(since_id) ||
        since_id->valuedouble < 0 ||
        since_id->valuedouble > (double)CHAT_MESSAGE_MAX_SAFE_ID) {
        return false;
    }

    uint64_t parsed = (uint64_t)since_id->valuedouble;
    if ((double)parsed != since_id->valuedouble) {
        return false;
    }

    *since_id_out = parsed;
    return true;
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
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(ctx));
    cJSON_AddNumberToObject(root, "heartbeat_interval_s", HEARTBEAT_INTERVAL_S);

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

esp_err_t chat_history_send_info_to_client(app_context_t *ctx, int fd)
{
    char *payload = chat_history_build_info_payload(ctx);
    if (payload == NULL) {
        esp_err_t ret = chat_ws_send_error(ctx, fd, "server_busy", "History boundary is temporarily unavailable");
        return ret == ESP_OK ? ESP_ERR_NO_MEM : ret;
    }

    esp_err_t ret = chat_ws_send_text(ctx, fd, payload);
    free(payload);
    return ret;
}

bool chat_history_broadcast_info(app_context_t *ctx)
{
    char *payload = chat_history_build_info_payload(ctx);
    if (payload == NULL) {
        ESP_LOGW(TAG, "Failed to build history boundary payload");
        return false;
    }

    bool closed_client = chat_ws_broadcast(ctx, payload);
    free(payload);
    return closed_client;
}

esp_err_t chat_history_send_to_client(app_context_t *ctx, int fd, uint64_t since_id)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint64_t replay_until = ctx->message_id_counter;
    xSemaphoreGive(ctx->message_mutex);

    uint64_t cursor = since_id;
    int sent = 0;
    for (int replayed = 0; replayed < MAX_MESSAGES && cursor < replay_until; replayed++) {
        if (xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        const message_t *next = NULL;
        for (int i = 0; i < MAX_MESSAGES; i++) {
            const message_t *candidate = &ctx->message_buffer[i];
            if (candidate->payload != NULL && candidate->id > cursor && candidate->id <= replay_until &&
                (next == NULL || candidate->id < next->id)) {
                next = candidate;
            }
        }

        char *payload = NULL;
        uint64_t next_id = next ? next->id : 0;
        if (next != NULL) {
            size_t length = strlen(next->payload) + 1;
            payload = malloc(length);
            if (payload != NULL) {
                memcpy(payload, next->payload, length);
            }
        }
        xSemaphoreGive(ctx->message_mutex);

        if (next_id == 0) {
            break;
        }
        if (payload == NULL) {
            esp_err_t ret = chat_ws_send_error(ctx, fd, "server_busy", "Message history is temporarily unavailable");
            return ret == ESP_OK ? ESP_ERR_NO_MEM : ret;
        }

        // Keep only one copied message alive; network I/O never holds message_mutex.
        esp_err_t ret = chat_ws_send_text(ctx, fd, payload);
        free(payload);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "History send failed for fd=%d: %s", fd, esp_err_to_name(ret));
            chat_ws_close_client(ctx, fd);
            return ret;
        }

        cursor = next_id;
        sent++;
    }

    ESP_LOGI(TAG, "Sent %d history messages to fd=%d since_id=%" PRIu64, sent, fd, since_id);
    return ESP_OK;
}

static esp_err_t ensure_id_state_locked(app_context_t *ctx)
{
    if (ctx->message_id_counter_valid) {
        return ESP_OK;
    }

    chat_message_id_state_t state = { 0 };
    esp_err_t ret = chat_message_ids_load(&state);
    if (!state.counter_loaded) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }
    ctx->message_id_counter = state.current_id;
    ctx->boot_start_id = state.boot_start_id;
    ctx->message_id_counter_valid = true;
    return ESP_OK;
}

esp_err_t chat_history_ensure_id_state(app_context_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(ctx->message_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ensure_id_state_locked(ctx);
    xSemaphoreGive(ctx->message_mutex);
    return ret;
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

    ret = ensure_id_state_locked(ctx);
    if (ret != ESP_OK) {
        goto out;
    }

    if (ctx->message_id_counter >= CHAT_MESSAGE_MAX_SAFE_ID) {
        ret = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    uint64_t id = ctx->message_id_counter + 1;
    int64_t timestamp = current_timestamp_s(ctx);

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
