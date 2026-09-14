#ifndef MODEM_RADIO_POLICY_H
#define MODEM_RADIO_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_RADIO_RESPONSE_CAPACITY 512u

/* The adapter supplies the complete, NUL-terminated AT response, including a
 * complete terminal OK line. It returns false on transport errors, timeouts or
 * truncation or embedded NUL bytes. Commands below omit CR: the adapter adds
 * the wire terminator.
 * Calls are synchronous, serialized by the caller, and never retried here. */
typedef bool (*modem_radio_at_fn)(void *context, const char *command,
                                char *response, size_t capacity,
                                unsigned timeout_ms);

typedef enum {
    MODEM_RADIO_RESPONSE_WAIT = 0,
    MODEM_RADIO_RESPONSE_OK,
    MODEM_RADIO_RESPONSE_ERROR
} modem_radio_response_status_t;

/* Classifies the complete cumulative response from esp_modem_command().
 * length excludes a trailing NUL. Embedded NUL/invalid controls, oversized
 * responses, terminal errors or conflicting terminal lines fail closed.
 * An incomplete terminal line remains WAIT. Unknown data/URCs are not OK.
 * Reparse the cumulative buffer; do not append it again on every callback. */
modem_radio_response_status_t modem_radio_response_status(const char *response,
                                                         size_t length);

/* Run after every DCE's AT sync. A matching readback preserves RF/search state.
 * Otherwise disable RF, apply LTE-only / CAT-M-only / exactly B3+B20, then
 * verify all settings. A changed configuration remains RF-off for APN setup.
 * Any failure attempts RF-off and returns false; it never enables RF. */
bool modem_radio_prepare(modem_radio_at_fn at, void *context);

/* Run after the existing attach-APN command, before registration/CMUX/DATA.
 * Reverify all three settings. CFUN=1 needs no write; only verified CFUN=4
 * may transition to CFUN=1, followed by readback. Any failure attempts RF-off.
 * The caller must not start PPP if either prepare or resume returns false. */
bool modem_radio_resume(modem_radio_at_fn at, void *context);

#ifdef __cplusplus
}
#endif
#endif
