#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * OTA-1 — firmware update engine. Three paths share this module (and the
 * single "one update at a time" slot — fw_ota_in_progress() is true while
 * ANY of them runs, which also freezes modem.c's uplink watchdog):
 *
 *   1. ESP32 self-OTA over LTE (the original OTA-1): esp_https_ota into the
 *      passive ota_0/ota_1 slot, SHA-256 read-back verification, bootloader
 *      rollback. Commanded by net.fw_update with target 0/ESP32.
 *
 *   2. fw_xfer RECEIVER (Workbench-over-USB path): the browser pushes an
 *      ESP32 app image through the RP2040 router as net.fw_xfer_begin/
 *      data/end frames (NOT WS-9-enveloped — they originate on the local
 *      USB link where physical access is the auth model, same as every
 *      other local command). esp_ota_* into the passive slot, the SAME
 *      read-back SHA-256 verify, then set-boot + reboot. Stop-and-wait:
 *      every REQ is answered with a 1-byte-result RESP before the sender
 *      may send the next frame.
 *
 *   3. RP2040 RELAY (LTE path for the RP2040): net.fw_update with
 *      target=RP2040 makes the ESP32 stream the image over HTTPS
 *      chunk-by-chunk (never buffered whole — heap is ~270 KB) and drive
 *      the fw_xfer SENDER side over the UART link, strictly stop-and-wait.
 *      The stream digest is checked against the commanded sha256 BEFORE
 *      END(commit=1) — garbage is never committed remotely. After commit
 *      the ESP32 watches for the RP2040's system.hello (reboot into the
 *      new image) for up to 90 s and reports done/timeout.
 *
 * Command path for 1+3: the panel publishes a WS-9-signed net.fw_update REQ
 * on c/{iccid}/cmd/request; wups_link's MQTT downlink hook calls
 * fw_ota_try_handle_downlink() AFTER cmdauth verified the envelope — the op
 * is executed on the ESP32 itself, never forwarded to the RP2040.
 *
 * Progress is reported as compact JSON events on t/{iccid}/event
 * ({"fw_update":"started"|"progress"|"verifying"|"rebooting"|"done"|"error",
 * ...} — relay events additionally carry "target":"rp2040") and as a
 * "FW UPDATE" banner on the RP2040 OLED (ui.display_msg).
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

/* Hard cap on one whole OTA attempt (connect + download + verify).
 * Field data 2026-08-20 (unit ...9940): a ~970 KB image on a ~14 kbps
 * Cat-M link needs ~9.5-10 min — the old 10-min cap killed attempt after
 * attempt seconds from the finish line. 20 min gives 2x headroom (covers
 * ~6.5 kbps sustained); genuine stalls are still caught much earlier by
 * the 30 s per-socket-op timeout (FW_OTA_HTTP_TIMEOUT_MS). */
#define FW_OTA_TIMEOUT_S         (20 * 60)

/*
 * Validate {url, sha256, len, target} and start the download task (self-OTA
 * for target 0/WUPS_FW_TARGET_ESP32, fw_xfer relay for
 * WUPS_FW_TARGET_RP2040). Returns:
 *   ESP_OK                — accepted, task started
 *   ESP_ERR_INVALID_ARG   — not https://, bad length/target, malformed sha
 *   ESP_ERR_INVALID_STATE — an update is already running, or (self-OTA
 *                           only) the running image is still pending-verify
 *                           and could not be confirmed (brick-guard above)
 *   ESP_ERR_NOT_FOUND     — no OTA update partition (partition table?)
 * `sha256_hex` must be exactly 64 hex chars (NUL-terminated here; the wire
 * format carries them un-terminated). Caller-owned buffers are copied.
 */
esp_err_t fw_ota_request(const char *url, const char *sha256_hex,
                         uint32_t image_len, uint8_t target);

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

/*
 * fw_xfer RECEIVER (path 2) — called from wups_link's RX dispatch for a
 * net.fw_xfer_{begin,data,end} REQ addressed to us. Runs the session state
 * machine, sends the 1-byte-result RESP back to `src` (echoing `seq`), and
 * on a committed END reboots into the new image. `op` is the WUPS NET op
 * byte; `payload`/`len` are the frame payload.
 */
void fw_ota_xfer_on_req(uint8_t op, uint8_t src, uint8_t seq,
                        const uint8_t *payload, uint16_t len);

/* fw_xfer SENDER correlation (path 3) — called from wups_link's RX dispatch
 * for a net.fw_xfer_* RESP from the RP2040. Wakes the relay task's
 * stop-and-wait if op+seq match the in-flight REQ; no-op otherwise. */
void fw_ota_xfer_on_resp(uint8_t op, uint8_t seq,
                         const uint8_t *payload, uint16_t len);

/* Periodic (heartbeat) idle guard for the fw_xfer receiver: drops a session
 * that saw no frame for WUPS_FW_XFER_IDLE_TIMEOUT_S (sender died mid-push),
 * releasing the update slot + modem watchdog freeze. No-op otherwise. */
void fw_ota_xfer_tick(void);

/* Called from wups_link's RX dispatch when the RP2040 broadcasts
 * system.hello — the relay path (path 3) uses it as the "RP2040 rebooted
 * into the new image" signal. Cheap counter bump, safe from any task. */
void fw_ota_notify_rp2040_hello(void);
