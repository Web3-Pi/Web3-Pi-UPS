/*
 * secp256k1 ECDSA signing with recovery id.
 * Uses micro-ecc (BSD-2) for scalar/point arithmetic and MbedTLS SHA-256 for RFC 6979.
 */
#include "arkiv_crypto/secp256k1.h"
#include "arkiv_crypto/keccak256.h"
#include "uECC.h"
#include "uECC_vli.h"
// IDF 6.0 / mbedtls 3.6: legacy sha256.h moved under mbedtls/private/.
// Declaring private identifiers exposes the still-usable mbedtls_sha256_* API.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <string.h>

#define SHA256_LEN 32
#define SHA256_BLOCK 64
#define SCALAR_LEN 32

/* Max number of uECC_word_t's needed for a secp256k1 scalar/coordinate.
 * With uECC_WORD_SIZE=4 → 8 words; with =8 → 4 words; allocate 8 to cover both. */
#define MAX_WORDS 8

/* ---------------------------------------------------------------- HMAC */

static void sha256(const uint8_t* in, size_t len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, in, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void hmac_sha256(const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        uint8_t out[32])
{
    uint8_t k_pad[SHA256_BLOCK];
    uint8_t tk[SHA256_LEN];

    if (key_len > SHA256_BLOCK) {
        sha256(key, key_len, tk);
        key = tk;
        key_len = SHA256_LEN;
    }

    memset(k_pad, 0, sizeof(k_pad));
    memcpy(k_pad, key, key_len);
    for (size_t i = 0; i < SHA256_BLOCK; i++) k_pad[i] ^= 0x36;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_pad, SHA256_BLOCK);
    mbedtls_sha256_update(&ctx, msg, msg_len);
    uint8_t inner[SHA256_LEN];
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    memset(k_pad, 0, sizeof(k_pad));
    memcpy(k_pad, key, key_len);
    for (size_t i = 0; i < SHA256_BLOCK; i++) k_pad[i] ^= 0x5c;

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_pad, SHA256_BLOCK);
    mbedtls_sha256_update(&ctx, inner, SHA256_LEN);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/* --------------------------------------------------------------- RFC 6979 */

typedef struct {
    uint8_t K[32];
    uint8_t V[32];
} rfc6979_ctx;

/* priv: 32 bytes BE; hash_mod_n: 32 bytes BE (hash already reduced mod n). */
static void rfc6979_init(rfc6979_ctx* r, const uint8_t priv[32], const uint8_t hash_mod_n[32])
{
    memset(r->V, 0x01, sizeof(r->V));
    memset(r->K, 0x00, sizeof(r->K));

    uint8_t buf[32 + 1 + 32 + 32];
    memcpy(buf, r->V, 32);
    buf[32] = 0x00;
    memcpy(buf + 33, priv, 32);
    memcpy(buf + 65, hash_mod_n, 32);
    hmac_sha256(r->K, 32, buf, sizeof(buf), r->K);
    hmac_sha256(r->K, 32, r->V, 32, r->V);

    memcpy(buf, r->V, 32);
    buf[32] = 0x01;
    memcpy(buf + 33, priv, 32);
    memcpy(buf + 65, hash_mod_n, 32);
    hmac_sha256(r->K, 32, buf, sizeof(buf), r->K);
    hmac_sha256(r->K, 32, r->V, 32, r->V);
}

/* Fill t[0..31] with the next RFC 6979 candidate. Returns bytes generated (32). */
static void rfc6979_next_t(rfc6979_ctx* r, uint8_t t[32])
{
    hmac_sha256(r->K, 32, r->V, 32, r->V);
    memcpy(t, r->V, 32);
}

static void rfc6979_reseed(rfc6979_ctx* r)
{
    uint8_t buf[33];
    memcpy(buf, r->V, 32);
    buf[32] = 0x00;
    hmac_sha256(r->K, 32, buf, sizeof(buf), r->K);
    hmac_sha256(r->K, 32, r->V, 32, r->V);
}

/* ----------------------------------------------------------- helpers */

/* Read 32 BE bytes into native VLI; reduce once mod n if >= n. Result is guaranteed < n. */
static void bytes32_to_scalar_mod_n(const uint8_t b[32], uECC_word_t out[MAX_WORDS], uECC_Curve curve)
{
    const uECC_word_t* n = uECC_curve_n(curve);
    wordcount_t nw = (wordcount_t)uECC_curve_num_n_words(curve);
    uECC_vli_bytesToNative(out, b, SCALAR_LEN);
    if (uECC_vli_cmp(out, n, nw) >= 0) {
        uECC_vli_sub(out, out, n, nw);
    }
}

/* --------------------------------------------- pubkey / address derivation */

