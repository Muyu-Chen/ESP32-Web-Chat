#include "chat/sessions.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "common/utils.h"
#include "server/websocket_server.h"

static const char *TAG = "CHAT_SESSIONS";

static void clear_slot_identity(client_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    slot->fd = -1;
    slot->active = false;
    slot->closing = false;
    slot->joined = false;
    slot->is_alive = false;
    slot->time_offset_valid = false;
    slot->time_offset_s = 0;
    slot->registered_at_s = 0;
    slot->last_ping_s = 0;
    slot->user_id[0] = '\0';
    slot->name[0] = '\0';
}

static bool mark_slot_closing(client_slot_t *slot)
{
    if (slot == NULL || !slot->active) {
        return false;
    }

    bool was_joined = slot->joined;
    slot->closing = true;
    slot->joined = false;
    slot->is_alive = false;
    slot->time_offset_valid = false;
    slot->time_offset_s = 0;
    slot->user_id[0] = '\0';
    slot->name[0] = '\0';
    return was_joined;
}

bool chat_sessions_update_identity(app_context_t *ctx, int fd, const char *user_id, const char *name)
{
    bool updated = false;
    int stale_fds[MAX_CLIENTS];
    int stale_count = 0;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    int free_slot = -1;
    int fd_slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && !ctx->client_slots[i].closing && ctx->client_slots[i].fd == fd) {
            fd_slot = i;
            continue;
        }
        if (!ctx->client_slots[i].active && free_slot < 0) {
            free_slot = i;
        }
    }

    int target = fd_slot >= 0 ? fd_slot : free_slot;
    if (target >= 0) {
        bool identity_changed = ctx->client_slots[target].fd != fd ||
            ctx->client_slots[target].user_id[0] == '\0' ||
            user_id == NULL ||
            strcmp(ctx->client_slots[target].user_id, user_id) != 0;
        ctx->client_slots[target].fd = fd;
        ctx->client_slots[target].active = true;
        ctx->client_slots[target].closing = false;
        ctx->client_slots[target].joined = true;
        ctx->client_slots[target].is_alive = true;
        ctx->client_slots[target].registered_at_s = device_uptime_s();
        ctx->client_slots[target].last_ping_s = ctx->client_slots[target].registered_at_s;
        if (identity_changed) {
            ctx->client_slots[target].time_offset_valid = false;
            ctx->client_slots[target].time_offset_s = 0;
        }
        copy_bounded(ctx->client_slots[target].user_id, sizeof(ctx->client_slots[target].user_id), user_id);
        copy_bounded(ctx->client_slots[target].name, sizeof(ctx->client_slots[target].name), name);
        updated = true;

        if (fd_slot < 0) {
            ESP_LOGW(TAG, "Recovered missing WebSocket client slot for fd=%d", fd);
        }
    }

    if (updated && user_id != NULL) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (i == target || !ctx->client_slots[i].active || ctx->client_slots[i].user_id[0] == '\0') {
                continue;
            }
            if (strcmp(ctx->client_slots[i].user_id, user_id) == 0) {
                if (ctx->client_slots[i].fd >= 0 && ctx->client_slots[i].fd != fd && stale_count < MAX_CLIENTS) {
                    stale_fds[stale_count++] = ctx->client_slots[i].fd;
                }
                mark_slot_closing(&ctx->client_slots[i]);
            }
        }
    }

    xSemaphoreGive(ctx->client_mutex);

    for (int i = 0; i < stale_count; i++) {
        esp_err_t notify_ret = chat_ws_send_error(ctx, stale_fds[i], "session_replaced",
                                                  "This user connected from another browser session");
        if (notify_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to notify replaced session fd=%d: %s",
                     stale_fds[i], esp_err_to_name(notify_ret));
        }
        chat_ws_close_client(ctx, stale_fds[i]);
    }

    return updated;
}

