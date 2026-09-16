#include "perf_recovery.h"
#include "sdkconfig.h"

#if CONFIG_WUPS_PERF_RECOVERY_BENCH

#include <inttypes.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_ota.h"
#include "modem.h"
#include "mqtt.h"

#define RECOVERY_CYCLES 2u
#define RECOVERY_TIMEOUT_US INT64_C(240000000)
#define RECOVERY_STABLE_US INT64_C(30000000)
#define RECOVERY_POLL_MS 100u

enum { REQUEST_IDLE, REQUEST_PENDING, REQUEST_ACCEPTED };
static atomic_uint s_request;
static const char *TAG = "perf_recovery";

bool perf_recovery_take_request(void)
{
    unsigned expected = REQUEST_PENDING;
    return atomic_compare_exchange_strong_explicit(
        &s_request, &expected, REQUEST_ACCEPTED,
        memory_order_acq_rel, memory_order_acquire);
}

static void cancel_pending(void)
{
    unsigned expected = REQUEST_PENDING;
    /* Do not undo an already accepted operation in the PPP owner's task. */
    (void)atomic_compare_exchange_strong_explicit(
        &s_request, &expected, REQUEST_IDLE,
        memory_order_acq_rel, memory_order_acquire);
}

static bool healthy(void)
{
    return modem_ppp_is_up() && mqtt_publication_proof_fresh() &&
           !fw_ota_in_progress();
}

static bool wait_stable(void)
{
    int64_t deadline = esp_timer_get_time() + RECOVERY_TIMEOUT_US;
    int64_t since = -1;
    while (esp_timer_get_time() < deadline) {
        int64_t now = esp_timer_get_time();
        if (healthy()) {
            if (since < 0) since = now;
            if (now - since >= RECOVERY_STABLE_US) return true;
        } else {
            since = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(RECOVERY_POLL_MS));
    }
    return false;
}

void perf_recovery_run(void)
{
    ESP_LOGI(TAG, "start cycles=%u kind=owner_requested_reconnect "
             "stable_ms=30000 timeout_ms=240000", RECOVERY_CYCLES);
    for (unsigned cycle = 1; cycle <= RECOVERY_CYCLES; ++cycle) {
        if (!wait_stable()) {
            ESP_LOGE(TAG, "failed cycle=%u stage=healthy_baseline", cycle);
            return;
        }
        int64_t begin = esp_timer_get_time();
        int64_t deadline = begin + RECOVERY_TIMEOUT_US;
        bool accepted = false, down = false, recovered = false;
        atomic_store_explicit(&s_request, REQUEST_PENDING, memory_order_release);
        ESP_LOGI(TAG, "request cycle=%u wall_us=%" PRId64, cycle, begin);
        while (esp_timer_get_time() < deadline) {
            bool owner_accepted = atomic_load_explicit(
                &s_request, memory_order_acquire) == REQUEST_ACCEPTED;
            if (owner_accepted && !accepted) {
                accepted = true;
                ESP_LOGI(TAG, "owner_accepted cycle=%u elapsed_ms=%" PRId64,
                         cycle, (esp_timer_get_time() - begin) / 1000);
            }
            if (accepted && !modem_ppp_is_up() && !down) {
                down = true;
                ESP_LOGI(TAG, "down cycle=%u elapsed_ms=%" PRId64,
                         cycle, (esp_timer_get_time() - begin) / 1000);
            }
            if (down && healthy()) {
                recovered = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RECOVERY_POLL_MS));
        }
        if (!recovered) {
            cancel_pending();
            ESP_LOGE(TAG, "failed cycle=%u stage=reconnect accepted=%d down=%d",
                     cycle, accepted, down);
            return;
        }
        atomic_store_explicit(&s_request, REQUEST_IDLE, memory_order_release);
        ESP_LOGI(TAG, "complete cycle=%u elapsed_ms=%" PRId64
                 " observed_down=1 fresh_uplink=1 bench_stack_free=%u",
                 cycle, (esp_timer_get_time() - begin) / 1000,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (!wait_stable()) {
        ESP_LOGE(TAG, "failed stage=final_stability");
        return;
    }
    ESP_LOGI(TAG, "finished cycles=%u fresh_uplink=1", RECOVERY_CYCLES);
}

#else

bool perf_recovery_take_request(void)
{
    return false;
}

void perf_recovery_run(void)
{
}

#endif
