#include "mqtt_sdk_adapter.h"
#include "mqtt_client_priv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define STOPPED_BIT (1u << 0)
static struct esp_mqtt_client client;
static mqtt_config_storage_t config;
static uint32_t event_bits;
static unsigned starts, hooks, deleted_tasks, cleanup_probes;
static bool fail_create, immediate_task, in_cleanup, transport_closed, outbox_empty;

void mock_api_lock(esp_mqtt_client_handle_t c) { ++c->lock_depth; }
void mock_api_unlock(esp_mqtt_client_handle_t c)
{ assert(c->lock_depth); --c->lock_depth; }
uint32_t xEventGroupGetBits(EventGroupHandle_t group) { return *group; }
uint32_t xEventGroupClearBits(EventGroupHandle_t group, uint32_t bits)
{ uint32_t old = *group; *group &= ~bits; return old; }

static void before_restart(void *arg)
{
    assert(arg == &client);
    assert(client.lock_depth == 1);
    assert(client.state == MQTT_STATE_DISCONNECTED && !client.run);
    assert((event_bits & STOPPED_BIT) == 0); /* reservation precedes hook */
    assert(transport_closed && outbox_empty);
    ++hooks;
}

uint32_t xEventGroupSetBits(EventGroupHandle_t group, uint32_t bits)
{
    *group |= bits;
    if (in_cleanup) {
        /* The SDK has just crossed its definitive stopped boundary. Starting
         * immediately here is safe even before old task's vTaskDelete(NULL). */
        ++cleanup_probes;
        assert(client.state == MQTT_STATE_DISCONNECTED);
        assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_OK);
    }
    return *group;
}

void esp_mqtt_task(void *arg)
{
    /* Pinned task-entry prefix, before its first API lock. */
    esp_mqtt_client_handle_t c = arg;
    c->run = true;
    c->state = MQTT_STATE_INIT;
    xEventGroupClearBits(c->status_bits, STOPPED_BIT);
}

int xTaskCreate(TaskFunction_t fn, const char *name, int stack, void *arg,
                int priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)priority;
    assert(fn == esp_mqtt_task);
    assert(client.lock_depth == 2); /* adapter + actual SDK recursive entry */
    ++starts;
    if (fail_create) return 0;
    *handle = &client;
    if (immediate_task) fn(arg);
    return pdTRUE;
}
int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, int stack,
                            void *arg, int priority, TaskHandle_t *handle, int core)
{
    assert(core == MQTT_TASK_CORE);
    return xTaskCreate(fn, name, stack, arg, priority, handle);
}

void esp_transport_close(void *transport)
{
    (void)transport;
    transport_closed = true;
    assert(in_cleanup);
    ++cleanup_probes;
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
}
void outbox_delete_all_items(void *outbox)
{
    (void)outbox;
    outbox_empty = true;
    assert(in_cleanup);
    ++cleanup_probes;
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
}
void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL); /* old cleanup tail never dereferences client again */
    ++deleted_tasks;
}

static void reset(mqtt_client_state_t state, bool run, bool stopped)
{
    memset(&client, 0, sizeof(client));
    client.state = state;
    client.run = run;
    client.status_bits = &event_bits;
    client.config = &config;
    event_bits = stopped ? STOPPED_BIT : 0;
    starts = hooks = deleted_tasks = cleanup_probes = 0;
    fail_create = immediate_task = in_cleanup = false;
    transport_closed = outbox_empty = stopped;
}

static void test_live_and_incomplete_cleanup_rejected(void)
{
    reset(MQTT_STATE_INIT, true, false);
    assert(mqtt_sdk_revive_stopped(NULL, before_restart, &client) == ESP_ERR_INVALID_ARG);
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
    client.run = false; /* fatal error detected, cleanup still pending */
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
    client.state = MQTT_STATE_DISCONNECTED; /* before SDK sets STOPPED_BIT */
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
    assert(starts == 0 && hooks == 0 && client.lock_depth == 0);
    reset(MQTT_STATE_INIT, true, true); /* stale bit cannot authorize INIT */
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
    reset(MQTT_STATE_DISCONNECTED, true, true); /* inconsistent -> fail closed */
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
}

static void test_start_reservation_and_allocation_failure(void)
{
    reset(MQTT_STATE_DISCONNECTED, false, true);
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_OK);
    assert(starts == 1 && hooks == 1 && client.lock_depth == 0);
    assert(!client.run && client.state == MQTT_STATE_DISCONNECTED);
    /* New task not scheduled yet: the old STOPPED_BIT must not authorize a
     * second task on the same still-DISCONNECTED client. */
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);
    assert(starts == 1 && hooks == 1);
    esp_mqtt_task(&client);
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_ERR_INVALID_STATE);

    reset(MQTT_STATE_DISCONNECTED, false, true);
    fail_create = true;
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_FAIL);
    assert(starts == 1 && hooks == 1 && (event_bits & STOPPED_BIT));
    fail_create = false;
    immediate_task = true;
    assert(mqtt_sdk_revive_stopped(&client, before_restart, &client) == ESP_OK);
    assert(starts == 2 && hooks == 2 && client.run && client.state == MQTT_STATE_INIT);
    assert(!(event_bits & STOPPED_BIT) && client.lock_depth == 0);
}

static void test_real_cleanup_boundary_interleavings(void)
{
    reset(MQTT_STATE_INIT, false, false);
    in_cleanup = true;
    immediate_task = true;
    sdk_fatal_cleanup(&client); /* actual SDK cleanup tail; injected after each stage */
    in_cleanup = false;
    assert(cleanup_probes == 3 && starts == 1 && hooks == 1 && deleted_tasks == 1);
    assert(client.run && client.state == MQTT_STATE_INIT);
    assert(!(event_bits & STOPPED_BIT) && client.lock_depth == 0);
}

int main(void)
{
    test_live_and_incomplete_cleanup_rejected();
    test_start_reservation_and_allocation_failure();
    test_real_cleanup_boundary_interleavings();
    printf("mqtt SDK adapter: 3 lifecycle groups passed (core_selection=%d)\n",
           MQTT_CORE_SELECTION_ENABLED);
    return 0;
}
