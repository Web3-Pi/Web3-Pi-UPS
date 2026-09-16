/* Compile the complete production runtime unchanged into this translation
 * unit. Static names are visible only to inspect lifecycle invariants; all
 * exercised runtime behavior enters through its public API/SDK callback. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include "mqtt.c"
#include "mqtt_sdk_adapter.h"
#include "../../common/protocol.h"

struct host_task {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    TaskFunction_t function;
    void *argument;
    char name[24];
    unsigned notifications;
    atomic_bool deleted;
};
struct host_queue {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t item_size;
    unsigned capacity, count, head;
    uint8_t *data;
};
struct host_client { unsigned marker; atomic_bool stopped; };
static _Thread_local TaskHandle_t host_current_task;
static _Thread_local unsigned host_critical_depth;
static _Thread_local bool host_in_callback;
static atomic_bool host_stopping, host_ppp = true;
static atomic_uint host_ppp_generation = 1;
static atomic_uint_fast64_t host_clock_ms = 1000;
static atomic_int host_sdk_calls, host_init_calls, host_register_calls, host_destroy_calls;
static atomic_int host_start_calls, host_reconnect_calls, host_subscribe_calls;
static atomic_int host_revive_calls, host_restart_hooks;
static atomic_int host_outbox_bytes, host_enqueue_calls, host_enqueue_failures;
static atomic_int host_enqueue_failure_result = -2;
static atomic_bool host_outbox_block, host_early_probe_ack;
static atomic_int host_probe_enqueue_calls, host_last_probe_id, host_monitor_polls;
static atomic_int host_owner_idle;
static atomic_int host_task_creates, host_task_deletes, host_queue_creates, host_queue_deletes;
static atomic_int host_queue_failures, host_task_fail_at, host_register_failures, host_start_failures;
static atomic_int host_reconnect_result = ESP_OK;
static atomic_bool host_auto_connect = true;
static esp_event_handler_t host_handler;
static int host_next_packet_id;
static pthread_mutex_t host_sdk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t host_sdk_condition = PTHREAD_COND_INITIALIZER;
static bool host_sdk_block;
static atomic_bool host_sdk_entered;
static pthread_mutex_t host_uart_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t host_uart_condition = PTHREAD_COND_INITIALIZER;
static bool host_uart_block;
static atomic_bool host_uart_entered;
static atomic_int host_commands, host_uart_rx_acks;
static pthread_mutex_t host_records_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { char topic[64]; uint8_t payload[256]; size_t len; int qos, retain; } host_records[64];
static int host_record_count;
static uint8_t host_command_bytes[16][32];
static size_t host_command_lengths[16];

static uint64_t host_wall_ms(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static void host_pause(void)
{
    struct timespec delay = {.tv_nsec = 2000000}; nanosleep(&delay, NULL);
}
#define WAIT_FOR(condition) do { \
    uint64_t until = host_wall_ms() + 3000; \
    while (!(condition) && host_wall_ms() < until) host_pause(); \
    assert(condition); \
} while (0)

static void host_timed_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 2000000;
    if (until.tv_nsec >= 1000000000) { until.tv_nsec -= 1000000000; ++until.tv_sec; }
    int rc = pthread_cond_timedwait(condition, mutex, &until);
    assert(rc == 0 || rc == ETIMEDOUT);
}
void host_enter_critical(portMUX_TYPE *lock)
{ assert(pthread_mutex_lock(lock) == 0); ++host_critical_depth; }
void host_exit_critical(portMUX_TYPE *lock)
{ assert(host_critical_depth); --host_critical_depth; assert(pthread_mutex_unlock(lock) == 0); }
void host_log(const char *format, ...) { (void)format; }
void esp_log_level_set(const char *tag, int level) { (void)tag; (void)level; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "host-fixture"; }
int64_t esp_timer_get_time(void) { return (int64_t)atomic_load(&host_clock_ms) * 1000; }
int esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
uint32_t esp_get_free_heap_size(void) { return 100000; }
uint32_t esp_get_minimum_free_heap_size(void) { return 90000; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 1000; }
const char *identity_iccid(void) { return "0000000000000000000"; }
const char *identity_mqtt_password_hex(void) { return "synthetic-host-test-value"; }
const char *identity_imei(void) { return "fixture"; }
const char *identity_fw_version(void) { return "host-test"; }
const char *identity_hw_version(void) { return "fixture"; }
wups_backend_mode_t backend_mode_get(void) { return WUPS_BACKEND_MODE_MQTT; }
bool modem_ppp_is_up(void) { return atomic_load(&host_ppp); }
uint32_t modem_ppp_generation(void) { return atomic_load(&host_ppp) ? atomic_load(&host_ppp_generation) : 0; }
void modem_notify_mqtt_down(void) {}
bool fw_ota_in_progress(void) { return false; }
uint32_t wups_link_frame_age_s(void) { return 0; }

static void *host_task_entry(void *argument)
{
    host_current_task = argument;
    host_current_task->function(host_current_task->argument);
    return NULL;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                       void *argument, unsigned priority, TaskHandle_t *handle)
{
    (void)stack; (void)priority;
    int attempt = atomic_fetch_add(&host_task_creates, 1) + 1;
    if (attempt == atomic_load(&host_task_fail_at)) { *handle = NULL; return pdFALSE; }
    TaskHandle_t task = calloc(1, sizeof(*task)); assert(task);
    pthread_mutex_init(&task->mutex, NULL); pthread_cond_init(&task->condition, NULL);
    task->function = function; task->argument = argument;
    snprintf(task->name, sizeof(task->name), "%s", name); *handle = task;
    assert(pthread_create(&task->thread, NULL, host_task_entry, task) == 0);
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task); pthread_mutex_lock(&task->mutex); ++task->notifications;
    pthread_cond_broadcast(&task->condition); pthread_mutex_unlock(&task->mutex);
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(host_current_task && !host_critical_depth);
    TaskHandle_t task = host_current_task;
    if (strcmp(task->name, "mqtt_owner") == 0) atomic_fetch_add(&host_owner_idle, 1);
    pthread_mutex_lock(&task->mutex);
    while (!task->notifications && !atomic_load(&task->deleted) && !atomic_load(&host_stopping)) {
        if (!wait) break;
        host_timed_wait(&task->condition, &task->mutex);
        if (wait != portMAX_DELAY) break;
    }
    uint32_t notifications = task->notifications;
    if (clear) task->notifications = 0;
    else if (notifications) --task->notifications;
    bool stop = atomic_load(&task->deleted) || atomic_load(&host_stopping);
    pthread_mutex_unlock(&task->mutex);
    if (stop) pthread_exit(NULL);
    return notifications;
}
void vTaskDelete(TaskHandle_t task)
{
    assert(task && task != host_current_task);
    atomic_store(&task->deleted, true); xTaskNotifyGive(task);
    assert(pthread_join(task->thread, NULL) == 0);
    pthread_cond_destroy(&task->condition); pthread_mutex_destroy(&task->mutex);
    free(task); atomic_fetch_add(&host_task_deletes, 1);
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks; assert(!host_critical_depth);
    if (host_current_task && strcmp(host_current_task->name, "mqtt_health") == 0)
        atomic_fetch_add(&host_monitor_polls, 1);
    if (atomic_load(&host_stopping)) pthread_exit(NULL);
    host_pause();
}
QueueHandle_t xQueueCreate(unsigned length, size_t item_size)
{
    if (atomic_load(&host_queue_failures) > 0) {
        atomic_fetch_sub(&host_queue_failures, 1); return NULL;
    }
    QueueHandle_t q = calloc(1, sizeof(*q)); assert(q);
    q->data = calloc(length, item_size); assert(q->data);
    q->capacity = length; q->item_size = item_size;
    pthread_mutex_init(&q->mutex, NULL); pthread_cond_init(&q->condition, NULL);
    atomic_fetch_add(&host_queue_creates, 1); return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    assert(q && wait == 0 && !host_critical_depth);
    pthread_mutex_lock(&q->mutex);
    bool room = q->count < q->capacity;
    if (room) {
        unsigned tail = (q->head + q->count) % q->capacity;
        memcpy(q->data + tail * q->item_size, item, q->item_size);
        ++q->count; pthread_cond_broadcast(&q->condition);
    }
    pthread_mutex_unlock(&q->mutex); return room ? pdTRUE : pdFALSE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    assert(q && wait == portMAX_DELAY && !host_critical_depth);
    pthread_mutex_lock(&q->mutex);
    while (!q->count && !atomic_load(&host_stopping))
        host_timed_wait(&q->condition, &q->mutex);
    if (atomic_load(&host_stopping)) { pthread_mutex_unlock(&q->mutex); pthread_exit(NULL); }
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity; --q->count;
    pthread_mutex_unlock(&q->mutex); return pdTRUE;
}
void vQueueDelete(QueueHandle_t q)
{
    assert(q); pthread_cond_destroy(&q->condition); pthread_mutex_destroy(&q->mutex);
    free(q->data); free(q); atomic_fetch_add(&host_queue_deletes, 1);
}

static void host_sdk_boundary(void)
{
    /* Fails immediately if an actual producer, callback or health monitor
     * reaches ANY SDK operation, or owner holds the application lock. */
    assert(host_current_task && strcmp(host_current_task->name, "mqtt_owner") == 0);
    assert(!host_critical_depth && !host_in_callback);
    atomic_fetch_add(&host_sdk_calls, 1);
}
static void host_sdk_wait(void)
{
    pthread_mutex_lock(&host_sdk_mutex);
    while (host_sdk_block) {
        atomic_store(&host_sdk_entered, true);
        pthread_cond_wait(&host_sdk_condition, &host_sdk_mutex);
    }
    pthread_mutex_unlock(&host_sdk_mutex);
}
static void host_event(int id, esp_mqtt_event_t *event)
{
    assert(host_handler); bool previous = host_in_callback; host_in_callback = true;
    host_handler(NULL, "MQTT", id, event); host_in_callback = previous;
}
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    host_sdk_boundary(); assert(config->network.timeout_ms == 300000);
    assert(config->session.keepalive == 600);
    assert(config->session.message_retransmit_timeout == 5000);
    assert(config->network.bounded_service);
    struct host_client *client = malloc(sizeof(*client)); assert(client);
    client->marker = 0xface; atomic_init(&client->stopped, false);
    atomic_fetch_add(&host_init_calls, 1); return client;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int32_t id,
                                         esp_event_handler_t handler, void *argument)
{
    (void)id; (void)argument; host_sdk_boundary(); assert(client->marker == 0xface);
    atomic_fetch_add(&host_register_calls, 1);
    if (atomic_load(&host_register_failures) > 0) {
        atomic_fetch_sub(&host_register_failures, 1); return ESP_FAIL;
    }
    host_handler = handler; return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    host_sdk_boundary(); assert(client->marker == 0xface); client->marker = 0;
    free(client); atomic_fetch_add(&host_destroy_calls, 1); return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    host_sdk_boundary(); assert(client->marker == 0xface);
    atomic_fetch_add(&host_start_calls, 1);
    if (atomic_load(&host_start_failures) > 0) {
        atomic_fetch_sub(&host_start_failures, 1); return ESP_FAIL;
    }
    atomic_store(&client->stopped, false);
    if (atomic_load(&host_auto_connect)) { esp_mqtt_event_t e = {0}; host_event(MQTT_EVENT_CONNECTED, &e); }
    return ESP_OK;
}
esp_err_t mqtt_sdk_revive_stopped(esp_mqtt_client_handle_t client,
                                 void (*before_restart)(void *), void *arg)
{
    /* The separate adapter regression compiles its actual pinned SDK gate.
     * Here model only its contract at the runtime's SDK boundary. */
    host_sdk_boundary(); assert(client->marker == 0xface);
    atomic_fetch_add(&host_revive_calls, 1);
    if (!atomic_load(&client->stopped)) return ESP_ERR_INVALID_STATE;
    assert(before_restart); before_restart(arg);
    atomic_fetch_add(&host_restart_hooks, 1);
    portENTER_CRITICAL(&s_lock);
    assert(!s_guard.connected && s_guard.reset_armed && s_guard.reset_boundary_seen);
    portEXIT_CRITICAL(&s_lock);
    return esp_mqtt_client_start(client);
}
esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t client)
{
    host_sdk_boundary(); assert(client->marker == 0xface);
    atomic_fetch_add(&host_reconnect_calls, 1); return atomic_load(&host_reconnect_result);
}
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client)
{
    host_sdk_boundary(); assert(client->marker == 0xface);
    esp_mqtt_event_t e = {0}; host_event(MQTT_EVENT_DISCONNECTED, &e); return ESP_OK;
}
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client)
{
    host_sdk_boundary(); assert(client->marker == 0xface);
    if (atomic_load(&host_outbox_block)) host_sdk_wait();
    return atomic_load(&host_outbox_bytes);
}
esp_err_t esp_mqtt_client_get_service_status(esp_mqtt_client_handle_t client,
                                             esp_mqtt_service_status_t *status)
{
    /* Deliberately no SDK-boundary wait: this API is used by the independent
     * monitor even while a separate SDK call is suspended. */
    assert(client->marker == 0xface);
    memset(status, 0, sizeof(*status));
    return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos)
{
    (void)topic; host_sdk_boundary(); assert(client->marker == 0xface && qos == 1);
    atomic_fetch_add(&host_subscribe_calls, 1); return ++host_next_packet_id;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic,
                             const char *data, int len, int qos, int retain, bool store)
{
    host_sdk_boundary(); assert(client->marker == 0xface && store && len >= 0 && len <= 256);
    host_sdk_wait(); /* hold the mocked SDK API mutex at actual enqueue */
    atomic_fetch_add(&host_enqueue_calls, 1);
    if (atomic_load(&host_enqueue_failures) > 0) {
        atomic_fetch_sub(&host_enqueue_failures, 1);
        return atomic_load(&host_enqueue_failure_result); /* no SDK admission */
    }
    /* Model real SDK zero-length strlen behavior to catch stale bytes. */
    if (len == 0) assert(data && strlen(data) == 0);
    pthread_mutex_lock(&host_records_mutex);
    assert(host_record_count < 64);
    int index = host_record_count++;
    snprintf(host_records[index].topic, sizeof(host_records[index].topic), "%s", topic);
    if (len) memcpy(host_records[index].payload, data, (size_t)len);
    host_records[index].len = (size_t)len;
    host_records[index].qos = qos; host_records[index].retain = retain;
    pthread_mutex_unlock(&host_records_mutex);
    int packet_id = qos ? ++host_next_packet_id : 0;
    if (qos == 1 && strcmp(topic, s_topic_telemetry) == 0) {
        atomic_store(&host_last_probe_id, packet_id);
        atomic_fetch_add(&host_probe_enqueue_calls, 1);
        if (atomic_load(&host_early_probe_ack)) {
            /* This is the real runtime's probe SDK call before it has its ID.
             * An unrelated early PUBACK must not prove health. Matching ACK
             * also remains buffered until enqueue returns and is admitted. */
            portENTER_CRITICAL(&s_lock); assert(s_guard.probe_state == MQTT_PROBE_ARMING);
            portEXIT_CRITICAL(&s_lock);
            esp_mqtt_event_t ack = {.msg_id = packet_id + 1000};
            host_event(MQTT_EVENT_PUBLISHED, &ack);
            mqtt_health_snapshot_t health; mqtt_get_health(&health); assert(!health.proof_fresh);
            ack.msg_id = packet_id; host_event(MQTT_EVENT_PUBLISHED, &ack);
            mqtt_get_health(&health); assert(!health.proof_fresh && !health.probe_admitted);
        }
    }
    return packet_id;
}
static bool host_recorded(const char *topic)
{
    pthread_mutex_lock(&host_records_mutex); bool found = false;
    for (int i = 0; i < host_record_count; ++i) if (strcmp(host_records[i].topic, topic) == 0) found = true;
    pthread_mutex_unlock(&host_records_mutex); return found;
}
static void host_receive_command(const char *topic, size_t topic_len, const void *payload, size_t len)
{
    (void)topic; (void)topic_len;
    assert(host_current_task && strcmp(host_current_task->name, "mqtt_commands") == 0);
    assert(!host_critical_depth && !host_in_callback && len <= 32);
    int index = atomic_load(&host_commands); assert(index < 16);
    if (len) memcpy(host_command_bytes[index], payload, len);
    host_command_lengths[index] = len;
    atomic_fetch_add(&host_commands, 1);
    pthread_mutex_lock(&host_uart_mutex);
    while (host_uart_block) {
        atomic_store(&host_uart_entered, true);
        pthread_cond_wait(&host_uart_condition, &host_uart_mutex);
    }
    pthread_mutex_unlock(&host_uart_mutex);
}
static void host_emit_data(const char *payload, int len)
{
    esp_mqtt_event_t e = {.topic = "c/test/cmd/request", .topic_len = 18,
        .data = payload, .data_len = len, .total_data_len = len};
    host_event(MQTT_EVENT_DATA, &e);
}
static void host_shutdown(void)
{
    atomic_store(&host_stopping, true);
    pthread_mutex_lock(&host_sdk_mutex); host_sdk_block = false;
    pthread_cond_broadcast(&host_sdk_condition); pthread_mutex_unlock(&host_sdk_mutex);
    pthread_mutex_lock(&host_uart_mutex); host_uart_block = false;
    pthread_cond_broadcast(&host_uart_condition); pthread_mutex_unlock(&host_uart_mutex);
    if (s_monitor_task) vTaskDelete(s_monitor_task);
    if (s_command_task) vTaskDelete(s_command_task);
    if (s_owner_task) vTaskDelete(s_owner_task);
    if (s_commands) vQueueDelete(s_commands);
    /* Published clients intentionally live until reboot in production. */
    free(atomic_load(&s_client));
}
static void host_synchronize_owner(void)
{
    int before = atomic_load(&host_owner_idle);
    wake_owner();
    WAIT_FOR(atomic_load(&host_owner_idle) > before);
}
static void test_blocked_sdk(void)
{
    assert(mqtt_runtime_init() == ESP_OK);
    mqtt_set_data_handler(host_receive_command);
    assert(mqtt_client_start() == ESP_OK);
    WAIT_FOR(mqtt_sdk_is_started() && host_recorded("t/0000000000000000000/identify"));
    pthread_mutex_lock(&host_sdk_mutex); host_sdk_block = true; pthread_mutex_unlock(&host_sdk_mutex);
    char topic[] = "t/test/binary"; uint8_t payload[] = {0, 255, 0, 3};
    assert(mqtt_publish_raw(topic, payload, sizeof(payload), 0, 0) == 0);
    memset(payload, 9, sizeof(payload)); memset(topic, 'x', sizeof(topic) - 1);
    WAIT_FOR(atomic_load(&host_sdk_entered));
    uint64_t blocked_since = host_wall_ms();
    uint64_t stall_ms = 15000;
    const char *override = getenv("MQTT_TEST_SDK_STALL_MS");
    if (override) {
        char *end = NULL; unsigned long parsed = strtoul(override, &end, 10);
        assert(*override && end && !*end && parsed <= 15000);
        stall_ms = parsed;
    }
    atomic_fetch_add(&host_clock_ms, 15000); /* configured stall, deterministic clock */
    int calls = atomic_load(&host_sdk_calls);
    uint64_t start = host_wall_ms(), receipt = 0;
    assert(mqtt_publish_tracked("t/test/receipt", "event", 5, 1, 0, &receipt) == 0);
    assert(mqtt_receipt_take(receipt) == MQTT_RECEIPT_PENDING);
    assert(mqtt_publish_raw("t/test/empty", NULL, 0, 1, 1) == 0);
    assert(mqtt_client_start() == ESP_OK); /* supervisor-style idempotent request */
    atomic_fetch_add(&host_uart_rx_acks, 1); /* next local RX dispatch remains reachable */
    mqtt_diagnostics_t diagnostics; mqtt_get_diagnostics(&diagnostics);
    assert(diagnostics.health.worker_busy && atomic_load(&host_uart_rx_acks) == 1);
    assert(host_wall_ms() - start < 100 && atomic_load(&host_sdk_calls) == calls);

    pthread_mutex_lock(&host_uart_mutex); host_uart_block = true; pthread_mutex_unlock(&host_uart_mutex);
    host_emit_data("first", 5); WAIT_FOR(atomic_load(&host_uart_entered));
    char command[] = {0, (char)255, 7};
    start = host_wall_ms(); host_emit_data(command, 3); memset(command, 9, sizeof(command));
    for (unsigned i = 0; i < 5; ++i) host_emit_data("queued", 6);
    assert(host_wall_ms() - start < 100);
    mqtt_get_diagnostics(&diagnostics);
    assert(diagnostics.commands_rejected >= 2 && atomic_load(&host_sdk_calls) == calls);
    esp_mqtt_event_t fragment = {.topic = "topic", .topic_len = 5, .data = "x",
        .data_len = 1, .total_data_len = 2};
    host_event(MQTT_EVENT_DATA, &fragment);
    mqtt_get_diagnostics(&diagnostics); assert(diagnostics.commands_rejected >= 3);

    /* Recheck public producer-side operations for the complete wall-clock
     * stall. Neither the SDK worker nor the command/UART worker can progress. */
    unsigned responsive_checks = 0;
    while (host_wall_ms() - blocked_since < stall_ms) {
        start = host_wall_ms();
        assert(mqtt_client_start() == ESP_OK);
        assert(mqtt_receipt_take(receipt) == MQTT_RECEIPT_PENDING);
        mqtt_get_diagnostics(&diagnostics);
        assert(diagnostics.health.worker_busy && atomic_load(&host_sdk_calls) == calls);
        assert(host_wall_ms() - start < 100);
        atomic_fetch_add(&host_uart_rx_acks, 1);
        ++responsive_checks; host_pause();
    }
    uint64_t held_ms = host_wall_ms() - blocked_since;
    assert(held_ms >= stall_ms);

    pthread_mutex_lock(&host_sdk_mutex); host_sdk_block = false;
    pthread_cond_broadcast(&host_sdk_condition); pthread_mutex_unlock(&host_sdk_mutex);
    WAIT_FOR(host_recorded("t/test/empty"));
    assert(atomic_load(&host_commands) == 1); /* blocked command/UART did not stop SDK worker */
    mqtt_receipt_status_t receipt_state = mqtt_receipt_take(receipt);
    uint64_t receipt_until = host_wall_ms() + 3000;
    while (receipt_state == MQTT_RECEIPT_PENDING && host_wall_ms() < receipt_until) {
        host_pause(); receipt_state = mqtt_receipt_take(receipt);
    }
    assert(receipt_state == MQTT_RECEIPT_SDK_ACCEPTED);
    pthread_mutex_lock(&host_records_mutex);
    for (int i = 0; i < host_record_count; ++i) {
        if (strcmp(host_records[i].topic, "t/test/binary") == 0) {
            const uint8_t expected[] = {0, 255, 0, 3};
            assert(host_records[i].len == 4 && memcmp(expected, host_records[i].payload, 4) == 0);
        }
        if (strcmp(host_records[i].topic, "t/test/empty") == 0)
            assert(host_records[i].len == 0 && host_records[i].qos == 1 && host_records[i].retain == 1);
    }
    pthread_mutex_unlock(&host_records_mutex);
    pthread_mutex_lock(&host_uart_mutex); host_uart_block = false;
    pthread_cond_broadcast(&host_uart_condition); pthread_mutex_unlock(&host_uart_mutex);
    WAIT_FOR(atomic_load(&host_commands) == 5);
    assert(host_command_lengths[1] == 3 && host_command_bytes[1][0] == 0 &&
           host_command_bytes[1][1] == 255 && host_command_bytes[1][2] == 7);
    host_shutdown();
    printf("PASS actual runtime: SDK held %llu ms wall time, %u responsive producer/supervisor checks; DATA callback and command/UART independent; copies and empty retain preserved\n",
           (unsigned long long)held_ms, responsive_checks);
}
static void test_init_failures(void)
{
    mqtt_ota_state_changed(true); /* transfer begins before allocation succeeds */
    atomic_store(&host_queue_failures, 1);
    assert(mqtt_runtime_init() == ESP_ERR_NO_MEM && !atomic_load(&s_ready));
    for (int stage = 1; stage <= 3; ++stage) {
        atomic_store(&host_task_fail_at, atomic_load(&host_task_creates) + stage);
        assert(mqtt_runtime_init() == ESP_ERR_NO_MEM && !atomic_load(&s_ready));
        assert(!s_owner_task && !s_command_task && !s_monitor_task && !s_commands);
        assert(atomic_load(&host_sdk_calls) == 0 && atomic_load(&s_client) == NULL);
        assert(atomic_load(&host_queue_creates) == atomic_load(&host_queue_deletes));
    }
    atomic_store(&host_task_fail_at, 0);
    assert(mqtt_runtime_init() == ESP_OK);
    mqtt_health_snapshot_t health; mqtt_get_health(&health); assert(health.ota_active);
    mqtt_ota_state_changed(false); mqtt_get_health(&health); assert(!health.ota_active);
    int creates = atomic_load(&host_task_creates);
    assert(mqtt_runtime_init() == ESP_OK && atomic_load(&host_task_creates) == creates);
    assert(atomic_load(&host_sdk_calls) == 0);
    host_shutdown();
    puts("PASS actual runtime initialization: queue/all three task failure positions quiesce partial tasks; retry idempotent; no SDK before init barrier; pre-init OTA latch survives retries");
}
static void test_registration(bool registration_failure)
{
    atomic_store(&host_auto_connect, false);
    if (registration_failure) atomic_store(&host_register_failures, 1);
    else atomic_store(&host_start_failures, 1);
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    if (registration_failure) {
        WAIT_FOR(atomic_load(&host_destroy_calls) == 1);
        assert(atomic_load(&s_client) == NULL && !mqtt_sdk_is_started());
    } else {
        WAIT_FOR(atomic_load(&host_start_calls) == 1);
        assert(atomic_load(&s_client) != NULL && !mqtt_sdk_is_started());
    }
    host_synchronize_owner(); /* retry deadline has been committed before clock advance */
    assert(mqtt_publish_raw("t/test/before-ready", "x", 1, 0, 0) == 0);
    atomic_store(&host_clock_ms, 11000); wake_owner();
    WAIT_FOR(mqtt_sdk_is_started());
    assert(atomic_load(&host_register_calls) == (registration_failure ? 2 : 1));
    assert(atomic_load(&host_init_calls) == (registration_failure ? 2 : 1));
    assert(atomic_load(&host_start_calls) == (registration_failure ? 1 : 2));
    host_shutdown();
    puts(registration_failure ? "PASS actual SDK setup: failed registration destroys only private handle; queued producer survives retry" :
         "PASS actual SDK setup: failed task start reuses registered handle, no duplicate init/callback registration");
}
static void test_backoff(void)
{
    atomic_store(&host_auto_connect, false); atomic_store(&host_reconnect_result, ESP_FAIL);
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(mqtt_sdk_is_started());
    host_synchronize_owner();
    esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        .connect_return_code = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED};
    esp_mqtt_event_t event = {.error_handle = &error};
    for (int i = 0; i < 3; ++i) host_event(MQTT_EVENT_ERROR, &event);
    atomic_store(&host_clock_ms, 10999);
    for (int i = 0; i < 4; ++i) { assert(mqtt_client_start() == ESP_OK); host_synchronize_owner(); }
    assert(atomic_load(&host_reconnect_calls) == 0);
    atomic_store(&host_clock_ms, 11000); wake_owner(); WAIT_FOR(atomic_load(&host_reconnect_calls) == 1);
    host_synchronize_owner(); atomic_store(&host_reconnect_result, ESP_OK);
    atomic_store(&host_clock_ms, 20999); host_synchronize_owner();
    assert(atomic_load(&host_reconnect_calls) == 1);
    atomic_store(&host_clock_ms, 21000); wake_owner(); WAIT_FOR(atomic_load(&host_reconnect_calls) == 2);
    host_synchronize_owner(); atomic_store(&host_clock_ms, 31000); host_synchronize_owner();
    assert(atomic_load(&host_reconnect_calls) == 2);
    atomic_store(&host_clock_ms, 51000); wake_owner(); WAIT_FOR(atomic_load(&host_reconnect_calls) == 3);
    host_synchronize_owner(); atomic_store(&host_clock_ms, 81000); host_synchronize_owner();
    assert(atomic_load(&host_reconnect_calls) == 3);
    atomic_store(&host_clock_ms, 111000); wake_owner(); WAIT_FOR(atomic_load(&host_reconnect_calls) == 4);
    host_shutdown();
    puts("PASS actual owner reconnect schedule: rejected SDK request stays at 10s; accepted requests select 30/60s auth backoff; application start requests do not advance it");
}
static void test_ppp_retry(bool authentication)
{
    atomic_store(&host_auto_connect, false);
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(mqtt_sdk_is_started()); host_synchronize_owner();
    esp_mqtt_error_codes_t error = {
        .error_type = authentication ? MQTT_ERROR_TYPE_CONNECTION_REFUSED : MQTT_ERROR_TYPE_TCP_TRANSPORT,
        .connect_return_code = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED};
    esp_mqtt_event_t event = {.error_handle = &error};
    for (int i = 0; i < 6; ++i) host_event(MQTT_EVENT_ERROR, &event);
    const uint64_t attempts[] = {11000, 41000, 101000};
    for (unsigned i = 0; i < 3; ++i) {
        atomic_store(&host_clock_ms, attempts[i]); wake_owner();
        WAIT_FOR(atomic_load(&host_reconnect_calls) == (int)i + 1);
        host_synchronize_owner();
    }
    /* The old transport has a 120s retry pending. Same-session start requests
     * do not shorten it, but a genuinely new PPP generation may do so. */
    atomic_store(&host_clock_ms, 102000);
    assert(mqtt_client_start() == ESP_OK); host_synchronize_owner();
    assert(atomic_load(&host_reconnect_calls) == 3);
    atomic_store(&host_ppp, false); host_synchronize_owner();
    atomic_fetch_add(&host_ppp_generation, 1); atomic_store(&host_ppp, true);
    assert(mqtt_client_start() == ESP_OK); host_synchronize_owner();
    if (authentication) {
        assert(atomic_load(&host_reconnect_calls) == 3);
        atomic_store(&host_clock_ms, 220999); host_synchronize_owner();
        assert(atomic_load(&host_reconnect_calls) == 3);
        atomic_store(&host_clock_ms, 221000); wake_owner();
    }
    WAIT_FOR(atomic_load(&host_reconnect_calls) == 4); host_synchronize_owner();
    for (int i = 0; i < 4; ++i) {
        assert(mqtt_client_start() == ESP_OK); host_synchronize_owner();
    }
    assert(atomic_load(&host_reconnect_calls) == 4);
    host_shutdown();
    puts(authentication ? "PASS PPP generation preserves authentication backoff" :
         "PASS PPP generation expedites obsolete 120s transport backoff exactly once");
}
static bool host_reset_commit(void *context)
{
    assert(host_critical_depth == 1);
    ++*(unsigned *)context;
    return true;
}
static void test_recovery_commit(void)
{
    atomic_store(&host_auto_connect, false);
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(mqtt_sdk_is_started()); host_synchronize_owner();
    atomic_store(&host_ppp, false); host_synchronize_owner();
    mqtt_health_snapshot_t old, current;
    mqtt_get_health(&old);
    unsigned commits = 0;
    assert(mqtt_recovery_try_commit(&old, host_reset_commit, &commits) && commits == 1);
    esp_mqtt_event_t event = {0};
    host_event(MQTT_EVENT_CONNECTED, &event);
    assert(!mqtt_recovery_try_commit(&old, host_reset_commit, &commits));
    host_event(MQTT_EVENT_DISCONNECTED, &event);
    /* Even a complete reconnect+disconnect during diagnostics invalidates
     * a decision from the preceding session. */
    assert(!mqtt_recovery_try_commit(&old, host_reset_commit, &commits));
    mqtt_get_health(&current);
    assert(mqtt_recovery_try_commit(&current, host_reset_commit, &commits) && commits == 2);
    mqtt_ota_state_changed(true);
    assert(!mqtt_recovery_try_commit(&current, host_reset_commit, &commits));
    mqtt_ota_state_changed(false);
    sdk_begin(); atomic_store(&host_clock_ms, 40000);
    assert(!mqtt_recovery_try_commit(&current, host_reset_commit, &commits));
    sdk_end();
    esp_mqtt_error_codes_t error = {.error_type = MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        .connect_return_code = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED};
    event.error_handle = &error;
    for (int i = 0; i < 3; ++i) host_event(MQTT_EVENT_ERROR, &event);
    assert(!mqtt_recovery_try_commit(&current, host_reset_commit, &commits));
    assert(commits == 2);
    host_shutdown();
    puts("PASS reset commit rejects current CONNECTED, changed generation, OTA, stalled worker and auth refusal");
}
static void test_revive(void)
{
    atomic_store(&host_auto_connect, false); atomic_store(&host_reconnect_result, ESP_FAIL);
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(mqtt_sdk_is_started()); host_synchronize_owner();
    for (int i = 1; i <= 40; ++i) {
        if (i == 21) {
            atomic_store(&atomic_load(&s_client)->stopped, true);
            atomic_store(&host_auto_connect, true);
        }
        atomic_store(&host_clock_ms, 1000 + (uint64_t)i * 10000);
        wake_owner(); WAIT_FOR(atomic_load(&host_reconnect_calls) == i);
        host_synchronize_owner();
        if (i == 20) {
            assert(atomic_load(&host_revive_calls) == 1);
            assert(atomic_load(&host_start_calls) == 1 && atomic_load(&host_restart_hooks) == 0);
        }
    }
    assert(atomic_load(&host_revive_calls) == 2 && atomic_load(&host_restart_hooks) == 1);
    assert(atomic_load(&host_start_calls) == 2 && atomic_load(&host_init_calls) == 1);
    assert(mqtt_is_connected());
    mqtt_health_snapshot_t health; mqtt_get_health(&health);
    assert(health.connected && health.generation == 1);
    host_shutdown();
    puts("PASS actual owner revival: twenty rejected reconnects cannot restart a live SDK; stopped-client hook establishes boundary before immediate CONNECTED callback");
}
static uint64_t host_retry_due(const char *topic)
{
    uint64_t due = 0;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MQTT_DISPATCH_CAPACITY; ++i) {
        const mqtt_dispatch_slot_t *slot = &s_queue.slots[i];
        if (slot->state == MQTT_DISPATCH_SLOT_QUEUED && strcmp(slot->item.topic, topic) == 0)
            due = slot->retry_after_ms;
    }
    portEXIT_CRITICAL(&s_lock); return due;
}
static void host_assert_once_in_order(const char *first, const char *second)
{
    int first_index = -1, second_index = -1;
    pthread_mutex_lock(&host_records_mutex);
    for (int i = 0; i < host_record_count; ++i) {
        if (strcmp(host_records[i].topic, first) == 0) { assert(first_index == -1); first_index = i; }
        if (strcmp(host_records[i].topic, second) == 0) { assert(second_index == -1); second_index = i; }
    }
    pthread_mutex_unlock(&host_records_mutex);
    assert(first_index >= 0 && second_index > first_index);
}
static void test_pressure(void)
{
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(host_recorded("t/0000000000000000000/identify")); host_synchronize_owner();
    int attempts = atomic_load(&host_enqueue_calls);
    atomic_store(&host_outbox_bytes, 24 * 1024);
    assert(mqtt_publish_raw("t/pressure/n1", "one", 3, 1, 0) == 0);
    WAIT_FOR(host_retry_due("t/pressure/n1") != 0);
    assert(mqtt_publish_raw("t/pressure/n2", "two", 3, 1, 0) == 0);
    uint64_t receipt = 0;
    assert(mqtt_publish_tracked("t/pressure/event", "event", 5, 1, 0, &receipt) == 0);
    WAIT_FOR(host_recorded("t/pressure/event")); host_synchronize_owner();
    assert(atomic_load(&host_enqueue_calls) == attempts + 1);
    assert(!host_recorded("t/pressure/n1") && !host_recorded("t/pressure/n2"));
    assert(mqtt_receipt_take(receipt) == MQTT_RECEIPT_SDK_ACCEPTED);
    atomic_store(&host_outbox_bytes, 0);
    uint64_t due = host_retry_due("t/pressure/n1");
    atomic_store(&host_clock_ms, due - 1); host_synchronize_owner();
    assert(atomic_load(&host_enqueue_calls) == attempts + 1);
    atomic_store(&host_clock_ms, due); wake_owner();
    WAIT_FOR(host_recorded("t/pressure/n2")); host_synchronize_owner();
    assert(atomic_load(&host_enqueue_calls) == attempts + 3);
    host_assert_once_in_order("t/pressure/n1", "t/pressure/n2");

    attempts = atomic_load(&host_enqueue_calls);
    atomic_store(&host_outbox_bytes, 32 * 1024);
    assert(mqtt_publish_raw("t/full/normal", "one", 3, 0, 0) == 0);
    assert(mqtt_publish_tracked("t/full/event", "event", 5, 1, 0, &receipt) == 0);
    WAIT_FOR(host_retry_due("t/full/normal") && host_retry_due("t/full/event"));
    host_synchronize_owner();
    assert(atomic_load(&host_enqueue_calls) == attempts);
    assert(mqtt_receipt_take(receipt) == MQTT_RECEIPT_PENDING);
    due = host_retry_due("t/full/event");
    atomic_store(&host_outbox_bytes, 0); atomic_store(&host_clock_ms, due); wake_owner();
    WAIT_FOR(host_recorded("t/full/normal")); host_synchronize_owner();
    assert(atomic_load(&host_enqueue_calls) == attempts + 2);
    assert(mqtt_receipt_take(receipt) == MQTT_RECEIPT_SDK_ACCEPTED);
    host_assert_once_in_order("t/full/event", "t/full/normal");

    for (int failure = -2; failure <= -1; ++failure) {
        char first[32], second[32];
        snprintf(first, sizeof(first), "t/failure/%d/first", -failure);
        snprintf(second, sizeof(second), "t/failure/%d/second", -failure);
        mqtt_diagnostics_t before, after; mqtt_get_diagnostics(&before);
        attempts = atomic_load(&host_enqueue_calls);
        atomic_store(&host_enqueue_failure_result, failure); atomic_store(&host_enqueue_failures, 1);
        assert(mqtt_publish_raw(first, "one", 3, 1, 0) == 0);
        WAIT_FOR(host_retry_due(first) != 0); host_synchronize_owner();
        assert(atomic_load(&host_enqueue_calls) == attempts + 1 && !host_recorded(first));
        mqtt_get_diagnostics(&after);
        assert(after.sdk_accepted == before.sdk_accepted && after.sdk_rejected == before.sdk_rejected + 1);
        assert(mqtt_publish_raw(second, "two", 3, 1, 0) == 0);
        due = host_retry_due(first);
        atomic_store(&host_clock_ms, due - 1); host_synchronize_owner();
        assert(atomic_load(&host_enqueue_calls) == attempts + 1 && !host_recorded(second));
        atomic_store(&host_clock_ms, due); wake_owner();
        WAIT_FOR(host_recorded(second)); host_synchronize_owner();
        assert(atomic_load(&host_enqueue_calls) == attempts + 3);
        host_assert_once_in_order(first, second);
        mqtt_get_diagnostics(&after);
        assert(after.sdk_accepted == before.sdk_accepted + 2 && after.sdk_rejected == before.sdk_rejected + 1);
    }
    mqtt_diagnostics_t diagnostics; mqtt_get_diagnostics(&diagnostics); assert(diagnostics.queue.depth == 0);
    host_shutdown();
    puts("PASS actual runtime pressure: 24KiB preserves critical reserve; 32KiB defers all; cleared pressure and SDK full/OOM retries preserve FIFO, metrics and exactly-once SDK admission");
}
static bool host_has_proof(void)
{
    mqtt_health_snapshot_t health; mqtt_get_health(&health); return health.proof_fresh;
}
static mqtt_health_failure_t host_observed_failure(void)
{
    /* Inspect without polling: this must have been advanced by a real runtime
     * task, not by the test's diagnostics accessor. */
    portENTER_CRITICAL(&s_lock); mqtt_health_failure_t failure = s_health.failure;
    portEXIT_CRITICAL(&s_lock); return failure;
}
static void host_publish_net_frame(void)
{
    wups_net_status_v2_t status = {.version = 2, .state = 5, .rssi_dBm = -70};
    uint8_t frame[WUPS_FRAMING_BYTES + sizeof(status)] = {
        WUPS_SYNC1, WUPS_SYNC2, WUPS_ADDR_BROADCAST, WUPS_ADDR_ESP32,
        WUPS_CLASS_NET, WUPS_OP_NET_STATUS, WUPS_FLAG_EVENT, 1, sizeof(status), 0
    };
    memcpy(frame + WUPS_HEADER_BYTES, &status, sizeof(status));
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < WUPS_HEADER_BYTES + sizeof(status); ++i) { a += frame[i]; b += a; }
    frame[sizeof(frame) - 4] = a; frame[sizeof(frame) - 3] = b;
    frame[sizeof(frame) - 2] = WUPS_END1; frame[sizeof(frame) - 1] = WUPS_END2;
    mqtt_publish_net_status(frame, sizeof(frame));
    memset(frame, 0xcc, sizeof(frame)); /* cache and queued snapshot own copies */
}
static void test_probe(void)
{
    assert(mqtt_runtime_init() == ESP_OK && mqtt_client_start() == ESP_OK);
    WAIT_FOR(host_recorded("t/0000000000000000000/identify")); host_synchronize_owner();
    mqtt_health_snapshot_t health; mqtt_get_health(&health);
    assert(health.connected && !health.proof_fresh && health.probe_due);
    assert(atomic_load(&host_probe_enqueue_calls) == 0);
    esp_mqtt_event_t event = {.msg_id = 12345};
    host_event(MQTT_EVENT_PUBLISHED, &event); assert(!host_has_proof());

    atomic_store(&host_early_probe_ack, true); host_publish_net_frame();
    WAIT_FOR(host_has_proof()); host_synchronize_owner();
    assert(atomic_load(&host_probe_enqueue_calls) == 1);
    int old_id = atomic_load(&host_last_probe_id);
    mqtt_get_health(&health);
    assert(!health.probe_pending && health.last_ack_ms == 1000 && health.generation == 1);
    pthread_mutex_lock(&host_records_mutex);
    bool valid_frame = false;
    for (int i = 0; i < host_record_count; ++i) {
        if (host_records[i].qos == 1 && strcmp(host_records[i].topic, s_topic_telemetry) == 0) {
            assert(host_records[i].len == WUPS_FRAMING_BYTES + sizeof(wups_net_status_v2_t));
            assert(host_records[i].payload[0] == WUPS_SYNC1 && host_records[i].payload[10] == 2);
            assert(host_records[i].retain == 0); valid_frame = true;
        }
    }
    pthread_mutex_unlock(&host_records_mutex); assert(valid_frame);

    atomic_store(&host_early_probe_ack, false);
    host_event(MQTT_EVENT_DISCONNECTED, &event); host_event(MQTT_EVENT_CONNECTED, &event);
    assert(!host_has_proof());
    WAIT_FOR(atomic_load(&host_probe_enqueue_calls) == 2); host_synchronize_owner();
    int new_id = atomic_load(&host_last_probe_id); assert(new_id != old_id);
    mqtt_get_health(&health); assert(health.generation == 2 && health.probe_admitted);
    event.msg_id = old_id; host_event(MQTT_EVENT_PUBLISHED, &event); assert(!host_has_proof());
    event.msg_id = new_id + 1000; host_event(MQTT_EVENT_PUBLISHED, &event); assert(!host_has_proof());
    event.msg_id = new_id; host_event(MQTT_EVENT_PUBLISHED, &event); assert(host_has_proof());

    mqtt_ota_state_changed(true); assert(!host_has_proof());
    event.msg_id = new_id; host_event(MQTT_EVENT_PUBLISHED, &event); assert(!host_has_proof());
    host_synchronize_owner(); assert(atomic_load(&host_probe_enqueue_calls) == 2);
    mqtt_get_health(&health); assert(health.ota_active && !health.probe_due);
    mqtt_ota_state_changed(false); assert(!host_has_proof());
    WAIT_FOR(atomic_load(&host_probe_enqueue_calls) == 3); host_synchronize_owner();
    event.msg_id = new_id; host_event(MQTT_EVENT_PUBLISHED, &event); assert(!host_has_proof());
    new_id = atomic_load(&host_last_probe_id);
    event.msg_id = new_id; host_event(MQTT_EVENT_PUBLISHED, &event); assert(host_has_proof());

    mqtt_ota_state_changed(true); atomic_store(&host_outbox_bytes, 32 * 1024);
    mqtt_ota_state_changed(false);
    host_synchronize_owner();
    mqtt_get_health(&health);
    assert(health.probe_due && !health.probe_pending && !health.probe_admitted);
    assert(atomic_load(&host_probe_enqueue_calls) == 3);
    mqtt_get_health(&health); uint64_t deadline = health.probe_deadline_ms;
    assert(deadline == atomic_load(&host_clock_ms) + 300000 && !health.proof_fresh);
    /* Hold the owner before SDK admission while reported outbox is full.
     * Only the separate monitor can advance the deadline during this hold. */
    pthread_mutex_lock(&host_sdk_mutex); host_sdk_block = true; pthread_mutex_unlock(&host_sdk_mutex);
    atomic_store(&host_sdk_entered, false); atomic_store(&host_outbox_block, true); wake_owner();
    WAIT_FOR(atomic_load(&host_sdk_entered));
    assert(atomic_load(&host_probe_enqueue_calls) == 3);
    int monitor_before = atomic_load(&host_monitor_polls);
    atomic_store(&host_clock_ms, deadline - 1);
    WAIT_FOR(atomic_load(&host_monitor_polls) > monitor_before);
    assert(host_observed_failure() == MQTT_HEALTH_FAILURE_NONE);
    atomic_store(&host_clock_ms, deadline);
    WAIT_FOR(host_observed_failure() == MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);
    mqtt_get_health(&health);
    assert(health.connected && health.degraded && !health.proof_fresh && !health.probe_admitted);
    assert(health.worker_busy && health.worker_stalled && atomic_load(&host_probe_enqueue_calls) == 3);
    host_shutdown();
    puts("PASS actual runtime probes: CONNECT/unrelated/old PUBACK cannot prove health; cached QoS1 frame and early matching PUBACK establish proof only after admission; OTA requires fresh proof; independent monitor expires blocked admission at 300s");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "isolation") == 0) test_blocked_sdk();
    else if (strcmp(argv[1], "init") == 0) test_init_failures();
    else if (strcmp(argv[1], "registration") == 0) test_registration(true);
    else if (strcmp(argv[1], "start") == 0) test_registration(false);
    else if (strcmp(argv[1], "backoff") == 0) test_backoff();
    else if (strcmp(argv[1], "ppp_transport") == 0) test_ppp_retry(false);
    else if (strcmp(argv[1], "ppp_auth") == 0) test_ppp_retry(true);
    else if (strcmp(argv[1], "recovery_commit") == 0) test_recovery_commit();
    else if (strcmp(argv[1], "revive") == 0) test_revive();
    else if (strcmp(argv[1], "pressure") == 0) test_pressure();
    else if (strcmp(argv[1], "probe") == 0) test_probe();
    else assert(false);
    return 0;
}
