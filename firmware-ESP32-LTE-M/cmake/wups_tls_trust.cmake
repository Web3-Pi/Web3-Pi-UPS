# The wrapper delegates to an exported but non-public ESP-IDF callback.
# Any SDK change requires review; never silently bypass distrust checks.
if(NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE OR
   NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE OR
   NOT CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE OR
   CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEPRECATED_LIST OR
   NOT CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH STREQUAL "certs/roots.pem")
    message(FATAL_ERROR "TLS trust: regenerate sdkconfig using reviewed project defaults; SDK/default/deprecated roots are not permitted")
endif()
set(wups_bundle_source "$ENV{IDF_PATH}/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c")
file(SHA256 "${wups_bundle_source}" wups_bundle_source_hash)
if(NOT wups_bundle_source_hash STREQUAL "e44d1e0a42a9d33cfc072ea005e93c8a0337c5ebcbfb9a1cf1554930f4f2816f")
    message(FATAL_ERROR "TLS trust: ESP-IDF bundle callback changed; review tls_trust.c before updating the source pin")
endif()
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/check_tls_trust.py"
                RESULT_VARIABLE wups_trust_result)
if(NOT wups_trust_result EQUAL 0)
    message(FATAL_ERROR "TLS trust: CA input verification failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${wups_bundle_source}"
    "${CMAKE_CURRENT_LIST_DIR}/../certs/roots.pem"
    "${CMAKE_CURRENT_LIST_DIR}/../certs/manifest.json"
    "${CMAKE_CURRENT_LIST_DIR}/../main/tls_distrust.h")
