#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Track 2 / ADR-0011 — Arkiv JSON-RPC over LTE-M + the w3pups-cmd poll
 * task. READ side only: the device never writes commands (owner does, in
 * the browser; §4.2). On a verified command the bare WUPS frame is handed
 * to the RP2040 as a net.downlink event — byte-identical to the Track 1
 * MQTT path (Decision C).
 */

/* Seconds (esp_timer monotonic) of the last successful JSON-RPC round-trip
 * this boot; 0 = none yet. modem.c's uplink watchdog uses the freshness of
 * this stamp as the Arkiv-mode health signal — writes/polls prove the
 * uplink, the WS subscription is only a latency optimization. */
uint32_t arkiv_rpc_last_success_s(void);

/* eth_blockNumber → current chain height (the §4.4 clock). */
esp_err_t arkiv_eth_block_number(uint64_t *out_block);

/* eth_getTransactionCount(addr, "pending") → next nonce for `addr` (20 B).
 * "pending" so we don't collide with our own in-flight submissions. */
esp_err_t arkiv_eth_get_tx_count(const uint8_t addr[20], uint64_t *out_nonce);

/* eth_getBalance(addr, "latest") → balance of `addr` (20 B) in wei. The node
 * returns a 0x-hex quantity; this is the Braga native token (GLM, 18 decimals).
 * Balances under a few GLM fit comfortably in uint64 (max ~18.44 GLM in wei);
 * a value that overflows 64 bits returns ESP_ERR_INVALID_SIZE so the caller can
 * surface "high" rather than a wrapped figure. Used by the OLED wallet
 * "Balance" screen so the owner can confirm they funded the right address. */
esp_err_t arkiv_eth_get_balance(const uint8_t addr[20], uint64_t *out_wei);

/* eth_gasPrice → suggested wei. The writer applies its own floor on top. */
esp_err_t arkiv_eth_gas_price(uint64_t *out_wei);

/* eth_sendRawTransaction → submit a signed legacy tx. `raw` is the RLP
 * bytes produced by arkiv::tx::sign_legacy_tx. On HTTP 2xx the node's
 * "result" (a 0x-prefixed 32-byte tx hash) is copied into `out_hash` —
 * useful for traces; pass NULL/0 to skip. */
esp_err_t arkiv_eth_send_raw_tx(const uint8_t *raw, size_t raw_len,
                                char *out_hash, size_t out_hash_cap);

/* Run an arkiv_query with a caller-built filter expression (already in the
 * Arkiv filter syntax, with its quotes JSON-escaped as \"). Collects the
 * raw JSON-RPC response, NUL-terminated, into resp[0..resp_cap). Returns
 * ESP_OK only on HTTP 2xx with a non-empty body. The filter MUST be
 * injection-safe — only the digits-only ICCID + fixed type strings are
 * ever interpolated (identity.c validated the ICCID). Used by the P2-4b
 * w3pups-claim poll; the w3pups-cmd poll keeps its own verified path. */
esp_err_t arkiv_rpc_query(const char *filter, char *resp, size_t resp_cap);

/* Start the background poll task. Self-gating: it only queries when the
 * device is Arkiv-provisioned AND owner-bound (ARKIV_CLAIMED) AND has an
 * ICCID; otherwise it idles. Safe to call unconditionally at boot.
 *
 * Interval is dynamic: aggressive (ARKIV_POLL_INTERVAL_MS, 5 s) while the
 * WS subscriber is down, rare (ARKIV_POLL_INTERVAL_FALLBACK_MS, 5 min)
 * while WS is healthy. That keeps cmd latency snappy on WS outage and
 * collapses HTTP overhead to a belt-and-suspenders cadence in steady
 * state — see web3pi_scope/notes/ARKIV-data-usage.md §E for the LTE-M math. */
void arkiv_poll_start(void);

/* One-shot poll (same body the background task uses). Public so the WS
 * worker can run it as a catch-up sweep after each (re)connect, and as
 * the fallback path on observed entity events. Self-gating like poll_task. */
void arkiv_rpc_poll_once(void);

/* Fetch a specific entity by `entityKey` (0x-prefixed 32-byte hex), verify
 * and forward to RP2040 if it checks out. Used by the WS subscriber when
 * an ArkivEntityCreated notification arrives for our owner.
 *
 * v1: this currently delegates to `arkiv_rpc_poll_once` (the entityKey
 * acts as a wake-up signal; the regular poll finds the freshest unseen
 * cmd by seq, which is what we'd otherwise do anyway). A future
 * optimisation can switch to a real `key = "0x…"` filter to skip the
 * scan; right now the simpler path keeps surface area small. */
void arkiv_rpc_fetch_and_verify_by_key(const char *entity_key);
