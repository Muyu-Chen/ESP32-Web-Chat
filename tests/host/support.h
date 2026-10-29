#pragma once

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat/protocol.h"
#include "chat/history.h"
#include "chat/sessions.h"
#include "common/utils.h"
#include "server/websocket_server.h"
#include "storage/message_id_store.h"

static app_context_t ctx;
static int client_lock, message_lock;
static int errors, closes, sends, broadcasts, delay_count;
static uint64_t sent_ids[16];
static esp_err_t error_result, queue_result;
static esp_err_t load_result;
static uint64_t load_current_id;
static int64_t now_s;
static TaskHandle_t current_task;
static void (*queued_work)(void *);
static void *queued_arg;
static void (*heartbeat_entry)(void *);
static void *heartbeat_arg;
static jmp_buf sweep_end;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
int64_t esp_timer_get_time(void) { return now_s * 1000000; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t ticks)
{
    (void)ticks;
    assert(mutex && !*mutex);
    *mutex = 1;
    return pdTRUE;
}

void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(*mutex); *mutex = 0; }

BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    heartbeat_entry = entry;
    heartbeat_arg = arg;
    return pdPASS;
}

void vTaskDelay(uint32_t ticks)
{
    (void)ticks;
    if (++delay_count == 2) { longjmp(sweep_end, 1); }
}

esp_err_t httpd_queue_work(httpd_handle_t server, void (*work)(void *), void *arg)
{
    assert(server);
    if (queue_result != ESP_OK) { return queue_result; }
    assert(!queued_work);
    queued_work = work;
    queued_arg = arg;
    return ESP_OK;
}

esp_err_t chat_ws_send_text(app_context_t *app, int fd, const char *payload)
{
    (void)fd;
    assert(current_task == app->httpd_task_handle);
    assert(!client_lock && !message_lock);
    cJSON *root = cJSON_Parse(payload);
    assert(root);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    sent_ids[sends++ % 16] = cJSON_IsNumber(id) ? (uint64_t)id->valuedouble : 0;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t chat_ws_send_error(app_context_t *app, int fd, const char *code, const char *message)
{
    (void)app; (void)fd; (void)code; (void)message;
    errors++;
    return error_result;
}

void chat_ws_close_client(app_context_t *app, int fd)
{
    assert(current_task == app->httpd_task_handle);
    assert(!client_lock && !message_lock);
    closes++;
    chat_sessions_mark_closing(app, fd);
}

bool chat_ws_broadcast(app_context_t *app, const char *payload)
{
    (void)payload;
    assert(current_task == app->httpd_task_handle);
    assert(!client_lock && !message_lock);
    broadcasts++;
    return false;
}

esp_err_t chat_message_ids_persist(uint64_t id, uint64_t boot_start)
{
    (void)id; (void)boot_start;
    return ESP_OK;
}

esp_err_t chat_message_ids_load(chat_message_id_state_t *state)
{
    *state = (chat_message_id_state_t) {
        .current_id = load_current_id,
        .boot_start_id = load_current_id + 1,
        .counter_loaded = load_result == ESP_OK,
    };
    return load_result;
}

static void reset(void)
{
    for (int i = 0; i < MAX_MESSAGES; i++) { free(ctx.message_buffer[i].payload); }
    memset(&ctx, 0, sizeof(ctx));
    ctx.client_mutex = &client_lock;
    ctx.message_mutex = &message_lock;
    ctx.server = &ctx;
    ctx.httpd_task_handle = &ctx;
    ctx.boot_start_id = 1;
    ctx.message_id_counter_valid = true;
    current_task = ctx.httpd_task_handle;
    errors = closes = sends = broadcasts = 0;
    error_result = queue_result = ESP_OK;
    load_result = ESP_OK;
    load_current_id = 0;
    queued_work = NULL;
    now_s = 0;
}

static esp_err_t dispatch(int fd, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    esp_err_t ret = chat_protocol_handle_json(&ctx, fd, root);
    cJSON_Delete(root);
    return ret;
}

static void join(int fd)
{
    assert(chat_sessions_ensure_slot(&ctx, fd));
    assert(dispatch(fd, "{\"type\":\"join\",\"from\":\"alice\",\"name\":\"Alice\"}") == ESP_OK);
}

static void schedule_sweep(void)
{
    current_task = &delay_count;
    delay_count = 0;
    if (setjmp(sweep_end) == 0) { heartbeat_entry(heartbeat_arg); }
}

static void run_work(void)
{
    assert(queued_work);
    void (*work)(void *) = queued_work;
    void *arg = queued_arg;
    queued_work = NULL;
    current_task = ctx.httpd_task_handle;
    work(arg);
}
