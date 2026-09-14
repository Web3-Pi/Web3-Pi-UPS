#include "modem_diag_clock.h"
#include "freertos/FreeRTOS.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_uint checks, failures;
#define CHECK(condition) do { \
    atomic_fetch_add(&checks, 1); \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        atomic_fetch_add(&failures, 1); \
    } \
} while (0)

static _Thread_local unsigned lock_depth;
static atomic_int_fast64_t monotonic_us = 10000000;
static struct timeval wall = {.tv_sec = 1767225600, .tv_usec = 123456};
static bool wall_failure, gmtime_failure, strftime_failure;
static atomic_uint wall_reads, log_count;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_log[384];

void diag_test_lock(portMUX_TYPE *lock)
{
    CHECK(lock_depth == 0);
    CHECK(pthread_mutex_lock(lock) == 0);
    lock_depth++;
}

void diag_test_unlock(portMUX_TYPE *lock)
{
    CHECK(lock_depth == 1);
    lock_depth--;
    CHECK(pthread_mutex_unlock(lock) == 0);
}

int64_t esp_timer_get_time(void)
{
    CHECK(lock_depth == 0);
    return atomic_load(&monotonic_us);
}

int diag_test_gettimeofday(struct timeval *tv, void *zone)
{
    CHECK(lock_depth == 0);
    CHECK(zone == NULL);
    atomic_fetch_add(&wall_reads, 1);
    if (wall_failure) return -1;
    *tv = wall;
    return 0;
}

struct tm *diag_test_gmtime_r(const time_t *value, struct tm *result)
{
    CHECK(lock_depth == 0);
    if (gmtime_failure) return NULL;
    return gmtime_r(value, result);
}

size_t diag_test_strftime(char *buffer, size_t size, const char *format, const struct tm *value)
{
    CHECK(lock_depth == 0);
    if (strftime_failure) return 0;
    return strftime(buffer, size, format, value);
}

void diag_test_log(const char *tag, const char *format, ...)
{
    CHECK(lock_depth == 0);
    CHECK(!strcmp(tag, "modem_clock"));
    char line[384];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    CHECK(n > 0 && (size_t)n < sizeof(line));
    CHECK(strstr(line, "mono_ms=") != NULL);
    CHECK(pthread_mutex_lock(&log_lock) == 0);
    memcpy(last_log, line, (size_t)n + 1);
    CHECK(pthread_mutex_unlock(&log_lock) == 0);
    atomic_fetch_add(&log_count, 1);
}

static void contains(const char *text)
{
    CHECK(strstr(last_log, text) != NULL);
}

static void lacks(const char *text)
{
    CHECK(strstr(last_log, text) == NULL);
}

static void establish(void)
{
    struct timeval sample = {.tv_sec = 1767225600, .tv_usec = 123456};
    modem_diag_clock_sync(&sample);
}

static void unsynced(void)
{
    wall.tv_sec = 0;
    modem_diag_clock_log("early AT");
    contains("event=early AT mono_ms=10000 utc=UNSYNCED clock=unsynced");
    lacks("sync_age_s=");
    wall.tv_sec = 1767225600; /* RTC/build-like plausible date is still not proof. */
    modem_diag_clock_log("AT+CNMP?");
    contains("utc=UNSYNCED");
    lacks("2026");
    CHECK(atomic_load(&wall_reads) == 0);
    modem_diag_clock_log(NULL);
    contains("event=unspecified");
}

static void late_sync(void)
{
    modem_diag_clock_log("before initial wait");
    contains("utc=UNSYNCED");
    atomic_store(&monotonic_us, 30000000); /* Initial 15 s wait already timed out. */
    modem_diag_clock_log("wait timed out");
    contains("utc=UNSYNCED");
    establish();
    CHECK(atomic_load(&wall_reads) == 0); /* Callback neither reads nor sets wall time. */
    atomic_store(&monotonic_us, 35000000);
    modem_diag_clock_log("late SNTP complete");
    contains("mono_ms=35000 utc=2026-01-01T00:00:00.123Z clock=sntp sync_age_s=5");
}

static void utc(void)
{
    CHECK(setenv("TZ", "HST10", 1) == 0);
    tzset();
    establish();
    modem_diag_clock_log("Hawaii timezone");
    contains("utc=2026-01-01T00:00:00.123Z");
    CHECK(setenv("TZ", "JST-9", 1) == 0);
    tzset();
    modem_diag_clock_log("Japan timezone");
    contains("utc=2026-01-01T00:00:00.123Z");
    wall.tv_usec = 999999;
    modem_diag_clock_log("milliseconds");
    contains("00:00:00.999Z");
}

