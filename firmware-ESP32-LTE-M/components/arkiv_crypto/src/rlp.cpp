/*
 * Minimal RLP encoder for Arkiv transactions.
 */
#include "arkiv_crypto/rlp.h"
#include <cstring>

namespace arkiv::rlp {

namespace {

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void emit_length(size_t len, uint8_t short_base, uint8_t long_base, std::vector<uint8_t>& out)
{
    if (len < 56) {
        out.push_back(static_cast<uint8_t>(short_base + len));
        return;
    }
    /* Widen to 64-bit: on RV32 size_t is 32-bit, so `len >> 56` is a shift wider than the
     * type — UB on the standard, and RV32 masks the shift count to 5 bits so the high
     * byte loop aliases back onto the low byte, producing bogus 5-byte length headers. */
    uint64_t len64 = static_cast<uint64_t>(len);
    uint8_t be[8];
    size_t be_len = 0;
    for (int i = 7; i >= 0; i--) {
        uint8_t b = static_cast<uint8_t>((len64 >> (i * 8)) & 0xff);
        if (be_len > 0 || b != 0) {
            be[be_len++] = b;
        }
    }
    out.push_back(static_cast<uint8_t>(long_base + be_len));
    for (size_t i = 0; i < be_len; i++) {
        out.push_back(be[i]);
    }
}

}  // namespace

void encode_bytes(const uint8_t* data, size_t len, std::vector<uint8_t>& out)
{
    if (len == 1 && data[0] < 0x80) {
        out.push_back(data[0]);
        return;
    }
    emit_length(len, 0x80, 0xb7, out);
    out.insert(out.end(), data, data + len);
}

void encode_bytes(const std::vector<uint8_t>& v, std::vector<uint8_t>& out)
{
    encode_bytes(v.data(), v.size(), out);
}

void encode_string(std::string_view s, std::vector<uint8_t>& out)
{
    encode_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
}

void encode_uint(uint64_t value, std::vector<uint8_t>& out)
{
    if (value == 0) {
        out.push_back(0x80);
        return;
    }
    uint8_t be[8];
    size_t len = 0;
    for (int i = 7; i >= 0; i--) {
        uint8_t b = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
        if (len > 0 || b != 0) {
            be[len++] = b;
        }
    }
    encode_bytes(be, len, out);
}

void encode_hex(std::string_view hex, std::vector<uint8_t>& out)
{
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.remove_prefix(2);
    }
    std::vector<uint8_t> raw;
    raw.reserve(hex.size() / 2 + 1);
    if (hex.size() % 2 != 0) {
        int n = hex_nibble(hex[0]);
        if (n < 0) n = 0;
        raw.push_back(static_cast<uint8_t>(n));
        hex.remove_prefix(1);
    }
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) { hi = lo = 0; }
        raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    encode_bytes(raw.data(), raw.size(), out);
}

void encode_list_raw(const std::vector<uint8_t>& children, std::vector<uint8_t>& out)
{
    emit_length(children.size(), 0xc0, 0xf7, out);
    out.insert(out.end(), children.begin(), children.end());
}

/* --------------------------------------------------------- ListEncoder */

void ListEncoder::append_bytes(const uint8_t* data, size_t len)
{
    encode_bytes(data, len, _children);
}

void ListEncoder::append_bytes(const std::vector<uint8_t>& v)
{
    encode_bytes(v, _children);
}

void ListEncoder::append_string(std::string_view s)
{
    encode_string(s, _children);
}

void ListEncoder::append_uint(uint64_t v)
{
    encode_uint(v, _children);
}

void ListEncoder::append_hex(std::string_view hex)
{
    encode_hex(hex, _children);
}

void ListEncoder::append_raw(const std::vector<uint8_t>& already_encoded)
{
    _children.insert(_children.end(), already_encoded.begin(), already_encoded.end());
}

void ListEncoder::finish(std::vector<uint8_t>& out) const
{
    encode_list_raw(_children, out);
}

}  // namespace arkiv::rlp
