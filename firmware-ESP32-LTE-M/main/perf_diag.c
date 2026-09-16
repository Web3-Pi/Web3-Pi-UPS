#include "perf_diag.h"

#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

void perf_diag_cpu_delta(const perf_diag_cpu_sample_t *previous,
                         const perf_diag_cpu_sample_t *current,
                         uint64_t tolerance_us,
                         perf_diag_cpu_delta_t *result)
{
    if (!result) return;
    *result = (perf_diag_cpu_delta_t){.idle_bp = {-1, -1}};
    if (!previous || !current || current->wall_us <= previous->wall_us) return;
    uint64_t elapsed = current->wall_us - previous->wall_us;
    result->interval_us = elapsed;
    if (elapsed < PERF_DIAG_MIN_INTERVAL_US || elapsed > PERF_DIAG_MAX_INTERVAL_US)
        return;
    for (unsigned core = 0; core < 2; ++core) {
        if (current->idle_us[core] < previous->idle_us[core]) continue;
        uint64_t idle = current->idle_us[core] - previous->idle_us[core];
        result->idle_delta_us[core] = idle;
        if (idle > elapsed) {
            if (idle - elapsed > tolerance_us) continue;
            result->clamped_mask |= 1u << core;
            idle = elapsed;
        }
        /* elapsed <= 120 s, hence this multiplication cannot overflow U64. */
        result->idle_bp[core] = (int32_t)((idle * UINT64_C(10000)) / elapsed);
        result->valid_mask |= 1u << core;
    }
}

#if CONFIG_WUPS_PERF_DIAG

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || \
    !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER || \
    !CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64
#error "perf_diag requires FreeRTOS runtime stats with ESP_TIMER and U64 counters"
#endif
#if CONFIG_FREERTOS_SMP || CONFIG_FREERTOS_NUMBER_OF_CORES != 2
#error "perf_diag is reviewed for ESP-IDF FreeRTOS dual-core, not experimental SMP"
#endif

#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "transport_diag.h"

_Static_assert(sizeof(configRUN_TIME_COUNTER_TYPE) == sizeof(uint64_t),
               "runtime counters must not wrap after 71 minutes");

#define PERF_DIAG_STACK_BYTES 4096u
#define PERF_DIAG_PRIORITY 1u
#define PERF_DIAG_HEAP_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static const char *TAG = "perf_diag";
static TaskHandle_t s_task;

/* The IDF 6.0.2 getter holds the kernel lock while reading each U64 counter.
 * It includes completed task slices, not the currently running idle slice.
 * Sampling the two cores sequentially also gives a small boundary skew.
 * These are scheduler-accounted idle estimates, not separate ISR timings. */
static uint64_t sample_cpu(perf_diag_cpu_sample_t *sample)
{
    int64_t begin = esp_timer_get_time();
    sample->idle_us[0] = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(0);
    sample->idle_us[1] = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(1);
    int64_t end = esp_timer_get_time();
    if (begin < 0 || end < begin) {
        sample->wall_us = 0;
        return 0;
    }
    uint64_t span = (uint64_t)(end - begin);
    sample->wall_us = (uint64_t)begin + span / 2;
    return span;
}

static int affinity(TaskHandle_t task)
{
    BaseType_t core = xTaskGetCoreID(task);
    return core == tskNO_AFFINITY ? -1 : (int)core;
}

static uint64_t elapsed_us(int64_t begin)
{
    int64_t end = esp_timer_get_time();
    return begin >= 0 && end >= begin ? (uint64_t)(end - begin) : 0;
}

