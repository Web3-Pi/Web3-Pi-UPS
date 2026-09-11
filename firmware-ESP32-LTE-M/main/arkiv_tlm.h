#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Track 2 / ADR-0011 P4 §4.6 — Paranoic telemetry emitter.
 *
 * Cache + periodic emit of `w3pups-telemetry` entities. The ESP32 itself
 * doesn't generate power/host/net status frames (CH32X / RPi do) — it
 * observes them on their way to MQTT (the RP2040 issues `net.publish` REQs
 * for the "telemetry" subtopic) and snapshots the inner payload by class.
 *
 * The arkiv_tlm task wakes every ARKIV_TLM_PERIOD_MS, packs the currently-
 * fresh snapshots into a single TLV blob, signs and submits it as one
 * Arkiv entity. One tx per cadence keeps gas costs predictable.
 *
 * Payload format (TLV):
 *
 *   u8  version   = 1
 *   u8  flags     = 0  (reserved)
 *   { repeated until end }
 *     u8  cls       (WUPS_CLASS_*)
 *     u8  op        (WUPS_OP_*_STATUS)
 *     u16 len       LE
 *     u8  data[len] (inner status struct, no WUPS header/checksum)
 *
 * Backend ingest decodes by walking items and using the existing
 * decode{Power,Host,Net}StatusV1 helpers per (cls, op).
 */

#define ARKIV_TLM_PERIOD_MS  (30 * 1000)
#define ARKIV_TLM_MAX_INNER  64

/* Called by wups_link's net.publish handler whenever a "telemetry"
 * subtopic frame passes through. The caller passes the FULL WUPS frame
 * (header + inner + checksum) so we can classify by header; we copy only
 * the inner status struct (≤ ARKIV_TLM_MAX_INNER). Unrecognized classes
 * are ignored — defense in depth against unexpected payloads.
 *
 * Safe to call from any task; an internal mutex serializes cache writes
 * against the periodic emit. */
void arkiv_tlm_observe_frame(const uint8_t *frame, uint16_t frame_len);

/* Start the periodic emit task. Self-gating: idles until cmdauth_arkiv is
 * ARKIV_CLAIMED + the writer is ready + at least one cache slot is fresh.
 * Safe to call unconditionally at boot. */
void arkiv_tlm_start(void);

/* Device-wallet balance (issue #8) — read by the OLED "Balance" screen.
 * Refreshed on the emit task only, INDEPENDENT of telemetry: gated on the
 * device key + PPP, then on demand / every 30 s until the first read lands /
 * every ~4 min after (a failed read retries next tick). An UNCLAIMED or
 * unfunded unit (every submit fails) therefore still learns its balance.
 * Never an RPC on the caller's task — a blocking eth_getBalance on the 4 KB
 * wups_rx button task overflowed it. */
typedef enum {
    ARKIV_TLM_BAL_NONE = 0, /* no read completed yet this boot                */
    ARKIV_TLM_BAL_OK,       /* wei is current                                 */
    ARKIV_TLM_BAL_HIGH,     /* balance >= 2^64 wei (> 18.446744 W3P), wei unset */
    ARKIV_TLM_BAL_FAIL,     /* last read failed; last_good says what is known */
} arkiv_tlm_bal_state_t;

typedef struct {
    arkiv_tlm_bal_state_t state;      /* outcome of the last attempt                  */
    arkiv_tlm_bal_state_t last_good;  /* NONE, OK (wei valid) or HIGH; survives FAIL  */
    uint64_t wei;
    uint32_t age_s;                   /* seconds since the last successful read (0 = none) */
} arkiv_tlm_balance_t;

/* Non-blocking snapshot of the cached balance; returns true when a value is
 * known (out->last_good != NONE). Safe from any task; NONE before
 * arkiv_tlm_start() (non-Arkiv modes). */
bool arkiv_tlm_balance(arkiv_tlm_balance_t *out);

/* Ask the emit task for a fresh read (served within ~1 s while the device key
 * is present and PPP is up; otherwise stays pending). Flag only — safe from
 * any task, never blocks, never does RPC. */
void arkiv_tlm_request_balance_refresh(void);

#ifdef __cplusplus
}
#endif
