#include "arkiv_crypto/ops_tx_data.h"
#include "arkiv_crypto/brotli_store.h"
#include "arkiv_crypto/rlp.h"

namespace arkiv {

static constexpr uint32_t BLOCK_TIME_SECONDS = 2;

static uint32_t seconds_to_blocks(uint32_t seconds)
{
    /* ceil(seconds / BLOCK_TIME) */
    return (seconds + BLOCK_TIME_SECONDS - 1) / BLOCK_TIME_SECONDS;
}

/* Append an encoded attribute pair [keyBytes, valueBytes] to a parent list. */
static void append_attribute_pair(rlp::ListEncoder& parent, const Attribute& a, bool numeric_variant)
{
    rlp::ListEncoder pair;
    pair.append_string(a.key);
    if (numeric_variant) {
        if (a.numericValue == 0) {
            pair.append_string("");   /* special-cased empty-string encoding */
        } else {
            pair.append_uint(static_cast<uint64_t>(a.numericValue));
        }
    } else {
        pair.append_string(a.stringValue);
    }
    std::vector<uint8_t> encoded;
    pair.finish(encoded);
    parent.append_raw(encoded);
}

static std::vector<uint8_t> encode_attribute_list(const std::vector<Attribute>& attrs, bool numeric)
{
    rlp::ListEncoder list;
    for (const auto& a : attrs) {
        if (a.isNumeric != numeric) continue;
        append_attribute_pair(list, a, numeric);
    }
    std::vector<uint8_t> out;
    list.finish(out);
    return out;
}

static std::vector<uint8_t> encode_create(const CreateOp& c)
{
    rlp::ListEncoder item;
    item.append_uint(seconds_to_blocks(c.expiresInSeconds));
    item.append_string(c.contentType);
    item.append_bytes(c.payload);
    item.append_raw(encode_attribute_list(c.attributes, /*numeric=*/false));
    item.append_raw(encode_attribute_list(c.attributes, /*numeric=*/true));
    std::vector<uint8_t> out;
    item.finish(out);
    return out;
}

static std::vector<uint8_t> encode_update(const UpdateOp& u)
{
    rlp::ListEncoder item;
    item.append_bytes(u.entityKey);
    item.append_string(u.contentType);
    item.append_uint(seconds_to_blocks(u.expiresInSeconds));
    item.append_bytes(u.payload);
    item.append_raw(encode_attribute_list(u.attributes, /*numeric=*/false));
    item.append_raw(encode_attribute_list(u.attributes, /*numeric=*/true));
    std::vector<uint8_t> out;
    item.finish(out);
    return out;
}

static std::vector<uint8_t> encode_extension(const ExtensionOp& e)
{
    rlp::ListEncoder item;
    item.append_bytes(e.entityKey);
    item.append_uint(seconds_to_blocks(e.expiresInSeconds));
    std::vector<uint8_t> out;
    item.finish(out);
    return out;
}

static std::vector<uint8_t> encode_ownership_change(const OwnershipChangeOp& o)
{
    rlp::ListEncoder item;
    item.append_bytes(o.entityKey);
    item.append_bytes(o.newOwner);
    std::vector<uint8_t> out;
    item.finish(out);
    return out;
}

std::vector<uint8_t> build_ops_tx_data(const OpsTxData& ops)
{
    rlp::ListEncoder outer;

    rlp::ListEncoder creates;
    for (const auto& c : ops.creates) creates.append_raw(encode_create(c));
    std::vector<uint8_t> cb; creates.finish(cb);
    outer.append_raw(cb);

    rlp::ListEncoder updates;
    for (const auto& u : ops.updates) updates.append_raw(encode_update(u));
    std::vector<uint8_t> ub; updates.finish(ub);
    outer.append_raw(ub);

    rlp::ListEncoder deletes;
    for (const auto& key : ops.deletes) deletes.append_bytes(key);
    std::vector<uint8_t> db; deletes.finish(db);
    outer.append_raw(db);

    rlp::ListEncoder extensions;
    for (const auto& e : ops.extensions) extensions.append_raw(encode_extension(e));
    std::vector<uint8_t> eb; extensions.finish(eb);
    outer.append_raw(eb);

    rlp::ListEncoder ownership;
    for (const auto& o : ops.ownershipChanges) ownership.append_raw(encode_ownership_change(o));
    std::vector<uint8_t> ob; ownership.finish(ob);
    outer.append_raw(ob);

    std::vector<uint8_t> rlp_out;
    outer.finish(rlp_out);
    /* Arkiv expects tx.data = brotli(rlp(ops)). We emit an uncompressed-storage
     * Brotli stream so the node's decompressor yields the RLP bytes unchanged. */
    return brotli_store_encode(rlp_out);
}

}  // namespace arkiv
