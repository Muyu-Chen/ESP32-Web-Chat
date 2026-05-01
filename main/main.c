/*
 * ESP32 WiFi Chat Server
*/
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "nvs.h"

#include <string.h>
#include <time.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "message_id_store.h"
#include <errno.h>

#define CHAT_WIFI_SSID             CONFIG_CHAT_WIFI_SSID
#define CHAT_WIFI_PASS             CONFIG_CHAT_WIFI_PASSWORD
#define CHAT_ADMIN_PASS            CONFIG_CHAT_ADMIN_PASSWORD
#define CHAT_WIFI_CHANNEL          CONFIG_CHAT_WIFI_CHANNEL
#define CHAT_MAX_STA_CONN          CONFIG_CHAT_MAX_STA_CONN
#define MAX_CLIENTS                CONFIG_CHAT_MAX_WS_CLIENTS
#define MAX_MESSAGES               CONFIG_CHAT_MESSAGE_HISTORY_SIZE
#define HEARTBEAT_INTERVAL_S       CONFIG_CHAT_HEARTBEAT_INTERVAL_S
#define MAX_TEXT_BYTES             CONFIG_CHAT_MAX_MESSAGE_TEXT_LEN
#define MAX_WS_PAYLOAD_BYTES       CONFIG_CHAT_MAX_WS_PAYLOAD_BYTES

#define MAX_USER_ID_LEN            63
#define MAX_NAME_LEN               31
#define MAX_REQUEST_ID_LEN         63
#define MAX_GROUP_ID_LEN           63
#define MAX_GROUP_NAME_LEN         63
#define MAX_WIFI_SSID_LEN          32
#define MIN_WIFI_PASS_LEN          8
#define MAX_WIFI_PASS_LEN          63
#define MIN_ADMIN_PASS_LEN         4
#define MAX_ADMIN_PASS_LEN         32
#define SETTINGS_BODY_BYTES        512
#define DNS_PACKET_BYTES           256
#define DNS_ANSWER_BYTES           16
#define VALID_EPOCH_START_S        946684800LL
#define VALID_EPOCH_END_S          4102444800LL

static const char *TAG = "CHAT_SERVER";
static const char *DNS_TAG = "DNS";
static const char *SETTINGS_NAMESPACE = "chatcfg";

typedef struct {
    int fd;
    bool active;
    bool is_alive;
    char user_id[MAX_USER_ID_LEN + 1];
    char name[MAX_NAME_LEN + 1];
} client_slot_t;

typedef struct {
    char *payload;
    uint64_t id;
} message_t;

typedef struct {
    char ssid[MAX_WIFI_SSID_LEN + 1];
    char password[MAX_WIFI_PASS_LEN + 1];
    uint8_t channel;
    char admin_password[MAX_ADMIN_PASS_LEN + 1];
} chat_settings_t;

static client_slot_t client_slots[MAX_CLIENTS];
static SemaphoreHandle_t client_mutex;

static message_t message_buffer[MAX_MESSAGES];
static uint64_t message_id_counter = 0;
static uint64_t boot_start_id = 1;
static int message_buffer_head = 0;
static SemaphoreHandle_t message_mutex;

static chat_settings_t chat_settings;
static httpd_handle_t server = NULL;

static void load_chat_settings(void);
static void wifi_init_softap(void);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t favicon_get_handler(httpd_req_t *req);
static esp_err_t style_get_handler(httpd_req_t *req);
static esp_err_t script_get_handler(httpd_req_t *req);
static esp_err_t settings_get_handler(httpd_req_t *req);
static esp_err_t settings_post_handler(httpd_req_t *req);
static esp_err_t redirect_to_root_handler(httpd_req_t *req);
static esp_err_t ws_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);
static void dns_server_task(void *pvParameters);
static void heartbeat_task(void *pvParameters);

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static bool json_string_in_range(const cJSON *item, size_t max_len, bool allow_empty)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    size_t len = strlen(item->valuestring);
    return (allow_empty || len > 0) && len <= max_len;
}

static bool valid_wifi_password(const char *password)
{
    size_t len = password ? strlen(password) : 0;
    return len == 0 || (len >= MIN_WIFI_PASS_LEN && len <= MAX_WIFI_PASS_LEN);
}

static void reset_chat_settings_to_defaults(void)
{
    copy_bounded(chat_settings.ssid, sizeof(chat_settings.ssid), CHAT_WIFI_SSID);
    copy_bounded(chat_settings.password, sizeof(chat_settings.password), CHAT_WIFI_PASS);
    chat_settings.channel = CHAT_WIFI_CHANNEL;
    copy_bounded(chat_settings.admin_password, sizeof(chat_settings.admin_password), CHAT_ADMIN_PASS);

    if (chat_settings.ssid[0] == '\0') {
        copy_bounded(chat_settings.ssid, sizeof(chat_settings.ssid), "ESPChat");
    }
    if (!valid_wifi_password(chat_settings.password)) {
        copy_bounded(chat_settings.password, sizeof(chat_settings.password), "esp-chat");
    }
    if (chat_settings.channel < 1 || chat_settings.channel > 13) {
        chat_settings.channel = 1;
    }
    if (strlen(chat_settings.admin_password) < MIN_ADMIN_PASS_LEN) {
        copy_bounded(chat_settings.admin_password, sizeof(chat_settings.admin_password), "admin");
    }
}

