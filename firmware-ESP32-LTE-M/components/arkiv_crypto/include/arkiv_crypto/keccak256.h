/*
 * Keccak-256 (Ethereum variant, pad byte 0x01 — NOT SHA-3's 0x06).
 * Public-domain implementation bundled with this component.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARKIV_KECCAK256_DIGEST_LEN 32

typedef struct {
    uint64_t state[25];
    uint8_t  buf[136];
    size_t   buf_len;
} arkiv_keccak256_ctx;

void arkiv_keccak256_init(arkiv_keccak256_ctx* ctx);
void arkiv_keccak256_update(arkiv_keccak256_ctx* ctx, const uint8_t* data, size_t len);
void arkiv_keccak256_finish(arkiv_keccak256_ctx* ctx, uint8_t out[32]);

void arkiv_keccak256(const uint8_t* input, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
