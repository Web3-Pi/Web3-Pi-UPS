#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * OTA-1 — LTE OTA firmware update engine (esp_https_ota into the passive
 * ota_0/ota_1 slot, SHA-256 read-back verification, bootloader rollback).
 *
 * Command path: the panel publishes a WS-9-signed net.fw_update REQ on
 * c/{iccid}/cmd/request; wups_link's MQTT downlink hook calls
 * fw_ota_try_handle_downlink() AFTER cmdauth verified the envelope — the op
 * is executed on the ESP32 itself, never forwarded to the RP2040.
 *
 * Progress is reported as compact JSON events on t/{iccid}/event
 * ({"fw_update":"started"|"progress"|"verifying"|"rebooting"|"error",...})
 * and as a "FW UPDATE" banner on the RP2040 OLED (ui.display_msg).
 *
 * Rollback: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the freshly booted
 * OTA image is ESP_OTA_IMG_PENDING_VERIFY. modem.c's supervision loop calls
 * fw_ota_mark_uplink_healthy() on the first healthy uplink (one-shot cancel
 * of the rollback); main.c's heartbeat calls fw_ota_rollback_tick() so an
 * image that never gets a healthy uplink within FW_OTA_VERIFY_WINDOW_S
 * rolls itself back (esp_ota_mark_app_invalid_rollback_and_reboot). Both
 * are no-ops when the running app is not pending-verify.
 *
 * Brick-guard: fw_ota_request() confirms a still-pending-verify image
 * before starting (an authenticated fw.update over MQTT proves the uplink)
 * and refuses the update if that fails, and fw_ota_rollback_tick() never
 * fires while a download runs — so the rollback can never reboot into a
 * slot that esp_https_ota is half-way through overwriting.
 */

/* If a pending-verify image hasn't seen a healthy uplink this long after
 * boot, roll back to the previous slot. */
#define FW_OTA_VERIFY_WINDOW_S   (10 * 60)

/* Hard cap on one whole OTA attempt (connect + download + verify). */
#define FW_OTA_TIMEOUT_S         (10 * 60)

/*
 * Validate {url, sha256, len} and start the download task. Returns:
 *   ESP_OK                — accepted, OTA task started
 *   ESP_ERR_INVALID_ARG   — not https://, bad length, or malformed sha
 *   ESP_ERR_INVALID_STATE — an update is already running, or the running
 *                           image is still pending-verify and could not be
 *                           confirmed (see the brick-guard above)
 *   ESP_ERR_NOT_FOUND     — no OTA update partition (partition table?)
 * `sha256_hex` must be exactly 64 hex chars (NUL-terminated here; the wire
 * format carries them un-terminated). Caller-owned buffers are copied.
 */
esp_err_t fw_ota_request(const char *url, const char *sha256_hex,
                         uint32_t image_len);

/* True while the OTA task is downloading/verifying. modem.c's uplink
 * watchdog must not count trips or tear down PPP while this is set. */
bool fw_ota_in_progress(void);

/*
 * Inspect a verified inner WUPS frame from the MQTT downlink. If it is a
 * net.fw_update REQ, handle it locally (validate, ACK with a result-byte
 * RESP on t/{iccid}/cmd/response, kick off the download) and return true —
 * the caller must then NOT forward the frame to the RP2040. Returns false
 * for every other frame.
 */
bool fw_ota_try_handle_downlink(const uint8_t *frame, size_t frame_len);

/* Log the running partition label + OTA state at boot (call once from
 * app_main). Also latches whether this image is pending-verify. */
void fw_ota_boot_log(void);

/* Cancel rollback (mark app valid) — call when the uplink is seen healthy.
 * One-shot on success; a failed otadata write is retried on a later call.
 * No-op when not pending-verify / already marked. */
void fw_ota_mark_uplink_healthy(void);

/* Periodic safety net (call from the heartbeat loop): if the app is still
 * pending-verify FW_OTA_VERIFY_WINDOW_S after boot, mark it invalid and
 * reboot into the previous image. */
void fw_ota_rollback_tick(void);