static void load_chat_settings(void)
{
    reset_chat_settings_to_defaults();

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Using factory default settings: %s", esp_err_to_name(ret));
        return;
    }

    size_t len = sizeof(chat_settings.ssid);
    nvs_get_str(nvs, "ssid", chat_settings.ssid, &len);

    len = sizeof(chat_settings.password);
    nvs_get_str(nvs, "pass", chat_settings.password, &len);

    len = sizeof(chat_settings.admin_password);
    nvs_get_str(nvs, "admin", chat_settings.admin_password, &len);

    uint8_t channel = chat_settings.channel;
    if (nvs_get_u8(nvs, "chan", &channel) == ESP_OK) {
        chat_settings.channel = channel;
    }
    nvs_close(nvs);

    if (chat_settings.ssid[0] == '\0' ||
        !valid_wifi_password(chat_settings.password) ||
        chat_settings.channel < 1 ||
        chat_settings.channel > 13 ||
        strlen(chat_settings.admin_password) < MIN_ADMIN_PASS_LEN) {
        ESP_LOGW(TAG, "Stored settings were invalid; falling back to factory defaults");
        reset_chat_settings_to_defaults();
    }
}

static esp_err_t save_chat_settings(const chat_settings_t *settings)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, "ssid", settings->ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, "pass", settings->password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, "admin", settings->admin_password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "chan", settings->channel);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static void restart_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static int64_t current_timestamp_s(const cJSON *incoming)
{
    time_t now = 0;
    time(&now);
    if ((int64_t)now >= VALID_EPOCH_START_S) {
        return (int64_t)now;
    }

    cJSON *client_ts = incoming ? cJSON_GetObjectItem(incoming, "timestamp") : NULL;
    if (cJSON_IsNumber(client_ts) &&
        client_ts->valuedouble >= VALID_EPOCH_START_S &&
        client_ts->valuedouble <= VALID_EPOCH_END_S) {
        return (int64_t)client_ts->valuedouble;
    }

    return esp_timer_get_time() / 1000000LL;
}

static esp_err_t send_text_to_fd(int fd, const char *payload)
{
    if (server == NULL || payload == NULL || fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)payload;
    ws_pkt.len = strlen(payload);

    return httpd_ws_send_frame_async(server, fd, &ws_pkt);
}

static void broadcast_message(const char *payload)
{
    if (payload == NULL || xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!client_slots[i].active) {
            continue;
        }

        esp_err_t ret = send_text_to_fd(client_slots[i].fd, payload);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send to fd=%d: %s", client_slots[i].fd, esp_err_to_name(ret));
            client_slots[i].active = false;
            client_slots[i].user_id[0] = '\0';
            client_slots[i].name[0] = '\0';
        }
    }

    xSemaphoreGive(client_mutex);
}

static esp_err_t send_error_to_client(int fd, const char *code, const char *message)
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

    esp_err_t ret = send_text_to_fd(fd, payload);
    free(payload);
    return ret;
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode JSON");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static esp_err_t send_http_error(httpd_req_t *req, const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create error response");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "code", code ? code : "error");
    cJSON_AddStringToObject(root, "message", message ? message : "Request failed");

    esp_err_t ret = send_json_response(req, root);
    cJSON_Delete(root);
    return ret;
}

static char *read_request_body(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0 || req->content_len > max_len) {
        return NULL;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return NULL;
        }
        received += ret;
    }

    return body;
}

static uint64_t json_since_id(const cJSON *root)
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

typedef struct {
    uint64_t boot_start_id;
    uint64_t current_id;
    uint64_t earliest_id;
    uint64_t latest_id;
    uint64_t restore_before_id;
    int count;
    int capacity;
    bool has_more_before;
} history_bounds_t;

