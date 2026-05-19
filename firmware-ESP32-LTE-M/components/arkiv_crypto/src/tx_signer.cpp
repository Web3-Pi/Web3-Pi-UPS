#include "arkiv_crypto/tx_signer.h"
#include "arkiv_crypto/rlp.h"
#include "arkiv_crypto/keccak256.h"
#include "arkiv_crypto/secp256k1.h"

namespace arkiv::tx {

namespace {

void encode_tx_fields(rlp::ListEncoder& l, const LegacyTx& tx) {
    l.append_uint(tx.nonce);
    l.append_uint(tx.gas_price_wei);
    l.append_uint(tx.gas_limit);
    l.append_bytes(tx.to);
    l.append_uint(tx.value_wei);
    l.append_bytes(tx.data);
}

/* Minimal big-endian bytes of a 256-bit integer stored as 32 bytes. Strips
 * leading zeros; result of 0 → empty. */
std::vector<uint8_t> strip_leading_zeros(const uint8_t b[32]) {
    size_t start = 0;
    while (start < 32 && b[start] == 0) start++;
    return std::vector<uint8_t>(b + start, b + 32);
}

}  // namespace

std::vector<uint8_t> sign_legacy_tx(const LegacyTx& tx, const uint8_t priv[32]) {
    /* 1. Build the signing preimage: rlp([nonce, gasPrice, gasLimit, to, value, data, chainId, 0, 0]). */
    rlp::ListEncoder preimage;
    encode_tx_fields(preimage, tx);
    preimage.append_uint(tx.chain_id);
    preimage.append_uint(0);
    preimage.append_uint(0);
    std::vector<uint8_t> pre_rlp;
    preimage.finish(pre_rlp);

    uint8_t digest[32];
    arkiv_keccak256(pre_rlp.data(), pre_rlp.size(), digest);

    /* 2. Sign. */
    uint8_t sig[64];
    int recid = arkiv_secp256k1_sign_recoverable(priv, digest, sig);
    if (recid < 0) return {};

    /* 3. Compose final tx: rlp([nonce, gasPrice, gasLimit, to, value, data, v, r, s]) */
    rlp::ListEncoder final_tx;
    encode_tx_fields(final_tx, tx);
    /* v = chainId * 2 + 35 + recid */
    final_tx.append_uint(tx.chain_id * 2 + 35 + static_cast<uint64_t>(recid));
    /* r and s are encoded as byte strings with leading zeros stripped. */
    auto r_bytes = strip_leading_zeros(sig);
    auto s_bytes = strip_leading_zeros(sig + 32);
    final_tx.append_bytes(r_bytes);
    final_tx.append_bytes(s_bytes);
    std::vector<uint8_t> out;
    final_tx.finish(out);
    return out;
}

void tx_hash(const std::vector<uint8_t>& signed_raw, uint8_t out[32]) {
    arkiv_keccak256(signed_raw.data(), signed_raw.size(), out);
}

}  // namespace arkiv::tx
