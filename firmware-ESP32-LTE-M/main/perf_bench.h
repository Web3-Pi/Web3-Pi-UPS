#ifndef PERF_BENCH_H
#define PERF_BENCH_H

/* Research-only, one synthetic HTTPS duplex round per boot. Call once from
 * app_main after normal workers start. No-op with CONFIG_WUPS_PERF_BENCH off.
 * Uploads only generated zero bytes; never device telemetry or credentials. */
void perf_bench_start(void);

#endif
