/*
 * ESP32 WiFi Chat Server
 */
#include <assert.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_context.h"
#include "chat/sessions.h"
#include "common/settings.h"
#include "network/dns_server.h"
#include "network/softap.h"
#include "server/http_server.h"
#include "storage/message_id_store.h"

static const char *TAG = "CHAT_MAIN";

app_context_t g_app_context;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    memset(&g_app_context, 0, sizeof(g_app_context));
    g_app_context.boot_start_id = 1;

    g_app_context.client_mutex = xSemaphoreCreateMutex();
    g_app_context.message_mutex = xSemaphoreCreateMutex();
    assert(g_app_context.client_mutex && g_app_context.message_mutex);

    chat_message_id_state_t id_state = { 0 };
    esp_err_t id_ret = chat_message_ids_load(&id_state);
    if (id_ret != ESP_OK) {
        ESP_LOGW(TAG, "Message ids may not persist until NVS recovers: %s", esp_err_to_name(id_ret));
    }
    g_app_context.message_id_counter = id_state.current_id;
    g_app_context.boot_start_id = id_state.boot_start_id;

    chat_settings_load(&g_app_context);
    chat_softap_start(&g_app_context);

    chat_dns_start();
    chat_sessions_start_heartbeat(&g_app_context);
    chat_http_start_server(&g_app_context);
}
