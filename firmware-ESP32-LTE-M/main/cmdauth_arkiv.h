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
    uint64_t       counter;      /* monotonic per (owner,device)             */
    uint64_t       block;        /* Braga block the entity landed in (§4.4)  */
    const uint8_t *frame;        /* canonical WUPS frame (owner-signed)      */
    size_t         frame_len;
    const uint8_t *sig;          /* 64B owner secp256k1 r||s over the frame  */
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
 * Persists owner_addr + key_epoch, resets the counter namespace (fresh
 * per-owner, §10.6), sets claim_state = ARKIV_CLAIMED.
 */
esp_err_t cmdauth_arkiv_bind_owner(const uint8_t owner_addr[20], uint32_t epoch);

/* Accept an owner-signed epoch bump (revocation/ratchet, §4.5): monotonic
 * upward only. Caller must have verified the bump was owner-signed. */
esp_err_t cmdauth_arkiv_set_epoch(uint32_t epoch);

/* Factory-reset the Arkiv binding (§10.6): clears owner/epoch/counter,
 * state → UNCLAIMED. Physical-reset path only. */
esp_err_t cmdauth_arkiv_clear(void);