static void fill_history_bounds_locked(history_bounds_t *bounds)
{
    memset(bounds, 0, sizeof(*bounds));
    bounds->boot_start_id = boot_start_id;
    bounds->current_id = message_id_counter;
    bounds->capacity = MAX_MESSAGES;

    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (message_buffer[i].payload == NULL) {
            continue;
        }

        uint64_t id = message_buffer[i].id;
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

static uint64_t current_restore_before_id(void)
{
    uint64_t restore_before_id = boot_start_id;

    if (xSemaphoreTake(message_mutex, portMAX_DELAY) == pdTRUE) {
        history_bounds_t bounds;
        fill_history_bounds_locked(&bounds);
        restore_before_id = bounds.restore_before_id;
        xSemaphoreGive(message_mutex);
    }

    return restore_before_id;
}

static bool validate_to_object(cJSON *root)
{
    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (to == NULL) {
        to = cJSON_AddObjectToObject(root, "to");
        if (to == NULL) {
            return false;
        }
    }
    if (!cJSON_IsObject(to)) {
        return false;
    }

    cJSON *all = cJSON_GetObjectItem(to, "all");
    if (all == NULL) {
        all = cJSON_AddBoolToObject(to, "all", false);
    }
    if (!cJSON_IsBool(all)) {
        return false;
    }

    cJSON *users = cJSON_GetObjectItem(to, "users");
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

static bool json_array_contains_string(cJSON *array, const char *value)
{
    if (!cJSON_IsArray(array) || value == NULL) {
        return false;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && item->valuestring != NULL && strcmp(item->valuestring, value) == 0) {
            return true;
        }
    }

    return false;
}

static bool message_visible_to_user(cJSON *message, const char *user_id)
{
    if (!cJSON_IsObject(message) || user_id == NULL || user_id[0] == '\0') {
        return false;
    }

    cJSON *from = cJSON_GetObjectItem(message, "from");
    if (cJSON_IsString(from) && from->valuestring != NULL && strcmp(from->valuestring, user_id) == 0) {
        return true;
    }

    cJSON *to = cJSON_GetObjectItem(message, "to");
    if (!cJSON_IsObject(to)) {
        return false;
    }

    cJSON *all = cJSON_GetObjectItem(to, "all");
    if (cJSON_IsTrue(all)) {
        return true;
    }

    return json_array_contains_string(cJSON_GetObjectItem(to, "users"), user_id);
}

static esp_err_t relay_payload_to_targets(cJSON *to, const char *payload)
{
    if (!cJSON_IsObject(to) || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *all = cJSON_GetObjectItem(to, "all");
    cJSON *users = cJSON_GetObjectItem(to, "users");
    bool send_all = cJSON_IsTrue(all);

    if (!send_all && (!cJSON_IsArray(users) || cJSON_GetArraySize(users) == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t first_error = ESP_OK;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!client_slots[i].active || client_slots[i].user_id[0] == '\0') {
            continue;
        }
        if (!send_all && !json_array_contains_string(users, client_slots[i].user_id)) {
            continue;
        }

        esp_err_t ret = send_text_to_fd(client_slots[i].fd, payload);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        }
    }

    xSemaphoreGive(client_mutex);
    return first_error;
}

static bool update_client_identity(int fd, const char *user_id, const char *name)
{
    bool updated = false;

    if (xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_slots[i].active && client_slots[i].fd == fd) {
            copy_bounded(client_slots[i].user_id, sizeof(client_slots[i].user_id), user_id);
            copy_bounded(client_slots[i].name, sizeof(client_slots[i].name), name);
            client_slots[i].is_alive = true;
            updated = true;
            break;
        }
    }

    xSemaphoreGive(client_mutex);
    return updated;
}

static bool mark_client_alive(int fd)
{
    bool marked = false;

    if (xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_slots[i].active && client_slots[i].fd == fd) {
            client_slots[i].is_alive = true;
            marked = true;
            break;
        }
    }

    xSemaphoreGive(client_mutex);
    return marked;
}

static bool remove_client_by_fd(int fd)
{
    bool removed = false;

    if (xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (client_slots[i].active && client_slots[i].fd == fd) {
            client_slots[i].active = false;
            client_slots[i].is_alive = false;
            client_slots[i].user_id[0] = '\0';
            client_slots[i].name[0] = '\0';
            removed = true;
            break;
        }
    }

    xSemaphoreGive(client_mutex);
    return removed;
}

