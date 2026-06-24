#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/*
 * Track 2 / ADR-0011 — Paranoic (Arkiv) command authority.
 *
 * Parallel to cmdauth.c (WS-9) but the authority is the OWNER's wallet, not
 * a pinned backend key (plan §4.2/§4.3):
 *
 *   - device keypair: 32-byte secp256k1 private key in read-only `prov`
 *     (key `ak_dev_priv`, namespace `w3pups`, same partition as Track 0/1).
 *     The device's Ethereum address is derived from it; used later as the
 *     per-device wallet for telemetry/ACK (P4).
 *   - owner binding: the registered owner address is NOT provisioned — it
 *     is written to WRITABLE state only after the physical OLED+2-button
 *     trust-anchor confirm (§10.1), and cleared by factory-reset (§3, §10.6).
 *   - freshness: monotonic counter baseline (works with no clock) + the
 *     Braga block number as the §4.4 clock; key_epoch gates revocation
 *     (§4.5). Same anti-replay shape as cmdauth.c, separate NVS namespace.
 *
 * Decision C is unchanged: the ESP32 is the only verifier; on success only
 * the inner WUPS frame goes to the RP2040 (untouched), exactly as Track 1.
 */

typedef enum {
    ARKIV_UNCLAIMED    = 0, /* no owner bound — no Arkiv commands accepted   */
    ARKIV_MQTT_CLAIMED = 1, /* Default/MQTT mode (Track 0/1 authority)       */
    ARKIV_CLAIMED      = 2,  /* owner bound — Paranoic authority active      */
} arkiv_claim_state_t;

/* A w3pups-cmd entity, already fetched + field-extracted by the Arkiv RPC
 * layer (P2-2). Verification here is transport-agnostic. */
typedef struct {
    uint8_t        writer[20];   /* on-chain entity writer (Arkiv-reported)  */
    uint32_t       epoch;        /* key_epoch the owner signed under         */
    uint64_t       counter;      /* monotonic per (owner,device); also the
                                  * `seq` numeric attr from the entity        */
    uint64_t       block;        /* Braga block the entity landed in (§4.4)  */
    const char    *device_id;    /* ICCID ASCII (no NUL counted) — bound in
                                  * the signed digest (defence in depth      */
    const char    *command_id;   /* UUID ASCII (36 chars) — bound in digest;
                                  * carried as a string attribute on entity   */
    const uint8_t *frame;        /* canonical WUPS frame (owner-signed)      */
    size_t         frame_len;
    const uint8_t *sig;          /* 64B owner secp256k1 r||s over the
                                  * EIP-191 wrap of the cmd binding digest    */
} arkiv_cmd_t;

/* Load the device key from `prov` (derive its address) and the writable
 * Arkiv state (owner_addr / key_epoch / last_ctr / cur_block / claim_state).
 * Returns ESP_OK only if the device is Arkiv-provisioned (has `ak_dev_priv`).
 * A missing owner binding is NOT an error — that is just UNCLAIMED. */
esp_err_t cmdauth_arkiv_init(void);

/* True once cmdauth_arkiv_init() succeeded (device key present). */
bool cmdauth_arkiv_ready(void);

arkiv_claim_state_t cmdauth_arkiv_claim_state(void);

/* Device Ethereum address (20 bytes). Valid after a successful init. */
const uint8_t *cmdauth_arkiv_device_addr(void);

/* Device uncompressed secp256k1 public key (64 B, X||Y, no 0x04 prefix).
 * Valid after a successful init. Used to derive the §10.4 claim-code
 * (keccak(device_pub||boot_nonce)) — public, safe to expose. */
const uint8_t *cmdauth_arkiv_device_pub(void);

/* Registered owner address (20 bytes), derived from the bound owner_pub.
 * Returns NULL until the device is ARKIV_CLAIMED. Used by the WS
 * subscriber to filter incoming events to this owner only (topic[2] in
 * the eth_subscribe(logs) filter — see arkiv_ws.c). */
