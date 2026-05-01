#include "message_id_store.h"

#include <inttypes.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "MSG_ID_STORE";
static const char *MESSAGE_ID_NAMESPACE = "chatmsg";

esp_err_t chat_message_ids_persist(uint64_t current_id, uint64_t boot_start_id)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MESSAGE_ID_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u64(nvs, "current", current_id);
    if (ret == ESP_OK) {
        ret = nvs_set_u64(nvs, "boot_start", boot_start_id);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

esp_err_t chat_message_ids_load(chat_message_id_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t current_id = 0;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MESSAGE_ID_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        nvs_get_u64(nvs, "current", &current_id);
        nvs_close(nvs);
    } else {
        ESP_LOGI(TAG, "No stored message id state yet: %s", esp_err_to_name(ret));
    }

    if (current_id > CHAT_MESSAGE_MAX_SAFE_ID) {
        current_id = CHAT_MESSAGE_MAX_SAFE_ID;
    }

    state->current_id = current_id;
    state->boot_start_id = current_id < CHAT_MESSAGE_MAX_SAFE_ID
        ? current_id + 1
        : CHAT_MESSAGE_MAX_SAFE_ID;

    esp_err_t persist_ret = chat_message_ids_persist(state->current_id, state->boot_start_id);
    if (persist_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist boot message id state: %s", esp_err_to_name(persist_ret));
    }

    ESP_LOGI(TAG, "Message id state: current=%" PRIu64 " boot_start=%" PRIu64,
             state->current_id, state->boot_start_id);
    return persist_ret == ESP_OK ? ESP_OK : persist_ret;
}
