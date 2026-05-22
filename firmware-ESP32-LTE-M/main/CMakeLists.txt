idf_component_register(SRCS "main.c" "pmu.c" "modem.c" "mqtt.c"
                            "wups_link.c" "identity.c" "cmdauth.c"
                            "cmdauth_arkiv.c" "arkiv_rpc.c"
                            "arkiv_claim.c" "arkiv_writer.cpp"
                            "arkiv_ack.c" "arkiv_tlm.c"
                            "arkiv_event.c" "arkiv_ws.c"
                            "bip39.c"
                       PRIV_REQUIRES spi_flash esp_timer esp_hw_support
                                     esp_driver_i2c esp_driver_gpio
                                     esp_driver_uart
                                     esp_event esp_netif nvs_flash
                                     esp_http_client esp-tls lwip
                                     espressif__mqtt
                                     espressif__esp_websocket_client
                                     mbedtls microecc arkiv_crypto
                                     espressif__cjson
                       INCLUDE_DIRS "")
