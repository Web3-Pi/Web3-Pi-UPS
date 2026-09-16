#ifndef PERF_RECOVERY_H
#define PERF_RECOVERY_H

#include <stdbool.h>

/* Research bench only: the PPP owner consumes at most one pending request.
 * It then uses its existing normal DCE teardown/recreate path. */
bool perf_recovery_take_request(void);

/* Run two deliberate reconnect cycles after the duplex workload. No-op when
 * CONFIG_WUPS_PERF_RECOVERY_BENCH is off. Never controls PWRKEY from here. */
void perf_recovery_run(void);

#endif
