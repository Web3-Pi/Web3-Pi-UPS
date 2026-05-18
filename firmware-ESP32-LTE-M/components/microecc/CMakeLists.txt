# micro-ecc (uECC) — BSD-2, vendored from the arkiv_crypto component of
# ArkivChat/Arkiv-Mbed-ESP32-S3-LCD. Only the secp256k1 curve is enabled;
# WS-9 (ADR-0009) uses uECC_verify() to check backend command signatures.
idf_component_register(
    SRCS "uECC.c"
    INCLUDE_DIRS "."
)

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
