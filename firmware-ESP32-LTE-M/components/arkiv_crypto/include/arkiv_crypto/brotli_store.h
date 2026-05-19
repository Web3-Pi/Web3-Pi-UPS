#pragma once
/*
 * Minimal Brotli encoder that wraps the input as a single uncompressed
 * meta-block followed by an empty last meta-block (RFC 7932 "store" mode).
 *
 * Arkiv requires tx.data to be Brotli-compressed even though the payload is
 * already compact RLP. A valid Brotli stream with uncompressed meta-blocks
 * decodes to the original bytes, so no actual compression work is needed.
 */
#include <cstdint>
#include <vector>

namespace arkiv {

std::vector<uint8_t> brotli_store_encode(const std::vector<uint8_t>& input);

}  // namespace arkiv
