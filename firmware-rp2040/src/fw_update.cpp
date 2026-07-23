/*
 * OTA-2 — RP2040 self-update receiver. See fw_update.h for the design and
 * the pointers into the arduino-pico core OTA machinery this rides on.
 */
#include "fw_update.h"
#include "wups_proto.h"
#include "wups_router.h"
#include "sha256.h"
#include <LittleFS.h>
#include <PicoOTA.h>          /* picoOTA + _OTA_COMMAND_FILE ("otacommand.bin") */
#include <hardware/flash.h>   /* XIP_BASE */
#include <string.h>

/* LittleFS staging files. Flat names, mirroring the core's own convention
 * (UpdaterClass uses "firmware.bin", PicoOTA writes "otacommand.bin"). */
static const char* kStageFile   = "fwstage.bin";   /* incoming image        */
static const char* kPrevFile    = "fwprev.bin";    /* rollback snapshot     */
static const char* kPendingFile = "fwpending.bin"; /* pending-verify marker */

/* Image length gates. An arduino-pico app bin is a whole-flash image:
 * [boot2 0x000][OTA stub + partition table ..0x3000)[app vectors @0x3000].
 * Current build is ~270 KB; 16 KB is already far below any linkable image,
 * 640 KB caps the staging so image + snapshot always fit the 1 MB FS. */
static const uint32_t FW_APP_VECTORS_OFF = 0x3000u;
static const uint32_t FW_IMAGE_MIN_LEN   = 0x4000u;          /* 16 KB  */
static const uint32_t FW_IMAGE_MAX_LEN   = 640u * 1024u;     /* 640 KB */
static const uint32_t FW_FS_SLACK        = 32u * 1024u;      /* LittleFS
                        metadata + otacommand.bin + marker headroom */

/* Rollback policy: the new firmware must see at least one valid WUPS frame
 * within this window of boot, and must not eat the boot budget crash-
 * looping (each boot increments the marker; a watchdog-reset loop that at
 * least reaches fw_update_boot_init() therefore converges to rollback). */
static const uint32_t FW_VERIFY_WINDOW_MS  = 5u * 60u * 1000u;
static const uint32_t FW_PENDING_MAX_BOOTS = 8u;

static const uint32_t FW_XFER_IDLE_MS = WUPS_FW_XFER_IDLE_TIMEOUT_S * 1000u;

#define FW_PENDING_MAGIC 0x46575550u  /* "PUWF" little-endian ("FWUP") */
struct PendingMarker {
    uint32_t magic;
    uint32_t boots;   /* boots since the update was applied */
};

/* End of the RUNNING image in XIP flash (memmap_default.ld .flash_end).
 * &__flash_binary_end - XIP_BASE == the byte size firmware.bin had. */
extern "C" uint8_t __flash_binary_end;

/* --- module state ---------------------------------------------------- */

static bool s_fs_ok = false;

static struct {
    bool     open;
    uint32_t image_len;
    uint32_t received;
    uint8_t  sha[32];        /* raw digest from BEGIN */
    uint32_t last_req_ms;
    uint16_t buf_used;
    uint8_t  buf[4096];      /* RAM accumulator: one flash block per write,
                                and NEVER hand LittleFS an XIP source (lfs
                                bypasses its cache for large writes and
                                flash_range_program cannot read flash
                                while programming it) */
} S;

static File s_stage;

static bool     s_pending = false;   /* pending-verify (rollback armed) */
static uint32_t s_pending_boot_ms = 0;

/* --- helpers --------------------------------------------------------- */

/* system.log to the RPi agent (journald evidence). Mirrors main.cpp's
 * wupsSendLogToHost, kept local so this module stays self-contained. */
static void fw_log(uint8_t level, const char* text) {
    uint8_t pl[4 + 64];
    uint8_t tl = 0;
    while (text[tl] && tl < 64) { pl[4 + tl] = (uint8_t)text[tl]; tl++; }
    pl[0] = 1;
    pl[1] = level;
    pl[2] = tl;
    pl[3] = 0;
    wups_send(WUPS_PORT_RPI, WUPS_ADDR_RPI, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_LOG,
              WUPS_FLAG_EVENT, pl, (uint16_t)(4 + tl));
}

