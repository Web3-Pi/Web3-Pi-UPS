idf_component_register(SRCS "main.c" "pmu.c" "modem.c" "mqtt.c" "wups_link.c"
                       PRIV_REQUIRES spi_flash esp_timer
                                     esp_driver_i2c esp_driver_gpio
                                     esp_driver_uart
                                     esp_event esp_netif nvs_flash
                                     esp_http_client esp-tls lwip
                                     espressif__mqtt
                       INCLUDE_DIRS "")
