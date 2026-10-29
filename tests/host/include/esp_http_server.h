#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;
typedef int (*httpd_send_func_t)(httpd_handle_t, int, const char *, size_t, int);
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
esp_err_t httpd_sess_set_send_override(httpd_handle_t server, int fd, httpd_send_func_t send_func);
esp_err_t httpd_queue_work(httpd_handle_t server, void (*work)(void *), void *arg);
