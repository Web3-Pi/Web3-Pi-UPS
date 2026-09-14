#include "modem_diag_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "modem_clock";
static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_sync_seen;
static int64_t s_last_sync_us;

static bool plausible_time(const struct timeval *tv)
{
    /* Sanity floor only, never proof of synchronization. In particular a
     * retained or build-seeded wall clock still needs a callback this boot. */
    return tv && tv->tv_sec >= (time_t)1577836800 &&
           (int64_t)tv->tv_sec < INT64_C(253402300800) &&
           tv->tv_usec >= 0 && tv->tv_usec < 1000000;
}

void modem_diag_clock_sync(struct timeval *tv)
{
    if (!plausible_time(tv)) return;
    int64_t now = esp_timer_get_time();
    if (now < 0) return;
    portENTER_CRITICAL(&s_clock_lock);
    s_last_sync_us = now;
    s_sync_seen = true;
    portEXIT_CRITICAL(&s_clock_lock);
}

void modem_diag_clock_log(const char *event)
{
    /* Snapshot proof before reading wall time. A concurrent first callback
     * may conservatively leave this one anchor UNSYNCED, but cannot label a
     * previously sampled, unsynchronized wall time as an SNTP observation. */
    portENTER_CRITICAL(&s_clock_lock);
    bool synced = s_sync_seen;
    int64_t last_sync_us = s_last_sync_us;
    portEXIT_CRITICAL(&s_clock_lock);

    int64_t mono_us = esp_timer_get_time();
    int64_t mono_ms = mono_us >= 0 ? mono_us / 1000 : -1;
    const char *label = event ? event : "unspecified";
    if (!synced) {
        ESP_LOGI(TAG, "event=%s mono_ms=%" PRId64 " utc=UNSYNCED clock=unsynced",
                 label, mono_ms);
        return;
    }

    struct timeval tv;
    struct tm utc_tm;
    char seconds[24], utc[32];
    int64_t age_s = mono_us >= last_sync_us ? (mono_us - last_sync_us) / 1000000 : -1;
    bool available = mono_us >= 0 && age_s >= 0 &&
                     gettimeofday(&tv, NULL) == 0 && plausible_time(&tv) &&
                     gmtime_r(&tv.tv_sec, &utc_tm) != NULL &&
                     utc_tm.tm_year >= 120 && utc_tm.tm_year <= 8099;
    if (available) {
        size_t n = strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc_tm);
        available = n > 0;
        if (available) {
            int written = snprintf(utc, sizeof(utc), "%s.%03ldZ", seconds,
                                   (long)(tv.tv_usec / 1000));
            available = written > 0 && (size_t)written < sizeof(utc);
        }
    }
    if (!available) {
        ESP_LOGI(TAG, "event=%s mono_ms=%" PRId64 " utc=UNAVAILABLE clock=unavailable sync_age_s=%" PRId64,
                 label, mono_ms, age_s);
        return;
    }
    ESP_LOGI(TAG, "event=%s mono_ms=%" PRId64 " utc=%s clock=sntp sync_age_s=%" PRId64,
             label, mono_ms, utc, age_s);
}
