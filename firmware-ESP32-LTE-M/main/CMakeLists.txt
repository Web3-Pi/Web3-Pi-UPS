idf_component_register(SRCS "main.c" "pmu.c" "modem.c" "modem_radio_policy.c" "modem_awake_policy.c" "modem_signal.c"
                            "modem_support_diag.c" "modem_diag_clock.c" "mqtt.c"
                            "mqtt_health.c" "mqtt_packet_guard.c" "mqtt_dispatch_queue.c"
                            "mqtt_sdk_adapter.c"
                            "wups_link.c" "identity.c" "cmdauth.c"
                            "cmdauth_arkiv.c" "arkiv_rpc.c"
                            "arkiv_claim.c" "arkiv_writer.cpp"
                            "arkiv_ack.c" "arkiv_tlm.c"
                            "arkiv_event.c" "arkiv_ws.c"
                            "backend_mode.c"
                            "arkiv_crypto_selftest.c"
                            "http_cfg.c"
                            "http_backend.c"
                            "oled_menu.c"
                            "bip39.c"
                            "fw_ota.c" "fw_ota_http_policy.c"
                       PRIV_REQUIRES spi_flash esp_timer esp_hw_support
                                     esp_driver_i2c esp_driver_gpio
                                     esp_driver_uart
                                     esp_event esp_netif nvs_flash
                                     esp_http_client esp-tls lwip
                                     espressif__mqtt
                                     tcp_transport http_parser
                                     espressif__esp_websocket_client
                                     mbedtls microecc arkiv_crypto
                                     espressif__cjson
                                     esp_https_ota app_update esp_partition
                       INCLUDE_DIRS "")

# Optional fixed-APN artifacts. An empty value preserves the normal ICCID
# policy; invalid APNs fail configuration instead of silently changing it.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(WUPS_FIXED_APN "" CACHE STRING "Fixed modem APN; empty uses ICCID selection")
    set_property(CACHE WUPS_FIXED_APN PROPERTY STRINGS "" "iot.1nce.net" "sensor.net")
    if(NOT "${WUPS_FIXED_APN}" STREQUAL "" AND
       NOT "${WUPS_FIXED_APN}" STREQUAL "iot.1nce.net" AND
       NOT "${WUPS_FIXED_APN}" STREQUAL "sensor.net")
        message(FATAL_ERROR "WUPS_FIXED_APN must be empty, iot.1nce.net, or sensor.net")
    endif()
    if(NOT "${WUPS_FIXED_APN}" STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE WUPS_FIXED_APN="${WUPS_FIXED_APN}")
        message(STATUS "Modem APN fixed by build profile: ${WUPS_FIXED_APN}")
    endif()
    # Runtime telemetry and the ESP app descriptor must identify the same
    # artifact, including -1nce / -sensor suffixes supplied with PROJECT_VER.
    idf_build_get_property(wups_fw_version PROJECT_VER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION_STR="esp32:${wups_fw_version}")
endif()

# The stopped-task adapter reads a deliberately small private MQTT contract.
# A component update must be reviewed with its lifecycle tests before changing
# these pins. The reviewed MQTT component is vendored in components/; shared
# managed components and the installed SDK are never patched in place.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(wups_mqtt_component_dir espressif__mqtt COMPONENT_DIR)
    set(wups_mqtt_sdk_pins
        "mqtt_client.c|1a120957d6f8078a0cd27f4febac54389c5dce7f025069cad493e945d005f361"
        "lib/include/mqtt_client_priv.h|ee8f464f6cbf77a83468126bf22d91833b9c2b8985ba5860381acb99a655c565")
    foreach(wups_mqtt_pin IN LISTS wups_mqtt_sdk_pins)
        string(REPLACE "|" ";" wups_mqtt_pin_parts "${wups_mqtt_pin}")
        list(GET wups_mqtt_pin_parts 0 wups_mqtt_pin_file)
        list(GET wups_mqtt_pin_parts 1 wups_mqtt_pin_hash)
        file(SHA256 "${wups_mqtt_component_dir}/${wups_mqtt_pin_file}" wups_mqtt_actual_hash)
        if(NOT wups_mqtt_actual_hash STREQUAL wups_mqtt_pin_hash)
            message(FATAL_ERROR "MQTT lifecycle adapter SDK pin changed: ${wups_mqtt_pin_file}. Review mqtt_sdk_adapter and its lifecycle tests before updating the SHA-256 pin.")
        endif()
    endforeach()
    target_include_directories(${COMPONENT_LIB} PRIVATE "${wups_mqtt_component_dir}/lib/include")
endif()

# Bounded header reads and typed transport errors are part of OTA recovery's
# contract. Pin the local SDK component, including its public API.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(wups_https_ota_dir esp_https_ota COMPONENT_DIR)
    set(wups_https_ota_pins
        "CMakeLists.txt|8ed66893cbf138c512b083ef4d6a1d2efd60315f63c4300434c72de75f0846d8"
        "Kconfig|4e8bb698663427ec9409181b2b50f00bb146ba3659617585bfd8f95c3273b9df"
        "LICENSE|cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
        "include/esp_https_ota.h|4f00965e934f0479c94d269e93f1e1bb1db2c0b8597092c8384e4b32e10439fd"
        "sdkconfig.rename|61d6c68fe8b81c38d502c2f16961ce9b9cf82b865e46f78ef64d804351b1f703"
        "src/esp_https_ota.c|c38726feefc22f52a001d7b38c2b8bb675350f59d02b7a827db0129aee2fb6bd")
    foreach(wups_ota_pin IN LISTS wups_https_ota_pins)
        string(REPLACE "|" ";" wups_ota_parts "${wups_ota_pin}")
        list(GET wups_ota_parts 0 wups_ota_file)
        list(GET wups_ota_parts 1 wups_ota_hash)
        file(SHA256 "${wups_https_ota_dir}/${wups_ota_file}" wups_ota_actual)
        if(NOT wups_ota_actual STREQUAL wups_ota_hash)
            message(FATAL_ERROR "OTA SDK pin changed: ${wups_ota_file}. Review HTTP retry/resume tests before updating it.")
        endif()
    endforeach()
endif()
