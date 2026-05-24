#include "common/utils.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

#include "chat_config.h"

void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

bool json_string_in_range(const cJSON *item, size_t max_len, bool allow_empty)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    size_t len = strlen(item->valuestring);
    return (allow_empty || len > 0) && len <= max_len;
}

bool json_array_contains_string(cJSON *array, const char *value)
{
    if (!cJSON_IsArray(array) || value == NULL) {
        return false;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && item->valuestring != NULL && strcmp(item->valuestring, value) == 0) {
            return true;
        }
    }

    return false;
}

int64_t current_timestamp_s(const cJSON *incoming)
{
    time_t now = 0;
    time(&now);
    if ((int64_t)now >= VALID_EPOCH_START_S) {
        return (int64_t)now;
    }

    cJSON *client_ts = incoming ? cJSON_GetObjectItem(incoming, "timestamp") : NULL;
    if (cJSON_IsNumber(client_ts) &&
        client_ts->valuedouble >= VALID_EPOCH_START_S &&
        client_ts->valuedouble <= VALID_EPOCH_END_S) {
        return (int64_t)client_ts->valuedouble;
    }

    return esp_timer_get_time() / 1000000LL;
}