static char *build_online_users_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *to = NULL;
    cJSON *users = NULL;
    cJSON *data = NULL;
    char *payload = NULL;

    if (root == NULL) {
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

    if (xSemaphoreTake(client_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!client_slots[i].active || client_slots[i].user_id[0] == '\0') {
                continue;
            }

            cJSON *id = cJSON_CreateString(client_slots[i].user_id);
            if (id) {
                cJSON_AddItemToArray(users, id);
            }

            cJSON *entry = cJSON_CreateObject();
            if (entry) {
                cJSON_AddStringToObject(entry, "id", client_slots[i].user_id);
                cJSON_AddStringToObject(entry, "name", client_slots[i].name[0] ? client_slots[i].name : "New User");
                cJSON_AddItemToArray(data, entry);
            }
        }
        xSemaphoreGive(client_mutex);
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static void broadcast_online_users(void)
{
    char *payload = build_online_users_payload();
    if (payload == NULL) {
        ESP_LOGW(TAG, "Failed to build online user list");
        return;
    }

    broadcast_message(payload);
    free(payload);
}

static void send_online_users_to_client(int fd)
{
    char *payload = build_online_users_payload();
    if (payload == NULL) {
        send_error_to_client(fd, "server_busy", "Unable to build online user list");
        return;
    }

    send_text_to_fd(fd, payload);
    free(payload);
}

static char *build_history_info_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
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

    if (xSemaphoreTake(message_mutex, portMAX_DELAY) != pdTRUE) {
        cJSON_Delete(root);
        return NULL;
    }

    history_bounds_t bounds;
    fill_history_bounds_locked(&bounds);
    xSemaphoreGive(message_mutex);

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

static void send_history_info_to_client(int fd)
{
    char *payload = build_history_info_payload();
    if (payload == NULL) {
        send_error_to_client(fd, "server_busy", "History boundary is temporarily unavailable");
        return;
    }

    send_text_to_fd(fd, payload);
    free(payload);
}

static void broadcast_history_info(void)
{
    char *payload = build_history_info_payload();
    if (payload == NULL) {
        ESP_LOGW(TAG, "Failed to build history boundary payload");
        return;
    }

    broadcast_message(payload);
    free(payload);
}

static void send_history_to_client(int fd, uint64_t since_id)
{
    if (xSemaphoreTake(message_mutex, portMAX_DELAY) != pdTRUE) {
        send_error_to_client(fd, "server_busy", "Message history is temporarily unavailable");
        return;
    }

    int sent = 0;
    int current_pos = message_buffer_head;
    for (int i = 0; i < MAX_MESSAGES; i++) {
        int index = (current_pos + i) % MAX_MESSAGES;
        if (message_buffer[index].payload == NULL || message_buffer[index].id <= since_id) {
            continue;
        }

        esp_err_t ret = send_text_to_fd(fd, message_buffer[index].payload);
        if (ret == ESP_OK) {
            sent++;
        } else {
            ESP_LOGW(TAG, "History send failed for fd=%d: %s", fd, esp_err_to_name(ret));
            break;
        }
    }

    xSemaphoreGive(message_mutex);
    ESP_LOGI(TAG, "Sent %d history messages to fd=%d since_id=%" PRIu64, sent, fd, since_id);
}

static esp_err_t finalize_and_store_message(cJSON *root, char **payload_out)
{
    char *payload = NULL;
    esp_err_t ret = ESP_OK;

    if (payload_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *payload_out = NULL;

    if (xSemaphoreTake(message_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (message_id_counter >= CHAT_MESSAGE_MAX_SAFE_ID) {
        ret = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    uint64_t id = message_id_counter + 1;
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
        ret = chat_message_ids_persist(id, boot_start_id);
        if (ret != ESP_OK) {
            free(payload);
            goto out;
        }

        if (message_buffer[message_buffer_head].payload != NULL) {
            free(message_buffer[message_buffer_head].payload);
        }

        message_id_counter = id;
        message_buffer[message_buffer_head].payload = payload;
        message_buffer[message_buffer_head].id = id;
        message_buffer_head = (message_buffer_head + 1) % MAX_MESSAGES;
        *payload_out = payload;
    } else {
        ret = ESP_ERR_NO_MEM;
    }

out:
    xSemaphoreGive(message_mutex);
    return ret;
}

static esp_err_t handle_join_message(int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *name = cJSON_GetObjectItem(root, "name");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false)) {
        return send_error_to_client(fd, "bad_join", "Join requires valid from and name fields");
    }

    if (!update_client_identity(fd, from->valuestring, name->valuestring)) {
        return send_error_to_client(fd, "not_registered", "WebSocket client slot was not found");
    }

    send_history_to_client(fd, json_since_id(root));
    send_history_info_to_client(fd);
    send_online_users_to_client(fd);
    broadcast_online_users();
    return ESP_OK;
}

static esp_err_t handle_chat_message(int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *name = cJSON_GetObjectItem(root, "name");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false)) {
        return send_error_to_client(fd, "bad_message", "Message requires valid from and name fields");
    }

    update_client_identity(fd, from->valuestring, name->valuestring);

    if (strcmp(type->valuestring, "text") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!json_string_in_range(data, MAX_TEXT_BYTES, false)) {
            return send_error_to_client(fd, "bad_text", "Text message is empty or too long");
        }
        if (!validate_to_object(root)) {
            return send_error_to_client(fd, "bad_target", "Message target must be all users or a non-empty user list");
        }
    } else if (strcmp(type->valuestring, "newGroup") == 0) {
        cJSON *group_id = cJSON_GetObjectItem(root, "groupId");
        cJSON *group_name = cJSON_GetObjectItem(root, "groupName");
        cJSON *data = cJSON_GetObjectItem(root, "data");

        if (!json_string_in_range(group_id, MAX_GROUP_ID_LEN, false) ||
            !json_string_in_range(group_name, MAX_GROUP_NAME_LEN, false) ||
            !json_string_in_range(data, MAX_TEXT_BYTES, true) ||
            !validate_to_object(root)) {
            return send_error_to_client(fd, "bad_group", "Group creation requires groupId, groupName, data, and target users");
        }
    } else {
        return send_error_to_client(fd, "unknown_type", "Unsupported chat message type");
    }

    char *payload = NULL;
    esp_err_t store_ret = finalize_and_store_message(root, &payload);
    if (store_ret != ESP_OK || payload == NULL) {
        if (store_ret == ESP_ERR_INVALID_SIZE) {
            return send_error_to_client(fd, "id_exhausted", "Message id space is exhausted");
        }
        ESP_LOGW(TAG, "Unable to store message: %s", esp_err_to_name(store_ret));
        return send_error_to_client(fd, "server_busy", "Unable to persist message id");
    }

    broadcast_message(payload);
    broadcast_history_info();
    return ESP_OK;
}

