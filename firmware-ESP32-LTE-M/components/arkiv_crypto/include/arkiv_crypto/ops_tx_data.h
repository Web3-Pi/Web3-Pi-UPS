/*
 * Builder for Arkiv transaction `data` payload, matching opsToTxData from
 * arkiv-sdk-js/src/utils/arkivTransactions.ts byte-for-byte.
 *
 *   [ creates[], updates[], deletes[], extensions[], ownershipChanges[] ]
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arkiv {

struct Attribute {
    std::string key;
    std::string stringValue;   // used when !isNumeric
    int64_t     numericValue = 0;
    bool        isNumeric    = false;
};

struct CreateOp {
    std::string contentType;
    std::vector<uint8_t> payload;
    std::vector<Attribute> attributes;
    uint32_t expiresInSeconds = 0;   // converted to blocks (ceil / 2)
};

struct UpdateOp {
    std::vector<uint8_t> entityKey;  // 32 raw bytes
    std::string contentType;
    std::vector<uint8_t> payload;
    std::vector<Attribute> attributes;
    uint32_t expiresInSeconds = 0;
};

struct ExtensionOp {
    std::vector<uint8_t> entityKey;
    uint32_t expiresInSeconds = 0;
};

struct OwnershipChangeOp {
    std::vector<uint8_t> entityKey;
    std::vector<uint8_t> newOwner;   // 20 bytes
};

struct OpsTxData {
    std::vector<CreateOp>           creates;
    std::vector<UpdateOp>           updates;
    std::vector<std::vector<uint8_t>> deletes;
    std::vector<ExtensionOp>        extensions;
    std::vector<OwnershipChangeOp>  ownershipChanges;
};

/* Build the RLP-encoded tx `data` payload. */
std::vector<uint8_t> build_ops_tx_data(const OpsTxData& ops);

}  // namespace arkiv
