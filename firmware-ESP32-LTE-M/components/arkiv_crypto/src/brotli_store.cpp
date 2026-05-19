/*
 * Minimal Brotli "store" encoder — see header for rationale.
 *
 * Bit layout (all bit fields written LSB-first within each byte):
 *   Stream header:
 *     1 bit: WBITS selector — 0 means WBITS = 16 (smallest valid)
 *
 *   Meta-block 1 (uncompressed):
 *     1 bit:  ISLAST       = 0
 *     2 bits: MNIBBLES     = 00/01/10 → 4/5/6 nibbles of MLEN-1
 *     4*N:    MLEN-1       LSB-nibble first
 *     1 bit:  ISUNCOMPRESSED = 1
 *     pad:    zero bits up to next byte boundary
 *     MLEN bytes: raw data
 *
 *   Meta-block 2 (empty last):
 *     1 bit:  ISLAST       = 1
 *     1 bit:  ISLASTEMPTY  = 1
 *     pad:    zero bits up to next byte boundary
 *
 * Splits input into <=65536-byte chunks so MNIBBLES=4 always suffices.
 */
#include "arkiv_crypto/brotli_store.h"
#include <cstddef>

namespace arkiv {

namespace {

struct BitWriter {
    std::vector<uint8_t>& buf;
    uint8_t cur = 0;
    int bits_in_cur = 0;

    explicit BitWriter(std::vector<uint8_t>& out) : buf(out) {}

    void write_bit(unsigned b) {
        cur |= static_cast<uint8_t>((b & 1u) << bits_in_cur);
        if (++bits_in_cur == 8) flush_byte();
    }

    void write_bits(uint32_t value, int n) {
        for (int i = 0; i < n; i++) write_bit((value >> i) & 1u);
    }

    /* Align to next byte boundary, padding with zero bits. */
    void align_to_byte() {
        if (bits_in_cur != 0) flush_byte();
    }

    void flush_byte() {
        buf.push_back(cur);
        cur = 0;
        bits_in_cur = 0;
    }

    void finish() {
        if (bits_in_cur != 0) flush_byte();
    }
};

void write_uncompressed_chunk(BitWriter& bw, const uint8_t* data, size_t len, bool is_last)
{
    /* is_last is false for regular chunks — we always follow up with an empty
     * ISLASTEMPTY=1 metablock because uncompressed chunks cannot themselves be
     * marked ISLAST per RFC 7932 §9.2. */
    (void)is_last;
    bw.write_bit(0);                 /* ISLAST      = 0 */
    bw.write_bits(0, 2);             /* MNIBBLES    = 00 → 4 nibbles */
    bw.write_bits(static_cast<uint32_t>(len - 1), 16);
    bw.write_bit(1);                 /* ISUNCOMPRESSED = 1 */
    bw.align_to_byte();
    for (size_t i = 0; i < len; i++) bw.buf.push_back(data[i]);
    /* After writing raw bytes we are byte-aligned; next metablock header
     * starts from bit 0 of the next byte. */
}

}  // namespace

std::vector<uint8_t> brotli_store_encode(const std::vector<uint8_t>& input)
{
    std::vector<uint8_t> out;
    out.reserve(input.size() + 16);
    BitWriter bw(out);

    /* Stream header: WBITS = 16 encoded as a single '0' bit. */
    bw.write_bit(0);

    constexpr size_t kMaxChunk = 65536;
    size_t offset = 0;
    if (input.empty()) {
        /* Directly emit an empty last metablock. */
        bw.write_bit(1);   /* ISLAST */
        bw.write_bit(1);   /* ISLASTEMPTY */
        bw.align_to_byte();
        return out;
    }
    while (offset < input.size()) {
        size_t remaining = input.size() - offset;
        size_t chunk = remaining > kMaxChunk ? kMaxChunk : remaining;
        write_uncompressed_chunk(bw, input.data() + offset, chunk, /*is_last=*/false);
        offset += chunk;
    }

    /* Terminating empty last metablock. */
    bw.write_bit(1);   /* ISLAST      = 1 */
    bw.write_bit(1);   /* ISLASTEMPTY = 1 */
    bw.align_to_byte();

    return out;
}

}  // namespace arkiv
