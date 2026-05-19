/*
 * Keccak-256 (Ethereum variant) — CC0 / public domain.
 * Rate = 1088 bits (136 bytes), capacity = 512 bits, pad byte = 0x01.
 * Implements the canonical Keccak-f[1600] permutation.
 */
#include "arkiv_crypto/keccak256.h"
#include <string.h>

#define KECCAK_RATE 136  /* 1088 bits */

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

/* rho offsets indexed by (x, y) with (x, y) -> 5y + x */
static const unsigned RHO[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14
};

static inline uint64_t rotl64(uint64_t x, unsigned n)
{
    return (x << n) | (x >> (64 - n));
}

static inline uint64_t load64_le(const uint8_t* p)
{
    return (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static inline void store64_le(uint8_t* p, uint64_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static void keccakf(uint64_t s[25])
{
    uint64_t C[5];
    uint64_t B[25];

    for (unsigned r = 0; r < 24; r++) {
        /* theta */
        for (unsigned x = 0; x < 5; x++) {
            C[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        }
        for (unsigned x = 0; x < 5; x++) {
            uint64_t D = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
            for (unsigned y = 0; y < 25; y += 5) {
                s[y + x] ^= D;
            }
        }

        /* rho + pi: B[y, 2x+3y] = rotl(A[x,y], rho[x,y]) */
        for (unsigned x = 0; x < 5; x++) {
            for (unsigned y = 0; y < 5; y++) {
                unsigned src = 5 * y + x;
                unsigned dst = 5 * ((2 * x + 3 * y) % 5) + y;
                B[dst] = rotl64(s[src], RHO[src]);
            }
        }

        /* chi */
        for (unsigned y = 0; y < 25; y += 5) {
            uint64_t a0 = B[y], a1 = B[y + 1], a2 = B[y + 2], a3 = B[y + 3], a4 = B[y + 4];
            s[y + 0] = a0 ^ ((~a1) & a2);
            s[y + 1] = a1 ^ ((~a2) & a3);
            s[y + 2] = a2 ^ ((~a3) & a4);
            s[y + 3] = a3 ^ ((~a4) & a0);
            s[y + 4] = a4 ^ ((~a0) & a1);
        }

        /* iota */
        s[0] ^= RC[r];
    }
}

static void absorb_block(uint64_t s[25], const uint8_t* block)
{
    for (unsigned i = 0; i < KECCAK_RATE / 8; i++) {
        s[i] ^= load64_le(block + i * 8);
    }
    keccakf(s);
}

void arkiv_keccak256_init(arkiv_keccak256_ctx* ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void arkiv_keccak256_update(arkiv_keccak256_ctx* ctx, const uint8_t* data, size_t len)
{
    if (ctx->buf_len > 0) {
        size_t take = KECCAK_RATE - ctx->buf_len;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len  -= take;
        if (ctx->buf_len == KECCAK_RATE) {
            absorb_block(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len >= KECCAK_RATE) {
        absorb_block(ctx->state, data);
        data += KECCAK_RATE;
        len  -= KECCAK_RATE;
    }
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

void arkiv_keccak256_finish(arkiv_keccak256_ctx* ctx, uint8_t out[32])
{
    uint8_t pad[KECCAK_RATE];
    memset(pad, 0, sizeof(pad));
    if (ctx->buf_len > 0) {
        memcpy(pad, ctx->buf, ctx->buf_len);
    }
    pad[ctx->buf_len] = 0x01;           /* Keccak (Ethereum) domain separator */
    pad[KECCAK_RATE - 1] |= 0x80;
    absorb_block(ctx->state, pad);

    uint8_t buf[KECCAK_RATE];
    for (unsigned i = 0; i < KECCAK_RATE / 8; i++) {
        store64_le(buf + i * 8, ctx->state[i]);
    }
    memcpy(out, buf, 32);
}

void arkiv_keccak256(const uint8_t* input, size_t len, uint8_t out[32])
{
    arkiv_keccak256_ctx ctx;
    arkiv_keccak256_init(&ctx);
    arkiv_keccak256_update(&ctx, input, len);
    arkiv_keccak256_finish(&ctx, out);
}
