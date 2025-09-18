/*
 * ESP32 WiFi Chat Server
 *
 * This code is in the Public Domain (or CC0 licensed, at your option.)
*/
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
#include <errno.h>

#define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
#define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       8

#define MAX_CLIENTS 10
#define MAX_MESSAGES 100
#define HEARTBEAT_INTERVAL_S 30

static const char *TAG = "CHAT_SERVER";

// --- WebSocket Client Management ---
typedef struct {
    int fd;
    bool active;
    char name[32];
    bool is_alive;
} client_slot_t;

static client_slot_t client_slots[MAX_CLIENTS];
static SemaphoreHandle_t client_mutex;

// --- Message Ring Buffer ---
typedef struct {
    char *payload;
    uint32_t id;
} message_t;

static message_t message_buffer[MAX_MESSAGES];
static uint32_t message_id_counter = 0;
static int message_buffer_head = 0;
static SemaphoreHandle_t message_mutex;

// --- App State ---
static httpd_handle_t server = NULL;

// --- Function Prototypes ---
static void wifi_init_softap(void);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t favicon_get_handler(httpd_req_t *req);
static esp_err_t style_get_handler(httpd_req_t *req);
static esp_err_t script_get_handler(httpd_req_t *req);
static esp_err_t ws_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);
void broadcast_message(const char* payload, size_t len);
static void dns_server_task(void *pvParameters);
static void heartbeat_task(void *pvParameters);


// --- Core Logic ---

char* add_message_to_buffer(const char* text) {
    ESP_LOGI(TAG, "Adding message to buffer...");
    char* new_payload = NULL;
    if (xSemaphoreTake(message_mutex, portMAX_DELAY)) {
        if (message_buffer[message_buffer_head].payload) {
            free(message_buffer[message_buffer_head].payload);
        }
        new_payload = strdup(text);
        message_buffer[message_buffer_head].payload = new_payload;
        if (new_payload == NULL) {
            ESP_LOGE(TAG, "add_message_to_buffer: strdup failed!");
            xSemaphoreGive(message_mutex);
            return NULL;
        }
        ESP_LOGI(TAG, "strdup successful for new message.");
        message_buffer[message_buffer_head].id = message_id_counter;
        
        message_buffer_head = (message_buffer_head + 1) % MAX_MESSAGES;
        message_id_counter++;

        xSemaphoreGive(message_mutex);
    }
    ESP_LOGI(TAG, "Message added to buffer.");
    return new_payload;
}

void send_history_to_client(int fd) {
    if (xSemaphoreTake(message_mutex, portMAX_DELAY)) {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        int current_pos = message_buffer_head;
        for (int i = 0; i < MAX_MESSAGES; i++) {
            int index = (current_pos + i) % MAX_MESSAGES;
            if (message_buffer[index].payload) {
                ws_pkt.payload = (uint8_t*)message_buffer[index].payload;
                ws_pkt.len = strlen(message_buffer[index].payload);
                httpd_ws_send_frame_async(server, fd, &ws_pkt);
            }
        }
        xSemaphoreGive(message_mutex);
    }
}

void broadcast_message(const char* payload, size_t len) {
    ESP_LOGI(TAG, "Broadcasting message: %s", payload);
    if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.payload = (uint8_t*)payload;
        ws_pkt.len = len;
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_slots[i].active) {
                ESP_LOGI(TAG, "Sending to client fd %d", client_slots[i].fd);
                esp_err_t ret = httpd_ws_send_frame_async(server, client_slots[i].fd, &ws_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "httpd_ws_send_frame_async failed with %d", ret);
                }
            }
        }
        xSemaphoreGive(client_mutex);
    }
    ESP_LOGI(TAG, "Broadcast finished.");
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

    wifi_init_softap();

    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat_task", 4096, NULL, 5, NULL);

    server = start_webserver();
}

// --- Heartbeat Task ---
static void heartbeat_task(void *pvParameters)
{
    const char *ping_payload = "{\"type\":\"ping\"}";
    const size_t ping_len = strlen(ping_payload);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_S * 1000));

        if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
            ESP_LOGI(TAG, "Running heartbeat check");
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t*)ping_payload;
            ws_pkt.len = ping_len;
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_slots[i].active) {
                    if (!client_slots[i].is_alive) {
                        ESP_LOGW(TAG, "Client fd=%d seems dead, closing connection.", client_slots[i].fd);
                        httpd_sess_trigger_close(server, client_slots[i].fd);
                        client_slots[i].active = false;
                        continue;
                    }
                    
                    client_slots[i].is_alive = false;
                    esp_err_t ret = httpd_ws_send_frame_async(server, client_slots[i].fd, &ws_pkt);
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Ping failed for fd=%d with error %d", client_slots[i].fd, ret);
                    }
                }
            }
            xSemaphoreGive(client_mutex);
        }
    }
}


// --- DNS Server Task ---
const static char *DNS_TAG = "DNS";

