#pragma once

#include "esp_err.h"

typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;
esp_err_t httpd_queue_work(httpd_handle_t server, void (*work)(void *), void *arg);
