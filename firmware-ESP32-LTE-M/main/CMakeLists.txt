idf_component_register(SRCS "main.c" "pmu.c" "modem.c" "mqtt.c"
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
                            "fw_ota.c"
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

# The stopped-task adapter reads a deliberately small private MQTT contract.
# A component update must be reviewed with its lifecycle tests before changing
# these pins; no managed SDK source is patched or copied into the firmware.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(wups_mqtt_component_dir espressif__mqtt COMPONENT_DIR)
    set(wups_mqtt_sdk_pins
        "mqtt_client.c|4b24720b34c2bd44b0857a5251f5392663225c618595229540b35f1529663a9a"
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
