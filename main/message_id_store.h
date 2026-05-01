#pragma once

#include <stdint.h>

#include "esp_err.h"

#define CHAT_MESSAGE_MAX_SAFE_ID 9007199254740991ULL

typedef struct {
    uint64_t boot_start_id;
    uint64_t current_id;
} chat_message_id_state_t;

esp_err_t chat_message_ids_load(chat_message_id_state_t *state);
esp_err_t chat_message_ids_persist(uint64_t current_id, uint64_t boot_start_id);
