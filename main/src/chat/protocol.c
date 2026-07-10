#include "chat/protocol.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "chat/history.h"
#include "chat/sessions.h"
#include "common/utils.h"
#include "server/websocket_server.h"
#include "storage/message_id_store.h"

static const char *TAG = "CHAT_PROTOCOL";

static bool validate_to_object(cJSON *root)
{
    cJSON *to = cJSON_GetObjectItemCaseSensitive(root, "to");
    if (to == NULL) {
        to = cJSON_AddObjectToObject(root, "to");
        if (to == NULL) {
            return false;
        }
    }
    if (!cJSON_IsObject(to)) {
        return false;
    }

    cJSON *all = cJSON_GetObjectItemCaseSensitive(to, "all");
    if (all == NULL) {
        all = cJSON_AddBoolToObject(to, "all", false);
    }
    if (!cJSON_IsBool(all)) {
        return false;
    }

    cJSON *users = cJSON_GetObjectItemCaseSensitive(to, "users");
    if (users == NULL) {
        users = cJSON_AddArrayToObject(to, "users");
    }
    if (!cJSON_IsArray(users) || cJSON_GetArraySize(users) > MAX_CLIENTS + 1) {
        return false;
    }

    cJSON *user = NULL;
    cJSON_ArrayForEach(user, users) {
        if (!json_string_in_range(user, MAX_USER_ID_LEN, false)) {
            return false;
        }
    }

    return cJSON_IsTrue(all) || cJSON_GetArraySize(users) > 0;
}

static bool message_visible_to_user(cJSON *message, const char *user_id)
{
    if (!cJSON_IsObject(message) || user_id == NULL || user_id[0] == '\0') {
        return false;
    }

    cJSON *from = cJSON_GetObjectItemCaseSensitive(message, "from");
    if (cJSON_IsString(from) && from->valuestring != NULL && strcmp(from->valuestring, user_id) == 0) {
        return true;
    }

    cJSON *to = cJSON_GetObjectItemCaseSensitive(message, "to");
    if (!cJSON_IsObject(to)) {
        return false;
    }

    cJSON *all = cJSON_GetObjectItemCaseSensitive(to, "all");
    if (cJSON_IsTrue(all)) {
        return true;
    }

    return json_array_contains_string(cJSON_GetObjectItemCaseSensitive(to, "users"), user_id);
}

static bool json_safe_message_id(cJSON *item, bool allow_zero, uint64_t *id_out)
{
    if (!cJSON_IsNumber(item) ||
        item->valuedouble < (allow_zero ? 0 : 1) ||
        item->valuedouble > (double)CHAT_MESSAGE_MAX_SAFE_ID) {
        return false;
    }

    uint64_t parsed = (uint64_t)item->valuedouble;
    if ((double)parsed != item->valuedouble) {
        return false;
    }

    if (id_out != NULL) {
        *id_out = parsed;
    }
    return true;
}

static esp_err_t send_error_and_close(app_context_t *ctx, int fd, const char *code, const char *message)
{
    esp_err_t ret = chat_ws_send_error(ctx, fd, code, message);
    chat_ws_close_client(ctx, fd);
    return ret;
}

