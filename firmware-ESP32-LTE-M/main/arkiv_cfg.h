#pragma once

/*
 * Track 2 / ADR-0011 — Arkiv (Paranoic) configuration & on-chain entity
 * contract. Network: self-hosted Web3 Pi Arkiv chain (Panel ADR-0015;
 * replaced the Braga testnet after its 2026-08-12 sunset). Same wire
 * contract — the node runs the pinned Braga-era arkiv-op-geth build.
 */

/* Single RPC gateway (plan §4.7). Availability deliberately NOT protected. */
#define ARKIV_RPC_URL           "https://arkiv.web3pi.io/rpc"
/* 0x77337069 — ASCII "w3pi". EIP-155 signing must match the node's genesis. */
#define ARKIV_CHAIN_ID          1999859817ULL

#define ARKIV_CMD_ENTITY_TYPE   "w3pups-cmd"

/* Cmd channel cadence — driven by `arkiv_rpc.c poll_task`. With the WS
 * subscriber (`arkiv_ws.c`) running this is the FALLBACK cadence used
 * only while the WS is disconnected; in steady state the rare cadence
 * below applies and the HTTP loop barely touches the link.
 *  - INTERVAL_MS:          aggressive (WS down) — 5 s, original behaviour
 *  - INTERVAL_FALLBACK_MS: rare (WS healthy)   — 5 min, belt-and-suspenders */
#define ARKIV_POLL_INTERVAL_MS          5000
#define ARKIV_POLL_INTERVAL_FALLBACK_MS (5 * 60 * 1000)
#define ARKIV_HTTP_TIMEOUT_MS   8000
#define ARKIV_RPC_RESP_CAP      8192

/* w3pups-cmd sweep bounds (issue #9). One arkiv_query page of PAGE_N
 * entities — each ~0.7 KB on the wire (0x-hex WUPS frame up to 240 B, the
 * 128-hex sig, the attribute set) — stays far below ARKIV_RPC_RESP_CAP
 * even with full-size frames; a response over the cap is dropped WHOLE
 * (the sweep is skipped, never fed a truncated JSON), so the page is sized
 * for headroom, not throughput. A sweep first GATHERS: the node lists
 * newest-first and ignores orderBy, so it walks the window
 * `seq > last_ctr && seq < floor` down the seq axis (at most MAX_PAGES
 * queries) until a short page proves nothing older is pending, then
 * dispatches the lowest seqs — at most MAX_PER_SWEEP per sweep. PAGE is
 * the JSON-RPC hex quantity, PAGE_N the same number (keep in sync). A
 * sweep that had to stop with commands left behind reports it and the
 * caller re-arms after RESWEEP_DELAY_MS, at most MAX_RESWEEPS times per
 * trigger (8 × 8 = 64 commands before the regular cadence takes over). */
#define ARKIV_CMD_PAGE          "0x6"
#define ARKIV_CMD_PAGE_N        6
#define ARKIV_CMD_MAX_PER_SWEEP 8
#define ARKIV_CMD_MAX_PAGES     3
#define ARKIV_CMD_RESWEEP_DELAY_MS 1000
#define ARKIV_CMD_MAX_RESWEEPS  8

/* ACK tracker (arkiv_ack.c): in-flight (inner WUPS SEQ → command_id)
 * mappings. Sized to a full sweep (MAX_PER_SWEEP) so a burst of queued
 * commands can never evict a mapping whose RESP is still on its way. */
#define ARKIV_ACK_SLOTS         8

/* Writer job queue + retry policy (arkiv_writer.cpp). RETRIES counts TOTAL
 * attempts (first try + 2 retries, one per RETRY_BACKOFF_S entry).
 * QUEUE_LEN bounds the heap held by queued ack/event jobs (~0.5-1 KB
 * each). A submit that fails on transport/HTTP grounds (LTE hiccup,
 * gateway 5xx, node unreachable) is retried IN PLACE — same job — sleeping
 * RETRY_BACKOFF_S[attempt-1] seconds between attempts, as long as the job
 * is younger than JOB_MAX_AGE_S (checked before every attempt). When the
 * failure hit the eth_sendRawTransaction step itself the node may already
 * hold the tx, so the retry first asks eth_getTransactionByHash and only
 * resends (fresh nonce) a tx the node never saw — otherwise a lost reply
 * would create a second identical entity. A JSON-RPC error object from
 * eth_sendRawTransaction is permanent (bad tx, no funds) and dropped at
 * once, except the gateway-side codes -32603/-32005 (transient). 240 s
 * covers the panel's ≤ 5 min command-row window; an ACK older than that
 * only wastes gas. */
