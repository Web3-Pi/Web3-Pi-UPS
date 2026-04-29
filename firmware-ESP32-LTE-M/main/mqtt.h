#pragma once

#include "esp_err.h"

/*
 * MQTT client over the cellular PPP interface.
 *
 * Call exactly once, AFTER esp_netif has a PPP IP. The client runs in its
 * own task spawned by esp-mqtt; this function returns immediately.
 *
 * Connection parameters (broker URI, username, password) live in
 * main/secrets.h — see secrets.h.example for the template. TLS verification
 * uses the certificate bundle baked into the firmware (LE-issued cert on
 * the broker side is covered).
 */
esp_err_t mqtt_client_start(void);