static esp_err_t relay_payload_to_targets(app_context_t *ctx, cJSON *to, const char *payload)
{
    if (ctx == NULL || !cJSON_IsObject(to) || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *all = cJSON_GetObjectItemCaseSensitive(to, "all");
    cJSON *users = cJSON_GetObjectItemCaseSensitive(to, "users");
    bool send_all = cJSON_IsTrue(all);

    if (!send_all && (!cJSON_IsArray(users) || cJSON_GetArraySize(users) == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    int fds[MAX_CLIENTS];
    int fd_count = 0;

    if (xSemaphoreTake(ctx->client_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->client_slots[i].active || !ctx->client_slots[i].joined || ctx->client_slots[i].user_id[0] == '\0') {
            continue;
        }
        if (!send_all && !json_array_contains_string(users, ctx->client_slots[i].user_id)) {
            continue;
        }

        if (fd_count < MAX_CLIENTS) {
            fds[fd_count++] = ctx->client_slots[i].fd;
        }
    }

    xSemaphoreGive(ctx->client_mutex);

    esp_err_t first_error = ESP_OK;
    for (int i = 0; i < fd_count; i++) {
        esp_err_t ret = chat_ws_send_text(ctx, fds[i], payload);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
        if (ret != ESP_OK) {
            chat_ws_close_client(ctx, fds[i]);
        }
    }

    return first_error;
}

static esp_err_t require_joined_identity(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    bool valid_from = json_string_in_range(from, MAX_USER_ID_LEN, false);
    chat_identity_status_t identity = chat_sessions_check_identity(ctx, fd, valid_from ? from->valuestring : NULL);
    if (identity == CHAT_IDENTITY_NOT_JOINED) {
        return send_error_and_close(ctx, fd, "not_joined", "Join before sending this message");
    }
    if (!valid_from || identity != CHAT_IDENTITY_OK) {
        return send_error_and_close(ctx, fd, "bad_identity", "Message sender does not match the joined user");
    }

    chat_sessions_update_time_sample(ctx, fd, root);
    return ESP_OK;
}

static esp_err_t handle_join_message(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    uint64_t since_id = 0;

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false)) {
        return send_error_and_close(ctx, fd, "bad_join", "Join requires valid from and name fields");
    }
    if (!chat_history_parse_since_id(root, &since_id)) {
        return send_error_and_close(ctx, fd, "bad_since_id", "since_id must be a safe non-negative integer");
    }
    if (chat_sessions_check_identity(ctx, fd, from->valuestring) == CHAT_IDENTITY_MISMATCH) {
        return send_error_and_close(ctx, fd, "bad_identity", "Join identity cannot change on an open connection");
    }

    if (!chat_sessions_update_identity(ctx, fd, from->valuestring, name->valuestring)) {
        return send_error_and_close(ctx, fd, "not_registered", "WebSocket client slot was not found");
    }

    chat_sessions_update_time_sample(ctx, fd, root);
    esp_err_t sync_ret = chat_history_send_to_client(ctx, fd, since_id);
    if (sync_ret == ESP_OK) {
        sync_ret = chat_history_send_info_to_client(ctx, fd);
    }
    if (sync_ret == ESP_OK) {
        sync_ret = chat_sessions_send_online_users_to_client(ctx, fd);
    }
    if (sync_ret != ESP_OK) {
        chat_ws_close_client(ctx, fd);
        return sync_ret;
    }
    chat_sessions_broadcast_online_users(ctx);
    return ESP_OK;
}

static esp_err_t handle_chat_message(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false)) {
        return chat_ws_send_error(ctx, fd, "bad_message", "Message requires valid from and name fields");
    }

    if (strcmp(type->valuestring, "text") == 0) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        if (!json_string_in_range(data, MAX_TEXT_BYTES, false)) {
            return chat_ws_send_error(ctx, fd, "bad_text", "Text message is empty or too long");
        }
        if (!validate_to_object(root)) {
            return chat_ws_send_error(ctx, fd, "bad_target", "Message target must be all users or a non-empty user list");
        }
    } else if (strcmp(type->valuestring, "newGroup") == 0) {
        cJSON *group_id = cJSON_GetObjectItemCaseSensitive(root, "groupId");
        cJSON *group_name = cJSON_GetObjectItemCaseSensitive(root, "groupName");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

        if (!json_string_in_range(group_id, MAX_GROUP_ID_LEN, false) ||
            !json_string_in_range(group_name, MAX_GROUP_NAME_LEN, false) ||
            !json_string_in_range(data, MAX_TEXT_BYTES, true) ||
            !validate_to_object(root)) {
            return chat_ws_send_error(ctx, fd, "bad_group", "Group creation requires groupId, groupName, data, and target users");
        }
    } else {
        return chat_ws_send_error(ctx, fd, "unknown_type", "Unsupported chat message type");
    }

    char *payload = NULL;
    esp_err_t store_ret = chat_history_finalize_and_store_message(ctx, root, &payload);
    if (store_ret != ESP_OK || payload == NULL) {
        if (store_ret == ESP_ERR_INVALID_SIZE) {
            return chat_ws_send_error(ctx, fd, "id_exhausted", "Message id space is exhausted");
        }
        ESP_LOGW(TAG, "Unable to store message: %s", esp_err_to_name(store_ret));
        return chat_ws_send_error(ctx, fd, "server_busy", "Unable to persist message id");
    }

    bool presence_changed = chat_ws_broadcast(ctx, payload);
    presence_changed = chat_history_broadcast_info(ctx) || presence_changed;
    if (presence_changed) {
        chat_sessions_broadcast_online_users(ctx);
    }
    return ESP_OK;
}

