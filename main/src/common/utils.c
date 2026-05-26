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

int64_t device_uptime_s(void)
{
    return esp_timer_get_time() / 1000000LL;
}

static int time_sync_threshold(int count)
{
    if (count <= 0) {
        return 0;
    }
    return (count * 2 + 2) / 3;
}

static bool consensus_offset_s(app_context_t *ctx, int64_t *offset_out)
{
    int64_t offsets[MAX_CLIENTS];
    int count = 0;
    bool found = false;
    int64_t selected_total = 0;
    int selected_count = 0;

    if (ctx == NULL || offset_out == NULL || ctx->client_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(ctx->client_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx->client_slots[i].active ||
            !ctx->client_slots[i].joined ||
            !ctx->client_slots[i].time_offset_valid) {
            continue;
        }
        offsets[count++] = ctx->client_slots[i].time_offset_s;
    }

    xSemaphoreGive(ctx->client_mutex);

    int required = time_sync_threshold(count);
    if (required == 0) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        int64_t total = 0;
        int cluster_count = 0;
        int64_t lower = offsets[i];
        int64_t upper = lower + TIME_SYNC_TOLERANCE_S;
        for (int j = 0; j < count; j++) {
            if (offsets[j] >= lower && offsets[j] <= upper) {
                total += offsets[j];
                cluster_count++;
            }
        }

        if (cluster_count >= required && cluster_count > selected_count) {
            selected_total = total;
            selected_count = cluster_count;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    *offset_out = selected_total / selected_count;
    return true;
}

int64_t current_timestamp_s(app_context_t *ctx)
{
    time_t now = 0;
    time(&now);
    if ((int64_t)now >= VALID_EPOCH_START_S && (int64_t)now <= VALID_EPOCH_END_S) {
        return (int64_t)now;
    }

    int64_t uptime = device_uptime_s();
    int64_t offset = 0;
    if (consensus_offset_s(ctx, &offset)) {
        int64_t consensus_now = uptime + offset;
        if (consensus_now >= VALID_EPOCH_START_S && consensus_now <= VALID_EPOCH_END_S) {
            return consensus_now;
        }
    }

    return uptime;
}
