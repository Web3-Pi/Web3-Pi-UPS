#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Track 2 / ADR-0011 P4 §4.6 — Paranoic ACK round-trip.
 *
 * The cmd RESP arrives from the RP2040 over wups_link with the same inner
 * WUPS SEQ that the owner-issued REQ carried (Decision C — the RP2040 is
 * MQTT-side-agnostic, the ESP32 is the sole point that knows the command
 * came from Arkiv, not MQTT). We snapshot (SEQ → command_id) at the
 * cmdauth_arkiv_check accept point; when the RESP comes back the wups_link
 * handler asks this module whether it should emit a w3pups-ack instead of
 * publishing on `t/{iccid}/cmd/response`.
 *
 * Tracker is tiny (small N) because Arkiv command latency is poll-bound
 * (≤5 s pickup) and the RP2040 RESPs within ~100 ms — so at any time we
 * have at most one or two in-flight commands. Stale slots fall out on TTL.
 */

#define ARKIV_ACK_TRACK_TTL_MS 60000

/* Remember (inner WUPS SEQ → command_id) so a later cmd/response RESP can
 * be turned into a `w3pups-ack` Arkiv entity. command_id is COPIED. */
void arkiv_ack_track_pending(uint8_t seq, const char *command_id);

/* True iff there is a non-expired pending entry for this SEQ (the wups_link
 * net.publish handler uses this to short-circuit the MQTT path in arkiv
 * mode without dropping ACKs for unknown SEQs). */
bool arkiv_ack_has_pending(uint8_t seq);

/* Emit a w3pups-ack entity for the given inner SEQ. `resp_payload` is the
 * cmd RESP frame's payload bytes (first byte = result code by convention,
 * 0 = success). Returns true iff an entity was submitted (clears the slot);
 * false on missing pending / writer not ready / submit failure (slot kept
 * so a retry/upper-layer fallback can still observe it). */
bool arkiv_ack_emit(uint8_t seq, const uint8_t *resp_payload, size_t resp_len);
