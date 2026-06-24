/*
 * Arkiv payload AEAD (ADR-0011 P3) — HKDF-SHA256 key schedule.
 * AES-256-GCM seal + the high-level arkiv_aead_seal() land in later steps.
 */
#include "arkiv_crypto/aead.h"
#include "arkiv_crypto/secp256k1.h"   /* arkiv_hmac_sha256 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* AES-256-GCM primitive. On-device: mbedtls (IDF 6.0 / mbedtls 4.0 keeps the
 * legacy mbedtls_gcm_* API under mbedtls/private, exposed via the private-
 * identifiers macro — same pattern secp256k1.c uses for mbedtls_sha256).
 * Host KATs (-DARKIV_AEAD_HOST_GCM): OpenSSL EVP, an independent standard GCM. */
#if defined(ARKIV_AEAD_HOST_GCM)
#include <openssl/evp.h>
#else
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/gcm.h>
#endif

#define HKDF_HASH_LEN 32
#define HKDF_INFO_MAX 512

/* Portable secret wipe (volatile so the store isn't optimised away). Avoids a
 * hard mbedtls_platform_zeroize dependency so this TU also builds on host for KATs. */
static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

int arkiv_hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                      const uint8_t *salt, size_t salt_len,
                      const uint8_t *info, size_t info_len,
                      uint8_t *okm, size_t okm_len)
{
    if (okm_len == 0 || okm_len > 255u * HKDF_HASH_LEN) return -1;
    if (info_len > HKDF_INFO_MAX) return -2;

    /* Extract: PRK = HMAC-SHA256(salt, IKM). An empty salt is RFC-safe: HMAC's
     * sub-block-length key zero-padding == RFC 5869's "HashLen zero bytes". */
    uint8_t prk[HKDF_HASH_LEN];
    arkiv_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    /* Expand: T(0)="" ; T(i)=HMAC(PRK, T(i-1) || info || i) ; OKM = T(1)|T(2)|... */
    uint8_t t[HKDF_HASH_LEN];
    uint8_t blk[HKDF_HASH_LEN + HKDF_INFO_MAX + 1];
    size_t  t_len = 0;
    size_t  done = 0;
    uint8_t counter = 1;

    while (done < okm_len) {
        size_t off = 0;
        if (t_len) { memcpy(blk, t, t_len); off += t_len; }
        if (info_len) { memcpy(blk + off, info, info_len); off += info_len; }
        blk[off++] = counter;

        arkiv_hmac_sha256(prk, HKDF_HASH_LEN, blk, off, t);
        t_len = HKDF_HASH_LEN;

        size_t take = okm_len - done;
        if (take > HKDF_HASH_LEN) take = HKDF_HASH_LEN;
        memcpy(okm + done, t, take);
        done += take;
        counter++;   /* L<=255*32 guaranteed above, so no wrap */
    }

    secure_zero(prk, sizeof(prk));
    secure_zero(t, sizeof(t));
    secure_zero(blk, sizeof(blk));
    return 0;
}

/* ----------------------------------------------------------- AES-256-GCM */

#if defined(ARKIV_AEAD_HOST_GCM)

int arkiv_aead_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t *ct, uint8_t tag[16])
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return -1;
    int ok = 0, outl = 0;
    if (EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(c, NULL, NULL, key, nonce) != 1) goto done;
    if (aad_len && EVP_EncryptUpdate(c, NULL, &outl, aad, (int)aad_len) != 1) goto done;
    if (pt_len && EVP_EncryptUpdate(c, ct, &outl, pt, (int)pt_len) != 1) goto done;
    if (EVP_EncryptFinal_ex(c, ct + outl, &outl) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -2;
}

int arkiv_aead_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ct, size_t ct_len,
                           const uint8_t tag[16], uint8_t *pt)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    if (!c) return -1;
    int rc = -2, outl = 0;
    uint8_t tagcpy[16];
    memcpy(tagcpy, tag, 16);
    if (EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(c, NULL, NULL, key, nonce) != 1) goto done;
    if (aad_len && EVP_DecryptUpdate(c, NULL, &outl, aad, (int)aad_len) != 1) goto done;
    if (ct_len && EVP_DecryptUpdate(c, pt, &outl, ct, (int)ct_len) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tagcpy) != 1) goto done;
    rc = (EVP_DecryptFinal_ex(c, pt + outl, &outl) == 1) ? 0 : ARKIV_AEAD_AUTH_FAIL;
