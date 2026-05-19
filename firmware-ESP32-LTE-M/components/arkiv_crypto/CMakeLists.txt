# arkiv_crypto — Track 2 / ADR-0011. Lifted from
# ArkivChat/Arkiv-Mbed-ESP32-S3-LCD (Kaolin PoC). secp256k1 (sign-recoverable
# / verify / address), keccak256, RLP, Arkiv ops-tx + EIP-155 signer.
#
# IMPORTANT: the PoC bundled its own micro-ecc under
# src/third_party/micro_ecc/. This firmware ALREADY vendors that exact code
# as the `microecc` component (WS-9 / ADR-0009). Compiling uECC.c twice would
# double-define every uECC_* symbol, so the vendored copy is deliberately NOT
# brought over — this component REQUIRES `microecc` and uses its uECC.h /
# uECC_vli.h instead. tiny_keccak (CC0) is kept (microecc has no keccak).
idf_component_register(
    SRCS
        "src/secp256k1.c"
        "src/rlp.cpp"
        "src/brotli_store.cpp"
        "src/ops_tx_data.cpp"
        "src/tx_signer.cpp"
        "src/third_party/tiny_keccak/keccak256.c"
    INCLUDE_DIRS "include"
    REQUIRES microecc
    PRIV_REQUIRES mbedtls
)

# secp256k1.c includes uECC.h / uECC_vli.h; the uECC headers expose the VLI
# API and curve surface only when these macros are set at the call site too
# (they gate prototypes via #if). Must match the `microecc` component's
# definitions exactly so the API seen here equals the API compiled there.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    uECC_SUPPORTS_secp160r1=0
    uECC_SUPPORTS_secp192r1=0
    uECC_SUPPORTS_secp224r1=0
    uECC_SUPPORTS_secp256r1=0
    uECC_SUPPORTS_secp256k1=1
    uECC_OPTIMIZATION_LEVEL=2
    uECC_SQUARE_FUNC=1
    uECC_ENABLE_VLI_API=1
)
