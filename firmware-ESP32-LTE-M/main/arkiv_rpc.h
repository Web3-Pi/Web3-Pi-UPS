#pragma once

#include <stdint.h>
#include "esp_err.h"

/*
 * Track 2 / ADR-0011 — Arkiv JSON-RPC over LTE-M + the w3pups-cmd poll
 * task. READ side only: the device never writes commands (owner does, in
 * the browser; §4.2). On a verified command the bare WUPS frame is handed
 * to the RP2040 as a net.downlink event — byte-identical to the Track 1
 * MQTT path (Decision C).
 */

/* eth_blockNumber → current Braga height (the §4.4 clock). */
esp_err_t arkiv_eth_block_number(uint64_t *out_block);

/* Start the background poll task. Self-gating: it only queries when the
 * device is Arkiv-provisioned AND owner-bound (ARKIV_CLAIMED) AND has an
 * ICCID; otherwise it idles. Safe to call unconditionally at boot. */
void arkiv_poll_start(void);
