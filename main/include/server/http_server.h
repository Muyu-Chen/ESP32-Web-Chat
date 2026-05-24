#pragma once

#include "esp_http_server.h"

#include "app_context.h"

httpd_handle_t chat_http_start_server(app_context_t *ctx);