static void diagnostic_task(void *arg)
{
    (void)arg;
    TaskStatus_t own;
    /* Do not request stack scanning in vTaskGetInfo; scan our own small stack
     * separately once per reporting interval. No full task-list snapshots. */
    vTaskGetInfo(NULL, &own, pdFALSE, eInvalid);
    ESP_LOGI(TAG, "start period_ms=%u counter=esp_timer_us_u64 percent_scale=10000 "
             "cores=2 affinity=%d priority=%u idle0_affinity=%d idle1_affinity=%d "
             "stack_bytes=%u runtime_includes_isr=1",
             PERF_DIAG_PERIOD_MS, affinity(NULL), (unsigned)own.uxCurrentPriority,
             affinity(xTaskGetIdleTaskHandleForCore(0)),
             affinity(xTaskGetIdleTaskHandleForCore(1)), PERF_DIAG_STACK_BYTES);

    perf_diag_cpu_sample_t previous;
    uint64_t previous_span = sample_cpu(&previous);
    vTaskGetInfo(NULL, &own, pdFALSE, eInvalid);
    uint64_t previous_own_runtime = (uint64_t)own.ulRunTimeCounter;
    uint64_t previous_cycle_us = 0;
    uint64_t maximum_cycle_us = 0;
    uint32_t sequence = 0;

    for (;;) {
        /* Delay relative to completion, avoiding bursts of catch-up logs if
         * USB output blocks. Percentages use measured wall time, not 30 s. */
        vTaskDelay(pdMS_TO_TICKS(PERF_DIAG_PERIOD_MS));
        int64_t cycle_begin = esp_timer_get_time();
        perf_diag_cpu_sample_t current;
        uint64_t sample_span = sample_cpu(&current);
        perf_diag_cpu_delta_t delta;
        uint64_t tolerance = UINT64_C(2) * portTICK_PERIOD_MS * 1000 +
                             previous_span + sample_span;
        perf_diag_cpu_delta(&previous, &current, tolerance, &delta);

        multi_heap_info_t heap;
        heap_caps_get_info(&heap, PERF_DIAG_HEAP_CAPS);
        unsigned stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        vTaskGetInfo(NULL, &own, pdFALSE, eInvalid);
        uint64_t own_runtime = (uint64_t)own.ulRunTimeCounter;
        bool own_valid = own_runtime >= previous_own_runtime;
        uint64_t own_delta = own_valid ? own_runtime - previous_own_runtime : 0;
        uint64_t sample_us = elapsed_us(cycle_begin);
        int32_t busy0 = delta.idle_bp[0] < 0 ? -1 : 10000 - delta.idle_bp[0];
        int32_t busy1 = delta.idle_bp[1] < 0 ? -1 : 10000 - delta.idle_bp[1];

        ESP_LOGI(TAG, "cpu seq=%" PRIu32 " wall_us=%" PRIu64 " win_us=%" PRIu64
                 " valid_mask=%" PRIu32 " clamped_mask=%" PRIu32
                 " idle0_bp=%" PRId32 " busy0_bp=%" PRId32
                 " idle1_bp=%" PRId32 " busy1_bp=%" PRId32
                 " idle0_us=%" PRIu64 " idle1_us=%" PRIu64 " tolerance_us=%" PRIu64,
                 sequence, current.wall_us, delta.interval_us,
                 delta.valid_mask, delta.clamped_mask,
                 delta.idle_bp[0], busy0, delta.idle_bp[1], busy1,
                 delta.idle_delta_us[0], delta.idle_delta_us[1], tolerance);
        ESP_LOGI(TAG, "memory seq=%" PRIu32 " internal8_free=%u internal8_min=%u "
                 "internal8_largest=%u diag_stack_free=%u sample_us=%" PRIu64
                 " sample_span_us=%" PRIu64 " previous_cycle_us=%" PRIu64
                 " max_previous_cycle_us=%" PRIu64 " diag_runtime_us=%" PRIu64
                 " diag_runtime_valid=%d",
                 sequence, (unsigned)heap.total_free_bytes,
                 (unsigned)heap.minimum_free_bytes, (unsigned)heap.largest_free_block,
                 stack_free, sample_us, sample_span, previous_cycle_us,
                 maximum_cycle_us, own_delta, own_valid);
        transport_diag_log();

        previous_cycle_us = elapsed_us(cycle_begin);
        if (previous_cycle_us > maximum_cycle_us) maximum_cycle_us = previous_cycle_us;
        /* Rebase even after a bad window/counter read so one anomaly does not
         * poison every later sample. Raw window and validity remain in logs. */
        previous = current;
        previous_span = sample_span;
        previous_own_runtime = own_runtime;
        ++sequence;
    }
}

void perf_diag_start(void)
{
    if (s_task) return;
    if (xTaskCreate(diagnostic_task, "perf_diag", PERF_DIAG_STACK_BYTES, NULL,
                    PERF_DIAG_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "start_failed reason=task_allocation");
    }
}

#else

void perf_diag_start(void)
{
}

#endif
