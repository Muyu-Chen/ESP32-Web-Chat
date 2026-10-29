#pragma once

#include "esp_http_server.h"

esp_err_t chat_http_session_open_handler(httpd_handle_t server, int fd);