int arkiv_secp256k1_derive_pubkey(const uint8_t priv[32], uint8_t pub_out[64])
{
    uECC_Curve curve = uECC_secp256k1();
    if (uECC_compute_public_key(priv, pub_out, curve) != 1) {
        return -1;
    }
    return 0;
}

int arkiv_secp256k1_verify(const uint8_t pub[64], const uint8_t digest[32], const uint8_t sig[64])
{
    return uECC_verify(pub, digest, 32, sig, uECC_secp256k1());
}

int arkiv_secp256k1_derive_address(const uint8_t priv[32], uint8_t addr_out[20])
{
    uint8_t pub[64];
    int rc = arkiv_secp256k1_derive_pubkey(priv, pub);
    if (rc != 0) return rc;
    uint8_t hash[32];
    arkiv_keccak256(pub, 64, hash);
    memcpy(addr_out, hash + 12, 20);
    return 0;
}

/* ----------------------------------------------------- sign_recoverable */

int arkiv_secp256k1_sign_recoverable(const uint8_t priv[32],
                                     const uint8_t digest[32],
                                     uint8_t sig_out[64])
{
    uECC_Curve curve = uECC_secp256k1();
    const uECC_word_t* n = uECC_curve_n(curve);
    const uECC_word_t* G = uECC_curve_G(curve);
    wordcount_t nw = (wordcount_t)uECC_curve_num_n_words(curve);

    /* Validate privkey: 0 < d < n. */
    uECC_word_t d[MAX_WORDS];
    uECC_vli_bytesToNative(d, priv, SCALAR_LEN);
    if (uECC_vli_isZero(d, nw)) return -1;
    if (uECC_vli_cmp(d, n, nw) >= 0) return -2;

    /* z = digest mod n (single subtract covers all real hashes). */
    uECC_word_t z[MAX_WORDS];
    bytes32_to_scalar_mod_n(digest, z, curve);

    /* RFC 6979 seed: hash_mod_n as BE bytes. */
    uint8_t hash_mod_n_be[32];
    uECC_vli_nativeToBytes(hash_mod_n_be, SCALAR_LEN, z);

    rfc6979_ctx rfc;
    rfc6979_init(&rfc, priv, hash_mod_n_be);

    uECC_word_t k[MAX_WORDS];
    uECC_word_t r[MAX_WORDS];
    uECC_word_t s[MAX_WORDS];
    uECC_word_t kinv[MAX_WORDS];
    uECC_word_t rd[MAX_WORDS];
    uECC_word_t z_plus_rd[MAX_WORDS];
    uECC_word_t R[2 * MAX_WORDS];

    int recid = -1;
    for (int tries = 0; tries < 16; tries++) {
        uint8_t t[32];
        rfc6979_next_t(&rfc, t);
        uECC_vli_bytesToNative(k, t, SCALAR_LEN);
        if (uECC_vli_isZero(k, nw) || uECC_vli_cmp(k, n, nw) >= 0) {
            rfc6979_reseed(&rfc);
            continue;
        }

        /* R = k*G */
        uECC_point_mult(R, G, k, curve);

        /* r = R.x; if R.x >= n, reduce. recid low bit = R.y parity. */
        uECC_vli_set(r, R, nw);
        int rx_wrapped = 0;
        if (uECC_vli_cmp(r, n, nw) >= 0) {
            uECC_vli_sub(r, r, n, nw);
            rx_wrapped = 1;
        }
        if (uECC_vli_isZero(r, nw)) {
            rfc6979_reseed(&rfc);
            continue;
        }
        int cand_recid = (int)(R[nw] & 1) | (rx_wrapped << 1);

        /* s = k^-1 * (z + r*d) mod n */
        uECC_vli_modMult(rd, r, d, n, nw);
        uECC_vli_modAdd(z_plus_rd, z, rd, n, nw);
        uECC_vli_modInv(kinv, k, n, nw);
        uECC_vli_modMult(s, kinv, z_plus_rd, n, nw);
        if (uECC_vli_isZero(s, nw)) {
            rfc6979_reseed(&rfc);
            continue;
        }

        /* Low-S: if s > n/2, s = n - s and flip recid parity. */
        uECC_word_t half_n[MAX_WORDS];
        uECC_vli_set(half_n, n, nw);
        uECC_vli_rshift1(half_n, nw);
        if (uECC_vli_cmp(s, half_n, nw) > 0) {
            uECC_word_t neg_s[MAX_WORDS];
            uECC_vli_sub(neg_s, n, s, nw);
            uECC_vli_set(s, neg_s, nw);
            cand_recid ^= 1;
        }

        recid = cand_recid;
        break;
    }

    if (recid < 0) return -3;

    uECC_vli_nativeToBytes(sig_out, SCALAR_LEN, r);
    uECC_vli_nativeToBytes(sig_out + SCALAR_LEN, SCALAR_LEN, s);
    return recid;
}
