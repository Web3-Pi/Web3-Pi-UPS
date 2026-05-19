/*
 * Legacy (EIP-155) Ethereum transaction signing.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace arkiv::tx {

struct LegacyTx {
    uint64_t nonce         = 0;
    uint64_t gas_price_wei = 0;
    uint64_t gas_limit     = 0;
    std::vector<uint8_t> to;          // 20 bytes (may be empty for contract-create, not used here)
    uint64_t value_wei     = 0;       // 0 for Arkiv writes
    std::vector<uint8_t> data;        // RLP-encoded opsTxData for Arkiv, or empty
    uint64_t chain_id      = 0;
};

/* Build and sign a legacy EIP-155 transaction. Returns the raw RLP-encoded
 * signed tx bytes (suitable for eth_sendRawTransaction with "0x" prefix).
 *
 * priv: 32-byte private key (big-endian).
 * Returns empty vector on failure. */
std::vector<uint8_t> sign_legacy_tx(const LegacyTx& tx, const uint8_t priv[32]);

/* Compute the Ethereum tx hash (keccak256 of the signed raw bytes). */
void tx_hash(const std::vector<uint8_t>& signed_raw, uint8_t out[32]);

}  // namespace arkiv::tx
