/*
 * Minimal SHA-256 (FIPS 180-4) for the OTA-2 fw_xfer receiver.
 *
 * Self-contained public-domain-style implementation (the classic
 * init/update/final streaming shape popularised by Brad Conte's
 * crypto-algorithms), rewritten here so the GPL-3 firmware carries no
 * external licence baggage. Used only to verify the staged firmware
 * image against the raw digest carried in net.fw_xfer_begin — a few
 * hundred KB once per update, so speed is irrelevant.
 *
 * Streaming API so the ~300 KB staged image can be hashed in 4 KB
 * slices without a contiguous RAM buffer.
 */
#ifndef FW_SHA256_H
#define FW_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} fw_sha256_ctx;

void fw_sha256_init(fw_sha256_ctx *ctx);
void fw_sha256_update(fw_sha256_ctx *ctx, const uint8_t *data, size_t len);
/* Writes the 32-byte big-endian digest to `out`. */
void fw_sha256_final(fw_sha256_ctx *ctx, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* FW_SHA256_H */
