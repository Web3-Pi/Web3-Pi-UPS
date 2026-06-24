/* Unified host cross-impl KAT for arkiv_crypto (ADR-0011 P3 / ESP-FW-6).
 *
 * Exercises the REAL firmware code paths on host against node/OpenSSL/noble
 * golden vectors (gen_kat.mjs), no hardware:
 *   - ECDH   : micro-ecc uECC_shared_secret (the primitive arkiv_secp256k1_ecdh wraps)
 *   - HKDF   : aead.c arkiv_hkdf_sha256 (HMAC supplied by CommonCrypto CCHmac)
 *   - GCM    : aead.c arkiv_aead_gcm_encrypt/decrypt (-DARKIV_AEAD_HOST_GCM -> OpenSSL)
 *   - SEAL   : aead.c arkiv_aead_seal/open (added in step 4)
 *
 * HMAC/GCM standards are unambiguous, so CCHmac/OpenSSL == the firmware's
 * mbedtls; this proves the wrapper logic + byte layout. The mbedtls path itself
 * is confirmed on-device by the self-test in step 13.
 */
#include <stdio.h>
#include <string.h>
#include <CommonCrypto/CommonHMAC.h>
#include "uECC.h"
#include "arkiv_crypto/aead.h"
#include "kat_vectors.h"
#include "hkdf_vectors.h"
#include "gcm_vectors.h"
#include "seal_vectors.h"

/* Symbol aead.c (HKDF) calls — same HMAC-SHA256 the device computes via mbedtls. */
void arkiv_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    CCHmac(kCCHmacAlgSHA256, key, key_len, msg, msg_len, out);
}

static int host_rng(uint8_t *dest, unsigned size) {
    for (unsigned i = 0; i < size; i++) dest[i] = (uint8_t)(i * 7u + 1u);
    return 1;
}

static int fails = 0;
static void px(const char *l, const uint8_t *b, int n) {
    printf("  %s", l); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n");
}
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); fails++; } } while (0)

static void test_ecdh(void) {
    uECC_set_rng(host_rng);
    uECC_Curve c = uECC_secp256k1();
    CHECK(uECC_valid_public_key(kat_owner_pub, c) == 1, "valid_public_key(owner)");
    CHECK(uECC_valid_public_key(kat_dev_pub, c) == 1, "valid_public_key(dev)");
    uint8_t x1[32], x2[32];
    CHECK(uECC_shared_secret(kat_owner_pub, kat_dev_priv, x1, c) == 1, "shared_secret dev*owner");
    CHECK(memcmp(x1, kat_expected_shared_x, 32) == 0, "ECDH X dev*owner == ref");
    CHECK(uECC_shared_secret(kat_dev_pub, kat_owner_priv, x2, c) == 1, "shared_secret owner*dev");
    CHECK(memcmp(x2, kat_expected_shared_x, 32) == 0, "ECDH symmetric");
    uint8_t zero[64] = {0};
    CHECK(uECC_valid_public_key(zero, c) != 1, "zero key rejected");
    if (!fails) printf("ECDH  PASS (micro-ecc == OpenSSL/noble; symmetric; zero rejected)\n");
}

static void test_hkdf(void) {
    int before = fails;
    uint8_t okm[32];
    CHECK(arkiv_hkdf_sha256(kat_hkdf_ikm, sizeof(kat_hkdf_ikm),
            (const uint8_t *)kat_hkdf_salt, strlen(kat_hkdf_salt),
            (const uint8_t *)kat_hkdf_info, strlen(kat_hkdf_info),
            okm, sizeof(okm)) == 0, "hkdf rc");
    CHECK(memcmp(okm, kat_hkdf_expected_okm, 32) == 0, "HKDF okm == node hkdfSync");
    if (fails == before) printf("HKDF  PASS (aead.c == node hkdfSync, credentials.ts salt/info)\n");
    else px("got", okm, 32), px("exp", kat_hkdf_expected_okm, 32);
}

