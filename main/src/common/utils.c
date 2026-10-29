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

bool json_has_duplicate_keys(const cJSON *item)
{
    if (cJSON_IsObject(item)) {
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            if (child->string == NULL) {
                return true;
            }

            const cJSON *other = child->next;
            while (other != NULL) {
                if (other->string != NULL && strcmp(child->string, other->string) == 0) {
                    return true;
                }
                other = other->next;
            }

            if (json_has_duplicate_keys(child)) {
                return true;
            }
        }
    } else if (cJSON_IsArray(item)) {
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            if (json_has_duplicate_keys(child)) {
                return true;
            }
        }
    }

    return false;
}

static bool valid_utf8(const char *payload, size_t length)
{
    for (size_t i = 0; i < length;) {
        uint32_t codepoint = (unsigned char)payload[i++];
        unsigned continuation;
        uint32_t minimum;
        if (codepoint < 0x80) {
            continue;
        } else if (codepoint >= 0xc2 && codepoint <= 0xdf) {
            continuation = 1;
            minimum = 0x80;
            codepoint &= 0x1f;
        } else if (codepoint >= 0xe0 && codepoint <= 0xef) {
            continuation = 2;
            minimum = 0x800;
            codepoint &= 0x0f;
        } else if (codepoint >= 0xf0 && codepoint <= 0xf4) {
            continuation = 3;
            minimum = 0x10000;
            codepoint &= 0x07;
        } else {
            return false;
        }
        if (length - i < continuation) {
            return false;
        }
        for (unsigned j = 0; j < continuation; j++) {
            unsigned char byte = (unsigned char)payload[i++];
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

cJSON *chat_json_parse_object(const char *payload, size_t length)
{
    if (payload == NULL || length == 0 || !valid_utf8(payload, length)) {
        return NULL;
    }

    // Bound recursion before cJSON touches untrusted input on the HTTP task stack.
    unsigned depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)payload[i];
        if (ch == '\0') {
            return NULL;
        }
        if (in_string) {
            if (ch < 0x20) {
                return NULL;
            }
            if (ch == '\\') {
                if (i + 1 >= length) {
                    return NULL;
                }
                // cJSON stores C strings: decoded NULs would truncate IDs, keys and passwords.
                if (i + 5 < length && memcmp(payload + i + 1, "u0000", 5) == 0) {
                    return NULL;
                }
                i++;
            } else if (ch == '"') {
                in_string = false;
            }
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > MAX_JSON_NESTING_DEPTH) {
                return NULL;
            }
        } else if (ch == '}' || ch == ']') {
            if (depth == 0) {
                return NULL;
            }
            depth--;
        } else if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
            return NULL;
        }
    }
    if (in_string || depth != 0) {
        return NULL;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, length, &end, false);
    if (root != NULL) {
        while (end < payload + length && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
            end++;
        }
    }
    if (!cJSON_IsObject(root) || end != payload + length || json_has_duplicate_keys(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
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
