#include "network/softap.h"

#include <assert.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/dns.h"

#include "common/utils.h"

static const char *TAG = "CHAT_SOFTAP";

void chat_softap_start(app_context_t *ctx)
{
    assert(ctx);

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
    memcpy(wifi_config.ap.ssid, ctx->settings.ssid, strlen(ctx->settings.ssid));
    copy_bounded((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), ctx->settings.password);
    wifi_config.ap.ssid_len = strlen(ctx->settings.ssid);
    wifi_config.ap.channel = ctx->settings.channel;
    wifi_config.ap.max_connection = CHAT_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (strlen(ctx->settings.password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_get_ip_info(p_netif, &ip_info);
    ESP_LOGI(TAG, "SoftAP started: ssid=%s channel=%u ip=" IPSTR,
             ctx->settings.ssid, ctx->settings.channel, IP2STR(&ip_info.ip));
}