/* Push the RAM accumulator to the staging file. All flash work (block
 * erase + 256 B page programs) happens HERE, inside the REQ handler,
 * before the RESP goes out — the stop-and-wait invariant. */
static bool flush_stage_buf(void) {
    if (S.buf_used == 0) return true;
    rp2040.wdt_reset();
    size_t n = s_stage.write(S.buf, S.buf_used);
    if (n != S.buf_used) return false;
    s_stage.flush();
    rp2040.wdt_reset();
    S.buf_used = 0;
    return true;
}

/* Close + discard the staging session (abort / idle timeout / error). */
static void session_abort(void) {
    if (s_stage) s_stage.close();
    S.open = false;
    S.buf_used = 0;
    if (s_fs_ok && LittleFS.exists(kStageFile)) LittleFS.remove(kStageFile);
}

static void cleanup_pending_files(void) {
    if (!s_fs_ok) return;
    if (LittleFS.exists(kPendingFile)) LittleFS.remove(kPendingFile);
    if (LittleFS.exists(kPrevFile))    LittleFS.remove(kPrevFile);
    if (LittleFS.exists(kStageFile) && !S.open) LittleFS.remove(kStageFile);
}

/* SHA-256 read-back over the staged file vs the digest from BEGIN. Reads
 * go through lfs_flash_read (plain memcpy from XIP) — no flash ops. */
static bool verify_stage_sha(void) {
    File f = LittleFS.open(kStageFile, "r");
    if (!f) return false;
    fw_sha256_ctx ctx;
    fw_sha256_init(&ctx);
    int n;
    while ((n = f.read(S.buf, sizeof(S.buf))) > 0) {
        rp2040.wdt_reset();
        fw_sha256_update(&ctx, S.buf, (size_t)n);
    }
    f.close();
    uint8_t digest[32];
    fw_sha256_final(&ctx, digest);
    return memcmp(digest, S.sha, sizeof(digest)) == 0;
}

/* Structural sanity before arming: the OTA bootloader's boot_normal()
 * (ota/ota.c) loads SP from *(XIP_BASE+0x3000) and jumps to
 * *(XIP_BASE+0x3004), so a bootable image MUST carry a plausible vector
 * table there: initial SP inside RAM (0x20000000..0x20042000 — main RAM
 * through the top of SCRATCH_Y) and a thumb-bit reset vector pointing
 * into the image itself. */
static bool sanity_check_image(void) {
    File f = LittleFS.open(kStageFile, "r");
    if (!f) return false;
    uint32_t vec[2];
    bool ok = f.seek(FW_APP_VECTORS_OFF) &&
              f.read((uint8_t*)vec, sizeof(vec)) == (int)sizeof(vec);
    f.close();
    if (!ok) return false;

    const uint32_t sp  = vec[0];
    const uint32_t rst = vec[1];
    if (sp < 0x20000000u || sp > 0x20042000u) return false;
    if ((rst & 1u) == 0) return false;                      /* thumb bit */
    const uint32_t rst_addr = rst & ~1u;
    if (rst_addr <  XIP_BASE + FW_APP_VECTORS_OFF) return false;
    if (rst_addr >= XIP_BASE + S.image_len)        return false;
    return true;
}

/* Copy the RUNNING image out of XIP flash into the rollback snapshot.
 * Bounced through the RAM buffer — see the S.buf comment. Each 4 KB write
 * costs one block erase (~45 ms typ.) with a re-enabled-IRQ gap after it,
 * so UART frame loss stays in sub-second bursts. */
