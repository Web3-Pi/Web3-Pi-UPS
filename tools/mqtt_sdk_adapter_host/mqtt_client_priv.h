#include <stddef.h>
#pragma once
/* SDK/platform stand-ins only. Production adapter and SDK lifecycle excerpts
 * are compiled unchanged; field-layout compatibility is checked by IDF build. */
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "mqtt_client.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef uint32_t *EventGroupHandle_t;
typedef enum {
    MQTT_STATE_INIT, MQTT_STATE_DISCONNECTED, MQTT_STATE_CONNECTED,
    MQTT_STATE_WAIT_RECONNECT,
} mqtt_client_state_t;
typedef struct { int task_stack, task_prio; } mqtt_config_storage_t;
struct esp_mqtt_client {
    _Atomic mqtt_client_state_t state;
    bool run;
    EventGroupHandle_t status_bits;
    TaskHandle_t task_handle;
    mqtt_config_storage_t *config;
    void *transport, *outbox;
    unsigned lock_depth;
};

#ifndef MQTT_CORE_SELECTION_ENABLED
#define MQTT_CORE_SELECTION_ENABLED 0
#endif
#define MQTT_TASK_CORE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define MQTT_API_LOCK(c) mock_api_lock(c)
#define MQTT_API_UNLOCK(c) mock_api_unlock(c)

void mock_api_lock(esp_mqtt_client_handle_t client);
void mock_api_unlock(esp_mqtt_client_handle_t client);
uint32_t xEventGroupGetBits(EventGroupHandle_t group);
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits);
uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits);
int xTaskCreate(TaskFunction_t fn, const char *name, int stack, void *arg,
                int priority, TaskHandle_t *handle);
int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, int stack,
                            void *arg, int priority, TaskHandle_t *handle, int core);
void esp_mqtt_task(void *arg);
void esp_transport_close(void *transport);
void outbox_delete_all_items(void *outbox);
void vTaskDelete(TaskHandle_t task);
void sdk_fatal_cleanup(esp_mqtt_client_handle_t client);
