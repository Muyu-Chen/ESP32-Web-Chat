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

#define EXAMPLE_ESP_WIFI_SSID      "ESPChat"
#define EXAMPLE_ESP_WIFI_PASS      "esp-chat"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       8

#define MAX_CLIENTS 10
#define MAX_MESSAGES 100

static const char *TAG = "CHAT_SERVER";

// --- WebSocket Client Management ---
typedef struct {
    int fd;
    bool active;
    char name[32];
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
static esp_err_t ws_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);
void broadcast_message(const char* payload, size_t len);
static void dns_server_task(void *pvParameters);

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

    server = start_webserver();
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
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
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

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) { return ESP_FAIL; }

    int fd = httpd_req_to_sockfd(req);
    int client_index = -1;

    if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!client_slots[i].active) {
                client_slots[i].fd = fd;
                client_slots[i].active = true;
                strcpy(client_slots[i].name, "New User");
                client_index = i;
                break;
            }
        }
        xSemaphoreGive(client_mutex);
    }

    if (client_index == -1) {
        ESP_LOGE(TAG, "Max clients reached");
        close(fd);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Client connected (fd=%d) in slot %d", fd, client_index);
    send_history_to_client(fd);

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    while (1) {
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        
        ESP_LOGI(TAG, "[fd=%d] Waiting to receive frame...", fd);
        esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[fd=%d] httpd_ws_recv_frame failed with %d, breaking loop.", fd, ret);
            break;
        }
        ESP_LOGI(TAG, "[fd=%d] Received frame: len=%d, type=%d", fd, ws_pkt.len, ws_pkt.type);

        if (ws_pkt.len > 0) {
            buf = calloc(1, ws_pkt.len + 1);
            if (buf == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for WebSocket message");
                break;
            }
            ws_pkt.payload = buf;
            
            ESP_LOGI(TAG, "[fd=%d] Receiving payload...", fd);
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "[fd=%d] httpd_ws_recv_frame payload failed with %d", fd, ret);
                free(buf);
                break;
            }
            ESP_LOGI(TAG, "[fd=%d] Payload received: %s", fd, (char*)buf);

            cJSON *root = cJSON_Parse((const char*)ws_pkt.payload);
            if (root) {
                ESP_LOGI(TAG, "[fd=%d] JSON parsed successfully.", fd);
                cJSON_AddNumberToObject(root, "id", message_id_counter);
                // Only add server timestamp if client didn't provide one
                if (!cJSON_HasObjectItem(root, "timestamp")) {
                    cJSON_AddNumberToObject(root, "timestamp", time(NULL));
                }
                
                char *processed_msg = cJSON_PrintUnformatted(root);
                if (processed_msg) {
                    ESP_LOGI(TAG, "[fd=%d] Processed message: %s", fd, processed_msg);
                    char* safe_payload = add_message_to_buffer(processed_msg);
                    if (safe_payload) {
                        broadcast_message(safe_payload, strlen(safe_payload));
                    }
                    free(processed_msg);
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "[fd=%d] cJSON_Parse failed.", fd);
            }
            free(buf);
        }
    }

    ESP_LOGI(TAG, "Client disconnected (fd=%d) from slot %d", fd, client_index);
    if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
        client_slots[client_index].active = false;
        xSemaphoreGive(client_mutex);
    }
    return ESP_OK;
}