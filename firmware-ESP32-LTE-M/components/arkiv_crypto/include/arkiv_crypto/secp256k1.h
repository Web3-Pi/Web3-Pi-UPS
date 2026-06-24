/*
 * secp256k1 ECDSA signing with recovery id, built on micro-ecc + MbedTLS SHA-256 HMAC.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARKIV_SECP256K1_PRIVKEY_LEN 32
#define ARKIV_SECP256K1_PUBKEY_LEN  64   /* X||Y, no 0x04 prefix */
#define ARKIV_SECP256K1_SIG_LEN     64   /* r||s, both 32 bytes BE, already low-S */
#define ARKIV_ADDRESS_LEN           20

/* Derive Ethereum-style address (keccak256(X||Y)[12:32]) from a 32-byte private key.
 * Returns 0 on success, <0 on error. */
int arkiv_secp256k1_derive_address(const uint8_t priv[32], uint8_t addr_out[20]);

/* Derive uncompressed public key (64 bytes, X||Y) from private key.
 * Returns 0 on success, <0 on error. */
int arkiv_secp256k1_derive_pubkey(const uint8_t priv[32], uint8_t pub_out[64]);

/* Deterministic (RFC 6979) ECDSA sign over a 32-byte digest.
 * Writes r||s (low-S canonical) into sig_out[0..63].
 * Returns the recovery id (0 or 1) on success, or a negative value on error. */
int arkiv_secp256k1_sign_recoverable(const uint8_t priv[32],
                                     const uint8_t digest[32],
                                     uint8_t sig_out[64]);

/* Verify a 64-byte (r||s) ECDSA signature against a 32-byte digest + 64-byte pubkey (X||Y).
 * Returns 1 if valid, 0 otherwise. */
int arkiv_secp256k1_verify(const uint8_t pub[64],
                           const uint8_t digest[32],
                           const uint8_t sig[64]);

/* ECDH: compute the shared-secret X-coordinate = X(priv * peer_pub).
 *   priv      : 32-byte private scalar (our key)
 *   peer_pub  : 64-byte uncompressed public key (X||Y, no 0x04) of the remote party
 *   shared_x  : 32-byte output, big-endian X coordinate of priv*peer_pub
 *
 * Rejects an invalid / off-curve / point-at-infinity peer key (fail-closed) so a
 * hostile-supplied owner key cannot coerce a weak or attacker-known secret.
 *
 * SECURITY: shared_x is the RAW EC X coordinate. It MUST be passed through a KDF
 * (HKDF-Extract / hash) before use as a key material — NEVER used directly. This
 * matches @noble/curves getSharedSecret(priv, 0x04||peer_pub, false).subarray(1,33).
 *
 * Returns 0 on success, <0 on error (invalid peer key, or computation failure). */
int arkiv_secp256k1_ecdh(const uint8_t priv[32],
                         const uint8_t peer_pub[64],
                         uint8_t shared_x[32]);

/* 1 if pub (64B X||Y) is a valid secp256k1 point (on-curve, not the point at
 * infinity / all-zero); 0 otherwise. Use to reject a hostile-supplied key at
 * bind time before it is trusted. */
int arkiv_secp256k1_valid_pubkey(const uint8_t pub[64]);

/* HMAC-SHA256. out is 32 bytes. Exposed so HKDF (arkiv_crypto AEAD) can reuse the
 * exact same primitive used by the RFC 6979 signer. */
void arkiv_hmac_sha256(const uint8_t* key, size_t key_len,
                       const uint8_t* msg, size_t msg_len,
                       uint8_t out[32]);

#ifdef __cplusplus
}
#endif
