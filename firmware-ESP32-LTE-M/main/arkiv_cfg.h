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
 *                      sig=<128 hex: owner secp256k1 r||s over
 *                           keccak256(canonical WUPS frame), low-S>
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
 *                      claim_code=<18 hex: §10.4 front-running token,
 *                          bip39_pack_top_bits(keccak(device_pub||
 *                          boot_nonce), 66) — 9 bytes, low 6 bits 0>,
 *                      sig=<128 hex: owner secp256k1 r||s, low-S, over
 *                          the EIP-191 personal_sign of the binding
 *                          digest below — see note>
 *   numericAttributes: epoch=<key_epoch the binding starts at>
 *
 * Claim binding digest (owner proves the owner key authorised binding to
 * THIS device — defence beyond the Arkiv-reported writer):
 *
 *   bind = keccak256( "w3pups-claim\0"     (13 bytes incl. NUL)
 *            || device_id_ascii            (ICCID, no NUL)
 *            || owner_pub                  (64 bytes)
 *            || claim_code                 (9 bytes, canonical form)
 *            || epoch                      (uint32 little-endian) )
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
#define ARKIV_ATTR_CLAIM_CODE   "claim_code"
#define ARKIV_CLAIM_BIND_TAG    "w3pups-claim"   /* tag + implicit NUL */

/* §10.1 fingerprint: top 44 bits of keccak(owner_addr||device_id) → 4
 * BIP39 words; next 16 bits → 4-hex visual checksum. */
#define ARKIV_FP_WORDS          4
#define ARKIV_FP_CHECKSUM_BITS  16

/* §10.4 claim-code: top 66 bits of keccak(device_pub||boot_nonce) → 6
 * BIP39 words; canonical on-chain form is those 66 bits packed into 9
 * bytes (low 6 bits zero), see bip39_pack_top_bits(). */
#define ARKIV_CC_WORDS          6
#define ARKIV_CC_BITS           66
#define ARKIV_CC_BYTES          9
#define ARKIV_BOOT_NONCE_LEN    16

/* §10.4 / §10.9-2: the claim-code is regenerated on each UNCLAIMED entry
 * and expires after this window (proposal 15 min, owner-confirmable). On
 * expiry the boot_nonce is rotated and the OLED claim-code refreshed.
 * Button-triggered regen is the RP2040 side (P2-4c). */
#define ARKIV_CLAIM_CODE_TTL_MS (15 * 60 * 1000)

/* w3pups-claim poll cadence while UNCLAIMED (path B). Same order as the
 * command poll; claims are rare and user-paced. */
#define ARKIV_CLAIM_POLL_MS     5000