static bool validate_history_message_object(cJSON *message)
{
    if (!cJSON_IsObject(message)) {
        return false;
    }

    cJSON *id = cJSON_GetObjectItem(message, "id");
    cJSON *type = cJSON_GetObjectItem(message, "type");
    cJSON *from = cJSON_GetObjectItem(message, "from");
    cJSON *name = cJSON_GetObjectItem(message, "name");

    if (!cJSON_IsNumber(id) ||
        id->valuedouble <= 0 ||
        id->valuedouble > (double)CHAT_MESSAGE_MAX_SAFE_ID ||
        !json_string_in_range(type, 24, false) ||
        !json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !validate_to_object(message)) {
        return false;
    }

    if (strcmp(type->valuestring, "text") == 0) {
        return json_string_in_range(cJSON_GetObjectItem(message, "data"), MAX_TEXT_BYTES, false);
    }

    if (strcmp(type->valuestring, "newGroup") == 0) {
        return json_string_in_range(cJSON_GetObjectItem(message, "groupId"), MAX_GROUP_ID_LEN, false) &&
            json_string_in_range(cJSON_GetObjectItem(message, "groupName"), MAX_GROUP_NAME_LEN, false) &&
            json_string_in_range(cJSON_GetObjectItem(message, "data"), MAX_TEXT_BYTES, true);
    }

    return false;
}

static bool history_response_targets_match_message(cJSON *root, cJSON *message)
{
    cJSON *to = cJSON_GetObjectItem(root, "to");
    cJSON *users = cJSON_IsObject(to) ? cJSON_GetObjectItem(to, "users") : NULL;
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

static esp_err_t handle_history_request_message(int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *request_id = cJSON_GetObjectItem(root, "requestId");
    cJSON *restore_before = cJSON_GetObjectItem(root, "restore_before_id");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !json_string_in_range(request_id, MAX_REQUEST_ID_LEN, false) ||
        !cJSON_IsNumber(restore_before) ||
        restore_before->valuedouble <= 1 ||
        restore_before->valuedouble > (double)CHAT_MESSAGE_MAX_SAFE_ID) {
        return send_error_to_client(fd, "bad_history_request", "History request is invalid");
    }

    update_client_identity(fd, from->valuestring, name->valuestring);

    uint64_t requested_before = (uint64_t)restore_before->valuedouble;
    uint64_t allowed_before = current_restore_before_id();
    if (requested_before > allowed_before) {
        requested_before = allowed_before;
    }
    if (requested_before <= 1) {
        return send_error_to_client(fd, "no_restorable_history", "No older server history boundary is available");
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "restore_before_id");
    if (cJSON_AddNumberToObject(root, "restore_before_id", (double)requested_before) == NULL) {
        return send_error_to_client(fd, "server_busy", "Unable to relay history request");
    }

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        return send_error_to_client(fd, "server_busy", "Unable to relay history request");
    }

    broadcast_message(payload);
    free(payload);
    return ESP_OK;
}