bool chat_sessions_ensure_slot(app_context_t *ctx, int fd)
{
    bool ready = false;
    bool fd_known = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            fd_known = true;
            if (ctx->client_slots[i].closing) {
                break;
            }
            ctx->client_slots[i].is_alive = true;
            ready = true;
            break;
        }
    }

    if (!ready && !fd_known) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->client_slots[i].active) {
                ctx->client_slots[i].fd = fd;
                ctx->client_slots[i].active = true;
                ctx->client_slots[i].closing = false;
                ctx->client_slots[i].joined = false;
                ctx->client_slots[i].is_alive = true;
                ctx->client_slots[i].time_offset_valid = false;
                ctx->client_slots[i].time_offset_s = 0;
                ctx->client_slots[i].registered_at_s = device_uptime_s();
                ctx->client_slots[i].last_ping_s = ctx->client_slots[i].registered_at_s;
                ctx->client_slots[i].user_id[0] = '\0';
                copy_bounded(ctx->client_slots[i].name, sizeof(ctx->client_slots[i].name), "New User");
                ready = true;
                ESP_LOGI(TAG, "Registered WebSocket client slot for fd=%d", fd);
                break;
            }
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    return ready;
}

bool chat_sessions_is_joined(app_context_t *ctx, int fd)
{
    bool joined = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && !ctx->client_slots[i].closing && ctx->client_slots[i].fd == fd) {
            joined = ctx->client_slots[i].joined && ctx->client_slots[i].user_id[0] != '\0';
            break;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    return joined;
}

chat_identity_status_t chat_sessions_check_identity(app_context_t *ctx, int fd, const char *user_id)
{
    chat_identity_status_t status = CHAT_IDENTITY_NOT_JOINED;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return status;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_slot_t *slot = &ctx->client_slots[i];
        if (!slot->active || slot->closing || slot->fd != fd) {
            continue;
        }
        if (!slot->joined || slot->user_id[0] == '\0') {
            break;
        }
        status = user_id != NULL && strcmp(slot->user_id, user_id) == 0
            ? CHAT_IDENTITY_OK
            : CHAT_IDENTITY_MISMATCH;
        break;
    }

    xSemaphoreGive(ctx->client_mutex);
    return status;
}

void chat_sessions_update_time_sample(app_context_t *ctx, int fd, const cJSON *root)
{
    cJSON *timestamp = root ? cJSON_GetObjectItem(root, "timestamp") : NULL;
    if (ctx == NULL ||
        !cJSON_IsNumber(timestamp) ||
        timestamp->valuedouble < VALID_EPOCH_START_S ||
        timestamp->valuedouble > VALID_EPOCH_END_S) {
        return;
    }

    int64_t offset = (int64_t)timestamp->valuedouble - device_uptime_s();
    if (xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].joined && ctx->client_slots[i].fd == fd) {
            ctx->client_slots[i].time_offset_s = offset;
            ctx->client_slots[i].time_offset_valid = true;
            break;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
}

bool chat_sessions_mark_alive(app_context_t *ctx, int fd)
{
    bool marked = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && !ctx->client_slots[i].closing &&
            ctx->client_slots[i].joined && ctx->client_slots[i].fd == fd) {
            ctx->client_slots[i].is_alive = true;
            marked = true;
            break;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    return marked;
}

bool chat_sessions_mark_closing(app_context_t *ctx, int fd)
{
    bool was_joined = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            was_joined = mark_slot_closing(&ctx->client_slots[i]) || was_joined;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    return was_joined;
}

bool chat_sessions_remove_by_fd(app_context_t *ctx, int fd)
{
    bool removed = false;
    int removed_count = 0;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            clear_slot_identity(&ctx->client_slots[i]);
            removed = true;
            removed_count++;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    if (removed_count > 1) {
        ESP_LOGW(TAG, "Removed %d duplicate slots for fd=%d", removed_count, fd);
    }
    return removed;
}