static void test_gcm(void) {
    int before = fails;
    uint8_t ct[256], tag[16], pt[256];
    size_t ptlen = sizeof(kat_gcm_pt);
    CHECK(arkiv_aead_gcm_encrypt(kat_gcm_key, kat_gcm_nonce,
            kat_gcm_aad, sizeof(kat_gcm_aad), kat_gcm_pt, ptlen, ct, tag) == 0, "gcm enc rc");
    CHECK(memcmp(ct, kat_gcm_ct, ptlen) == 0, "GCM ct == node");
    CHECK(memcmp(tag, kat_gcm_tag, 16) == 0, "GCM tag == node");

    /* round-trip */
    CHECK(arkiv_aead_gcm_decrypt(kat_gcm_key, kat_gcm_nonce,
            kat_gcm_aad, sizeof(kat_gcm_aad), kat_gcm_ct, ptlen, kat_gcm_tag, pt) == 0, "gcm dec rc");
    CHECK(memcmp(pt, kat_gcm_pt, ptlen) == 0, "GCM round-trip pt");

    /* tamper tag -> AUTH_FAIL */
    uint8_t bad_tag[16]; memcpy(bad_tag, kat_gcm_tag, 16); bad_tag[0] ^= 0x01;
    CHECK(arkiv_aead_gcm_decrypt(kat_gcm_key, kat_gcm_nonce,
            kat_gcm_aad, sizeof(kat_gcm_aad), kat_gcm_ct, ptlen, bad_tag, pt) == ARKIV_AEAD_AUTH_FAIL,
          "tampered tag rejected");
    /* tamper AAD -> AUTH_FAIL */
    uint8_t bad_aad[sizeof(kat_gcm_aad)]; memcpy(bad_aad, kat_gcm_aad, sizeof(kat_gcm_aad)); bad_aad[0] ^= 0x01;
    CHECK(arkiv_aead_gcm_decrypt(kat_gcm_key, kat_gcm_nonce,
            bad_aad, sizeof(bad_aad), kat_gcm_ct, ptlen, kat_gcm_tag, pt) == ARKIV_AEAD_AUTH_FAIL,
          "tampered AAD rejected");
    /* tamper ciphertext -> AUTH_FAIL */
    uint8_t bad_ct[256]; memcpy(bad_ct, kat_gcm_ct, ptlen); bad_ct[0] ^= 0x01;
    CHECK(arkiv_aead_gcm_decrypt(kat_gcm_key, kat_gcm_nonce,
            kat_gcm_aad, sizeof(kat_gcm_aad), bad_ct, ptlen, kat_gcm_tag, pt) == ARKIV_AEAD_AUTH_FAIL,
          "tampered ciphertext rejected");

    if (fails == before) printf("GCM   PASS (aead.c == node aes-256-gcm; round-trip; tamper rejected x3)\n");
}