const uint8_t *cmdauth_arkiv_owner_addr(void);

/* Owner ENCRYPTION public key (64 B X||Y) — the ECDH peer for telemetry/
 * ack/event payload confidentiality (ADR-0013). Distinct from owner_pub:
 * owner-derived from a deterministic wallet signature, bound at claim.
 * Returns NULL until ARKIV_CLAIMED. */
const uint8_t *cmdauth_arkiv_enc_pub(void);

/* Current key_epoch (ratchet/revocation, §4.5). Used by the writer's payload
 * seal to scope each per-epoch K_dir. */
uint32_t cmdauth_arkiv_key_epoch(void);

/* Monotonic counter bumped on every binding change (bind / epoch set / clear).
 * The payload-seal key cache re-derives K_dir whenever this changes, so a
 * re-claim (new owner/enc_pub, possibly same epoch) can never reuse a stale key. */
uint32_t cmdauth_arkiv_binding_gen(void);

/* Highest Braga block processed (replay cursor, §4.4 fromBlock). 0 = none. */
uint64_t cmdauth_arkiv_cursor_block(void);

/*
 * Verify a fetched w3pups-cmd entity against the bound owner (§4.3):
 *   writer == registered owner_addr, owner signature over the frame valid,
 *   epoch == stored key_epoch, counter strictly monotonic.
 * On success persists the counter + block cursor and hands back the inner
 * WUPS frame to forward to the RP2040 (Decision C). Returns false (drop) on
 * any failure or when UNCLAIMED.
 */
bool cmdauth_arkiv_check(const arkiv_cmd_t *cmd,
                         const uint8_t **frame, size_t *frame_len);

/*
 * Bind / rebind the owner. MUST only be called by the OLED+2-button trust
 * gate after a physical confirm (§10.1) — never from the network path.
 * Takes the owner's uncompressed secp256k1 PUBLIC key (64 B, X||Y, no 0x04
 * prefix): the address for the §4.3 writer check is derived from it
 * (keccak256[12:]), and it is used to verify the owner signature over each
 * command frame (so a lying RPC cannot forge `writer`). Also takes the owner
 * ENCRYPTION key enc_pub (64 B X||Y, ADR-0013) — the ECDH peer for telemetry
 * confidentiality, bound in the claim digest. Both keys are validated on-curve
 * and refused if invalid. Persists owner_pub + enc_pub + key_epoch, resets the
 * counter namespace (fresh per-owner, §10.6), sets claim_state = ARKIV_CLAIMED.
 */
esp_err_t cmdauth_arkiv_bind_owner(const uint8_t owner_pub[64],
                                   const uint8_t enc_pub[64], uint32_t epoch);

/* Accept an owner-signed epoch bump (revocation/ratchet, §4.5): monotonic
 * upward only. Caller must have verified the bump was owner-signed. */
esp_err_t cmdauth_arkiv_set_epoch(uint32_t epoch);

/* Factory-reset the Arkiv binding (§10.6): clears owner/epoch/counter,
 * state → UNCLAIMED. Physical-reset path only. */
esp_err_t cmdauth_arkiv_clear(void);

/* Re-roll the device's Arkiv private key (ak_dev_priv in the `prov`
 * partition) and wipe the owner binding state (so the new address has
 * a fresh epoch/counter and no claimed owner). The new private key is
 * a 32 B uniformly random scalar in [1, n-1] — rejected and redrawn
 * on the astronomically rare invalid draw. On success, logs the new
 * device address and returns ESP_OK; caller is expected to esp_restart()
 * so the firmware re-reads the new key from prov NVS on next boot.
 *
 * This is the recovery path when an existing wallet's nonce stream has
 * gone bad on Braga (pending tx pileup after an outage) — owner re-funds
 * the new address out-of-band and re-claims the device. */
esp_err_t cmdauth_arkiv_regenerate_wallet(void);
