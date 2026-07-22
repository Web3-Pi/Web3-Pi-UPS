#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * MQTT client over the cellular PPP interface.
 *
 * Call exactly once, AFTER esp_netif has a PPP IP. The client runs in its
 * own task spawned by esp-mqtt; this function returns immediately.
 *
 * The broker URI is a compile-time constant in main/endpoints.h (public,
 * tracked in git). Per-device credentials — username = ICCID, password =
 * the per-device 32-byte secret — are read from the `prov` NVS partition
 * via identity.c (ADR-0008); nothing credential-like is compiled in. TLS
 * verification uses the certificate bundle baked into the firmware
 * (LE-issued cert on the broker side is covered).
 */
esp_err_t mqtt_client_start(void);

/*
 * Publish a message via the active client. Wraps esp_mqtt_client_publish
 * while connected; when the link is down (or the publish loses a race with
 * a disconnect) the frame is parked in the esp-mqtt RAM outbox instead
 * (MISC-9) and flushed automatically on reconnect. The outbox is bounded
 * (32 KB) and entries expire after CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS.
 * Returns msg_id (>= 0) on publish or park, -1 on error, -2 outbox full.
 * The payload is opaque bytes — caller chooses the encoding.
 */
int mqtt_publish_raw(const char *topic, const void *payload, size_t payload_len,
                     int qos, int retain);

/*
 * Uplink-health accessors for the modem's post-PPP watchdog (modem.c).
 * mqtt_is_connected() mirrors MQTT_EVENT_CONNECTED/DISCONNECTED;
 * mqtt_last_connected_s() is the esp_timer second the last CONNECTED
 * landed (0 = never this boot). Both are lock-free single-word reads,
 * safe from any task.
 */
bool mqtt_is_connected(void);
uint32_t mqtt_last_connected_s(void);

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
