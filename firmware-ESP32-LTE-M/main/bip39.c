#include "bip39.h"

#include <string.h>

#include "bip39_wordlist.h"   /* static const char bip39_words[2048][9] */

#define BIP39_BITS_PER_WORD 11
#define BIP39_WORD_COUNT    2048   /* 2^11 */

/* Extract an 11-bit big-endian field starting at absolute bit offset
 * `bit_off` from `data`. Caller guarantees the range is in bounds. */
static uint16_t take11(const uint8_t *data, size_t bit_off)
{
    uint16_t v = 0;
    for (int i = 0; i < BIP39_BITS_PER_WORD; ++i) {
        size_t b = bit_off + (size_t)i;
        uint8_t bit = (uint8_t)((data[b >> 3] >> (7 - (b & 7))) & 1u);
        v = (uint16_t)((v << 1) | bit);
    }
    return v; /* 0..2047 — exactly the wordlist index range */
}

int bip39_words_from_bits(const uint8_t *data, size_t data_len,
                          int nwords, char *out, size_t out_sz)
{
    if (!data || !out || nwords <= 0 || out_sz == 0) return -1;

    size_t need_bits = (size_t)nwords * BIP39_BITS_PER_WORD;
    if (data_len * 8u < need_bits) return -1;

    size_t pos = 0;
    out[0] = '\0';
    for (int w = 0; w < nwords; ++w) {
        uint16_t idx = take11(data, (size_t)w * BIP39_BITS_PER_WORD);
        /* idx is structurally < 2048; assert-by-clamp for total safety. */
        if (idx >= BIP39_WORD_COUNT) return -1;
        const char *word = bip39_words[idx];
        size_t wl = strlen(word);
        size_t add = wl + (w > 0 ? 1u : 0u); /* leading space after word 0 */
        if (pos + add + 1u > out_sz) return -1;
        if (w > 0) out[pos++] = ' ';
        memcpy(out + pos, word, wl);
        pos += wl;
        out[pos] = '\0';
    }
    return (int)pos;
}

int bip39_pack_top_bits(const uint8_t *digest, size_t digest_len,
                         int nbits, uint8_t *out, size_t out_len)
{
    if (!digest || !out || nbits <= 0) return -1;
    size_t nbytes = (size_t)(nbits + 7) / 8u;
    if (digest_len * 8u < (size_t)nbits || out_len < nbytes) return -1;

    for (size_t i = 0; i < nbytes; ++i) out[i] = 0;
    for (int i = 0; i < nbits; ++i) {
        uint8_t bit = (uint8_t)((digest[i >> 3] >> (7 - (i & 7))) & 1u);
        out[i >> 3] = (uint8_t)(out[i >> 3] | (bit << (7 - (i & 7))));
    }
    /* Trailing low bits of the final byte are already 0 (zero-init). */
    return (int)nbytes;
}