static esp_err_t handle_history_response_message(int fd, cJSON *root)
{
    cJSON *from = cJSON_GetObjectItem(root, "from");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *request_id = cJSON_GetObjectItem(root, "requestId");
    cJSON *message = cJSON_GetObjectItem(root, "message");

    if (!json_string_in_range(from, MAX_USER_ID_LEN, false) ||
        !json_string_in_range(name, MAX_NAME_LEN, false) ||
        !json_string_in_range(request_id, MAX_REQUEST_ID_LEN, false)) {
        return send_error_to_client(fd, "bad_history_response", "History response is missing sender fields");
    }
    if (!validate_to_object(root)) {
        return send_error_to_client(fd, "bad_history_response", "History response must target specific users");
    }

    cJSON *to = cJSON_GetObjectItem(root, "to");
    if (cJSON_IsTrue(cJSON_GetObjectItem(to, "all"))) {
        return send_error_to_client(fd, "bad_history_response", "History response must target specific users");
    }
    if (!validate_history_message_object(message)) {
        return send_error_to_client(fd, "bad_history_response", "History response contains an invalid message");
    }

    cJSON *id = cJSON_GetObjectItem(message, "id");
    uint64_t restore_before_id = current_restore_before_id();
    if (id->valuedouble >= (double)restore_before_id) {
        return send_error_to_client(fd, "bad_history_response", "History response is not older than the server boundary");
    }
    if (!history_response_targets_match_message(root, message)) {
        return send_error_to_client(fd, "bad_history_response", "History response is not visible to the requested user");
    }

    update_client_identity(fd, from->valuestring, name->valuestring);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        return send_error_to_client(fd, "server_busy", "Unable to relay history response");
    }

    esp_err_t ret = relay_payload_to_targets(to, payload);
    free(payload);
    if (ret != ESP_OK) {
        return send_error_to_client(fd, "relay_failed", "Unable to relay history response");
    }

    return ESP_OK;
}

static esp_err_t handle_ws_json(int fd, cJSON *root)
{
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!json_string_in_range(type, 24, false)) {
        return send_error_to_client(fd, "bad_type", "Message type is required");
    }

    if (strcmp(type->valuestring, "pong") == 0) {
        mark_client_alive(fd);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "join") == 0) {
        return handle_join_message(fd, root);
    }

    if (strcmp(type->valuestring, "getOnlineUser") == 0) {
        send_online_users_to_client(fd);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "text") == 0 || strcmp(type->valuestring, "newGroup") == 0) {
        return handle_chat_message(fd, root);
    }

    if (strcmp(type->valuestring, "historyRequest") == 0) {
        return handle_history_request_message(fd, root);
    }

    if (strcmp(type->valuestring, "historyResponse") == 0) {
        return handle_history_response_message(fd, root);
    }

    return send_error_to_client(fd, "unknown_type", "Unsupported message type");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    client_mutex = xSemaphoreCreateMutex();
    message_mutex = xSemaphoreCreateMutex();
    assert(client_mutex && message_mutex);

    chat_message_id_state_t id_state = { 0 };
    esp_err_t id_ret = chat_message_ids_load(&id_state);
    if (id_ret != ESP_OK) {
        ESP_LOGW(TAG, "Message ids may not persist until NVS recovers: %s", esp_err_to_name(id_ret));
    }
    message_id_counter = id_state.current_id;
    boot_start_id = id_state.boot_start_id;

    load_chat_settings();
    wifi_init_softap();

    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat_task", 4096, NULL, 5, NULL);

    server = start_webserver();
}

// --- Heartbeat Task ---
static void heartbeat_task(void *pvParameters)
{
    const char *ping_payload = "{\"type\":\"ping\"}";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_S * 1000));

        bool changed = false;
        if (xSemaphoreTake(client_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!client_slots[i].active) {
                continue;
            }

            if (!client_slots[i].is_alive) {
                ESP_LOGW(TAG, "Client fd=%d missed heartbeat; closing", client_slots[i].fd);
                httpd_sess_trigger_close(server, client_slots[i].fd);
                client_slots[i].active = false;
                client_slots[i].user_id[0] = '\0';
                client_slots[i].name[0] = '\0';
                changed = true;
                continue;
            }

            client_slots[i].is_alive = false;
            esp_err_t ret = send_text_to_fd(client_slots[i].fd, ping_payload);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Ping failed for fd=%d: %s", client_slots[i].fd, esp_err_to_name(ret));
                client_slots[i].active = false;
                changed = true;
            }
        }

        xSemaphoreGive(client_mutex);

        if (changed) {
            broadcast_online_users();
        }
    }
}


// --- DNS Server Task ---
// const static char *DNS_TAG = "DNS"; // Moved to global scope for better logging in this function

