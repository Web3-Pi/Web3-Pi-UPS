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
 *   - system.hello   from RP2040 → fw_ota relay reboot detector.
 *   - net.publish    REQ → forward via mqtt_publish_raw().
 *   - net.config     REQ → persist HTTP-mode endpoint in NVS.
 *   - net.fw_xfer_*  REQ → fw_ota receiver (Workbench USB push);
 *                    RESP → fw_ota relay sender stop-and-wait.
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

/* Render a complete frame (SYNC..END, SRC=ESP32, auto-assigned SEQ) into
 * out[cap] instead of putting it on UART2. For self-originated frames that
 * ride inside another transport — e.g. the modem's net.status EVENT
 * published raw on the MQTT telemetry topic, byte-identical to the frames
 * the RP2040 relays via net.publish. Returns the total frame length, or 0
 * if it doesn't fit. */
uint16_t wups_link_render_frame(uint8_t *out, size_t cap,
                                uint8_t dst, uint8_t cls, uint8_t op,
                                uint8_t flags,
                                const void *payload, uint16_t payload_len);

/* Diagnostic — log frame and byte counts at INFO level. Useful to call
 * from a periodic heartbeat to confirm the UART2 link is actually live. */
void wups_link_log_stats(void);

/*
 * Track 2 / ADR-0011 §10.1/§10.4 — trust-anchor round-trip with the
 * RP2040 OLED gate (the OLED + 2 buttons are RP2040-only; scoped,
 * owner-approved exception to Decision C, RP2040 stays a dumb renderer).
 *
 * Send a ui.trust_prompt REQ: the RP2040 renders `text` verbatim
 * (mode 0 = owner fingerprint to compare, mode 1 = claim-code to
 * transcribe) and, if confirm_secs > 0, runs the both-button hold.
 * confirm_secs == 0 means display-only (no result expected, e.g. the
 * claim-code screen). The claim driver is the only caller (single
 * in-flight prompt).
 */
void wups_link_trust_prompt(uint8_t mode, uint8_t confirm_secs,
                            uint32_t nonce, const char *text);

/*
 * Block up to `timeout_ms` for the ui.trust_result whose nonce matches
 * `nonce` (set by the preceding wups_link_trust_prompt()). On a match
 * writes the result code (0=confirmed, 1=timeout, 2=cancelled) to
 * *result_out and returns ESP_OK; ESP_ERR_TIMEOUT if none arrived;
 * ESP_ERR_INVALID_STATE if `nonce` isn't the armed one.
 */
esp_err_t wups_link_trust_wait(uint32_t nonce, uint32_t timeout_ms,
                               uint8_t *result_out);