char *chat_sessions_build_online_users_payload(app_context_t *ctx)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *to = NULL;
    cJSON *users = NULL;
    cJSON *data = NULL;
    char *payload = NULL;

    if (ctx == NULL || root == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(root, "type", "onlineUsers");
    cJSON_AddStringToObject(root, "from", "server");
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(ctx));

    to = cJSON_AddObjectToObject(root, "to");
    users = to ? cJSON_AddArrayToObject(to, "users") : NULL;
    data = cJSON_AddArrayToObject(root, "data");
    if (to == NULL || users == NULL || data == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddBoolToObject(to, "all", true);

    if (xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->client_slots[i].active || !ctx->client_slots[i].joined || ctx->client_slots[i].user_id[0] == '\0') {
                continue;
            }

            cJSON *id = cJSON_CreateString(ctx->client_slots[i].user_id);
            if (id) {
                cJSON_AddItemToArray(users, id);
            }

            cJSON *entry = cJSON_CreateObject();
            if (entry) {
                cJSON_AddStringToObject(entry, "id", ctx->client_slots[i].user_id);
                cJSON_AddStringToObject(entry, "name", ctx->client_slots[i].name[0] ? ctx->client_slots[i].name : "New User");
                cJSON_AddItemToArray(data, entry);
            }
        }
        xSemaphoreGive(ctx->client_mutex);
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

void chat_sessions_broadcast_online_users(app_context_t *ctx)
{
    char *payload = chat_sessions_build_online_users_payload(ctx);
    if (payload == NULL) {
        ESP_LOGW(TAG, "Failed to build online user list");
        return;
    }

    chat_ws_broadcast(ctx, payload);
    free(payload);
}

void chat_sessions_send_online_users_to_client(app_context_t *ctx, int fd)
{
    char *payload = chat_sessions_build_online_users_payload(ctx);
    if (payload == NULL) {
        chat_ws_send_error(ctx, fd, "server_busy", "Unable to build online user list");
        return;
    }

    chat_ws_send_text(ctx, fd, payload);
    free(payload);
}

static void heartbeat_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    const char *ping_payload = "{\"type\":\"ping\"}";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SESSION_SWEEP_INTERVAL_MS));

        bool changed = false;
        int64_t now_s = device_uptime_s();
        int ping_fds[MAX_CLIENTS];
        int ping_count = 0;
        int close_fds[MAX_CLIENTS];
        int close_count = 0;
        if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->client_slots[i].active || ctx->client_slots[i].closing) {
                continue;
            }

            if (!ctx->client_slots[i].joined) {
                if (now_s - ctx->client_slots[i].registered_at_s < WS_JOIN_TIMEOUT_S) {
                    continue;
                }
                ESP_LOGW(TAG, "Client fd=%d did not join; closing", ctx->client_slots[i].fd);
                if (close_count < MAX_CLIENTS) {
                    close_fds[close_count++] = ctx->client_slots[i].fd;
                }
                mark_slot_closing(&ctx->client_slots[i]);
                continue;
            }

            if (!ctx->client_slots[i].is_alive) {
                if (now_s - ctx->client_slots[i].last_ping_s < HEARTBEAT_INTERVAL_S) {
                    continue;
                }
                ESP_LOGW(TAG, "Client fd=%d missed heartbeat; closing", ctx->client_slots[i].fd);
                if (close_count < MAX_CLIENTS) {
                    close_fds[close_count++] = ctx->client_slots[i].fd;
                }
                mark_slot_closing(&ctx->client_slots[i]);
                changed = true;
                continue;
            }

            if (now_s - ctx->client_slots[i].last_ping_s < HEARTBEAT_INTERVAL_S) {
                continue;
            }

            ctx->client_slots[i].is_alive = false;
            ctx->client_slots[i].last_ping_s = now_s;
            if (ping_count < MAX_CLIENTS) {
                ping_fds[ping_count++] = ctx->client_slots[i].fd;
            }
        }

        xSemaphoreGive(ctx->client_mutex);

        for (int i = 0; i < close_count; i++) {
            if (ctx->server != NULL) {
                httpd_sess_trigger_close(ctx->server, close_fds[i]);
            }
        }

        for (int i = 0; i < ping_count; i++) {
            esp_err_t ret = chat_ws_send_text(ctx, ping_fds[i], ping_payload);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Ping failed for fd=%d: %s", ping_fds[i], esp_err_to_name(ret));
                chat_ws_close_client(ctx, ping_fds[i]);
                changed = true;
            }
        }

        if (changed) {
            chat_sessions_broadcast_online_users(ctx);
        }
    }
}

void chat_sessions_start_heartbeat(app_context_t *ctx)
{
    BaseType_t created = xTaskCreate(heartbeat_task, "heartbeat_task", 4096, ctx, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat task");
    }
}
