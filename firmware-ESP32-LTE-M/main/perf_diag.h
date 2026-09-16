#ifndef PERF_DIAG_H
#define PERF_DIAG_H

#include <stdint.h>

#define PERF_DIAG_PERIOD_MS 30000u
#define PERF_DIAG_MIN_INTERVAL_US UINT64_C(1000000)
#define PERF_DIAG_MAX_INTERVAL_US UINT64_C(120000000)

typedef struct {
    uint64_t wall_us;
    uint64_t idle_us[2];
} perf_diag_cpu_sample_t;

typedef struct {
    uint64_t interval_us;
    uint64_t idle_delta_us[2];
    /* Basis points: 10000 = 100%, -1 = unavailable. Each core has its own
     * wall-time denominator; never divide by the number of cores. */
    int32_t idle_bp[2];
    uint32_t valid_mask;
    uint32_t clamped_mask;
} perf_diag_cpu_delta_t;

/* Pure, allocation-free interval calculation, also available in host tests.
 * NULL previous means the first baseline. Bad wall windows invalidate both
 * cores; a regressing/implausible idle counter invalidates only that core.
 * Tiny idle overshoots within tolerance_us are clamped and explicitly marked.
 * Every rejected interval must still become the next caller baseline. */
void perf_diag_cpu_delta(const perf_diag_cpu_sample_t *previous,
                         const perf_diag_cpu_sample_t *current,
                         uint64_t tolerance_us,
                         perf_diag_cpu_delta_t *result);

/* Call once from app_main after normal workers have been started. Repeated
 * calls from that same startup task are harmless. No-op when disabled. */
void perf_diag_start(void);

#endif
