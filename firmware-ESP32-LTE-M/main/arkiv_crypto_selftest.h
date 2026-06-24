#pragma once
#include <stdbool.h>

/* Boot-time arkiv_crypto KAT vs the committed golden vectors. Proven on real
 * ESP32 silicon (mbedtls + HW AES == host/JS vectors); off by default to keep
 * boot lean. Flip to 1 to re-run it (e.g. after touching the crypto path). */
#define ARKIV_CRYPTO_SELFTEST 0

#ifdef __cplusplus
extern "C" {
#endif

/* Run the arkiv_crypto known-answer tests against the committed golden vectors
 * (components/arkiv_crypto/test/, cross-verified there vs node/OpenSSL/@noble).
 * Exercises the REAL on-device path — micro-ecc ECDH, mbedtls SHA-256 HMAC/HKDF,
 * and mbedtls + ESP32 hardware AES-256-GCM — and asserts byte-exact equality
 * with the host/JS vectors. Logs PASS/FAIL per stage. Returns true iff all pass. */
bool arkiv_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