static bool validate_history_message_object(cJSON *message)
{
    if (!cJSON_IsObject(message)) {
        return false;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(message, "timestamp");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(message, "type");
    cJSON *from = cJSON_GetObjectItemCaseSensitive(message, "from");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(message, "name");

    if (!json_safe_message_id(id, false, NULL) ||
        !cJSON_IsNumber(timestamp) ||
        !json_string_in_range(type, 24, false) ||
        !json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !validate_to_object(message)) {
        return false;
    }

    if (strcmp(type->valuestring, "text") == 0) {
        return json_string_in_range(cJSON_GetObjectItemCaseSensitive(message, "data"), MAX_TEXT_BYTES, false);
    }

    if (strcmp(type->valuestring, "newGroup") == 0) {
        return json_string_in_range(cJSON_GetObjectItemCaseSensitive(message, "groupId"), MAX_GROUP_ID_LEN, false) &&
            json_string_in_range(cJSON_GetObjectItemCaseSensitive(message, "groupName"), MAX_GROUP_NAME_LEN, false) &&
            json_string_in_range(cJSON_GetObjectItemCaseSensitive(message, "data"), MAX_TEXT_BYTES, true);
    }

    return false;
}

static bool history_response_targets_match_message(cJSON *root, cJSON *message)
{
    cJSON *to = cJSON_GetObjectItemCaseSensitive(root, "to");
    cJSON *users = cJSON_IsObject(to) ? cJSON_GetObjectItemCaseSensitive(to, "users") : NULL;
    if (!cJSON_IsArray(users) || cJSON_GetArraySize(users) == 0) {
        return false;
    }

    cJSON *target = NULL;
    cJSON_ArrayForEach(target, users) {
        if (!json_string_in_range(target, MAX_USER_ID_LEN, false) ||
            !message_visible_to_user(message, target->valuestring)) {
            return false;
        }
    }

    return true;
}

static esp_err_t handle_history_request_message(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    cJSON *restore_before = cJSON_GetObjectItemCaseSensitive(root, "restore_before_id");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !json_string_in_range(request_id, MAX_REQUEST_ID_LEN, false) ||
        !json_safe_message_id(restore_before, false, NULL) ||
        restore_before->valuedouble <= 1) {
        return chat_ws_send_error(ctx, fd, "bad_history_request", "History request is invalid");
    }

    uint64_t requested_before = 0;
    json_safe_message_id(restore_before, false, &requested_before);
    uint64_t allowed_before = chat_history_current_restore_before_id(ctx);
    if (requested_before > allowed_before) {
        requested_before = allowed_before;
    }
    if (requested_before <= 1) {
        return chat_ws_send_error(ctx, fd, "no_restorable_history", "No older server history boundary is available");
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "restore_before_id");
    if (cJSON_AddNumberToObject(root, "restore_before_id", (double)requested_before) == NULL) {
        return chat_ws_send_error(ctx, fd, "server_busy", "Unable to relay history request");
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        return chat_ws_send_error(ctx, fd, "server_busy", "Unable to relay history request");
    }

    bool presence_changed = chat_ws_broadcast(ctx, payload);
    free(payload);
    if (presence_changed) {
        chat_sessions_broadcast_online_users(ctx);
    }
    return ESP_OK;
}

static esp_err_t handle_history_response_message(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItemCaseSensitive(root, "from");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !json_string_in_range(request_id, MAX_REQUEST_ID_LEN, false)) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response is missing sender fields");
    }
    if (!validate_to_object(root)) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response must target specific users");
    }

    cJSON *to = cJSON_GetObjectItemCaseSensitive(root, "to");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(to, "all"))) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response must target specific users");
    }
    if (!validate_history_message_object(message)) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response contains an invalid message");
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
    uint64_t response_id = 0;
    json_safe_message_id(id, false, &response_id);
    uint64_t restore_before_id = chat_history_current_restore_before_id(ctx);
    if (response_id >= restore_before_id) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response is not older than the server boundary");
    }
    if (!history_response_targets_match_message(root, message)) {
        return chat_ws_send_error(ctx, fd, "bad_history_response", "History response is not visible to the requested user");
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        return chat_ws_send_error(ctx, fd, "server_busy", "Unable to relay history response");
    }

    esp_err_t ret = relay_payload_to_targets(ctx, to, payload);
    free(payload);
    if (ret != ESP_OK) {
        chat_sessions_broadcast_online_users(ctx);
        return chat_ws_send_error(ctx, fd, "relay_failed", "Unable to relay history response");
    }

    return ESP_OK;
}

esp_err_t chat_protocol_handle_json(app_context_t *ctx, int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!json_string_in_range(type, 24, false)) {
        if (!chat_sessions_is_joined(ctx, fd)) {
            return send_error_and_close(ctx, fd, "bad_type", "Message type is required");
        }
        return chat_ws_send_error(ctx, fd, "bad_type", "Message type is required");
    }

    if (strcmp(type->valuestring, "pong") == 0) {
        chat_sessions_mark_alive(ctx, fd);
        if (chat_sessions_is_joined(ctx, fd)) {
            chat_sessions_update_time_sample(ctx, fd, root);
        }
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "join") == 0) {
        return handle_join_message(ctx, fd, root);
    }

    esp_err_t identity_ret = require_joined_identity(ctx, fd, root);
    if (identity_ret != ESP_OK) {
        return identity_ret;
    }

    if (strcmp(type->valuestring, "getOnlineUser") == 0) {
        chat_sessions_send_online_users_to_client(ctx, fd);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "text") == 0 || strcmp(type->valuestring, "newGroup") == 0) {
        return handle_chat_message(ctx, fd, root);
    }

    if (strcmp(type->valuestring, "historyRequest") == 0) {
        return handle_history_request_message(ctx, fd, root);
    }

    if (strcmp(type->valuestring, "historyResponse") == 0) {
        return handle_history_response_message(ctx, fd, root);
    }

    return chat_ws_send_error(ctx, fd, "unknown_type", "Unsupported message type");
}