done:
    EVP_CIPHER_CTX_free(c);
    return rc;
}

#else  /* firmware: mbedtls */

int arkiv_aead_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t *ct, uint8_t tag[16])
{
    /* Heap context: mbedtls_gcm_context embeds an AES context (~300 B); keep it
     * off the 4 KB wups_rx/ack task stacks. */
    mbedtls_gcm_context *ctx = (mbedtls_gcm_context *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    mbedtls_gcm_init(ctx);
    int rc = mbedtls_gcm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(ctx, MBEDTLS_GCM_ENCRYPT, pt_len,
                                       nonce, 12, aad, aad_len, pt, ct, 16, tag);
    }
    mbedtls_gcm_free(ctx);   /* free on every path */
    free(ctx);
    return rc == 0 ? 0 : -2;
}

int arkiv_aead_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ct, size_t ct_len,
                           const uint8_t tag[16], uint8_t *pt)
{
    mbedtls_gcm_context *ctx = (mbedtls_gcm_context *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    mbedtls_gcm_init(ctx);
    int rc = mbedtls_gcm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        /* mbedtls_gcm_auth_decrypt is constant-time and writes no plaintext on
         * tag mismatch (returns MBEDTLS_ERR_GCM_AUTH_FAILED). */
        rc = mbedtls_gcm_auth_decrypt(ctx, ct_len, nonce, 12, aad, aad_len,
                                      tag, 16, ct, pt);
    }
    mbedtls_gcm_free(ctx);
    free(ctx);
    if (rc == 0) return 0;
    return (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) ? ARKIV_AEAD_AUTH_FAIL : -2;
}

#endif

/* --------------------------------------------------- Arkiv seal / key schedule */

static const char *purpose_for_type(uint8_t typeTag)
{
    switch (typeTag) {
    case ARKIV_AEAD_TYPE_TELEMETRY: return "telemetry";
    case ARKIV_AEAD_TYPE_ACK:       return "ack";
    case ARKIV_AEAD_TYPE_EVENT:     return "event";
    default:                        return NULL;
    }
}

int arkiv_aead_derive_key(const uint8_t shared_x[32],
                          const char *owner_addr_lower,
                          const char *device_pubhex_lower,
                          uint8_t typeTag, uint32_t epoch,
                          uint8_t k_dir[32])
{
    const char *purpose = purpose_for_type(typeTag);
    if (!purpose || !owner_addr_lower || !device_pubhex_lower) return -1;

    /* salt = "<owner_addr_lower>|<device_pubkey_128hex_lower>"  (credentials.ts). */
    char salt[200];
    int sl = snprintf(salt, sizeof(salt), "%s|%s", owner_addr_lower, device_pubhex_lower);
    if (sl <= 0 || (size_t)sl >= sizeof(salt)) return -2;

    /* info = "w3pups-arkiv-<purpose>|epoch=<decimal>"  (credentials.ts). */
    char info[64];
    int il = snprintf(info, sizeof(info), "w3pups-arkiv-%s|epoch=%u", purpose, (unsigned)epoch);
    if (il <= 0 || (size_t)il >= sizeof(info)) return -3;

    int rc = arkiv_hkdf_sha256(shared_x, 32,
                               (const uint8_t *)salt, (size_t)sl,
                               (const uint8_t *)info, (size_t)il,
                               k_dir, 32);
    secure_zero(salt, sizeof(salt));
    return rc;
}

/* nonce/header = 0x03 | typeTag | epoch_BE32 | seq_BE48 (low 48 bits of seq). */
static void pack_nonce(uint8_t n[ARKIV_AEAD_HDR_LEN], uint8_t typeTag, uint32_t epoch, uint64_t seq)
{
    n[0] = ARKIV_AEAD_SCHEME;
    n[1] = typeTag;
    n[2] = (uint8_t)(epoch >> 24); n[3] = (uint8_t)(epoch >> 16);
    n[4] = (uint8_t)(epoch >> 8);  n[5] = (uint8_t)(epoch);
    n[6]  = (uint8_t)(seq >> 40); n[7]  = (uint8_t)(seq >> 32);
    n[8]  = (uint8_t)(seq >> 24); n[9]  = (uint8_t)(seq >> 16);
    n[10] = (uint8_t)(seq >> 8);  n[11] = (uint8_t)(seq);
}