static void dns_server_task(void *pvParameters)
{
    uint8_t buffer[128];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY
    };

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(DNS_TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(DNS_TAG, "Failed to bind socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(DNS_TAG, "DNS Server started");

    while (1) {
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client, &client_len);
        if (len > 0) {
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

            buffer[2] |= 0x80;
            buffer[3] |= 0x80;
            buffer[7] = 1;

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

    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = ip_info.ip.addr;
    esp_netif_set_dns_info(p_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_get_ip_info(p_netif, &ip_info);
    ESP_LOGI(TAG, "SoftAP started, IP: " IPSTR, IP2STR(&ip_info.ip));
}

static esp_err_t redirect_to_root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Redirecting request for %s to root", req->uri);
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    
    char location_url[32];
    sprintf(location_url, "http://" IPSTR "/", IP2STR(&ip_info.ip));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location_url);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t local_server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // config.max_open_sockets = 16; // config in the esp32 Configuration
    config.max_uri_handlers = 10; // Increased from default 8

    ESP_LOGI(TAG, "Starting webserver with max_open_sockets = %d", config.max_open_sockets);

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

        httpd_uri_t catch_all = {
            .uri       = "/*",
            .method    = HTTP_GET,
            .handler   = redirect_to_root_handler
        };
        httpd_register_uri_handler(local_server, &catch_all);
    }
    return local_server;
}

// --- HTTP and WebSocket Handlers ---

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving root page");
    extern const unsigned char src_index_html_start[] asm("_binary_src_index_html_start");
    extern const unsigned char src_index_html_end[]   asm("_binary_src_index_html_end");
    const size_t index_html_size = (src_index_html_end - src_index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)src_index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

static esp_err_t style_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving style.css");
    extern const unsigned char src_style_css_start[] asm("_binary_src_style_css_start");
    extern const unsigned char src_style_css_end[]   asm("_binary_src_style_css_end");
    const size_t style_css_size = (src_style_css_end - src_style_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)src_style_css_start, style_css_size);
    return ESP_OK;
}

static esp_err_t script_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving script.js");
    extern const unsigned char src_script_js_start[] asm("_binary_src_script_js_start");
    extern const unsigned char src_script_js_end[]   asm("_binary_src_script_js_end");
    const size_t script_js_size = (src_script_js_end - src_script_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)src_script_js_start, script_js_size);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    // Check if this is the initial GET handshake
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, new client connected, fd=%d", fd);

        // Add client to our list
        int client_index = -1;
        if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!client_slots[i].active) {
                    client_slots[i].fd = fd;
                    client_slots[i].active = true;
                    client_slots[i].is_alive = true;
                    strcpy(client_slots[i].name, "New User");
                    client_index = i;
                    break;
                }
            }
            xSemaphoreGive(client_mutex);
        }

        if (client_index == -1) {
            ESP_LOGE(TAG, "Max clients reached, closing connection for fd=%d", fd);
            close(fd);
            return ESP_FAIL;
        }

        send_history_to_client(fd);
        return ESP_OK;
    }

    // --- This part handles subsequent data frames ---

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;

    // 1. Probe for frame length
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        int err = errno;
        if (err == ECONNRESET || err == ENOTCONN || err == EPIPE || err == ESHUTDOWN) {
            ESP_LOGI(TAG, "Client disconnected, fd=%d", fd);
            // Find and remove client from list
            if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_slots[i].fd == fd) {
                        client_slots[i].active = false;
                        break;
                    }
                }
                xSemaphoreGive(client_mutex);
            }
        } else if (err == EAGAIN || err == EWOULDBLOCK) {
            // Not an error, just no data. Return ESP_OK to continue.
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "httpd_ws_recv_frame failed (ret=%d, errno=%d) for fd=%d", ret, err, fd);
        }
        // The socket is already closed by the httpd server, no need to close(fd)
        return ESP_OK;
    }

    // 2. Receive payload if frame has length
    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for WebSocket buffer");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
                cJSON *root = cJSON_ParseWithLength((const char*)ws_pkt.payload, ws_pkt.len);
                if (root) {
                    cJSON *type = cJSON_GetObjectItem(root, "type");
                    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "pong") == 0) {
                        ESP_LOGI(TAG, "Pong received from fd=%d", fd);
                        if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
                            for (int i = 0; i < MAX_CLIENTS; i++) {
                                if (client_slots[i].fd == fd) {
                                    client_slots[i].is_alive = true;
                                    break;
                                }
                            }
                            xSemaphoreGive(client_mutex);
                        }
                    } else {
                        char *processed_msg = cJSON_PrintUnformatted(root);
                        if (processed_msg) {
                            char* safe_payload = add_message_to_buffer(processed_msg);
                            if (safe_payload) {
                                broadcast_message(safe_payload, strlen(safe_payload));
                            }
                            free(processed_msg);
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    ESP_LOGE(TAG, "cJSON_Parse failed for fd=%d", fd);
                }
            }
        }
        free(buf);
    }
    
    return ESP_OK;
}