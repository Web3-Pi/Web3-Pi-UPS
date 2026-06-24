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

/* Latest device-wallet balance (wei) cached by the emit task. Returns false
 * until the first successful refresh. Non-blocking, no RPC — the OLED "Balance"
 * screen reads this instead of doing a heavy eth_getBalance on the small
 * wups_rx button-event task (which overflowed its stack). */
bool arkiv_tlm_cached_balance_wei(uint64_t *out_wei);

#ifdef __cplusplus
}
#endif
