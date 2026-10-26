get_filename_component(WUPS_MAIN "${CMAKE_CURRENT_LIST_DIR}/../../../firmware-ESP32-LTE-M/main" ABSOLUTE)
idf_component_register(SRCS "probe.c" INCLUDE_DIRS "${WUPS_MAIN}"
    REQUIRES esp_http_client esp_timer espressif__esp_websocket_client espressif__cjson WHOLE_ARCHIVE)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=app_main")
