#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#include "app_context.h"

void copy_bounded(char *dst, size_t dst_size, const char *src);
bool json_string_in_range(const cJSON *item, size_t max_len, bool allow_empty);
bool json_array_contains_string(cJSON *array, const char *value);
int64_t device_uptime_s(void);
int64_t current_timestamp_s(app_context_t *ctx);
