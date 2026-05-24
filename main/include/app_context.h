#pragma once

#include <stdint.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "chat_config.h"
#include "chat_types.h"

typedef struct {
    client_slot_t client_slots[MAX_CLIENTS];
    SemaphoreHandle_t client_mutex;

    message_t message_buffer[MAX_MESSAGES];
    uint64_t message_id_counter;
    uint64_t boot_start_id;
    int message_buffer_head;
    SemaphoreHandle_t message_mutex;

    chat_settings_t settings;
    httpd_handle_t server;
    TaskHandle_t httpd_task_handle;
} app_context_t;

extern app_context_t g_app_context;
