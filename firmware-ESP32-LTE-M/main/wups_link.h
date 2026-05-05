/*
 * Web3 Pi UPS binary wire protocol v1 — ESP32-S3 leaf implementation.
 *
 * ESP32 is a leaf node at address WUPS_ADDR_ESP32. It implements only the
 * `system` and `net` classes. RP2040 is the hub of the bus; we never see
 * frames addressed to other nodes (the router drops them before they
 * reach our UART), so there is no routing logic on this side.
 *
 * Outbound:
 *   - system.hello broadcast on init.
 *   - RESP frames to inbound REQs (system.ping).
 *   - net.downlink (EVENT) every time the MQTT client receives a message.
 *
 * Inbound (handled in wups_link.c):
 *   - system.ping    REQ → system.ping RESP with uptime + fw_version.
 *   - net.publish    REQ → forward via mqtt_publish_raw().
 *
 * Wire format and payload structs are defined in
 * Web3-Pi-UPS/common/protocol.h (pulled in via wups_proto.h).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Bring up UART2 with HW flow control, install the RX task, register the
 * MQTT data handler, and broadcast `system.hello`. Idempotent failure
 * modes return a non-OK esp_err_t. */
esp_err_t wups_link_init(void);

/* Send a frame from this node (SRC = WUPS_ADDR_ESP32). SEQ is auto-
 * assigned. Use this for spontaneous EVENTs (e.g. net.downlink) and
 * outbound REQs. */
void wups_link_send(uint8_t dst, uint8_t cls, uint8_t op, uint8_t flags,
                    const void *payload, uint16_t payload_len);

/* Same as wups_link_send but with explicit SEQ — used for RESPs that
 * need to echo the originating REQ's SEQ for correlation. */
void wups_link_send_seq(uint8_t dst, uint8_t cls, uint8_t op,
                        uint8_t flags, uint8_t seq,
                        const void *payload, uint16_t payload_len);

/* Diagnostic — log frame and byte counts at INFO level. Useful to call
 * from a periodic heartbeat to confirm the UART2 link is actually live. */
void wups_link_log_stats(void);