#define ARKIV_WRITER_QUEUE_LEN       16
#define ARKIV_WRITER_RETRIES         3
#define ARKIV_WRITER_RETRY_BACKOFF_S {5, 15}
#define ARKIV_WRITER_JOB_MAX_AGE_S   240

/*
 * w3pups-cmd entity contract (what the owner's browser wallet writes; the
 * device reads + verifies). Metadata is plaintext by design (plan §11.1):
 *
 *   on-chain writer  = owner EOA (Arkiv-native; the §4.3 authority check)
 *   stringAttributes : type="w3pups-cmd", device_id=<ICCID>,
 *                      command_id=<UUID ASCII, 36 chars, panel command row>,
 *                      sig=<128 hex: owner secp256k1 r||s, low-S, over the
 *                           EIP-191 personal_sign of the cmd binding
 *                           digest below — see note>
 *   numericAttributes: seq=<u64 monotonic per owner|device — Paranoic
 *                           baseline, separate from the inner WUPS frame's
 *                           u8 SEQ which is just the RP2040 REQ↔RESP nonce>,
 *                      epoch=<key_epoch>
 *   payload          = the canonical WUPS frame, base64 (owner-signed)
 *
 * Command binding digest (the owner key authorised THIS specific frame
 * under THIS specific seq+epoch — defence beyond the Arkiv-reported writer,
 * so a hostile gateway cannot replay the same (frame, sig) with a swapped
 * seq/epoch and get the device to execute twice):
 *
 *   bind = keccak256( "w3pups-cmd\0"        (11 bytes incl. NUL)
 *            || device_id_ascii             (ICCID, no NUL)
 *            || epoch                       (uint32 little-endian)
 *            || seq                         (uint64 little-endian)
 *            || command_id_ascii            (UUID, 36 ASCII bytes, no NUL)
 *            || frame                       (canonical WUPS bytes) )
 *
 * Owner signs `bind` via EIP-191 personal_sign (browser wallets cannot raw-
 * sign an arbitrary hash); the device verifies against
 *   keccak256("\x19" "Ethereum Signed Message:\n32" || bind)
 * (exactly what viem signMessage({raw: bind}) hashes). The §4.4 block clock
 * advances via the entity's chain position; MVP uses the monotonic `seq`
 * as the strict replay baseline (cmdauth_arkiv).
 */
#define ARKIV_ATTR_TYPE         "type"
#define ARKIV_ATTR_DEVICE_ID    "device_id"
#define ARKIV_ATTR_COMMAND_ID   "command_id"
#define ARKIV_ATTR_SIG          "sig"
#define ARKIV_ATTR_SEQ          "seq"
#define ARKIV_ATTR_EPOCH        "epoch"
#define ARKIV_CMD_BIND_TAG      "w3pups-cmd"    /* tag + implicit NUL = 11 B */
/* UUIDs are canonical ASCII, e.g. "550e8400-e29b-41d4-a716-446655440000". */
#define ARKIV_COMMAND_ID_LEN    36

