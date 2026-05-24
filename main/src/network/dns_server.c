#include "network/dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "chat_config.h"

static const char *TAG = "DNS";

static void dns_server_task(void *pvParameters)
{
    (void)pvParameters;

    uint8_t buffer[DNS_PACKET_BYTES];
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Server started");

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

void chat_dns_start(void)
{
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}
