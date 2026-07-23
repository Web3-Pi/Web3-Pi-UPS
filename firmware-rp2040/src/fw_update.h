/*
 * OTA-2 — RP2040 self-update receiver (net.fw_xfer_* ops 0x23/0x24/0x25).
 *
 * The RP2040 receives its own next firmware image in stop-and-wait chunks
 * over the local WUPS link — from the ESP32 (which downloaded it over LTE
 * on a panel fw.update with target=RP2040) or from the host/Workbench over
 * USB-CDC. Wire contract: common/protocol.h (wups_net_fw_xfer_*_v1_t,
 * WUPS_FW_XFER_* result codes). Every REQ is answered with a 1-byte RESP
 * result by the dispatcher in main.cpp.
 *
 * Staging + apply ride the earlephilhower core's own OTA machinery
 * (~/.platformio/packages/framework-arduinopico):
 *   - the image is staged as a LittleFS file (needs board_build.
 *     filesystem_size in platformio.ini — see the flash map there);
 *   - on END(commit=1) the staged bytes are SHA-256-verified against the
 *     digest from BEGIN, sanity-checked (vector table at +0x3000: initial
 *     SP in RAM, thumb reset vector inside the image — the layout the OTA
 *     bootloader's boot_normal() in ota/ota.c jumps through), then armed
 *     via PicoOTA (libraries/PicoOTA — writes otacommand.bin, same flow as
 *     libraries/Updater UpdaterClass::end());
 *   - the OTA bootloader linked into EVERY arduino-pico image (lib/rp2040/
 *     ota.o, .OTA section of memmap_default.ld) applies it on the next
 *     boot: reads otacommand.bin, copies the file to XIP flash 4 KB pages
 *     at a time, erases the command record, reboots into the new app.
 *
 * Rollback: before arming, the RUNNING image is snapshotted out of XIP
 * flash into LittleFS and a pending-verify marker is written. The NEW
 * firmware (this same module) counts boots and waits for any valid WUPS
 * frame; traffic confirms the update, while ~5 min of silence — or a
 * crash-loop eating the boot budget — restages the snapshot and reboots
 * back into the previous firmware. A new image that cannot even reach
 * this code (hard-faults before setup()) is NOT recoverable in-band:
 * recovery is then Workbench USB (PICOBOOT) / physical BOOTSEL.
 *
 * Flash-stall reality (why the protocol is stop-and-wait): every LittleFS
 * program/erase runs with interrupts off (libraries/LittleFS/src/
 * LittleFS.cpp lfs_flash_prog/lfs_flash_erase), so UART RX drops bytes
 * during each op. All flash work happens synchronously in the REQ handler
 * BEFORE the RESP is sent, so the sender is never transmitting into a
 * stalled receiver. Longest single op is one 4 KB sector erase (typ.
 * ~45 ms, worst-case ~400 ms) — far below the 5 s tolerance of the CH32X
 * power.status staleness guard.
 */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

/* Mount LittleFS, clean stale staging files, and run the pending-verify /
 * rollback boot checks. Call once from setup(). May reboot (crash-loop
 * rollback). First boot after reflashing formats the filesystem (~10 s). */
void fw_update_boot_init(void);

/* Drive timeouts once per loop() pass (~50 ms): the 30 s session idle
 * discard (WUPS_FW_XFER_IDLE_TIMEOUT_S) and the ~5 min pending-verify
 * rollback. May reboot (rollback restage). */
void fw_update_tick(void);

/* Pending-verify heartbeat: call on EVERY locally-delivered WUPS frame.
 * Any valid frame proves the new firmware's RX path + router + dispatcher
 * work end-to-end, so it confirms the update and drops the rollback
 * snapshot. Cheap no-op when no verify is pending. */
void fw_update_note_frame_ok(void);

/* True while a transfer session is open. loop() shows the FW UPDATE
 * screen and suppresses buttons/alerts while this holds. */
bool fw_update_session_active(void);

/* Received-bytes progress, 0..100. */
uint8_t fw_update_progress_pct(void);

/* Render the minimal "FW UPDATE" progress screen (title + % + bar). */
void fw_update_render(Adafruit_SSD1306& oled);

/* Render the "applying" screen. Called by the dispatcher right before
 * fw_update_handle_end(commit=1) — the verify+snapshot+arm work blocks
 * loop() for several seconds, so this is the last OLED update before the
 * reboot into the OTA bootloader. */
void fw_update_render_applying(Adafruit_SSD1306& oled);

/* REQ handlers — payload/len are the raw WUPS payload. Each returns a
 * WUPS_FW_XFER_* result byte for the RESP. handle_end sets
 * *reboot_after_resp when the image was armed and the dispatcher must
 * send the RESP, drain TX, and reboot. */
uint8_t fw_update_handle_begin(const uint8_t* payload, uint16_t len);
uint8_t fw_update_handle_data(const uint8_t* payload, uint16_t len);
uint8_t fw_update_handle_end(const uint8_t* payload, uint16_t len,
                             bool* reboot_after_resp);

#endif /* FW_UPDATE_H */
