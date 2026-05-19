#pragma once

/*
 * Track 2 / ADR-0011 — Paranoic owner-binding driver (plan §10.4 path B,
 * Arkiv-from-scratch, the first-class path; §10.1 trust-anchor gate;
 * §10.2 state machine).
 *
 * Self-gating background task. While the device is Arkiv-provisioned and
 * UNCLAIMED it:
 *
 *   1. derives a per-boot claim-code (§10.4, front-running defence) and
 *      pushes it to the RP2040 OLED (display-only ui.trust_prompt);
 *   2. polls Arkiv for a `w3pups-claim` entity for its own ICCID;
 *   3. on a candidate, verifies device_id + claim-code + the owner
 *      signature over the §10.4 binding digest + on-chain writer;
 *   4. shows the owner fingerprint (§10.1) on the OLED and waits for the
 *      physical 2-button confirm (ui.trust_result);
 *   5. only on a confirmed result calls cmdauth_arkiv_bind_owner() →
 *      ARKIV_CLAIMED. The ESP32 is the sole authority; the RP2040 only
 *      renders text and reports the button hold (Decision C exception is
 *      scoped to that, owner-approved).
 *
 * Path A (MQTT→Arkiv wake) is a separate later slice; the claim-code
 * expiry/rotation is time-based here (button-triggered regen is P2-4c).
 */

/* Start the owner-binding task. Self-gates on
 * cmdauth_arkiv_ready() && claim_state == ARKIV_UNCLAIMED && ICCID known;
 * harmless to call unconditionally at boot. Idempotent. */
void arkiv_claim_start(void);
