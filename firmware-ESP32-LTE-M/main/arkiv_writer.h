#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Track 2 / ADR-0011 P4 — device-side Arkiv entity writer.
 *
 * The owner-binding (claim) and command-execution (cmd) paths only READ
 * from Arkiv. P4 introduces device-INITIATED writes: telemetry, ACK and
 * event entities signed by the per-device wallet (`ak_dev_priv`, provisioned
 * into the `prov` NVS partition alongside the Track 0/1 secrets) and paid
 * for in GLM gas owned by the device address. The owner deliberately funds
 * the device wallet on Braga — there is no master wallet (§4.2).
 *
 * This module is the single point that touches the wallet key: every
 * outbound entity goes through arkiv_writer_create_entity(), which:
 *   1. queries the chain for the current nonce (eth_getTransactionCount),
 *   2. picks a gas price (eth_gasPrice with a floor for slow blocks),
 *   3. builds an OpsTxData with a single CreateOp,
 *   4. signs an EIP-155 legacy tx with `ak_dev_priv`,
 *   5. submits via eth_sendRawTransaction, returning the tx hash.
 *
 * The C++ implementation wraps the C++ helpers in
 * `components/arkiv_crypto/`; this header is C-callable so the existing C
 * modules (arkiv_rpc, arkiv_claim, the new arkiv_ack / arkiv_tlm /
 * arkiv_event) can call it without dragging the namespace in.
 */

/* Storage contract address — `arkiv_storage_create_entity` is implemented
 * by a precompile/system contract at this address (the SDK targets it
 * directly). Matches the @arkiv-network/sdk `ARKIV_ADDRESS` constant
 * exactly: the trailing 5 bytes spell "arkiv" (0x61 'a' 72 'r' 6b 'k'
 * 69 'i' 76 'v'). A node sending a tx to anything else gets back
 * "non-golembase transaction". */
#define ARKIV_STORAGE_CONTRACT_HEX "0x00000000000000000000000000000061726b6976"

/* Attribute kinds for the C API. `value_str` is honoured when isNumeric==0,
 * `value_num` when isNumeric==1. */
typedef struct {
    const char *key;
    const char *value_str;
    int64_t     value_num;
    bool        is_numeric;
} arkiv_attr_t;

/* Load the device key from prov NVS, derive the address, and log it. Idempotent.
 * Must be called after identity_init() / cmdauth_arkiv_init() so prov is open. */
esp_err_t arkiv_writer_init(void);

/* True once the device key is loaded and the writer can sign + submit. */
bool arkiv_writer_ready(void);

/* The device's Ethereum address (20 bytes, derived from ak_dev_priv).
 * Valid after arkiv_writer_init() returns ESP_OK. */
const uint8_t *arkiv_writer_device_addr(void);

/* Monotonic u64 sequence for device-written entities (ack/telemetry/event).
 * Persisted to writable NVS so the backend ingest cursor (which never goes
 * backwards) survives device reboots. Single counter for all entity types
 * is fine — the backend's per-entity-type queries naturally partition them. */
uint64_t arkiv_writer_next_seq(void);

/* Submit a single CreateOp with the given attributes and payload. Returns
 * ESP_OK iff the JSON-RPC node accepted the raw tx (HTTP 2xx + result). On
 * success `out_tx_hash` (32 B) holds the keccak of the signed RLP; the
 * caller may persist it for trace/replay. `expires_in_seconds` is the
 * entity TTL on chain (the SDK converts to blocks at ~2 s/block).
 *
 * Thread-safety: an internal mutex serializes nonce queries + submits so
 * concurrent callers (telemetry task + an ACK fired from the RPC task)
 * don't pull the same nonce twice.
 *
 * BLOCKING: TLS handshake + tx signing + raw-tx submit can take seconds.
 * Callers running on small-stack tasks (e.g. wups_rx at 4 KB) must use the
 * enqueue variant below instead — see the stack overflow caught in P4. */
esp_err_t arkiv_writer_create_entity(const char        *content_type,
                                     const uint8_t     *payload,
                                     size_t             payload_len,
                                     uint32_t           expires_in_seconds,
                                     const arkiv_attr_t *attrs,
                                     size_t             attr_count,
                                     uint8_t            out_tx_hash[32]);

/* Non-blocking variant: copies the request into a heap-owned job, hands it
 * to the writer worker task (10 KB stack) which performs the same submit.
 * Use from any task whose stack can't accommodate a TLS handshake + RLP
 * signing — in particular wups_rx (4 KB).
 *
 * Returns true iff the job was queued. False on `arkiv_writer_init`
 * never having succeeded, OOM, or a full queue (drop = the missed
 * ack/event is preferable to blocking the wups_rx pipeline). */
bool arkiv_writer_enqueue_create_entity(const char         *content_type,
                                        const uint8_t      *payload,
                                        size_t              payload_len,
                                        uint32_t            expires_in_seconds,
                                        const arkiv_attr_t *attrs,
                                        size_t              attr_count);

#ifdef __cplusplus
}
#endif
