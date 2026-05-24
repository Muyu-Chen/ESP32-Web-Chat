#include "server/http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"

#include "common/settings.h"
#include "common/utils.h"
#include "server/websocket_server.h"

static const char *TAG = "CHAT_HTTP";

static void set_http_response_headers(httpd_req_t *req, const char *cache_control)
{
    httpd_resp_set_hdr(req, "Cache-Control", cache_control ? cache_control : "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (payload == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode JSON");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    set_http_response_headers(req, "no-store");
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

static void restart_task(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving root page");
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    set_http_response_headers(req, "no-store");
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
    set_http_response_headers(req, "no-store");
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
    set_http_response_headers(req, "no-store");
    httpd_resp_send(req, (const char *)style_css_start, style_css_size);
    return ESP_OK;
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    extern const unsigned char script_js_start[] asm("_binary_script_js_start");
    extern const unsigned char script_js_end[] asm("_binary_script_js_end");
    const size_t script_js_size = (script_js_end - script_js_start);

    httpd_resp_set_type(req, "application/javascript");
    set_http_response_headers(req, "no-store");
    httpd_resp_send(req, (const char *)script_js_start, script_js_size);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    app_context_t *ctx = req->user_ctx ? (app_context_t *)req->user_ctx : &g_app_context;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create settings response");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "ssid", ctx->settings.ssid);
    cJSON_AddNumberToObject(root, "channel", ctx->settings.channel);
    cJSON_AddBoolToObject(root, "passwordSet", ctx->settings.password[0] != '\0');
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
    app_context_t *ctx = req->user_ctx ? (app_context_t *)req->user_ctx : &g_app_context;
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
        strcmp(admin_password->valuestring, ctx->settings.admin_password) != 0) {
        cJSON_Delete(root);
        return send_http_error(req, "unauthorized", "Admin password is incorrect");
    }

    chat_settings_t updated = ctx->settings;

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
            if (!chat_settings_valid_wifi_password(password->valuestring)) {
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

    esp_err_t save_ret = chat_settings_save(&updated);
    if (save_ret != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(save_ret));
        return send_http_error(req, "save_failed", "Unable to save settings to NVS");
    }

    ctx->settings = updated;
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

static esp_err_t redirect_to_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Redirecting %s to root", req->uri);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    char location_url[32];
    snprintf(location_url, sizeof(location_url), "http://" IPSTR "/", IP2STR(&ip_info.ip));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location_url);
    set_http_response_headers(req, "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

httpd_handle_t chat_http_start_server(app_context_t *ctx)
{
    httpd_handle_t local_server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.close_fn = chat_ws_session_close_handler;

    int desired_sockets = MAX_CLIENTS + HTTP_STATIC_SOCKET_MARGIN;
#ifdef CONFIG_LWIP_MAX_SOCKETS
    int available_sockets = CONFIG_LWIP_MAX_SOCKETS - HTTPD_INTERNAL_SOCKETS - DNS_SERVER_SOCKETS;
    if (available_sockets < 1) {
        available_sockets = 1;
    }
    if (desired_sockets > available_sockets) {
        desired_sockets = available_sockets;
    }
    if (desired_sockets < MAX_CLIENTS) {
        ESP_LOGW(TAG, "Socket budget allows %d WebSocket sessions, below configured max clients=%d",
                 desired_sockets, MAX_CLIENTS);
    }
#endif
    config.max_open_sockets = desired_sockets;

    ESP_LOGI(TAG, "Starting webserver with max_open_sockets=%d", config.max_open_sockets);

    if (httpd_start(&local_server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &root);

        httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &favicon);

        httpd_uri_t style = { .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &style);

        httpd_uri_t script = { .uri = "/script.js", .method = HTTP_GET, .handler = script_get_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &script);

        httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = chat_ws_handler, .is_websocket = true, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &ws);

        httpd_uri_t settings_get = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &settings_get);

        httpd_uri_t settings_post = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &settings_post);

        httpd_uri_t catch_all = { .uri = "/*", .method = HTTP_GET, .handler = redirect_to_root_handler, .user_ctx = ctx };
        httpd_register_uri_handler(local_server, &catch_all);
    }

    if (ctx != NULL) {
        ctx->server = local_server;
    }
    return local_server;
}