static bool snapshot_running_image(void) {
    const uint32_t snap_len =
        (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    if (snap_len < FW_IMAGE_MIN_LEN || snap_len > FW_IMAGE_MAX_LEN) return false;

    File f = LittleFS.open(kPrevFile, "w");
    if (!f) return false;
    const uint8_t* src = (const uint8_t*)XIP_BASE;
    uint32_t off = 0;
    bool ok = true;
    while (off < snap_len) {
        uint32_t n = snap_len - off;
        if (n > sizeof(S.buf)) n = sizeof(S.buf);
        memcpy(S.buf, src + off, n);          /* XIP read BEFORE flash op */
        rp2040.wdt_reset();
        if (f.write(S.buf, n) != n) { ok = false; break; }
        off += n;
    }
    f.close();
    if (!ok) LittleFS.remove(kPrevFile);
    return ok;
}

static bool write_pending_marker(void) {
    PendingMarker m = { FW_PENDING_MAGIC, 0 };
    File f = LittleFS.open(kPendingFile, "w");
    if (!f) return false;
    bool ok = f.write((const uint8_t*)&m, sizeof(m)) == sizeof(m);
    f.close();
    if (!ok) LittleFS.remove(kPendingFile);
    return ok;
}

/* Arm the OTA bootloader with the rollback snapshot and reboot into the
 * previous firmware. Returns (without rebooting) only when there is no
 * usable snapshot — the caller then just clears the pending state and the
 * recovery story degrades to Workbench USB / BOOTSEL. */
static void restage_previous_and_reboot(const char* logmsg) {
    if (!s_fs_ok || !LittleFS.exists(kPrevFile)) return;
    picoOTA.begin();
    if (!picoOTA.addFile(kPrevFile) || !picoOTA.commit()) return;
    /* The restaged old firmware must boot clean, not see a stale marker
     * and "roll back" again (harmless but a pointless double reboot). */
    LittleFS.remove(kPendingFile);
    fw_log(1, logmsg);
    delay(100);            /* let the log frame drain to the agent */
    rp2040.reboot();
}

/* --- boot / periodic entry points ------------------------------------ */

void fw_update_boot_init(void) {
    S.open = false;
    s_fs_ok = LittleFS.begin();
    if (!s_fs_ok) {
        fw_log(1, "RP2040: LittleFS mount failed - fw_xfer disabled");
        return;
    }

    /* Stale OTA command file: after applying, the bootloader erases the
     * data block behind LittleFS's back (ota.c "very naughty" comment), so
     * an existing entry is either that husk or a command the bootloader
     * refused — remove it so PicoOTA state is always clean. */
    if (LittleFS.exists(_OTA_COMMAND_FILE)) LittleFS.remove(_OTA_COMMAND_FILE);

    if (LittleFS.exists(kPendingFile)) {
        /* We ARE the freshly applied image (or the rollback snapshot after
         * a power cut in the tiny commit→marker-removal window). */
        PendingMarker m = {};
        File f = LittleFS.open(kPendingFile, "r+");
        bool ok = f && f.read((uint8_t*)&m, sizeof(m)) == (int)sizeof(m) &&
                  m.magic == FW_PENDING_MAGIC;
        if (ok) {
            m.boots++;
            f.seek(0);
            f.write((const uint8_t*)&m, sizeof(m));
            f.close();
            if (m.boots > FW_PENDING_MAX_BOOTS) {
                restage_previous_and_reboot(
                    "RP2040: fw crash-looping after update - rolling back");
                /* no snapshot -> fall through and clear pending state */
            } else {
                s_pending = true;
                s_pending_boot_ms = millis();
                fw_log(2, "RP2040: new fw pending verify (WUPS traffic confirms)");
                return;
            }
        } else if (f) {
            f.close();
        }
        cleanup_pending_files();
        return;
    }

    /* No pending marker: anything left in staging is a dead session (power
     * cut mid-transfer) or a confirmed update's leftovers. Reclaim. */
    if (LittleFS.exists(kStageFile)) LittleFS.remove(kStageFile);
    if (LittleFS.exists(kPrevFile))  LittleFS.remove(kPrevFile);
}

void fw_update_tick(void) {
    if (S.open &&
        (uint32_t)(millis() - S.last_req_ms) > FW_XFER_IDLE_MS) {
        session_abort();
        fw_log(1, "RP2040: fw_xfer session idle timeout - discarded");
    }
    if (s_pending &&
        (uint32_t)(millis() - s_pending_boot_ms) > FW_VERIFY_WINDOW_MS) {
        restage_previous_and_reboot(
            "RP2040: no WUPS traffic after update - rolling back");
        /* Only reached without a usable snapshot: keep running, recovery
         * is Workbench USB / BOOTSEL. */
        s_pending = false;
        cleanup_pending_files();
    }
}

void fw_update_note_frame_ok(void) {
    if (!s_pending) return;
    s_pending = false;
    cleanup_pending_files();
    fw_log(2, "RP2040: fw update confirmed (WUPS traffic seen)");
}

/* --- OLED ------------------------------------------------------------- */

bool fw_update_session_active(void) { return S.open; }

uint8_t fw_update_progress_pct(void) {
    if (!S.open || S.image_len == 0) return 0;
    uint32_t pct = (uint32_t)(((uint64_t)S.received * 100u) / S.image_len);
    return (uint8_t)(pct > 100u ? 100u : pct);
}

void fw_update_render(Adafruit_SSD1306& oled) {
    uint8_t pct = fw_update_progress_pct();
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(5, 0);
    oled.print(F("FW UPDATE"));
    oled.setTextSize(2);                       /* 12 px/char */
    oled.setCursor(pct >= 100 ? 8 : (pct >= 10 ? 14 : 20), 10);
    oled.print(pct);
    oled.print(F("%"));
    oled.drawRect(0, 27, 64, 5, SSD1306_WHITE);
    uint8_t fillw = (uint8_t)((64u * pct) / 100u);
    if (fillw > 2) oled.fillRect(1, 28, (int16_t)(fillw - 2), 3, SSD1306_WHITE);
    oled.display();
}

void fw_update_render_applying(Adafruit_SSD1306& oled) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(5, 0);
    oled.print(F("FW UPDATE"));
    oled.setCursor(8, 12);
    oled.print(F("applying"));
    oled.setCursor(11, 22);
    oled.print(F("wait..."));
    oled.display();
}