static void dns_server_task(void *pvParameters)
{
    uint8_t buffer[DNS_PACKET_BYTES];
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(DNS_TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(DNS_TAG, "Failed to bind socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(DNS_TAG, "DNS Server started");

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client, &client_len);
        if (len < 12 || len + DNS_ANSWER_BYTES > (int)sizeof(buffer)) {
            continue;
        }

        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

        buffer[2] = 0x81;
        buffer[3] = 0x80;
        buffer[6] = 0x00;
        buffer[7] = 0x01;

            buffer[len++] = 0xc0;
            buffer[len++] = 0x0c;

            buffer[len++] = 0x00;
            buffer[len++] = 0x01;

            buffer[len++] = 0x00;
            buffer[len++] = 0x01;

            buffer[len++] = 0x00;
            buffer[len++] = 0x00;
            buffer[len++] = 0x00;
            buffer[len++] = 0x3c;

            buffer[len++] = 0x00;
            buffer[len++] = 0x04;

            memcpy(&buffer[len], &ip_info.ip.addr, 4);
            len += 4;

            sendto(sock, buffer, len, 0, (struct sockaddr *)&client, client_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

// --- Network and Server Setup ---

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *p_netif = esp_netif_create_default_wifi_ap();
    assert(p_netif);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(p_netif));

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(p_netif, &ip_info));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(p_netif));

    esp_netif_dns_info_t dns_info = { 0 };
    dns_info.ip.u_addr.ip4.addr = ip_info.ip.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(p_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    memcpy(wifi_config.ap.ssid, chat_settings.ssid, strlen(chat_settings.ssid));
    copy_bounded((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), chat_settings.password);
    wifi_config.ap.ssid_len = strlen(chat_settings.ssid);
    wifi_config.ap.channel = chat_settings.channel;
    wifi_config.ap.max_connection = CHAT_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (strlen(chat_settings.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_get_ip_info(p_netif, &ip_info);
    ESP_LOGI(TAG, "SoftAP started: ssid=%s channel=%u ip=" IPSTR, chat_settings.ssid, chat_settings.channel, IP2STR(&ip_info.ip));
}

static esp_err_t redirect_to_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Redirecting %s to root", req->uri);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    char location_url[32];
    snprintf(location_url, sizeof(location_url), "http://" IPSTR "/", IP2STR(&ip_info.ip));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t local_server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;

    int desired_sockets = MAX_CLIENTS + 3;
#ifdef CONFIG_LWIP_MAX_SOCKETS
    if (desired_sockets > CONFIG_LWIP_MAX_SOCKETS) {
        desired_sockets = CONFIG_LWIP_MAX_SOCKETS;
    }
#endif
    if (desired_sockets > config.max_open_sockets) {
        config.max_open_sockets = desired_sockets;
    }

    ESP_LOGI(TAG, "Starting webserver with max_open_sockets=%d", config.max_open_sockets);

    if (httpd_start(&local_server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(local_server, &root);

        httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler };
        httpd_register_uri_handler(local_server, &favicon);

        httpd_uri_t style = { .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler };
        httpd_register_uri_handler(local_server, &style);

        httpd_uri_t script = { .uri = "/script.js", .method = HTTP_GET, .handler = script_get_handler };
        httpd_register_uri_handler(local_server, &script);

        httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
        httpd_register_uri_handler(local_server, &ws);

        httpd_uri_t settings_get = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler };
        httpd_register_uri_handler(local_server, &settings_get);

        httpd_uri_t settings_post = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler };
        httpd_register_uri_handler(local_server, &settings_post);

        httpd_uri_t catch_all = { .uri = "/*", .method = HTTP_GET, .handler = redirect_to_root_handler };
        httpd_register_uri_handler(local_server, &catch_all);
    }

    return local_server;
}

