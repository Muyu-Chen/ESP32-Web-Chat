#include "common/settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "chat_config.h"
#include "common/utils.h"

static const char *TAG = "CHAT_SETTINGS";
static const char *SETTINGS_NAMESPACE = "chatcfg";

bool chat_settings_valid_wifi_password(const char *password)
{
    size_t len = password ? strlen(password) : 0;
    return len == 0 || (len >= MIN_WIFI_PASS_LEN && len <= MAX_WIFI_PASS_LEN);
}

void chat_settings_reset_to_defaults(chat_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    copy_bounded(settings->ssid, sizeof(settings->ssid), CHAT_WIFI_SSID);
    copy_bounded(settings->password, sizeof(settings->password), CHAT_WIFI_PASS);
    settings->channel = CHAT_WIFI_CHANNEL;
    copy_bounded(settings->admin_password, sizeof(settings->admin_password), CHAT_ADMIN_PASS);

    if (settings->ssid[0] == '\0') {
        copy_bounded(settings->ssid, sizeof(settings->ssid), "ESPChat");
    }
    if (!chat_settings_valid_wifi_password(settings->password)) {
        copy_bounded(settings->password, sizeof(settings->password), "esp-chat");
    }
    if (settings->channel < 1 || settings->channel > 13) {
        settings->channel = 1;
    }
    if (strlen(settings->admin_password) < MIN_ADMIN_PASS_LEN) {
        copy_bounded(settings->admin_password, sizeof(settings->admin_password), "admin");
    }
}

void chat_settings_load(app_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    chat_settings_reset_to_defaults(&ctx->settings);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "Using factory default settings: %s", esp_err_to_name(ret));
        return;
    }

    size_t len = sizeof(ctx->settings.ssid);
    nvs_get_str(nvs, "ssid", ctx->settings.ssid, &len);

    len = sizeof(ctx->settings.password);
    nvs_get_str(nvs, "pass", ctx->settings.password, &len);

    len = sizeof(ctx->settings.admin_password);
    nvs_get_str(nvs, "admin", ctx->settings.admin_password, &len);

    uint8_t channel = ctx->settings.channel;
    if (nvs_get_u8(nvs, "chan", &channel) == ESP_OK) {
        ctx->settings.channel = channel;
    }
    nvs_close(nvs);

    if (ctx->settings.ssid[0] == '\0' ||
        !chat_settings_valid_wifi_password(ctx->settings.password) ||
        ctx->settings.channel < 1 ||
        ctx->settings.channel > 13 ||
        strlen(ctx->settings.admin_password) < MIN_ADMIN_PASS_LEN) {
        ESP_LOGW(TAG, "Stored settings were invalid; falling back to factory defaults");
        chat_settings_reset_to_defaults(&ctx->settings);
    }
}

esp_err_t chat_settings_save(const chat_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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
