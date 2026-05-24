#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_context.h"
#include "chat_types.h"

bool chat_settings_valid_wifi_password(const char *password);
void chat_settings_reset_to_defaults(chat_settings_t *settings);
void chat_settings_load(app_context_t *ctx);
esp_err_t chat_settings_save(const chat_settings_t *settings);
