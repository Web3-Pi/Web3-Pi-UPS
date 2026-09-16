#include "transport_diag.h"
#include "driver/uart.h"
#include "netif/ppp/pppos.h"
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int __wrap_uart_write_bytes(uart_port_t, const void *, size_t);
int __wrap_uart_read_bytes(uart_port_t, void *, uint32_t, uint32_t);
esp_err_t __wrap_uart_get_buffered_data_len(uart_port_t, size_t *);
esp_err_t __wrap_uart_driver_install(uart_port_t, int, int, int, QueueHandle_t *, int);
esp_err_t __wrap_uart_driver_delete(uart_port_t);
BaseType_t __wrap_xQueueReceive(QueueHandle_t, void *, TickType_t);
err_t __wrap_pppos_input_tcpip_as_ram_pbuf(ppp_pcb *, u8_t *, int);

static atomic_uint checks, failures;
static const char *scenario;
#define CHECK(condition) do { \
    atomic_fetch_add(&checks, 1); \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d [%s]: %s\n", __func__, __LINE__, scenario, #condition); \
        atomic_fetch_add(&failures, 1); \
    } \
} while (0)
enum operation { WRITE, READ, OCCUPANCY, INSTALL, DELETE, RECEIVE, PPP };
typedef struct {
    enum operation operation;
    int port, result, after_core, rx_size, tx_size, queue_size, flags, ppp_length;
    void *data;
    ppp_pcb *ppp;
    size_t length, occupancy;
    uint32_t ticks;
    QueueHandle_t queue, *queue_slot;
    unsigned calls;
    uint64_t duration;
    uart_event_type_t event;
    bool reinstall;
} expectation;
static _Thread_local expectation expected;
static _Thread_local int64_t now_us = 1000000;
static _Thread_local unsigned lock_depth;
static _Thread_local unsigned lock_calls;
static _Thread_local int current_core;
static unsigned log_count;
static char logs[8][1024];
static bool inject_after_snapshot;
static unsigned queue_tokens[3];

void mock_lock(portMUX_TYPE *lock)
{
    ++lock_calls;
    CHECK(lock_depth == 0);
    CHECK(pthread_mutex_lock(lock) == 0);
    ++lock_depth;
}
void mock_unlock(portMUX_TYPE *lock)
{
    CHECK(lock_depth == 1);
    --lock_depth;
    CHECK(pthread_mutex_unlock(lock) == 0);
}
int64_t esp_timer_get_time(void) { CHECK(lock_depth == 0); return now_us; }
int xPortGetCoreID(void) { CHECK(lock_depth == 0); return current_core; }

