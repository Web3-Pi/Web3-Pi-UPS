#include "perf_diag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, scenarios;
static const char *scenario;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d [%s]: %s\n", __func__, __LINE__, scenario, #condition); \
        ++failures; \
    } \
} while (0)
#define COUNT(items) (sizeof(items) / sizeof((items)[0]))

static void begin(const char *name) { scenario = name; ++scenarios; }

static perf_diag_cpu_delta_t measure(const perf_diag_cpu_sample_t *previous,
                                     const perf_diag_cpu_sample_t *current,
                                     uint64_t tolerance_us)
{
    perf_diag_cpu_delta_t result;
    memset(&result, 0xa5, sizeof(result)); /* Every output must be initialized. */
    perf_diag_cpu_delta(previous, current, tolerance_us, &result);
    return result;
}

static void invalid(const perf_diag_cpu_delta_t *result)
{
    CHECK(result->valid_mask == 0);
    CHECK(result->clamped_mask == 0);
    CHECK(result->idle_bp[0] == -1 && result->idle_bp[1] == -1);
}

static void test_baseline_and_windows(void)
{
    perf_diag_cpu_sample_t previous = { UINT64_C(1000000000), { 750000000, 125000000 } };
    perf_diag_cpu_sample_t current = { UINT64_C(1030000000), { 772500000, 132500000 } };
    begin("missing sample is baseline, never zero CPU usage");
    perf_diag_cpu_delta_t result = measure(NULL, &current, 1000);
    invalid(&result);
    result = measure(&previous, NULL, 1000);
    invalid(&result);
    result = measure(NULL, NULL, 1000);
    invalid(&result);

    static const uint64_t invalid_intervals[] = { 0, 1, 999999, 120000001, 5000000000 };
    for (size_t i = 0; i < COUNT(invalid_intervals); ++i) {
        begin("zero, short or stale sampling window");
        current.wall_us = previous.wall_us + invalid_intervals[i];
        result = measure(&previous, &current, 1000);
        invalid(&result);
    }
    begin("wall clock regression does not unsigned-wrap");
    current.wall_us = previous.wall_us - 1;
    result = measure(&previous, &current, 1000);
    invalid(&result);

    static const uint64_t valid_intervals[] = { 1000000, 30000000, 120000000 };
    for (size_t i = 0; i < COUNT(valid_intervals); ++i) {
        begin("inclusive window boundaries and fully busy core");
        current = previous;
        current.wall_us += valid_intervals[i];
        current.idle_us[1] += valid_intervals[i];
        result = measure(&previous, &current, 0);
        CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
        CHECK(result.interval_us == valid_intervals[i]);
        CHECK(result.idle_bp[0] == 0 && result.idle_bp[1] == 10000);
        CHECK(result.idle_delta_us[0] == 0 && result.idle_delta_us[1] == valid_intervals[i]);
    }
}

static void test_per_core_deltas(void)
{
    begin("separate cores use wall time, not both-core total");
    perf_diag_cpu_sample_t previous = { UINT64_C(1000000000), { 900000000, 100000000 } };
    perf_diag_cpu_sample_t current = { UINT64_C(1030000000), { 922500000, 107500000 } };
    perf_diag_cpu_delta_t result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.interval_us == 30000000);
    CHECK(result.idle_delta_us[0] == 22500000 && result.idle_delta_us[1] == 7500000);
    CHECK(result.idle_bp[0] == 7500 && result.idle_bp[1] == 2500);

    begin("next report reflects its own interval, not boot-time averages");
    previous = current;
    current.wall_us += 30000000;
    current.idle_us[0] += 3000000;
    current.idle_us[1] += 27000000;
    result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.idle_bp[0] == 1000 && result.idle_bp[1] == 9000);

    begin("sub-basis-point fractions round down");
    previous = (perf_diag_cpu_sample_t){ 10000000, { 0, 0 } };
    current = (perf_diag_cpu_sample_t){ 11000000, { 333333, 666667 } };
    result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.idle_bp[0] == 3333 && result.idle_bp[1] == 6666);
}

