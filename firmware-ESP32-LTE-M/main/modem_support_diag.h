#ifndef MODEM_SUPPORT_DIAG_H
#define MODEM_SUPPORT_DIAG_H

#include <stdbool.h>
#include "esp_modem_c_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_SUPPORT_DIAG_CAPACITY 8192u
#define MODEM_SUPPORT_DIAG_BATCH_MS 6000u
#define MODEM_SUPPORT_DIAG_COMMAND_MS 1500u

/* Read-only, best-effort 1NCE diagnostics. Call synchronously only from the
 * PPP supervisor; the static collector/cursor are not reentrant. ready() must
 * reject OTA, a lost PPP link, and active plain-DATA mode as appropriate for
 * the caller's phase. It is rechecked before every command. An already-issued
 * command can still wait for its bounded timeout.
 *
 * The deadline bounds new command waits, including elapsed logging time;
 * scheduler/SDK cleanup and final log output may add overhead. No immediate
 * retry occurs. A partial pass resumes at the next unattempted command on a
 * later call, and completing the list restores its normal starting order.
 * The function never changes modem settings, PPP or recovery state. */
void modem_support_diag_run(esp_modem_dce_t *dce,
                            bool (*ready)(void *), void *context);

#ifdef __cplusplus
}
#endif
#endif
