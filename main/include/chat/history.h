#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"

#include "app_context.h"

bool chat_history_parse_since_id(const cJSON *root, uint64_t *since_id_out);
void chat_history_fill_bounds_locked(app_context_t *ctx, history_bounds_t *bounds);
uint64_t chat_history_current_restore_before_id(app_context_t *ctx);
char *chat_history_build_info_payload(app_context_t *ctx);
void chat_history_send_info_to_client(app_context_t *ctx, int fd);
void chat_history_broadcast_info(app_context_t *ctx);
void chat_history_send_to_client(app_context_t *ctx, int fd, uint64_t since_id);
esp_err_t chat_history_finalize_and_store_message(app_context_t *ctx, cJSON *root, char **payload_out);
