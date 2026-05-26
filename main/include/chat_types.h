#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "chat_config.h"

typedef struct {
    int fd;
    bool active;
    bool joined;
    bool is_alive;
    bool time_offset_valid;
    int64_t time_offset_s;
    char user_id[MAX_USER_ID_LEN + 1];
    char name[MAX_NAME_LEN + 1];
} client_slot_t;

typedef struct {
    char *payload;
    uint64_t id;
} message_t;

typedef struct {
    char ssid[MAX_WIFI_SSID_LEN + 1];
    char password[MAX_WIFI_PASS_LEN + 1];
    uint8_t channel;
    char admin_password[MAX_ADMIN_PASS_LEN + 1];
} chat_settings_t;

typedef struct {
    uint64_t boot_start_id;
    uint64_t current_id;
    uint64_t earliest_id;
    uint64_t latest_id;
    uint64_t restore_before_id;
    int count;
    int capacity;
    bool has_more_before;
} history_bounds_t;
