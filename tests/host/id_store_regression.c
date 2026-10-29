#include "support.h"
#include "nvs.h"

#define chat_message_ids_load tested_ids_load
#define chat_message_ids_persist tested_ids_persist
#include "../../main/src/storage/message_id_store.c"
#undef chat_message_ids_load
#undef chat_message_ids_persist

static esp_err_t open_result, get_result, commit_result;
static uint64_t stored_id, written_id;
static int writes, commits, nvs_closes;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "chatmsg") == 0);
    *handle = 1;
    return mode == NVS_READONLY ? open_result : ESP_OK;
}

esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value)
{
    assert(handle == 1 && strcmp(key, "current") == 0);
    if (get_result == ESP_OK) { *value = stored_id; }
    return get_result;
}

esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value)
{
    assert(handle == 1);
    writes++;
    if (strcmp(key, "current") == 0) { written_id = value; }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    commits++;
    return commit_result;
}

void nvs_close(nvs_handle_t handle) { assert(handle == 1); nvs_closes++; }

static void configure(void)
{
    open_result = get_result = commit_result = ESP_OK;
    stored_id = 42;
    written_id = 999;
    writes = commits = nvs_closes = 0;
}

int main(void)
{
    chat_message_id_state_t state;
    configure();
    open_result = ESP_FAIL;
    assert(tested_ids_load(&state) == ESP_FAIL && !state.counter_loaded);
    assert(writes == 0 && commits == 0 && written_id == 999);

    configure();
    get_result = ESP_ERR_INVALID_STATE;
    assert(tested_ids_load(&state) == ESP_ERR_INVALID_STATE && !state.counter_loaded);
    assert(writes == 0 && commits == 0 && nvs_closes == 1);

    configure();
    open_result = ESP_ERR_NVS_NOT_FOUND;
    assert(tested_ids_load(&state) == ESP_OK && state.counter_loaded);
    assert(state.current_id == 0 && state.boot_start_id == 1 && written_id == 0);

    configure();
    assert(tested_ids_load(&state) == ESP_OK && state.counter_loaded);
    assert(state.current_id == 42 && state.boot_start_id == 43 && written_id == 42);

    configure();
    commit_result = ESP_FAIL;
    assert(tested_ids_load(&state) == ESP_FAIL && state.counter_loaded);
    assert(state.current_id == 42 && state.boot_start_id == 43);
    puts("ID store regressions passed: failed reads never overwrite the counter, first boot and write recovery");
}
