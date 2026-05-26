#pragma once

#include <stdbool.h>

#include "cJSON.h"

#include "app_context.h"

bool chat_sessions_update_identity(app_context_t *ctx, int fd, const char *user_id, const char *name);
bool chat_sessions_ensure_slot(app_context_t *ctx, int fd);
bool chat_sessions_is_joined(app_context_t *ctx, int fd);
bool chat_sessions_identity_matches(app_context_t *ctx, int fd, const char *user_id);
void chat_sessions_update_time_sample(app_context_t *ctx, int fd, const cJSON *root);
bool chat_sessions_mark_alive(app_context_t *ctx, int fd);
bool chat_sessions_remove_by_fd(app_context_t *ctx, int fd);
char *chat_sessions_build_online_users_payload(app_context_t *ctx);
void chat_sessions_broadcast_online_users(app_context_t *ctx);
void chat_sessions_send_online_users_to_client(app_context_t *ctx, int fd);
void chat_sessions_start_heartbeat(app_context_t *ctx);