static void invalid(void)
{
    struct timeval bad[] = {
        {.tv_sec = 0, .tv_usec = 0},
        {.tv_sec = -1, .tv_usec = 0},
        {.tv_sec = 1767225600, .tv_usec = -1},
        {.tv_sec = 1767225600, .tv_usec = 1000000},
        {.tv_sec = 253402300800, .tv_usec = 0},
    };
    modem_diag_clock_sync(NULL);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        modem_diag_clock_sync(&bad[i]);
    modem_diag_clock_log("invalid callbacks");
    contains("utc=UNSYNCED");
    establish();
    wall_failure = true;
    modem_diag_clock_log("gettimeofday failed");
    contains("utc=UNAVAILABLE clock=unavailable sync_age_s=0");
    wall_failure = false;
    gmtime_failure = true;
    modem_diag_clock_log("gmtime failed");
    contains("utc=UNAVAILABLE");
    gmtime_failure = false;
    strftime_failure = true;
    modem_diag_clock_log("strftime failed");
    contains("utc=UNAVAILABLE");
    strftime_failure = false;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        wall = bad[i];
        modem_diag_clock_log("invalid current wall time");
        contains("utc=UNAVAILABLE");
        lacks("clock=sntp");
    }
    wall = (struct timeval){.tv_sec = 1767225600, .tv_usec = 0};
    modem_diag_clock_log("wall time recovered");
    contains("utc=2026-01-01T00:00:00.000Z clock=sntp");
}

static void age(void)
{
    establish();
    atomic_store(&monotonic_us, 10000999);
    modem_diag_clock_log("initial");
    contains("mono_ms=10000");
    contains("sync_age_s=0");
    atomic_store(&monotonic_us, 3610000000);
    modem_diag_clock_log("PPP redial; no new SNTP");
    contains("clock=sntp sync_age_s=3600");
    establish();
    atomic_store(&monotonic_us, 3611500000);
    modem_diag_clock_log("reanchored");
    contains("sync_age_s=1");
    /* A 64-bit monotonic anchor remains valid beyond a 32-bit microsecond counter. */
    atomic_store(&monotonic_us, INT64_C(5000000000000));
    establish();
    atomic_store(&monotonic_us, INT64_C(5000010000000));
    modem_diag_clock_log("long uptime");
    contains("mono_ms=5000010000");
    contains("sync_age_s=10");
}

static void backward(void)
{
    establish();
    modem_diag_clock_log("before correction");
    contains("utc=2026-01-01T00:00:00.123Z");
    atomic_store(&monotonic_us, 30000000);
    wall.tv_sec -= 10;
    modem_diag_clock_log("wall stepped backwards");
    contains("mono_ms=30000 utc=2025-12-31T23:59:50.123Z clock=sntp sync_age_s=20");
    establish();
    modem_diag_clock_log("new SNTP anchor");
    contains("sync_age_s=0");
    atomic_store(&monotonic_us, 1000000); /* Impossible timer reset within one boot: fail closed. */
    modem_diag_clock_log("invalid monotonic relation");
    contains("utc=UNAVAILABLE clock=unavailable sync_age_s=-1");
}

static void *sync_thread(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 1000; ++i) establish();
    return NULL;
}

static void *log_thread(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 1000; ++i) modem_diag_clock_log("concurrent");
    return NULL;
}

static void concurrent(void)
{
    pthread_t sync, logger;
    CHECK(pthread_create(&sync, NULL, sync_thread, NULL) == 0);
    CHECK(pthread_create(&logger, NULL, log_thread, NULL) == 0);
    CHECK(pthread_join(sync, NULL) == 0);
    CHECK(pthread_join(logger, NULL) == 0);
    CHECK(atomic_load(&log_count) == 1000);
    modem_diag_clock_log("final");
    contains("clock=sntp sync_age_s=0");
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "unsynced")) unsynced();
    else if (!strcmp(argv[1], "late_sync")) late_sync();
    else if (!strcmp(argv[1], "utc")) utc();
    else if (!strcmp(argv[1], "invalid")) invalid();
    else if (!strcmp(argv[1], "age")) age();
    else if (!strcmp(argv[1], "backward")) backward();
    else if (!strcmp(argv[1], "concurrent")) concurrent();
    else return 2;
    printf("modem_diag_clock %s: %u checks, %u failures\n", argv[1],
           atomic_load(&checks), atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}
