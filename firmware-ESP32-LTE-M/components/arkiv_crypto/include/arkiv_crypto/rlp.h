/*
 * Minimal RLP encoder for Arkiv transaction payloads.
 *
 * Conventions:
 *   - encode_uint(0) emits 0x80 (the empty string), matching viem/Ethereum RLP.
 *   - encode_bytes of empty slice also emits 0x80.
 *   - List headers are emitted in canonical short-form (<56 bytes) / long-form.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace arkiv::rlp {

void encode_bytes(const uint8_t* data, size_t len, std::vector<uint8_t>& out);
void encode_bytes(const std::vector<uint8_t>& v, std::vector<uint8_t>& out);
void encode_string(std::string_view s, std::vector<uint8_t>& out);

/* Minimal big-endian encoding of value, stripped of leading zeros.
 * 0 → 0x80 (empty string). */
void encode_uint(uint64_t value, std::vector<uint8_t>& out);

/* Hex string (with or without 0x prefix) → raw bytes → encode_bytes.
 * Leading zeros in the hex representation are preserved as bytes — call
 * encode_uint instead if you want minimal-length encoding. */
void encode_hex(std::string_view hex, std::vector<uint8_t>& out);

/* Prepend a list header sized for `payload_len` onto `out`, then append payload. */
void encode_list_raw(const std::vector<uint8_t>& children, std::vector<uint8_t>& out);

/* Accumulator for a nested list. Children can be any pre-encoded RLP bytes
 * (via append_raw) or individually-encoded primitives. */
class ListEncoder {
public:
    void append_bytes(const uint8_t* data, size_t len);
    void append_bytes(const std::vector<uint8_t>& v);
    void append_string(std::string_view s);
    void append_uint(uint64_t v);
    void append_hex(std::string_view hex);
    void append_raw(const std::vector<uint8_t>& already_encoded);

    /* Emit the full RLP list (header + children) into `out`. */
    void finish(std::vector<uint8_t>& out) const;

    /* Return just the children buffer (caller wraps it). */
    const std::vector<uint8_t>& children() const { return _children; }

private:
    std::vector<uint8_t> _children;
};

}  // namespace arkiv::rlp
