idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES espressif__mqtt esp_netif esp_event esp-tls mbedtls lwip esp_timer
    EMBED_TXTFILES "fixture-cert.pem" "fixture-key.pem")