static void test_large_counters(void)
{
    begin("U32 boundary crossing remains a small valid delta");
    perf_diag_cpu_sample_t previous = {
        UINT64_C(8000000000), { UINT64_C(4290000000), UINT64_C(4280000000) }
    };
    perf_diag_cpu_sample_t current = {
        UINT64_C(8030000000), { UINT64_C(4305000000), UINT64_C(4310000000) }
    };
    perf_diag_cpu_delta_t result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.idle_bp[0] == 5000 && result.idle_bp[1] == 10000);

    begin("adding large counter origins cannot change interval utilization");
    previous.wall_us += UINT64_C(1) << 48;
    current.wall_us += UINT64_C(1) << 48;
    previous.idle_us[0] += UINT64_C(1) << 45;
    current.idle_us[0] += UINT64_C(1) << 45;
    previous.idle_us[1] += UINT64_C(1) << 46;
    current.idle_us[1] += UINT64_C(1) << 46;
    result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.idle_bp[0] == 5000 && result.idle_bp[1] == 10000);

    begin("near U64 maximum, subtraction precedes scaling");
    previous = (perf_diag_cpu_sample_t){ UINT64_MAX - 30000000,
        { UINT64_MAX - 15000000, UINT64_MAX - 30000000 } };
    current = (perf_diag_cpu_sample_t){ UINT64_MAX, { UINT64_MAX, UINT64_MAX } };
    result = measure(&previous, &current, 0);
    CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
    CHECK(result.idle_bp[0] == 5000 && result.idle_bp[1] == 10000);
}

static void test_bad_runtime_samples(void)
{
    for (unsigned core = 0; core < 2; ++core) {
        unsigned other = 1u - core;
        begin("runtime regression invalidates only the affected core");
        perf_diag_cpu_sample_t previous = { 10000000, { 4000000, 4000000 } };
        perf_diag_cpu_sample_t current = { 11000000, { 4500000, 4500000 } };
        current.idle_us[core] = previous.idle_us[core] - 1;
        perf_diag_cpu_delta_t result = measure(&previous, &current, 0);
        CHECK(result.valid_mask == (1u << other) && result.clamped_mask == 0);
        CHECK(result.idle_bp[core] == -1 && result.idle_bp[other] == 5000);

        begin("counter reset can recover on the following sample");
        previous = current;
        current.wall_us += 1000000;
        current.idle_us[0] += 1000000;
        current.idle_us[1] += 1000000;
        result = measure(&previous, &current, 0);
        CHECK(result.valid_mask == 3 && result.clamped_mask == 0);
        CHECK(result.idle_bp[0] == 10000 && result.idle_bp[1] == 10000);

        begin("small idle overshoot clamps visibly at inclusive tolerance");
        previous = (perf_diag_cpu_sample_t){ 10000000, { 0, 0 } };
        current = (perf_diag_cpu_sample_t){ 11000000, { 500000, 500000 } };
        current.idle_us[core] = 1000100;
        result = measure(&previous, &current, 100);
        CHECK(result.valid_mask == 3 && result.clamped_mask == (1u << core));
        CHECK(result.idle_bp[core] == 10000 && result.idle_bp[other] == 5000);

        begin("overshoot beyond tolerance is invalid rather than plausible idle");
        current.idle_us[core]++;
        result = measure(&previous, &current, 100);
        CHECK(result.valid_mask == (1u << other) && result.clamped_mask == 0);
        CHECK(result.idle_bp[core] == -1 && result.idle_bp[other] == 5000);

        begin("huge runtime jump cannot overflow into a valid percentage");
        current.idle_us[core] = UINT64_MAX;
        result = measure(&previous, &current, 100);
        CHECK(result.valid_mask == (1u << other) && result.clamped_mask == 0);
        CHECK(result.idle_bp[core] == -1 && result.idle_bp[other] == 5000);
    }
}

int main(void)
{
    test_baseline_and_windows();
    test_per_core_deltas();
    test_large_counters();
    test_bad_runtime_samples();
    printf("modem_perf_diag: %u scenarios, %u checks, %u failures\n", scenarios, checks, failures);
    return failures ? 1 : 0;
}
