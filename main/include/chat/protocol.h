#pragma once

#include "esp_err.h"
#include "cJSON.h"

#include "app_context.h"

esp_err_t chat_protocol_handle_json(app_context_t *ctx, int fd, cJSON *root);
