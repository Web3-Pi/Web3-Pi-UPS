/*
 * Arkiv (Paranoic / ADR-0011 P3) payload AEAD: HKDF-SHA256 key schedule +
 * AES-256-GCM seal, over a static-ECDH per-epoch ratchet. Owner-only (posture B).
 * Built on arkiv_hmac_sha256 (secp256k1.c) + mbedtls GCM.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HKDF-SHA256 (RFC 5869): PRK = HMAC(salt, IKM); OKM = Expand(PRK, info, L).
 *   okm_len : in (0, 255*32]
 *   salt    : may be empty (RFC: treated as 32 zero bytes; HMAC zero-padding yields the same)
 *   info    : <= 512 bytes
 * Byte-identical to node crypto.hkdfSync('sha256', ikm, salt, info, okm_len) and
 * @noble/hashes hkdf(sha256, ...). Returns 0 on success, <0 on error. */
int arkiv_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                      const uint8_t *salt, size_t salt_len,
                      const uint8_t *info, size_t info_len,
                      uint8_t *okm, size_t okm_len);

/* AES-256-GCM, 96-bit nonce, 128-bit tag (the standard the reader's
 * node:crypto / WebCrypto / @noble use). Firmware uses mbedtls; host KATs swap
 * in OpenSSL via -DARKIV_AEAD_HOST_GCM. Both implement identical standard GCM.
 *
 * encrypt: ct must hold pt_len bytes; tag gets the 16-byte tag. 0 ok, <0 error.
 * decrypt: CONSTANT-TIME tag check. Returns 0 on success, ARKIV_AEAD_AUTH_FAIL
 *          (-3) on a bad tag / tampered AAD/ciphertext (never leaks plaintext),
 *          other <0 on internal error. pt must hold ct_len bytes. */
#define ARKIV_AEAD_AUTH_FAIL (-3)

int arkiv_aead_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t *ct, uint8_t tag[16]);

int arkiv_aead_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ct, size_t ct_len,
                           const uint8_t tag[16], uint8_t *pt);

/* ------------------------------------------------------------- Arkiv seal */
/* Entity type tags (also the nonce/AAD typeTag, giving each stream a disjoint
 * nonce space even though they share the device seq counter). */
#define ARKIV_AEAD_TYPE_TELEMETRY 0x01
#define ARKIV_AEAD_TYPE_ACK       0x02
#define ARKIV_AEAD_TYPE_EVENT     0x03

#define ARKIV_AEAD_SCHEME    0x03   /* body[0] / nonce[0] magic-version */
#define ARKIV_AEAD_HDR_LEN   12     /* 0x03 | typeTag | epoch_BE32 | seq_BE48  == the nonce */
#define ARKIV_AEAD_TAG_LEN   16
#define ARKIV_AEAD_OVERHEAD  (ARKIV_AEAD_HDR_LEN + ARKIV_AEAD_TAG_LEN)  /* 28 */

/* Per-epoch, per-stream key:
 *   K_dir = HKDF-SHA256(ikm=shared_x,
 *                       salt = "<owner_addr_lower>|<device_pubkey_128hex_lower>",
 *                       info = "w3pups-arkiv-<purpose>|epoch=<dec>")
 * purpose ∈ {telemetry, ack, event} chosen by typeTag (domain separation).
 * owner_addr_lower / device_pubhex_lower must already be lowercased (== credentials.ts).
 * Returns 0 on success, <0 on error. */
int arkiv_aead_derive_key(const uint8_t shared_x[32],
                          const char *owner_addr_lower,
                          const char *device_pubhex_lower,
                          uint8_t typeTag, uint32_t epoch,
                          uint8_t k_dir[32]);

/* Seal pt into the on-chain body. Layout (== reader contract):
 *   body[0..11]            = nonce  (0x03 | typeTag | epoch_BE32 | seq_BE48)
 *   body[12 .. 12+ptlen-1] = ciphertext (== pt_len, GCM has no padding)
 *   body[last 16]          = GCM tag
 * AAD = nonce(12) || device_id || entity_type || (command_id or "").
 * out_cap must be >= pt_len + ARKIV_AEAD_OVERHEAD. Returns 0, sets *out_len. */
int arkiv_aead_seal(uint8_t typeTag, uint32_t epoch, uint64_t seq, const uint8_t k_dir[32],
                    const char *device_id, const char *entity_type, const char *command_id_or_null,
                    const uint8_t *pt, size_t pt_len,
                    uint8_t *out, size_t out_cap, size_t *out_len);

/* Inverse of arkiv_aead_seal (reference / round-trip; the production reader is
 * the browser). Reconstructs the nonce + AAD from body + the plaintext fields,
 * verifies the tag (constant-time). Returns 0, ARKIV_AEAD_AUTH_FAIL on tamper,
 * or other <0. Rejects a wrong scheme byte. */
int arkiv_aead_open(const uint8_t *body, size_t body_len, const uint8_t k_dir[32],
                    const char *device_id, const char *entity_type, const char *command_id_or_null,
                    uint8_t *pt, size_t pt_cap, size_t *pt_len);

#ifdef __cplusplus
}
#endif