static void test_seal(void) {
    int before = fails;
    uint8_t kdir[32], body[512], pt[512];
    size_t body_len = 0, pt_len = 0;

    /* --- telemetry: derive K_dir, seal byte-exact == JS, open round-trip --- */
    CHECK(arkiv_aead_derive_key(seal_shared_x, seal_owner_lower, seal_devpub_lower,
            SEAL_TLM_TYPETAG, SEAL_TLM_EPOCH, kdir) == 0, "derive tlm rc");
    CHECK(memcmp(kdir, seal_tlm_kdir, 32) == 0, "K_dir(tlm) == JS hkdf");
    CHECK(arkiv_aead_seal(SEAL_TLM_TYPETAG, SEAL_TLM_EPOCH, SEAL_TLM_SEQ, kdir,
            seal_device_id, seal_tlm_entity, NULL,
            seal_tlm_pt, sizeof(seal_tlm_pt), body, sizeof(body), &body_len) == 0, "seal tlm rc");
    CHECK(body_len == sizeof(seal_tlm_body), "tlm body_len");
    CHECK(memcmp(body, seal_tlm_body, sizeof(seal_tlm_body)) == 0, "SEAL tlm body == JS (byte-exact)");
    CHECK(arkiv_aead_open(seal_tlm_body, sizeof(seal_tlm_body), kdir,
            seal_device_id, seal_tlm_entity, NULL, pt, sizeof(pt), &pt_len) == 0, "open tlm rc");
    CHECK(pt_len == sizeof(seal_tlm_pt) && memcmp(pt, seal_tlm_pt, pt_len) == 0, "open tlm round-trip");

    /* tamper device_id (a plaintext attribute) -> AAD changes -> AUTH_FAIL */
    CHECK(arkiv_aead_open(seal_tlm_body, sizeof(seal_tlm_body), kdir,
            "8988228066614189921", seal_tlm_entity, NULL, pt, sizeof(pt), &pt_len) == ARKIV_AEAD_AUTH_FAIL,
          "tampered device_id rejected");
    /* wrong epoch in the K_dir (derive at epoch 8) must not decrypt epoch-7 body */
    uint8_t kdir8[32];
    arkiv_aead_derive_key(seal_shared_x, seal_owner_lower, seal_devpub_lower, SEAL_TLM_TYPETAG, 8, kdir8);
    CHECK(arkiv_aead_open(seal_tlm_body, sizeof(seal_tlm_body), kdir8,
            seal_device_id, seal_tlm_entity, NULL, pt, sizeof(pt), &pt_len) == ARKIV_AEAD_AUTH_FAIL,
          "epoch-8 key rejects epoch-7 body (ratchet)");

    /* --- ack: command_id bound into AAD --- */
    uint8_t kack[32];
    CHECK(arkiv_aead_derive_key(seal_shared_x, seal_owner_lower, seal_devpub_lower,
            SEAL_ACK_TYPETAG, SEAL_ACK_EPOCH, kack) == 0, "derive ack rc");
    CHECK(memcmp(kack, seal_ack_kdir, 32) == 0, "K_dir(ack) == JS hkdf (domain-separated)");
    CHECK(memcmp(kack, seal_tlm_kdir, 32) != 0, "ack key != tlm key (purpose domain sep)");
    CHECK(arkiv_aead_seal(SEAL_ACK_TYPETAG, SEAL_ACK_EPOCH, SEAL_ACK_SEQ, kack,
            seal_device_id, seal_ack_entity, seal_ack_cmdid,
            seal_ack_pt, sizeof(seal_ack_pt), body, sizeof(body), &body_len) == 0, "seal ack rc");
    CHECK(memcmp(body, seal_ack_body, sizeof(seal_ack_body)) == 0, "SEAL ack body == JS (byte-exact)");
    CHECK(arkiv_aead_open(seal_ack_body, sizeof(seal_ack_body), kack,
            seal_device_id, seal_ack_entity, seal_ack_cmdid, pt, sizeof(pt), &pt_len) == 0, "open ack rc");
    /* wrong command_id -> AUTH_FAIL (proves command_id binding) */
    CHECK(arkiv_aead_open(seal_ack_body, sizeof(seal_ack_body), kack,
            seal_device_id, seal_ack_entity, "550e8400-e29b-41d4-a716-446655440001",
            pt, sizeof(pt), &pt_len) == ARKIV_AEAD_AUTH_FAIL, "wrong command_id rejected");
    /* cross-stream key misuse: tlm key must not open an ack body */
    CHECK(arkiv_aead_open(seal_ack_body, sizeof(seal_ack_body), seal_tlm_kdir,
            seal_device_id, seal_ack_entity, seal_ack_cmdid, pt, sizeof(pt), &pt_len) == ARKIV_AEAD_AUTH_FAIL,
          "tlm key rejects ack body (cross-stream)");

    if (fails == before) printf("SEAL  PASS (byte-exact tlm+ack body == JS; round-trip; AAD/epoch/cmdid/cross-stream tamper rejected)\n");
}

int main(void) {
    test_ecdh();
    test_hkdf();
    test_gcm();
    test_seal();
    if (fails == 0) { printf("\nALL HOST KATs PASS\n"); return 0; }
    printf("\nHOST KATs FAILED: %d error(s)\n", fails);
    return 1;
}
