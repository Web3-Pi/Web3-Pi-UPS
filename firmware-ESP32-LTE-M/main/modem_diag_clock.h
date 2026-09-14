#ifndef MODEM_DIAG_CLOCK_H
#define MODEM_DIAG_CLOCK_H

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assign to esp_sntp_config_t.sync_cb before initializing SNTP. Only a valid
 * callback in this boot establishes an SNTP observation; init/wait results or
 * a plausible RTC date alone never do. Does not set the system clock. */
void modem_diag_clock_sync(struct timeval *tv);

/* Emit one local time anchor without networking, waiting, or clock mutation.
 * UTC is UTC (gmtime_r), never local timezone. sync_age_s describes the last
 * observed SNTP sample, not a claim that NTP is currently reachable. */
void modem_diag_clock_log(const char *event);

#ifdef __cplusplus
}
#endif
#endif
