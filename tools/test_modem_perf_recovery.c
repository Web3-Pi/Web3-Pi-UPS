#include "perf_recovery.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
static const char *scenario;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d [%s]: %s\n", __func__, __LINE__, scenario, #condition); \
        ++failures; \
    } \
} while (0)
static int64_t now_us, request_us, accepted_us, first_request_us = -1;
static unsigned requests, accepted, completed, failed, finished, delays, logs;
static bool request_active, owner_has_request;

static bool is_case(const char *name) { return !strcmp(scenario, name); }
int64_t esp_timer_get_time(void) { return now_us; }
unsigned uxTaskGetStackHighWaterMark(void *task) { CHECK(task == NULL); return 2816; }
bool fw_ota_in_progress(void)
{
    return is_case("ota_baseline") || (is_case("ota_pending") && request_active) ||
           (is_case("final_unstable") && completed == 2);
}
bool modem_ppp_is_up(void)
{
    if (!request_active || !owner_has_request || is_case("no_down") ||
        (is_case("second_failed") && requests == 2)) return true;
    return now_us - accepted_us >= 700000;
}
bool mqtt_publication_proof_fresh(void)
{
    if (is_case("baseline_flap") && now_us >= 15000000 && now_us < 16000000) return false;
    if (!request_active || !owner_has_request) return true;
    if (is_case("no_proof")) return false;
    return now_us - accepted_us >= 1200000;
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(ticks == 100);
    now_us += (int64_t)ticks * 1000;
    if (++delays > 20000) {
        fprintf(stderr, "Recovery scenario failed to terminate: %s\n", scenario);
        exit(2);
    }
    if (request_active && !fw_ota_in_progress() && !is_case("no_owner")) {
        if (perf_recovery_take_request()) {
            CHECK(!owner_has_request);
            CHECK(now_us >= request_us);
            ++accepted;
            owner_has_request = true;
            accepted_us = now_us;
        }
        CHECK(!perf_recovery_take_request()); /* The owner consumes each request once. */
    }
}
void mock_log(const char *tag, const char *format, ...)
{
    CHECK(!strcmp(tag, "perf_recovery"));
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    CHECK(length >= 0 && (size_t)length < sizeof(line));
    ++logs;
    if (!strncmp(line, "request ", 8)) {
        CHECK(!request_active);
        ++requests;
        request_active = true;
        owner_has_request = false;
        request_us = now_us;
        if (first_request_us < 0) first_request_us = now_us;
    } else if (!strncmp(line, "complete ", 9)) {
        CHECK(owner_has_request && modem_ppp_is_up() && mqtt_publication_proof_fresh());
        CHECK(!fw_ota_in_progress());
        ++completed;
        request_active = false;
    } else if (!strncmp(line, "failed ", 7)) {
        ++failed;
        request_active = false;
    } else if (!strncmp(line, "finished ", 9)) {
        ++finished;
        CHECK(completed == 2 && modem_ppp_is_up() && mqtt_publication_proof_fresh());
        CHECK(!fw_ota_in_progress());
    }
}

static void run_checks(void)
{
    CHECK(!perf_recovery_take_request());
    perf_recovery_run();
    CHECK(!perf_recovery_take_request());
    unsigned final_requests = requests;
    now_us += INT64_C(600000000);
    CHECK(!perf_recovery_take_request() && requests == final_requests);
    if (is_case("healthy") || is_case("baseline_flap")) {
        CHECK(requests == 2 && accepted == 2 && completed == 2 && finished == 1 && failed == 0);
        CHECK(first_request_us >= (is_case("baseline_flap") ? 46000000 : 30000000));
        CHECK(now_us >= INT64_C(692400000)); /* Three stable windows plus both recoveries. */
    } else if (is_case("ota_baseline")) {
        CHECK(requests == 0 && accepted == 0 && completed == 0 && failed == 1 && finished == 0);
    } else if (is_case("no_owner") || is_case("ota_pending")) {
        CHECK(requests == 1 && accepted == 0 && completed == 0 && failed == 1 && finished == 0);
    } else if (is_case("second_failed")) {
        CHECK(requests == 2 && accepted == 2 && completed == 1 && failed == 1 && finished == 0);
    } else if (is_case("final_unstable")) {
        CHECK(requests == 2 && accepted == 2 && completed == 2 && failed == 1 && finished == 0);
    } else {
        CHECK(requests == 1 && accepted == 1 && completed == 0 && failed == 1 && finished == 0);
    }
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    scenario = argv[1];
    if (CONFIG_WUPS_PERF_RECOVERY_BENCH) run_checks();
    else {
        CHECK(!perf_recovery_take_request());
        perf_recovery_run();
        CHECK(!perf_recovery_take_request());
        CHECK(logs == 0 && delays == 0 && requests == 0 && now_us == 0);
    }
    printf("modem_perf_recovery %s: %u checks, %u failures\n", scenario, checks, failures);
    return failures ? 1 : 0;
}
