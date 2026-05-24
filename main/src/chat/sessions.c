#include "chat/sessions.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "common/utils.h"
#include "server/websocket_server.h"

static const char *TAG = "CHAT_SESSIONS";

bool chat_sessions_update_identity(app_context_t *ctx, int fd, const char *user_id, const char *name)
{
    bool updated = false;
    int stale_fds[MAX_CLIENTS];
    int stale_count = 0;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    int free_slot = -1;
    int same_user_slot = -1;
    int fd_slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            fd_slot = i;
            continue;
        }
        if (ctx->client_slots[i].active && user_id != NULL && ctx->client_slots[i].user_id[0] != '\0' &&
            strcmp(ctx->client_slots[i].user_id, user_id) == 0) {
            same_user_slot = i;
        }
        if (!ctx->client_slots[i].active && free_slot < 0) {
            free_slot = i;
        }
    }

    int target = fd_slot >= 0 ? fd_slot : (same_user_slot >= 0 ? same_user_slot : free_slot);
    if (target >= 0) {
        int old_target_fd = ctx->client_slots[target].fd;
        ctx->client_slots[target].fd = fd;
        ctx->client_slots[target].active = true;
        ctx->client_slots[target].is_alive = true;
        copy_bounded(ctx->client_slots[target].user_id, sizeof(ctx->client_slots[target].user_id), user_id);
        copy_bounded(ctx->client_slots[target].name, sizeof(ctx->client_slots[target].name), name);
        updated = true;

        if (fd_slot < 0) {
            ESP_LOGW(TAG, "Recovered missing WebSocket client slot for fd=%d", fd);
        }
        if (fd_slot < 0 && same_user_slot >= 0 && target == same_user_slot &&
            old_target_fd >= 0 && old_target_fd != fd && stale_count < MAX_CLIENTS) {
            stale_fds[stale_count++] = old_target_fd;
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
                ctx->client_slots[i].fd = -1;
                ctx->client_slots[i].active = false;
                ctx->client_slots[i].is_alive = false;
                ctx->client_slots[i].user_id[0] = '\0';
                ctx->client_slots[i].name[0] = '\0';
            }
        }
    }

    xSemaphoreGive(ctx->client_mutex);

    for (int i = 0; i < stale_count; i++) {
        if (ctx->server != NULL) {
            httpd_sess_trigger_close(ctx->server, stale_fds[i]);
        }
    }

    return updated;
}

bool chat_sessions_ensure_slot(app_context_t *ctx, int fd)
{
    bool ready = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            ctx->client_slots[i].is_alive = true;
            ready = true;
            break;
        }
    }

    if (!ready) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->client_slots[i].active) {
                ctx->client_slots[i].fd = fd;
                ctx->client_slots[i].active = true;
                ctx->client_slots[i].is_alive = true;
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

bool chat_sessions_mark_alive(app_context_t *ctx, int fd)
{
    bool marked = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            ctx->client_slots[i].is_alive = true;
            marked = true;
            break;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
    return marked;
}

bool chat_sessions_remove_by_fd(app_context_t *ctx, int fd)
{
    bool removed = false;

    if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx->client_slots[i].active && ctx->client_slots[i].fd == fd) {
            ctx->client_slots[i].fd = -1;
            ctx->client_slots[i].active = false;
            ctx->client_slots[i].is_alive = false;
            ctx->client_slots[i].user_id[0] = '\0';
            ctx->client_slots[i].name[0] = '\0';
            removed = true;
            break;
        }
    }

    xSemaphoreGive(ctx->client_mutex);
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
    cJSON_AddNumberToObject(root, "timestamp", current_timestamp_s(NULL));

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
            if (!ctx->client_slots[i].active || ctx->client_slots[i].user_id[0] == '\0') {
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
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_S * 1000));

        bool changed = false;
        int ping_fds[MAX_CLIENTS];
        int ping_count = 0;
        int close_fds[MAX_CLIENTS];
        int close_count = 0;
        if (ctx == NULL || xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!ctx->client_slots[i].active) {
                continue;
            }

            if (!ctx->client_slots[i].is_alive) {
                ESP_LOGW(TAG, "Client fd=%d missed heartbeat; closing", ctx->client_slots[i].fd);
                if (close_count < MAX_CLIENTS) {
                    close_fds[close_count++] = ctx->client_slots[i].fd;
                }
                ctx->client_slots[i].fd = -1;
                ctx->client_slots[i].active = false;
                ctx->client_slots[i].is_alive = false;
                ctx->client_slots[i].user_id[0] = '\0';
                ctx->client_slots[i].name[0] = '\0';
                changed = true;
                continue;
            }

            ctx->client_slots[i].is_alive = false;
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
    xTaskCreate(heartbeat_task, "heartbeat_task", 4096, ctx, 5, NULL);
}
