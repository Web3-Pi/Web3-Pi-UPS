get_filename_component(WUPS_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../firmware-ESP32-LTE-M/main" ABSOLUTE)
idf_component_register(SRCS "main.c" "${WUPS_MAIN}/tls_trust.c"
    INCLUDE_DIRS "." "${WUPS_MAIN}" REQUIRES mbedtls esp_timer esp_hw_support)