static void begin(enum operation operation, int port, int result)
{
    expected = (expectation){ .operation = operation, .port = port, .result = result,
                              .after_core = current_core };
}
static void real_call(enum operation operation)
{
    CHECK(lock_depth == 0 && expected.operation == operation && expected.calls == 0);
    ++expected.calls;
    now_us += (int64_t)expected.duration;
    current_core = expected.after_core;
}
int __real_uart_write_bytes(uart_port_t port, const void *data, size_t length)
{
    real_call(WRITE);
    CHECK(port == expected.port && data == expected.data && length == expected.length);
    return expected.result;
}
int __real_uart_read_bytes(uart_port_t port, void *data, uint32_t length, uint32_t ticks)
{
    real_call(READ);
    CHECK(port == expected.port && data == expected.data && length == expected.length);
    CHECK(ticks == expected.ticks);
    if (expected.result > 0) memset(data, 'R', (size_t)expected.result);
    return expected.result;
}
esp_err_t __real_uart_get_buffered_data_len(uart_port_t port, size_t *length)
{
    real_call(OCCUPANCY);
    CHECK(port == expected.port && length == expected.data);
    if (length) *length = expected.occupancy;
    return expected.result;
}
esp_err_t __real_uart_driver_install(uart_port_t port, int rx, int tx, int count,
                                    QueueHandle_t *queue, int flags)
{
    real_call(INSTALL);
    CHECK(port == expected.port && rx == expected.rx_size && tx == expected.tx_size);
    CHECK(count == expected.queue_size && queue == expected.queue_slot && flags == expected.flags);
    if (queue && expected.result == ESP_OK) *queue = expected.queue;
    return expected.result;
}
esp_err_t __real_uart_driver_delete(uart_port_t port)
{
    real_call(DELETE);
    CHECK(port == expected.port);
    return expected.result;
}
static void install(int port, QueueHandle_t queue, int result)
{
    QueueHandle_t slot = NULL;
    begin(INSTALL, port, result);
    expected.rx_size = 2048; expected.tx_size = 0; expected.queue_size = queue ? 8 : 0;
    expected.queue_slot = &slot; expected.queue = queue; expected.flags = 17;
    CHECK(__wrap_uart_driver_install(port, 2048, 0, expected.queue_size, &slot, 17) == result);
    CHECK(expected.calls == 1 && (result != ESP_OK || slot == queue));
}
static void remove_driver(int port, int result)
{
    begin(DELETE, port, result);
    CHECK(__wrap_uart_driver_delete(port) == result && expected.calls == 1);
}
BaseType_t __real_xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    real_call(RECEIVE);
    CHECK(queue == expected.queue && item == expected.data && ticks == expected.ticks);
    if (expected.reinstall) {
        expectation saved = expected;
        remove_driver(1, ESP_OK);
        install(1, queue, ESP_OK); /* Reuse exactly the same queue address. */
        expected = saved;
    }
    if (item && expected.result == pdTRUE)
        *(uart_event_t *)item = (uart_event_t){ .type = expected.event, .size = 71 };
    return expected.result;
}
err_t __real_pppos_input_tcpip_as_ram_pbuf(ppp_pcb *ppp, u8_t *data, int length)
{
    real_call(PPP);
    CHECK(ppp == expected.ppp && data == expected.data && length == expected.ppp_length);
    return expected.result;
}
static void write_sample(int port, size_t length, int result, uint64_t duration, int after_core)
{
    static char payload[32] = "unchanged transport data";
    begin(WRITE, port, result);
    expected.data = payload; expected.length = length;
    expected.duration = duration; expected.after_core = after_core;
    unsigned before_logs = log_count;
    CHECK(__wrap_uart_write_bytes(port, payload, length) == result);
    CHECK(expected.calls == 1 && log_count == before_logs);
    CHECK(!strcmp(payload, "unchanged transport data"));
}
void mock_log(const char *tag, const char *format, ...)
{
    CHECK(lock_depth == 0 && log_count < 8);
    if (log_count >= 8) abort();
    char *line = logs[log_count++];
    int prefix = snprintf(line, sizeof(logs[0]), "%s ", tag);
    va_list arguments;
    va_start(arguments, format);
    int count = vsnprintf(line + prefix, sizeof(logs[0]) - (size_t)prefix, format, arguments);
    va_end(arguments);
    CHECK(prefix > 0 && count >= 0 && (size_t)(prefix + count) < sizeof(logs[0]));
    if (inject_after_snapshot) {
        inject_after_snapshot = false;
        write_sample(1, 7, 7, 22, 1);
    }
}
static void report(void)
{
    log_count = 0;
    transport_diag_log();
    CHECK(log_count == 5);
}
static uint64_t metric(int port, const char *key)
{
    char identity[32], field[64];
    if (port) snprintf(identity, sizeof(identity), "PERF_UART uart=%d ", port);
    else snprintf(identity, sizeof(identity), "PERF_PPP ");
    snprintf(field, sizeof(field), " %s=", key);
    for (unsigned i = 0; i < log_count; ++i) {
        if (strncmp(logs[i], identity, strlen(identity))) continue;
        const char *value = strstr(logs[i], field);
        if (value) return strtoull(value + strlen(field), NULL, 0);
    }
    CHECK(false);
    return UINT64_MAX;
}
static void test_write(void)
{
    write_sample(1, 10, 10, 12, 1);
    write_sample(1, 10, -7, 50, 1);
    write_sample(1, 10, 3, 500000, 0);
    write_sample(1, 0, 0, 0, 0);
    write_sample(2, 20, 20, 8, 0);
    write_sample(0, 4, 4, 999, 0);
    report();
    CHECK(metric(1, "tx_calls") == 4 && metric(1, "tx_bytes") == 13);
    CHECK(metric(1, "tx_wall_us") == 500062 && metric(1, "tx_max_us") == 500000);
    CHECK(metric(1, "short") == 1 && metric(1, "tx_err") == 1 && metric(1, "tx_cores") == 3);
    CHECK(metric(2, "tx_calls") == 1 && metric(2, "tx_bytes") == 20);
    CHECK(metric(2, "tx_wall_us") == 8 && metric(2, "tx_max_us") == 8);
}
static void test_read(void)
{
    char buffer[20];
    const int results[] = {7, -9, 0};
    const unsigned durations[] = {8, 1000, 4000};
    for (unsigned i = 0; i < 3; ++i) {
        memset(buffer, 'Z', sizeof(buffer));
        begin(READ, 1, results[i]);
        expected.data = buffer; expected.length = sizeof(buffer); expected.ticks = 73;
        expected.duration = durations[i]; expected.after_core = 1;
        CHECK(__wrap_uart_read_bytes(1, buffer, sizeof(buffer), 73) == results[i]);
        CHECK(expected.calls == 1 && log_count == 0);
        CHECK(buffer[0] == (results[i] > 0 ? 'R' : 'Z'));
    }
    report();
    CHECK(metric(1, "rx_calls") == 3 && metric(1, "rx_bytes") == 7);
    CHECK(metric(1, "rx_err") == 1 && metric(1, "rx_call_max_us") == 4000);
    CHECK(metric(1, "rx_cores") == 3 && metric(2, "rx_calls") == 0);
}
static void test_occupancy(void)
{
    size_t length = 0;
    const size_t samples[] = {0, 1024, 9999, 12, SIZE_MAX};
    for (unsigned i = 0; i < 5; ++i) {
        begin(OCCUPANCY, 1, i == 2 ? -3 : ESP_OK);
        expected.data = i == 3 ? NULL : &length; expected.occupancy = samples[i];
        CHECK(__wrap_uart_get_buffered_data_len(1, expected.data) == expected.result);
        CHECK(expected.calls == 1);
    }
    report();
    CHECK(metric(1, "rx_samples") == 3 && metric(1, "rx_sample_peak") == UINT32_MAX);
    CHECK(metric(2, "rx_samples") == 0);
}
static void receive(QueueHandle_t queue, int result, uart_event_type_t event, bool reinstall)
{
    uart_event_t item = {0};
    begin(RECEIVE, 0, result);
    expected.queue = queue; expected.data = &item; expected.ticks = 113;
    expected.event = event; expected.reinstall = reinstall;
    CHECK(__wrap_xQueueReceive(queue, &item, 113) == result && expected.calls == 1);
    if (result == pdTRUE) CHECK(item.type == event && item.size == 71);
}
static void test_events(void)
{
    install(1, &queue_tokens[0], ESP_OK);
    install(2, &queue_tokens[1], ESP_OK);
    for (unsigned event = UART_DATA; event <= UART_PARITY_ERR; ++event)
        receive(&queue_tokens[0], pdTRUE, (uart_event_type_t)event, false);
    receive(&queue_tokens[1], pdTRUE, UART_BUFFER_FULL, false);
    unsigned before = lock_calls;
    receive(&queue_tokens[2], pdTRUE, UART_FIFO_OVF, false);
    CHECK(lock_calls == before); /* Unrelated queues retain the lock-free fast path. */
    receive(&queue_tokens[0], pdFALSE, UART_FIFO_OVF, false);
    report();
    CHECK(metric(1, "consumed") == 6 && metric(2, "consumed") == 1);
    CHECK(metric(1, "fifo_ovf") == 1 && metric(1, "ring_full") == 1);
    CHECK(metric(1, "frame_err") == 1 && metric(1, "parity_err") == 1 && metric(1, "break") == 1);
    CHECK(metric(2, "ring_full") == 1 && metric(2, "fifo_ovf") == 0);
    CHECK(metric(1, "active") == 1 && metric(1, "queue") == 8);
    CHECK(metric(1, "rx_buf") == 2048 && metric(1, "tx_buf") == 0);
}
static void test_generation(void)
{
    install(1, &queue_tokens[0], ESP_OK);
    receive(&queue_tokens[0], pdTRUE, UART_FIFO_OVF, true);
    receive(&queue_tokens[0], pdTRUE, UART_FRAME_ERR, false);
    report();
    CHECK(metric(1, "install") == 2 && metric(1, "delete") == 1);
    CHECK(metric(1, "fifo_ovf") == 0 && metric(1, "frame_err") == 1);
    CHECK(metric(1, "consumed") == 1 && metric(1, "active") == 1);
}
static void test_lifecycle(void)
{
    install(1, &queue_tokens[0], ESP_OK);
    install(1, &queue_tokens[1], -4);
    remove_driver(1, -5);
    receive(&queue_tokens[0], pdTRUE, UART_FIFO_OVF, false);
    receive(&queue_tokens[1], pdTRUE, UART_FRAME_ERR, false);
    report();
    CHECK(metric(1, "install") == 1 && metric(1, "delete") == 0 && metric(1, "active") == 1);
    CHECK(metric(1, "consumed") == 1 && metric(1, "fifo_ovf") == 1);
    remove_driver(1, ESP_OK);
    receive(&queue_tokens[0], pdTRUE, UART_PARITY_ERR, false);
    report();
    CHECK(metric(1, "active") == 0 && metric(1, "delete") == 1 && metric(1, "consumed") == 0);
    install(1, NULL, ESP_OK);
    report();
    CHECK(metric(1, "active") == 1 && metric(1, "queue") == 0);
}
static void test_snapshot(void)
{
    install(1, &queue_tokens[0], ESP_OK);
    write_sample(1, 5, 5, 11, 0);
    inject_after_snapshot = true;
    report();
    CHECK(metric(1, "tx_calls") == 1 && metric(1, "tx_bytes") == 5 && metric(1, "tx_max_us") == 11);
    now_us += 30000000;
    report();
    CHECK(metric(1, "tx_calls") == 1 && metric(1, "tx_bytes") == 7 && metric(1, "tx_max_us") == 22);
    CHECK(metric(1, "install") == 0 && metric(1, "active") == 1 && metric(1, "queue") == 8);
    CHECK(metric(1, "window_ms") == 30000);
    report();
    CHECK(metric(1, "tx_calls") == 0 && metric(1, "tx_bytes") == 0 && metric(1, "tx_max_us") == 0);
}
static void *producer(void *argument)
{
    current_core = (int)(uintptr_t)argument;
    for (unsigned i = 0; i < 250; ++i) write_sample(1, 1, 1, 1, current_core);
    return NULL;
}
static void test_concurrent(void)
{
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_create(&threads[i], NULL, producer, (void *)(uintptr_t)(i % 2)) == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    report();
    CHECK(metric(1, "tx_calls") == 1000 && metric(1, "tx_bytes") == 1000);
    CHECK(metric(1, "tx_wall_us") == 1000 && metric(1, "tx_max_us") == 1 && metric(1, "tx_cores") == 3);
}
static void test_ppp(void)
{
    ppp_pcb connection = {17};
    u8_t bytes[100] = {0};
    const int lengths[] = {100, 20, -1};
    for (unsigned i = 0; i < 3; ++i) {
        begin(PPP, 0, i == 0 ? ERR_OK : -3);
        expected.ppp = &connection; expected.data = bytes; expected.ppp_length = lengths[i];
        expected.after_core = 1;
        CHECK(__wrap_pppos_input_tcpip_as_ram_pbuf(&connection, bytes, lengths[i]) == expected.result);
        CHECK(expected.calls == 1 && log_count == 0);
    }
    report();
    CHECK(metric(0, "ingress_calls") == 3 && metric(0, "offered_bytes") == 120);
    CHECK(metric(0, "ingress_fail") == 2 && metric(0, "cores") == 3);
    report();
    CHECK(metric(0, "ingress_calls") == 0 && metric(0, "offered_bytes") == 0);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    scenario = argv[1];
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"write", test_write}, {"read", test_read}, {"occupancy", test_occupancy},
        {"events", test_events}, {"generation", test_generation}, {"lifecycle", test_lifecycle},
        {"snapshot", test_snapshot}, {"concurrent", test_concurrent}, {"ppp", test_ppp}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(scenario, cases[i].name)) continue;
        cases[i].run();
        printf("modem_transport_diag %s: %u checks, %u failures\n", scenario,
               atomic_load(&checks), atomic_load(&failures));
        return atomic_load(&failures) ? 1 : 0;
    }
    return 2;
}