/* --- REQ handlers ------------------------------------------------------ */

uint8_t fw_update_handle_begin(const uint8_t* payload, uint16_t len) {
    if (len < sizeof(wups_net_fw_xfer_begin_v1_t)) return WUPS_FW_XFER_BAD_REQ;
    wups_net_fw_xfer_begin_v1_t b;
    memcpy(&b, payload, sizeof(b));
    if (b.version != 1)                    return WUPS_FW_XFER_BAD_REQ;
    if (b.target != WUPS_FW_TARGET_RP2040) return WUPS_FW_XFER_BAD_REQ;
    if (b.image_len < FW_IMAGE_MIN_LEN ||
        b.image_len > FW_IMAGE_MAX_LEN)    return WUPS_FW_XFER_BAD_REQ;
    if (!s_fs_ok)                          return WUPS_FW_XFER_FLASH_ERR;

    /* BEGIN implicitly aborts any session in progress (protocol.h). */
    session_abort();

    /* Fail fast if image + rollback snapshot can't both fit. */
    FSInfo info = {};
    LittleFS.info(info);
    uint64_t free_b = (info.totalBytes > info.usedBytes)
                          ? info.totalBytes - info.usedBytes : 0;
    const uint32_t snap_len =
        (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    if ((uint64_t)b.image_len + snap_len + FW_FS_SLACK > free_b) {
        return WUPS_FW_XFER_FLASH_ERR;
    }

    s_stage = LittleFS.open(kStageFile, "w");
    if (!s_stage) return WUPS_FW_XFER_FLASH_ERR;

    S.open      = true;
    S.image_len = b.image_len;
    S.received  = 0;
    S.buf_used  = 0;
    memcpy(S.sha, b.sha256, sizeof(S.sha));
    S.last_req_ms = millis();
    fw_log(2, "RP2040: fw_xfer session opened");
    return WUPS_FW_XFER_OK;
}

uint8_t fw_update_handle_data(const uint8_t* payload, uint16_t len) {
    if (!S.open) return WUPS_FW_XFER_BAD_REQ;
    S.last_req_ms = millis();

    if (len < sizeof(wups_net_fw_xfer_data_v1_hdr_t) + 1) {
        return WUPS_FW_XFER_BAD_REQ;
    }
    uint32_t offset;
    memcpy(&offset, payload, sizeof(offset));
    const uint16_t chunk = (uint16_t)(len - sizeof(wups_net_fw_xfer_data_v1_hdr_t));
    if (chunk > WUPS_FW_XFER_CHUNK)            return WUPS_FW_XFER_BAD_REQ;
    if (offset != S.received)                  return WUPS_FW_XFER_SEQ_MISMATCH;
    if ((uint64_t)S.received + chunk > S.image_len) return WUPS_FW_XFER_BAD_REQ;

    /* Accumulate into the RAM buffer; hit flash only on 4 KB boundaries so
     * a DATA REQ stalls the UARTs at most one erase+program (< 0.5 s). */
    const uint8_t* p = payload + sizeof(wups_net_fw_xfer_data_v1_hdr_t);
    uint16_t rem = chunk;
    while (rem) {
        uint16_t take = (uint16_t)(sizeof(S.buf) - S.buf_used);
        if (take > rem) take = rem;
        memcpy(S.buf + S.buf_used, p, take);
        S.buf_used = (uint16_t)(S.buf_used + take);
        p   += take;
        rem  = (uint16_t)(rem - take);
        if (S.buf_used == sizeof(S.buf)) {
            if (!flush_stage_buf()) {
                session_abort();               /* sender restarts from BEGIN */
                return WUPS_FW_XFER_FLASH_ERR;
            }
        }
    }
    S.received += chunk;
    return WUPS_FW_XFER_OK;
}

uint8_t fw_update_handle_end(const uint8_t* payload, uint16_t len,
                             bool* reboot_after_resp) {
    *reboot_after_resp = false;
    if (!S.open) return WUPS_FW_XFER_BAD_REQ;
    S.last_req_ms = millis();

    if (len < sizeof(wups_net_fw_xfer_end_v1_t)) return WUPS_FW_XFER_BAD_REQ;
    wups_net_fw_xfer_end_v1_t e;
    memcpy(&e, payload, sizeof(e));
    if (e.version != 1) return WUPS_FW_XFER_BAD_REQ;

    if (!e.commit) {                            /* explicit abort */
        session_abort();
        fw_log(2, "RP2040: fw_xfer session aborted by sender");
        return WUPS_FW_XFER_OK;
    }

    if (S.received != S.image_len) {            /* short image */
        session_abort();
        return WUPS_FW_XFER_BAD_REQ;
    }
    if (!flush_stage_buf()) {
        session_abort();
        return WUPS_FW_XFER_FLASH_ERR;
    }
    s_stage.close();
    S.open = false;   /* transfer phase over; the file outlives the session */

    if (!verify_stage_sha()) {
        LittleFS.remove(kStageFile);
        fw_log(1, "RP2040: fw_xfer SHA-256 mismatch - discarded");
        return WUPS_FW_XFER_VERIFY_FAIL;
    }
    if (!sanity_check_image()) {
        LittleFS.remove(kStageFile);
        fw_log(1, "RP2040: fw_xfer image sanity check failed - discarded");
        return WUPS_FW_XFER_BAD_REQ;
    }

    /* Rollback snapshot of the running image. Non-fatal on failure: the
     * update proceeds, just without in-band rollback (documented). */
    bool have_snapshot = snapshot_running_image();
    if (have_snapshot) {
        if (!write_pending_marker()) {
            LittleFS.remove(kPrevFile);
            have_snapshot = false;
        }
    }
    if (!have_snapshot) {
        fw_log(1, "RP2040: no rollback snapshot - update is one-way");
    }

    /* Arm the OTA bootloader (same flow as UpdaterClass::end()). */
    picoOTA.begin();
    if (!picoOTA.addFile(kStageFile) || !picoOTA.commit()) {
        cleanup_pending_files();
        LittleFS.remove(kStageFile);
        return WUPS_FW_XFER_FLASH_ERR;
    }

    fw_log(2, "RP2040: fw image staged + armed, rebooting to apply");
    *reboot_after_resp = true;   /* dispatcher: RESP first, drain TX, reboot */
    return WUPS_FW_XFER_OK;
}
