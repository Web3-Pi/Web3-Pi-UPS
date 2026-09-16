#include "perf_bench.h"
#include "sdkconfig.h"

#if CONFIG_WUPS_PERF_BENCH

#if !CONFIG_WUPS_PERF_DIAG
#error "Synthetic duplex bench requires CONFIG_WUPS_PERF_DIAG"
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "perf_bench_ca.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "perf_diag.h"

#define BENCH_BYTES 524288u
#define BENCH_BUFFER 1024u
#define BENCH_RESPONSE_LIMIT 8192u
#define BENCH_CALL_TIMEOUT_MS 60000
#define BENCH_TRANSFER_US INT64_C(120000000)
#define BENCH_PROOF_WAIT_US INT64_C(300000000)
#define BENCH_BASELINE_US INT64_C(120000000)
#define BENCH_PROGRESS_US INT64_C(30000000)
#define BENCH_WORKER_STACK 6144u
#define BENCH_GO BIT0
#define BENCH_ABORT BIT1
#define BENCH_DOWNLOAD_DONE BIT2
#define BENCH_UPLOAD_DONE BIT3

static const char *TAG = "perf_bench";
static const char *DOWNLOAD_URL = "https://speed.cloudflare.com/__down?bytes=524288";
static const char *UPLOAD_URL = "https://speed.cloudflare.com/__up";

typedef struct {
    bool upload;
    EventGroupHandle_t gate;
    EventBits_t done_bit;
} worker_config_t;

static worker_config_t s_workers[2];
static bool s_started;

/* Deadlines are checked between SDK calls. The blocking HTTP implementation
 * can perform several socket reads inside one call, so this is deliberately
 * not described as hard asynchronous cancellation. No task/socket is killed
 * by another task. Timeout is shortened to the remaining budget each call. */
static bool set_budget(esp_http_client_handle_t client, int64_t deadline,
                       esp_err_t *error)
{
    int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) {
        *error = ESP_ERR_TIMEOUT;
        return false;
    }
    int timeout_ms = (int)((remaining + 999) / 1000);
    if (timeout_ms > BENCH_CALL_TIMEOUT_MS) timeout_ms = BENCH_CALL_TIMEOUT_MS;
    *error = esp_http_client_set_timeout_ms(client, timeout_ms);
    return *error == ESP_OK;
}

static bool within_deadline(int64_t deadline, esp_err_t *error)
{
    if (esp_timer_get_time() < deadline) return true;
    *error = ESP_ERR_TIMEOUT;
    return false;
}

static void progress(const char *role, uint32_t bytes, int64_t begin,
                     int64_t *last_progress)
{
    int64_t now = esp_timer_get_time();
    if (now - *last_progress < BENCH_PROGRESS_US) return;
    *last_progress = now;
    ESP_LOGI(TAG, "progress kind=synthetic_duplex role=%s bytes=%" PRIu32
             " elapsed_ms=%" PRId64, role, bytes, (now - begin) / 1000);
}

