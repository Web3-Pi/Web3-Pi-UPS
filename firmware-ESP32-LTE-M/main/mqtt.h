#pragma once

#include <stddef.h>

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

/*
 * Publish a message via the active client. Wraps esp_mqtt_client_publish.
 * Returns the broker-assigned msg_id on success (>= 0), -1 if the client
 * isn't connected yet, or any other negative esp-mqtt error code. The
 * payload is opaque bytes — caller chooses the encoding.
 */
int mqtt_publish_raw(const char *topic, const void *payload, size_t payload_len,
                     int qos, int retain);

/*
 * Callback type for incoming MQTT messages. Topic and payload pointers
 * are valid only for the duration of the callback; copy if you need to
 * keep them. Strings are NOT NUL-terminated — use the explicit lengths.
 *
 * Called from the esp-mqtt client task on MQTT_EVENT_DATA. Don't block
 * for long — the same task drives keepalive and reconnect.
 */
typedef void (*mqtt_data_cb_t)(const char *topic, size_t topic_len,
                               const void *payload, size_t payload_len);

/*
 * Install (or clear, with NULL) the inbound MQTT data callback. Last
 * write wins. Currently used by wups_link to forward arriving messages
 * as net.downlink frames over UART2 to RP2040.
 */
void mqtt_set_data_handler(mqtt_data_cb_t cb);

/*
 * ICCID-scoped topic strings — valid after mqtt_client_start() has been
 * called. Returned pointers reference statics that live for the lifetime
 * of the firmware. Empty string if mqtt_client_start() hasn't run yet.
 *
 * Used by wups_link to publish raw WUPS frames straight onto the right
 * uplink topic (telemetry / event / cmd response).
 */
const char *mqtt_topic_telemetry(void);
const char *mqtt_topic_event(void);
const char *mqtt_topic_cmd_response(void);
const char *mqtt_topic_cmd_request(void);
