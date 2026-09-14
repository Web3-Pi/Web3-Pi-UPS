#ifndef MODEM_AWAKE_POLICY_H
#define MODEM_AWAKE_POLICY_H

#include "modem_radio_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enforce CSCLK=0, CPSMS=0 and CAT-M eDRX disabled before starting PPP.
 * Calls are synchronous and serialized by the caller. A matching initial
 * readback preserves RF and issues no setters. PSM/eDRX changes first disable
 * and verify RF, then use only documented AUTO_SAVE setters. CSCLK-only
 * changes do not cycle RF. Every failure attempts CFUN=4 and returns false.
 * This module never enables RF, restarts the modem, or changes NB-IoT/PSM
 * optimization masks. The caller performs its existing APN/radio-resume flow.
 *
 * SIMCom does not document cross-family CEDRXS -> CEDRX readback mirroring.
 * A modem which does not report CAT-M enabled=0 after CEDRXS=0 is rejected,
 * not assumed to have applied the requested setting. No CEDRX setter is used:
 * that separate interface explicitly requires a module restart. */
bool modem_awake_prepare(modem_radio_at_fn at, void *context);

/* Pure reads; never mutate RF/settings. Require complete, unique valid
 * readbacks, including the terminal OK, and all three disabled settings. */
bool modem_awake_verify_config(modem_radio_at_fn at, void *context);

/* Pure reads after registration: CPSMRDP mode=0 and CEDRXRDP AcT-type=0.
 * Optional timer fields may be absent/nonzero. Unknown/unsupported/timeout
 * is false, not disabled. The caller may retry these reads after settling,
 * without further setters, resets or RF cycling. Success is required before
 * entering CMUX/DATA/PPP; no sleeps or retries are hidden in this module. */
bool modem_awake_verify_active(modem_radio_at_fn at, void *context);

#ifdef __cplusplus
}
#endif
#endif