static void transfer(worker_config_t *worker)
{
    const char *role = worker->upload ? "upload" : "download";
    int64_t begin = esp_timer_get_time();
    int64_t deadline = begin + BENCH_TRANSFER_US;
    int64_t last_progress = begin;
    uint32_t bytes = 0, response_bytes = 0;
    int http_status = -1, last_io = 0;
    esp_err_t error = ESP_OK;
    const char *stage = "init";
    bool complete = false;
    char buffer[BENCH_BUFFER] = {0};
    esp_http_client_config_t config = {
        .url = worker->upload ? UPLOAD_URL : DOWNLOAD_URL,
        .method = worker->upload ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .cert_pem = PERF_BENCH_CA,
        .timeout_ms = BENCH_CALL_TIMEOUT_MS,
        .buffer_size = BENCH_BUFFER,
        .buffer_size_tx = BENCH_BUFFER,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .user_agent = "WUPS-synthetic-duplex-bench",
    };
    ESP_LOGI(TAG, "start kind=synthetic_duplex role=%s target_bytes=%u "
             "wall_us=%" PRId64 " budget_ms=120000 call_timeout_ms=60000 "
             "deadline=between_sdk_calls upload_payload=zeros",
             role, BENCH_BYTES, begin);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        error = ESP_ERR_NO_MEM;
        goto finished;
    }
    if (!within_deadline(deadline, &error)) goto cleanup;
    if (worker->upload) {
        error = esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        if (error != ESP_OK || !within_deadline(deadline, &error)) goto cleanup;
    }
    stage = "open";
    if (!set_budget(client, deadline, &error)) goto cleanup;
    error = esp_http_client_open(client, worker->upload ? (int)BENCH_BYTES : 0);
    if (!within_deadline(deadline, &error) || error != ESP_OK) goto cleanup;

    if (worker->upload) {
        stage = "write";
        while (bytes < BENCH_BYTES) {
            if (!set_budget(client, deadline, &error)) goto cleanup;
            unsigned length = BENCH_BYTES - bytes;
            if (length > sizeof(buffer)) length = sizeof(buffer);
            last_io = esp_http_client_write(client, buffer, (int)length);
            if (last_io > 0 && last_io <= (int)length) bytes += (uint32_t)last_io;
            if (!within_deadline(deadline, &error)) goto cleanup;
            if (last_io <= 0 || last_io > (int)length) {
                error = ESP_ERR_HTTP_WRITE_DATA;
                goto cleanup;
            }
            progress(role, bytes, begin, &last_progress);
        }
    }

    stage = "headers";
    if (!set_budget(client, deadline, &error)) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    http_status = esp_http_client_get_status_code(client);
    if (!within_deadline(deadline, &error)) goto cleanup;
    if (content_length < 0 || http_status < 200 || http_status >= 300) {
        error = ESP_FAIL;
        goto cleanup;
    }
    if (!worker->upload && content_length > 0 && content_length != BENCH_BYTES) {
        error = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    stage = "read";
    /* Drain cached body bytes too: fetch_headers may already have parsed the
     * complete response while application bytes still await a read. */
    for (;;) {
        if (!set_budget(client, deadline, &error)) goto cleanup;
        last_io = esp_http_client_read(client, buffer, sizeof(buffer));
        if (last_io > 0 && last_io <= (int)sizeof(buffer)) {
            response_bytes += (uint32_t)last_io;
            if (!worker->upload) bytes = response_bytes;
        }
        if (!within_deadline(deadline, &error)) goto cleanup;
        if (last_io < 0 || last_io > (int)sizeof(buffer) ||
            (last_io == 0 && !esp_http_client_is_complete_data_received(client))) {
            error = ESP_ERR_HTTP_FETCH_HEADER;
            goto cleanup;
        }
        if (response_bytes > (worker->upload ? BENCH_RESPONSE_LIMIT : BENCH_BYTES)) {
            error = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        progress(role, bytes, begin, &last_progress);
        if (last_io == 0) break;
    }
    complete = bytes == BENCH_BYTES && within_deadline(deadline, &error);
    if (!complete && error == ESP_OK) error = ESP_ERR_INVALID_SIZE;
    stage = complete ? "done" : "length";

cleanup:
    esp_http_client_cleanup(client);
finished:
    if (!within_deadline(deadline, &error)) complete = false;
    ESP_LOGI(TAG, "end kind=synthetic_duplex role=%s bytes=%" PRIu32
             " response_bytes=%" PRIu32 " http=%d elapsed_ms=%" PRId64
             " error=%s io_rc=%d complete=%d stage=%s worker_stack_free=%u",
             role, bytes, response_bytes, http_status,
             (esp_timer_get_time() - begin) / 1000,
             esp_err_to_name(error), last_io, complete, stage,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static uint64_t cpu_sample(perf_diag_cpu_sample_t *sample)
{
    uint64_t before = (uint64_t)esp_timer_get_time();
    sample->idle_us[0] = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(0);
    sample->idle_us[1] = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(1);
    uint64_t after = (uint64_t)esp_timer_get_time();
    sample->wall_us = before + (after - before) / 2;
    return after - before;
}

static void transfer_task(void *arg)
{
    worker_config_t *worker = arg;
    EventBits_t bits = xEventGroupWaitBits(worker->gate, BENCH_GO | BENCH_ABORT,
                                          pdFALSE, pdFALSE, portMAX_DELAY);
    if (!(bits & BENCH_ABORT)) transfer(worker);
    xEventGroupSetBits(worker->gate, worker->done_bit);
    vTaskDelete(NULL);
}

static bool baseline_ready(void)
{
    int64_t deadline = esp_timer_get_time() + BENCH_PROOF_WAIT_US;
    ESP_LOGI(TAG, "wait kind=synthetic_duplex proof_wait_ms=300000 baseline_ms=120000");
    while (!mqtt_publication_proof_fresh()) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "skip kind=synthetic_duplex reason=no_publication_proof");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    int64_t begin = esp_timer_get_time();
    ESP_LOGI(TAG, "baseline_start kind=synthetic_duplex wall_us=%" PRId64, begin);
    while (esp_timer_get_time() - begin < BENCH_BASELINE_US) {
        if (!mqtt_publication_proof_fresh()) {
            ESP_LOGW(TAG, "skip kind=synthetic_duplex reason=baseline_proof_lost");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return mqtt_publication_proof_fresh();
}

static void coordinator_task(void *arg)
{
    (void)arg;
    if (!baseline_ready()) goto finished;
    EventGroupHandle_t gate = xEventGroupCreate();
    if (!gate) {
        ESP_LOGE(TAG, "skip kind=synthetic_duplex reason=gate_allocation");
        goto finished;
    }
    EventBits_t created = 0;
    for (unsigned index = 0; index < 2; ++index) {
        s_workers[index] = (worker_config_t){
            .upload = index == 1, .gate = gate,
            .done_bit = index ? BENCH_UPLOAD_DONE : BENCH_DOWNLOAD_DONE,
        };
        if (xTaskCreate(transfer_task, index ? "bench_up" : "bench_down",
                        BENCH_WORKER_STACK, &s_workers[index], 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "skip kind=synthetic_duplex reason=worker_allocation index=%u", index);
            if (created) {
                xEventGroupSetBits(gate, BENCH_ABORT);
                xEventGroupWaitBits(gate, created, pdFALSE, pdTRUE, portMAX_DELAY);
            }
            /* Keep the one-shot gate alive until reset. A worker may still
             * be returning from xEventGroupSetBits after waking this task. */
            goto finished;
        }
        created |= s_workers[index].done_bit;
    }
    ESP_LOGI(TAG, "round_start kind=synthetic_duplex workers=2 priority=3 affinity=-1");
    perf_diag_cpu_sample_t before, after;
    uint64_t sample_us = cpu_sample(&before);
    xEventGroupSetBits(gate, BENCH_GO);
    xEventGroupWaitBits(gate, created, pdFALSE, pdTRUE, portMAX_DELAY);
    sample_us += cpu_sample(&after);
    /* See allocation-failure path: no deletion while workers return. */
    perf_diag_cpu_delta_t delta;
    perf_diag_cpu_delta(&before, &after,
                       sample_us + UINT64_C(2000000) / configTICK_RATE_HZ, &delta);
    ESP_LOGI(TAG, "round_end kind=synthetic_duplex repeats=0 wall_us=%" PRIu64
             " busy0_bp=%" PRId32 " busy1_bp=%" PRId32 " valid_mask=%" PRIu32,
             delta.interval_us,
             delta.idle_bp[0] < 0 ? -1 : 10000 - delta.idle_bp[0],
             delta.idle_bp[1] < 0 ? -1 : 10000 - delta.idle_bp[1], delta.valid_mask);
finished:
    vTaskDelete(NULL);
}

void perf_bench_start(void)
{
    if (s_started) return;
    s_started = true;
    if (xTaskCreate(coordinator_task, "bench_wait", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGE(TAG, "skip kind=synthetic_duplex reason=coordinator_allocation");
}

#else

void perf_bench_start(void)
{
}

#endif
