#pragma once

/*
 * Track 2 / ADR-0011 — Arkiv (Paranoic) configuration & on-chain entity
 * contract. Braga testnet only (project scope: no mainnet).
 */

/* Single RPC gateway (plan §4.7). Availability deliberately NOT protected. */
#define ARKIV_RPC_URL           "https://braga.hoodi.arkiv.network/rpc"
#define ARKIV_CHAIN_ID          60138453102ULL

#define ARKIV_CMD_ENTITY_TYPE   "w3pups-cmd"

/* Commands are rare on Arkiv (latency + gas). 5 s poll keeps latency a few
 * seconds (poll + Braga block) per the accepted §7 trade-off. */
#define ARKIV_POLL_INTERVAL_MS  5000
#define ARKIV_HTTP_TIMEOUT_MS   8000
#define ARKIV_RPC_RESP_CAP      8192

/*
 * w3pups-cmd entity contract (what the owner's browser wallet writes; the
 * device reads + verifies). Metadata is plaintext by design (plan §11.1):
 *
 *   on-chain writer  = owner EOA (Arkiv-native; the §4.3 authority check)
 *   stringAttributes : type="w3pups-cmd", device_id=<ICCID>,
 *                      sig=<128 hex: owner secp256k1 r||s over the frame>
 *   numericAttributes: seq=<monotonic per owner|device>, epoch=<key_epoch>
 *   payload          = the canonical WUPS frame, base64 (owner-signed)
 *
 * The §4.4 block clock advances via the entity's chain position; MVP uses
 * the monotonic `seq` as the strict replay baseline (cmdauth_arkiv).
 */
#define ARKIV_ATTR_TYPE         "type"
#define ARKIV_ATTR_DEVICE_ID    "device_id"
#define ARKIV_ATTR_SIG          "sig"
#define ARKIV_ATTR_SEQ          "seq"
#define ARKIV_ATTR_EPOCH        "epoch"
