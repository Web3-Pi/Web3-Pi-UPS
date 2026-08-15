#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Track 2 / ADR-0011 — Arkiv command channel over WebSocket (eth_subscribe).
 *
 * Replaces the 5-second HTTP `arkiv_query` cmd-poll with a long-lived WSS
 * subscription to the self-hosted node (`wss://arkiv.web3pi.io/ws/<token>`,
 * see arkiv_ws.c / arkiv_ws_token.h.example). The node emits
 * `ArkivEntityCreated(...)` (and friends) on every storage write; we
 * subscribe to it with a server-side filter on
 *   address = ARKIV_STORAGE_CONTRACT,
 *   topic[0] = OR-list of event-signature topic0s,
 *   topic[2] = padded owner address.
 *
 * The notification carries only entityKey + owner + blockNumber — the full
 * entity (attributes, payload) is then fetched with a one-shot HTTPS
 * `arkiv_query(key = "...")` and handed to the existing
 * `cmdauth_arkiv_check` verifier. So this module is a pure REPLACEMENT for
 * the polling driver, not for the verifier or the WUPS frame forward.
 *
 * Expected LTE-M transfer reduction (web3pi_scope/notes/ARKIV-data-usage.md §E): the
 * cmd-channel drops from ~2.57 GB/mo (HTTP poll cold-handshake every 5 s)
 * to ~3 MB/mo (one WS session + ping/pong + a handful of events).
 *
 * Architecture notes:
 *  - Single active subscription per device (one owner = one stream).
 *  - The event-handler callback runs on the esp_websocket_client task; it
 *    must NOT block — it dispatches the entityKey to a worker that does
 *    the HTTPS fetch + verify (FreeRTOS queue, decoupled task).
 *  - esp_websocket_client auto-reconnects; on every reconnect we re-send
 *    the `eth_subscribe` so the subscription_id refreshes, and trigger a
 *    one-shot `arkiv_query(filter)` catch-up for missed events while the
 *    socket was down.
 *  - `arkiv_rpc.c`'s HTTP poll task stays as a belt-and-suspenders
 *    fallback at a 5-minute cadence — see ARKIV_POLL_INTERVAL_MS.
 */

/* Start the subscriber. Idempotent. The owner address is the on-chain
 * filter (topic[2]) — only events for this owner reach our callback.
 * Safe to call once cmdauth_arkiv is ARKIV_CLAIMED + network is up
 * (caller's responsibility — the WS client itself doesn't gate on PPP). */
esp_err_t arkiv_ws_start(const uint8_t owner_addr[20]);

/* Stop the subscriber, send eth_unsubscribe + close the WS. Idempotent. */
void arkiv_ws_stop(void);

/* True when the WS is currently connected AND a subscription_id has been
 * received from the server. Used by `arkiv_rpc.c` to thread the HTTP
 * fallback poll's cadence: when WS is healthy, fall back to the rare
 * (5 min) interval; when WS is down, keep the legacy 5 s cadence so a
 * prolonged outage doesn't strand a command. */
bool arkiv_ws_subscribed(void);

#ifdef __cplusplus
}
#endif