// --- HTTP and WebSocket Handlers ---

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving root page");
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving favicon");
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);

    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t style_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving style.css");
    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[]   asm("_binary_style_css_end");
    const size_t style_css_size = (style_css_end - style_css_start);

    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    httpd_resp_send(req, (const char *)style_css_start, style_css_size);
    return ESP_OK;
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    extern const unsigned char script_js_start[] asm("_binary_script_js_start");
    extern const unsigned char script_js_end[] asm("_binary_script_js_end");
    const size_t script_js_size = (script_js_end - script_js_start);

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    httpd_resp_send(req, (const char *)script_js_start, script_js_size);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create settings response");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "ssid", chat_settings.ssid);
    cJSON_AddNumberToObject(root, "channel", chat_settings.channel);
    cJSON_AddBoolToObject(root, "passwordSet", chat_settings.password[0] != '\0');
    cJSON_AddNumberToObject(root, "maxSsidLength", MAX_WIFI_SSID_LEN);
    cJSON_AddNumberToObject(root, "minPasswordLength", MIN_WIFI_PASS_LEN);
    cJSON_AddNumberToObject(root, "maxPasswordLength", MAX_WIFI_PASS_LEN);
    cJSON_AddNumberToObject(root, "minAdminPasswordLength", MIN_ADMIN_PASS_LEN);

    esp_err_t ret = send_json_response(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char *body = read_request_body(req, SETTINGS_BODY_BYTES);
    if (body == NULL) {
        return send_http_error(req, "bad_body", "Settings payload is empty or too large");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_http_error(req, "bad_json", "Settings payload must be a JSON object");
    }

    cJSON *admin_password = cJSON_GetObjectItem(root, "adminPassword");
    if (!json_string_in_range(admin_password, MAX_ADMIN_PASS_LEN, false) ||
        strcmp(admin_password->valuestring, chat_settings.admin_password) != 0) {
        cJSON_Delete(root);
        return send_http_error(req, "unauthorized", "Admin password is incorrect");
    }

    chat_settings_t updated = chat_settings;

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    if (ssid != NULL) {
        if (!json_string_in_range(ssid, MAX_WIFI_SSID_LEN, false)) {
            cJSON_Delete(root);
            return send_http_error(req, "bad_ssid", "SSID must be 1 to 32 bytes");
        }
        copy_bounded(updated.ssid, sizeof(updated.ssid), ssid->valuestring);
    }

    cJSON *channel = cJSON_GetObjectItem(root, "channel");
    if (channel != NULL) {
        if (!cJSON_IsNumber(channel) || channel->valueint < 1 || channel->valueint > 13) {
            cJSON_Delete(root);
            return send_http_error(req, "bad_channel", "Wi-Fi channel must be between 1 and 13");
        }
        updated.channel = (uint8_t)channel->valueint;
    }

    cJSON *open_network = cJSON_GetObjectItem(root, "openNetwork");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsTrue(open_network)) {
        updated.password[0] = '\0';
    } else if (password != NULL) {
        if (!cJSON_IsString(password) || password->valuestring == NULL) {
            cJSON_Delete(root);
            return send_http_error(req, "bad_password", "Password must be a string");
        }
        if (password->valuestring[0] != '\0') {
            if (!valid_wifi_password(password->valuestring)) {
                cJSON_Delete(root);
                return send_http_error(req, "bad_password", "WPA2 password must be 8 to 63 bytes");
            }
            copy_bounded(updated.password, sizeof(updated.password), password->valuestring);
        }
    }

    cJSON *new_admin_password = cJSON_GetObjectItem(root, "newAdminPassword");
    if (new_admin_password != NULL && cJSON_IsString(new_admin_password) && new_admin_password->valuestring[0] != '\0') {
        if (!json_string_in_range(new_admin_password, MAX_ADMIN_PASS_LEN, false) ||
            strlen(new_admin_password->valuestring) < MIN_ADMIN_PASS_LEN) {
            cJSON_Delete(root);
            return send_http_error(req, "bad_admin_password", "New admin password is too short");
        }
        copy_bounded(updated.admin_password, sizeof(updated.admin_password), new_admin_password->valuestring);
    }

    esp_err_t save_ret = save_chat_settings(&updated);
    if (save_ret != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(save_ret));
        return send_http_error(req, "save_failed", "Unable to save settings to NVS");
    }

    chat_settings = updated;
    bool reboot = cJSON_IsTrue(cJSON_GetObjectItem(root, "reboot"));
    cJSON_Delete(root);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddBoolToObject(response, "rebootRequired", true);
    cJSON_AddBoolToObject(response, "restarting", reboot);
    cJSON_AddStringToObject(response, "message", reboot ? "Settings saved. ESP32 is restarting." : "Settings saved. Restart the ESP32 to apply Wi-Fi changes.");

    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);

    if (reboot) {
        xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
    }

    return ret;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake complete, fd=%d", fd);

        bool added = false;
        if (xSemaphoreTake(client_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!client_slots[i].active) {
                    client_slots[i].fd = fd;
                    client_slots[i].active = true;
                    client_slots[i].is_alive = true;
                    copy_bounded(client_slots[i].name, sizeof(client_slots[i].name), "New User");
                    client_slots[i].user_id[0] = '\0';
                    added = true;
                    break;
                }
            }
            xSemaphoreGive(client_mutex);
        }

        if (!added) {
            ESP_LOGW(TAG, "Max clients reached; rejecting fd=%d", fd);
            httpd_sess_trigger_close(server, fd);
            return ESP_FAIL;
        }

        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        int err = errno;
        if (err == ECONNRESET || err == ENOTCONN || err == EPIPE || err == ESHUTDOWN) {
            ESP_LOGI(TAG, "Client disconnected, fd=%d", fd);
            if (remove_client_by_fd(fd)) {
                broadcast_online_users();
            }
        } else if (err != EAGAIN && err != EWOULDBLOCK) {
            ESP_LOGW(TAG, "Frame probe failed for fd=%d ret=%d errno=%d", fd, ret, err);
        }
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    if (ws_pkt.len > MAX_WS_PAYLOAD_BYTES) {
        ESP_LOGW(TAG, "Payload too large from fd=%d: %d bytes", fd, (int)ws_pkt.len);
        send_error_to_client(fd, "payload_too_large", "WebSocket payload is too large");
        httpd_sess_trigger_close(server, fd);
        remove_client_by_fd(fd);
        broadcast_online_users();
        return ESP_OK;
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
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (remove_client_by_fd(fd)) {
            broadcast_online_users();
        }
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        send_error_to_client(fd, "bad_frame", "Only text WebSocket frames are supported");
        free(buf);
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)ws_pkt.payload, ws_pkt.len);
    if (root == NULL || !cJSON_IsObject(root)) {
        send_error_to_client(fd, "bad_json", "Invalid JSON object");
    } else {
        handle_ws_json(fd, root);
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}