/* AAD = nonce(12) || device_id || entity_type || (command_id or "").
 * The reader rebuilds this byte-for-byte; editing any plaintext attribute breaks the tag. */
static size_t build_aad(uint8_t *aad, size_t cap, const uint8_t nonce[ARKIV_AEAD_HDR_LEN],
                        const char *device_id, const char *entity_type, const char *command_id)
{
    size_t dl = device_id ? strlen(device_id) : 0;
    size_t el = entity_type ? strlen(entity_type) : 0;
    size_t cl = command_id ? strlen(command_id) : 0;
    size_t need = ARKIV_AEAD_HDR_LEN + dl + el + cl;
    if (need > cap) return 0;
    size_t o = 0;
    memcpy(aad + o, nonce, ARKIV_AEAD_HDR_LEN); o += ARKIV_AEAD_HDR_LEN;
    if (dl) { memcpy(aad + o, device_id, dl);   o += dl; }
    if (el) { memcpy(aad + o, entity_type, el); o += el; }
    if (cl) { memcpy(aad + o, command_id, cl);  o += cl; }
    return o;
}

int arkiv_aead_seal(uint8_t typeTag, uint32_t epoch, uint64_t seq, const uint8_t k_dir[32],
                    const char *device_id, const char *entity_type, const char *command_id_or_null,
                    const uint8_t *pt, size_t pt_len,
                    uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!purpose_for_type(typeTag)) return -1;
    if (out_cap < pt_len + ARKIV_AEAD_OVERHEAD) return -2;

    uint8_t nonce[ARKIV_AEAD_HDR_LEN];
    pack_nonce(nonce, typeTag, epoch, seq);

    uint8_t aad[ARKIV_AEAD_HDR_LEN + 256];
    size_t aad_len = build_aad(aad, sizeof(aad), nonce, device_id, entity_type, command_id_or_null);
    if (!aad_len) return -3;

    memcpy(out, nonce, ARKIV_AEAD_HDR_LEN);   /* header == nonce (reconstructed by reader) */
    uint8_t tag[ARKIV_AEAD_TAG_LEN];
    int rc = arkiv_aead_gcm_encrypt(k_dir, nonce, aad, aad_len,
                                    pt, pt_len, out + ARKIV_AEAD_HDR_LEN, tag);
    if (rc) return -4;
    memcpy(out + ARKIV_AEAD_HDR_LEN + pt_len, tag, ARKIV_AEAD_TAG_LEN);
    *out_len = pt_len + ARKIV_AEAD_OVERHEAD;
    return 0;
}

int arkiv_aead_open(const uint8_t *body, size_t body_len, const uint8_t k_dir[32],
                    const char *device_id, const char *entity_type, const char *command_id_or_null,
                    uint8_t *pt, size_t pt_cap, size_t *pt_len)
{
    if (body_len < ARKIV_AEAD_OVERHEAD) return -1;
    if (body[0] != ARKIV_AEAD_SCHEME)   return -2;
    size_t ct_len = body_len - ARKIV_AEAD_OVERHEAD;
    if (pt_cap < ct_len) return -3;

    const uint8_t *nonce = body;                 /* first 12 bytes == nonce */
    uint8_t aad[ARKIV_AEAD_HDR_LEN + 256];
    size_t aad_len = build_aad(aad, sizeof(aad), nonce, device_id, entity_type, command_id_or_null);
    if (!aad_len) return -4;

    const uint8_t *ct  = body + ARKIV_AEAD_HDR_LEN;
    const uint8_t *tag = body + ARKIV_AEAD_HDR_LEN + ct_len;
    int rc = arkiv_aead_gcm_decrypt(k_dir, nonce, aad, aad_len, ct, ct_len, tag, pt);
    if (rc) return rc;   /* ARKIV_AEAD_AUTH_FAIL on tamper, else other <0 */
    *pt_len = ct_len;
    return 0;
}
