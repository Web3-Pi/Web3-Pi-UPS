#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Track 2 / ADR-0011 P4 §4.6 / §12 — Paranoic event emitter.
 *
 * Class-level events (power_loss / battery_critical / mains_restored /
 * charge_full / fault, plus host equivalents) get their own Arkiv entity
 * (`w3pups-event`). The backend ingest is already wired in
 * apps/api/src/lib/arkiv/ingest.ts (ingestEvents) — it just reads the
 * `class` string attribute and writes to the `events` table.
 *
 * Unlike telemetry, events emit IMMEDIATELY on observation, not on a
 * cadence — they're rare and time-sensitive (a dead-man alert is useless
 * 30 s late). One tx per event.
 *
 * Observation is hooked from wups_link's net.publish handler whenever a
 * frame on the "event" subtopic passes through.
 */

/* Hook from wups_link: see a full WUPS frame on the way to MQTT "event".
 * Recognised power.event / host.event are translated to a class string
 * and submitted as a w3pups-event entity if we're in Paranoic mode. */
void arkiv_event_observe_frame(const uint8_t *frame, uint16_t frame_len);

#ifdef __cplusplus
}
#endif
