#include "transport_diag.h"
#include "sdkconfig.h"

#if CONFIG_WUPS_PERF_DIAG

#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "netif/ppp/pppos.h"

/* GNU ld wrappers. Keep the signatures identical to IDF 6.0.2. */
int __real_uart_write_bytes(uart_port_t port, const void *data, size_t length);
int __real_uart_read_bytes(uart_port_t port, void *data, uint32_t length,
                           uint32_t ticks_to_wait);
esp_err_t __real_uart_get_buffered_data_len(uart_port_t port, size_t *length);
esp_err_t __real_uart_driver_install(uart_port_t port, int rx_size, int tx_size,
                                    int queue_size, QueueHandle_t *queue, int flags);
esp_err_t __real_uart_driver_delete(uart_port_t port);
BaseType_t __real_xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
/* esp-netif's private helper; this exact signature is in netif/pppif.h.
 * Observing its return avoids replacing PPP's input callback. */
err_t __real_pppos_input_tcpip_as_ram_pbuf(ppp_pcb *ppp, u8_t *data, int length);

typedef struct {
    uint64_t tx_bytes, tx_wall_us, tx_max_us;
    uint64_t rx_bytes, rx_max_us;
    uint32_t tx_calls, tx_short, tx_errors, tx_cores;
    uint32_t rx_calls, rx_errors, rx_cores, rx_samples, rx_peak;
    uint32_t events, fifo_overflow, ring_full, frame_error, parity_error, breaks;
    uint32_t installs, deletes;
} uart_interval_t;

typedef struct {
    QueueHandle_t queue;
    uint32_t generation;
    int install_core, rx_size, tx_size, queue_size;
    bool active;
} uart_metadata_t;

typedef struct {
    uint64_t bytes;
    uint32_t calls, failures, cores;
} ppp_interval_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uart_interval_t s_uart[2];
static uart_metadata_t s_meta[2] = {{.install_core = -1}, {.install_core = -1}};
/* Queue handles are pointer-sized and lock-free on ESP32-S3. All stores
 * happen with s_lock held; most queues bypass that lock on the read path. */
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "queue observation needs lock-free pointers");
static _Atomic(QueueHandle_t) s_published_queue[2];
static ppp_interval_t s_ppp;
static int64_t s_last_report_us;

static int uart_index(uart_port_t port)
{
    if (port == UART_NUM_1) return 0;
    if (port == UART_NUM_2) return 1;
    return -1;
}

static uint32_t core_mask(void)
{
    unsigned core = (unsigned)xPortGetCoreID();
    return core < 32 ? (UINT32_C(1) << core) : 0;
}

static uint64_t elapsed_us(int64_t start)
{
    int64_t now = esp_timer_get_time();
    return now > start ? (uint64_t)(now - start) : 0;
}