/*
 * w3pups-claim entity contract — Track 2 / ADR-0011, plan §10.4 path B
 * (Arkiv-from-scratch, the first-class path). This is the CANONICAL spec;
 * the browser wallet (WS-4) and Panel MUST produce it byte-identically.
 *
 * The owner's browser wallet, after the user transcribes the device's
 * OLED claim-code into the Panel and connects the wallet, writes:
 *
 *   on-chain writer  = candidate owner EOA (the address that gets bound;
 *                       the §4.3 / §10.1 authority — verified, not trusted)
 *   stringAttributes : type="w3pups-claim", device_id=<ICCID>,
 *                      owner_pub=<128 hex: owner secp256k1 X||Y, no 04>,
 *                      enc_pub=<128 hex: owner ENCRYPTION secp256k1 X||Y, no
 *                          04 — the ECDH peer for telemetry confidentiality
 *                          (ADR-0013). NOT the wallet key: derived owner-side
 *                          from a deterministic personal_sign of a fixed
 *                          domain string. The device ECDHs dev_priv×enc_pub;
 *                          owner_pub stays the command/authority key>,
 *                      claim_code=<12 hex: §10.4 front-running token,
 *                          bip39_pack_top_bits(keccak(device_pub||
 *                          boot_nonce), 44) — 6 bytes, low 4 bits 0>,
 *                      sig=<128 hex: owner secp256k1 r||s, low-S, over
 *                          the EIP-191 personal_sign of the binding
 *                          digest below — see note>
 *   numericAttributes: epoch=<key_epoch the binding starts at>
 *
 * Claim binding digest (owner proves the owner key authorised binding to
 * THIS device — defence beyond the Arkiv-reported writer). enc_pub is bound
 * here so a hostile gateway cannot substitute its own encryption key and read
 * telemetry (ADR-0013):
 *
 *   bind = keccak256( "w3pups-claim\0"     (13 bytes incl. NUL)
 *            || device_id_ascii            (ICCID, no NUL)
 *            || owner_pub                  (64 bytes)
 *            || claim_code                 (6 bytes, canonical form)
 *            || epoch                      (uint32 little-endian)
 *            || enc_pub                    (64 bytes) )
 *
 * Browser wallets cannot raw-sign an arbitrary hash, so the owner signs
 * `bind` via EIP-191 personal_sign and `sig` verifies against
 *   keccak256("\x19" "Ethereum Signed Message:\n32" || bind)
 * (exactly what viem signMessage({raw: bind}) hashes). owner_pub is
 * recovered from that same signature browser-side (WS-4).
 *
 * Device check order (fail-closed, drop on any miss): device_id == own
 * ICCID; claim_code == locally derived (proves physical OLED sight, §10.4);
 * recover/verify owner sig over the digest with owner_pub; writer ==
 * keccak256(owner_pub)[12:]. Only then is the owner fingerprint (§10.1)
 * shown on the OLED for the physical 2-button confirm.
 */
#define ARKIV_CLAIM_ENTITY_TYPE "w3pups-claim"
#define ARKIV_ATTR_OWNER_PUB    "owner_pub"
#define ARKIV_ATTR_ENC_PUB      "enc_pub"        /* owner ECDH key (ADR-0013) */
#define ARKIV_ATTR_CLAIM_CODE   "claim_code"
#define ARKIV_CLAIM_BIND_TAG    "w3pups-claim"   /* tag + implicit NUL */

/* §10.1 fingerprint: top 44 bits of keccak(owner_addr||device_id) → 4
 * BIP39 words; next 16 bits → 4-hex visual checksum. */
#define ARKIV_FP_WORDS          4
#define ARKIV_FP_CHECKSUM_BITS  16

/* §10.4 claim-code: top 44 bits of keccak(device_pub||boot_nonce) → 4
 * BIP39 words (canon allows 4–6; 4 fits the 64x32 OLED on one screen and
 * the token's secrecy rests on physical OLED sight + gas-costed on-chain
 * guessing in a 15-min window, not on bit length). Canonical on-chain
 * form = those 44 bits packed into 6 bytes (low 4 bits zero), see
 * bip39_pack_top_bits(). MUST stay byte-identical to WS-4 (claim.ts). */
#define ARKIV_CC_WORDS          4
#define ARKIV_CC_BITS           44
#define ARKIV_CC_BYTES          6
#define ARKIV_BOOT_NONCE_LEN    16

/* §10.4 / §10.9-2: the claim-code is regenerated on each UNCLAIMED entry
 * and expires after this window (proposal 15 min, owner-confirmable). On
 * expiry the boot_nonce is rotated and the OLED claim-code refreshed.
 * Button-triggered regen is the RP2040 side (P2-4c). */
#define ARKIV_CLAIM_CODE_TTL_MS (15 * 60 * 1000)

/* w3pups-claim poll cadence while UNCLAIMED (path B). Same order as the
 * command poll; claims are rare and user-paced. */
#define ARKIV_CLAIM_POLL_MS     5000
