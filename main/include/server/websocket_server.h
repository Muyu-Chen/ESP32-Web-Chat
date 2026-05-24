#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include "app_context.h"

esp_err_t chat_ws_handler(httpd_req_t *req);
void chat_ws_session_close_handler(httpd_handle_t hd, int sockfd);
esp_err_t chat_ws_send_text(app_context_t *ctx, int fd, const char *payload);
void chat_ws_broadcast(app_context_t *ctx, const char *payload);
esp_err_t chat_ws_send_error(app_context_t *ctx, int fd, const char *code, const char *message);
void chat_ws_close_client(app_context_t *ctx, int fd);