int __wrap_uart_write_bytes(uart_port_t port, const void *data, size_t length)
{
    int index = uart_index(port);
    if (index < 0) return __real_uart_write_bytes(port, data, length);
    uint32_t cores = core_mask();
    int64_t start = esp_timer_get_time();
    int result = __real_uart_write_bytes(port, data, length);
    uint64_t duration = elapsed_us(start);
    cores |= core_mask();
    portENTER_CRITICAL(&s_lock);
    uart_interval_t *stats = &s_uart[index];
    ++stats->tx_calls;
    stats->tx_cores |= cores;
    stats->tx_wall_us += duration;
    if (duration > stats->tx_max_us) stats->tx_max_us = duration;
    if (result > 0) stats->tx_bytes += (unsigned)result;
    if (result < 0) ++stats->tx_errors;
    else if ((size_t)result < length) ++stats->tx_short;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

int __wrap_uart_read_bytes(uart_port_t port, void *data, uint32_t length,
                           uint32_t ticks_to_wait)
{
    int index = uart_index(port);
    if (index < 0) return __real_uart_read_bytes(port, data, length, ticks_to_wait);
    uint32_t cores = core_mask();
    int64_t start = esp_timer_get_time();
    int result = __real_uart_read_bytes(port, data, length, ticks_to_wait);
    uint64_t duration = elapsed_us(start);
    cores |= core_mask();
    portENTER_CRITICAL(&s_lock);
    uart_interval_t *stats = &s_uart[index];
    ++stats->rx_calls;
    stats->rx_cores |= cores;
    if (result > 0) stats->rx_bytes += (unsigned)result;
    if (result < 0) ++stats->rx_errors;
    if (duration > stats->rx_max_us) stats->rx_max_us = duration;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

esp_err_t __wrap_uart_get_buffered_data_len(uart_port_t port, size_t *length)
{
    esp_err_t result = __real_uart_get_buffered_data_len(port, length);
    int index = uart_index(port);
    if (index >= 0 && result == ESP_OK && length) {
        uint32_t sample = *length > UINT32_MAX ? UINT32_MAX : (uint32_t)*length;
        portENTER_CRITICAL(&s_lock);
        ++s_uart[index].rx_samples;
        if (sample > s_uart[index].rx_peak) s_uart[index].rx_peak = sample;
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

esp_err_t __wrap_uart_driver_install(uart_port_t port, int rx_size, int tx_size,
                                    int queue_size, QueueHandle_t *queue, int flags)
{
    esp_err_t result = __real_uart_driver_install(port, rx_size, tx_size,
                                                 queue_size, queue, flags);
    int index = uart_index(port);
    if (index >= 0 && result == ESP_OK) {
        int core = xPortGetCoreID();
        QueueHandle_t event_queue = queue_size > 0 && queue ? *queue : NULL;
        portENTER_CRITICAL(&s_lock);
        uart_metadata_t *meta = &s_meta[index];
        ++meta->generation;
        meta->queue = event_queue;
        meta->install_core = core;
        meta->rx_size = rx_size;
        meta->tx_size = tx_size;
        meta->queue_size = queue_size;
        meta->active = true;
        atomic_store_explicit(&s_published_queue[index], event_queue, memory_order_release);
        ++s_uart[index].installs;
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

esp_err_t __wrap_uart_driver_delete(uart_port_t port)
{
    esp_err_t result = __real_uart_driver_delete(port);
    int index = uart_index(port);
    if (index >= 0 && result == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++s_meta[index].generation;
        s_meta[index].queue = NULL;
        s_meta[index].active = false;
        atomic_store_explicit(&s_published_queue[index], NULL, memory_order_release);
        ++s_uart[index].deletes;
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

BaseType_t __wrap_xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    int index = -1;
    uint32_t generation = 0;
    if (queue) {
        if (queue == atomic_load_explicit(&s_published_queue[0], memory_order_acquire))
            index = 0;
        else if (queue == atomic_load_explicit(&s_published_queue[1], memory_order_acquire))
            index = 1;
    }
    if (index < 0) return __real_xQueueReceive(queue, item, ticks);
    /* Observe the existing consumer. Never receive, peek or reset separately.
     * Match before the real call: another queue can reuse a deleted handle. */
    portENTER_CRITICAL(&s_lock);
    if (queue == s_meta[index].queue) generation = s_meta[index].generation;
    else index = -1;
    portEXIT_CRITICAL(&s_lock);
    BaseType_t result = __real_xQueueReceive(queue, item, ticks);
    if (index >= 0 && result == pdTRUE && item) {
        portENTER_CRITICAL(&s_lock);
        if (s_meta[index].queue == queue && s_meta[index].generation == generation) {
            uart_interval_t *stats = &s_uart[index];
            uart_event_type_t type = ((const uart_event_t *)item)->type;
            ++stats->events;
            switch (type) {
            case UART_FIFO_OVF: ++stats->fifo_overflow; break;
            case UART_BUFFER_FULL: ++stats->ring_full; break;
            case UART_FRAME_ERR: ++stats->frame_error; break;
            case UART_PARITY_ERR: ++stats->parity_error; break;
            case UART_BREAK: ++stats->breaks; break;
            default: break;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

err_t __wrap_pppos_input_tcpip_as_ram_pbuf(ppp_pcb *ppp, u8_t *data, int length)
{
    uint32_t cores = core_mask();
    err_t result = __real_pppos_input_tcpip_as_ram_pbuf(ppp, data, length);
    cores |= core_mask();
    portENTER_CRITICAL(&s_lock);
    ++s_ppp.calls;
    if (length > 0) s_ppp.bytes += (unsigned)length;
    if (result != ERR_OK) ++s_ppp.failures;
    s_ppp.cores |= cores;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

void transport_diag_log(void)
{
    uart_interval_t stats[2];
    uart_metadata_t meta[2];
    ppp_interval_t ppp;
    int64_t now = esp_timer_get_time(), last;
    portENTER_CRITICAL(&s_lock);
    memcpy(stats, s_uart, sizeof(stats));
    memcpy(meta, s_meta, sizeof(meta));
    ppp = s_ppp;
    memset(s_uart, 0, sizeof(s_uart));
    memset(&s_ppp, 0, sizeof(s_ppp));
    last = s_last_report_us;
    s_last_report_us = now;
    portEXIT_CRITICAL(&s_lock);
    uint64_t window_ms = now > last ? (uint64_t)(now - last) / 1000 : 0;
    for (int i = 0; i < 2; ++i) {
        const uart_interval_t *s = &stats[i];
        ESP_LOGI("PERF_UART", "uart=%d window_ms=%" PRIu64 " active=%d install_return_core=%d "
                 "rx_buf=%d tx_buf=%d queue=%d install=%u delete=%u "
                 "tx_calls=%u tx_bytes=%" PRIu64 " tx_wall_us=%" PRIu64
                 " tx_max_us=%" PRIu64 " short=%u tx_err=%u tx_cores=0x%x",
                 i + 1, window_ms, meta[i].active, meta[i].install_core,
                 meta[i].rx_size, meta[i].tx_size, meta[i].queue_size,
                 (unsigned)s->installs, (unsigned)s->deletes,
                 (unsigned)s->tx_calls, s->tx_bytes, s->tx_wall_us, s->tx_max_us,
                 (unsigned)s->tx_short, (unsigned)s->tx_errors, (unsigned)s->tx_cores);
        ESP_LOGI("PERF_UART", "uart=%d rx_calls=%u rx_bytes=%" PRIu64
                 " rx_call_max_us=%" PRIu64 " rx_err=%u rx_cores=0x%x "
                 "rx_sample_peak=%u rx_samples=%u rx_peak_kind=%s events=%s consumed=%u "
                 "fifo_ovf=%u ring_full=%u frame_err=%u parity_err=%u break=%u",
                 i + 1, (unsigned)s->rx_calls, s->rx_bytes, s->rx_max_us,
                 (unsigned)s->rx_errors, (unsigned)s->rx_cores,
                 (unsigned)s->rx_peak, (unsigned)s->rx_samples,
                 s->rx_samples ? "sampled" : "unavailable",
                 meta[i].queue ? "observed" : "unavailable", (unsigned)s->events,
                 (unsigned)s->fifo_overflow, (unsigned)s->ring_full,
                 (unsigned)s->frame_error, (unsigned)s->parity_error, (unsigned)s->breaks);
    }
    ESP_LOGI("PERF_PPP", "window_ms=%" PRIu64 " ingress_calls=%u offered_bytes=%" PRIu64
             " ingress_fail=%u cores=0x%x (allocation/mailbox failures combined)",
             window_ms, (unsigned)ppp.calls, ppp.bytes, (unsigned)ppp.failures,
             (unsigned)ppp.cores);
}

#else

void transport_diag_log(void) {}

#endif
