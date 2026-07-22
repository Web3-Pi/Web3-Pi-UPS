/*
 * OTA-1 — LTE OTA firmware update engine. See fw_ota.h for the contract.
 *
 * Flow: WS-9 net.fw_update REQ → validate + ACK (result byte on
 * t/{iccid}/cmd/response) → download task streams the image over HTTPS
 * (esp_https_ota advanced API, cert bundle, redirects followed) into the
 * passive OTA slot → SHA-256 read-back of the written bytes is compared
 * against the commanded digest → esp_https_ota_finish flips the boot
 * partition → reboot. The freshly booted image is PENDING_VERIFY
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) until the uplink proves healthy.
 */

#include "fw_ota.h"
#include "identity.h"
#include "modem.h"
#include "mqtt.h"
#include "wups_link.h"
#include "wups_proto.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* IDF 6.0 / mbedtls 4.x: the low-level mbedtls_sha256_* API is the project
 * convention (the mbedtls_md_* wrapper fails at runtime here — see
 * http_backend.c). legacy sha256.h moved under mbedtls/private/. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>

#define TAG "fw_ota"

/* URL cap = what physically fits in one net.fw_update frame. */
#define FW_OTA_URL_MAX \
    (WUPS_MAX_PAYLOAD - sizeof(wups_net_fw_update_v1_hdr_t))   /* 168 */

/* Per-socket-op HTTP timeout. The TOTAL attempt is separately capped by
 * FW_OTA_TIMEOUT_S — a ~0.9 MB image over Cat-M takes minutes. */
#define FW_OTA_HTTP_TIMEOUT_MS  30000

/* SHA-256 read-back chunk (one flash sector). */
#define FW_OTA_READBACK_CHUNK   4096

/* --- state --------------------------------------------------------------- */

static volatile bool s_in_progress;

/* One update at a time — the task reads these, set by fw_ota_request(). */
static char     s_url[FW_OTA_URL_MAX + 1];
static uint8_t  s_sha_expected[32];
static uint32_t s_image_len;
static const esp_partition_t *s_update_part;

/* Rollback bookkeeping (see fw_ota.h). */
static bool s_pending_verify;
static bool s_marked_valid;

bool fw_ota_in_progress(void) { return s_in_progress; }

/* --- helpers ------------------------------------------------------------- */

/* Compact JSON status on t/{iccid}/event — same "small JSON on an uplink
 * topic" shape as mqtt.c's identify/status. QoS 1, not retained (point-in-
 * time signals; a replayed "rebooting" weeks later would mislead). */
static void emit_status(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void emit_status(const char *fmt, ...)
{
    char json[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json, sizeof(json), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(json)) return;
    ESP_LOGI(TAG, "%s", json);
    const char *topic = mqtt_topic_event();
    if (topic[0]) {
        (void)mqtt_publish_raw(topic, json, (size_t)n, /*qos=*/1, /*retain=*/0);
    }
}

/* "FW UPDATE" banner on the RP2040 OLED (ui.display_msg; NULL = clear) —
 * same frame shape as modem.c's modem_ui_alert(). */
static void ota_ui_banner(const char *msg)
{
    uint8_t buf[4 + 24];
    size_t tl = msg ? strlen(msg) : 0;
    if (tl > 24) tl = 24;
    buf[0] = 1; buf[1] = 0; buf[2] = (uint8_t)tl; buf[3] = 0;
    if (tl) memcpy(buf + 4, msg, tl);
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_UI, WUPS_OP_UI_DISPLAY_MSG,
                   WUPS_FLAG_REQ, buf, (uint16_t)(4 + tl));
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 64 hex chars → 32 raw bytes; false on any non-hex char. */
static bool sha256_hex_decode(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; ++i) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* --- download task ------------------------------------------------------- */

/* SHA-256 the first `len` bytes written to the update partition and compare
 * against the commanded digest. Read-back (rather than hashing the stream)
 * also proves the flash writes themselves landed intact. */
static bool verify_written_image(const esp_partition_t *part, uint32_t len)
{
    uint8_t *chunk = malloc(FW_OTA_READBACK_CHUNK);
    if (!chunk) {
        ESP_LOGE(TAG, "no heap for read-back buffer");
        return false;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (uint32_t off = 0; off < len; off += FW_OTA_READBACK_CHUNK) {
        uint32_t n = len - off;
        if (n > FW_OTA_READBACK_CHUNK) n = FW_OTA_READBACK_CHUNK;
        if (esp_partition_read(part, off, chunk, n) != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_read failed at 0x%" PRIx32, off);
            ok = false;
            break;
        }
        mbedtls_sha256_update(&ctx, chunk, n);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    free(chunk);

    if (!ok) return false;
    if (memcmp(digest, s_sha_expected, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "SHA-256 MISMATCH — refusing to boot the image");
        return false;
    }
    ESP_LOGI(TAG, "SHA-256 verified over %" PRIu32 " read-back bytes", len);
    return true;
}

static void ota_task(void *arg)
{
    (void)arg;
    esp_https_ota_handle_t handle = NULL;
    const char *stage = "begin";
    const char *detail = "";
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)FW_OTA_TIMEOUT_S * 1000000;

    ESP_LOGW(TAG, "OTA start: %s (%" PRIu32 " B) -> %s",
             s_url, s_image_len, s_update_part->label);
    ota_ui_banner("FW UPDATE");
    emit_status("{\"fw_update\":\"started\",\"len\":%" PRIu32 ",\"slot\":\"%s\"}",
                s_image_len, s_update_part->label);

    esp_http_client_config_t http_cfg = {
        .url               = s_url,
        .timeout_ms        = FW_OTA_HTTP_TIMEOUT_MS,
        /* Redirects are followed by esp_http_client's default policy;
         * the cert bundle covers any LE/public-CA hop. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 2048,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        /* partition.staging left NULL — esp_https_ota picks
         * esp_ota_get_next_update_partition(NULL), the same slot we
         * captured as s_update_part in fw_ota_request(). */
    };

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        detail = esp_err_to_name(err);
        goto fail;
    }

    stage = "download";
    int last_step_pct = 0;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (esp_timer_get_time() >= deadline_us) {
            stage = "timeout";
            detail = "OTA time cap exceeded";
            goto fail;
        }
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = (int)(((int64_t)read * 100) / (int64_t)s_image_len);
        if (pct >= last_step_pct + 25 && pct < 100) {
            last_step_pct = pct - (pct % 25);
            emit_status("{\"fw_update\":\"progress\",\"pct\":%d}", last_step_pct);
        }
    }
    if (err != ESP_OK) {
        detail = esp_err_to_name(err);
        goto fail;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        detail = "incomplete data";
        goto fail;
    }

    stage = "verify";
    int len_read = esp_https_ota_get_image_len_read(handle);
    if (len_read < 0 || (uint32_t)len_read != s_image_len) {
        ESP_LOGE(TAG, "length mismatch: got %d, commanded %" PRIu32,
                 len_read, s_image_len);
        detail = "length mismatch";
        goto fail;
    }
    emit_status("{\"fw_update\":\"verifying\"}");
    if (!verify_written_image(s_update_part, s_image_len)) {
        detail = "sha256 mismatch";
        goto fail;
    }

    /* Digest good — let esp_https_ota validate the image and flip the boot
     * partition. From here a failure means we did NOT change the boot slot. */
    stage = "finish";
    err = esp_https_ota_finish(handle);
    handle = NULL;
    if (err != ESP_OK) {
        detail = esp_err_to_name(err);
        goto fail;
    }

    ESP_LOGW(TAG, "OTA complete — rebooting into %s (rollback armed)",
             s_update_part->label);
    emit_status("{\"fw_update\":\"rebooting\"}");
    /* Give esp-mqtt time to flush the QoS-1 status over PPP+TLS (same
     * rationale as backend_mode's pre-reboot flush). The OLED banner is
     * left up — the reboot + RP2040 banner TTL clear it. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    /* unreached */

fail:
    ESP_LOGE(TAG, "OTA failed at %s: %s", stage, detail);
    if (handle) {
        esp_https_ota_abort(handle);
    }
    emit_status("{\"fw_update\":\"error\",\"stage\":\"%s\",\"detail\":\"%s\"}",
                stage, detail);
    ota_ui_banner(NULL);
    s_in_progress = false;   /* clear LAST — modem watchdog resumes now */
    vTaskDelete(NULL);
}

/* --- public API ---------------------------------------------------------- */

esp_err_t fw_ota_request(const char *url, const char *sha256_hex,
                         uint32_t image_len)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "update already in progress — rejecting");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || strncmp(url, "https://", 8) != 0 ||
        strlen(url) > FW_OTA_URL_MAX) {
        ESP_LOGW(TAG, "rejecting URL (https:// only, <= %u chars)",
                 (unsigned)FW_OTA_URL_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (!sha256_hex || strlen(sha256_hex) != 64 ||
        !sha256_hex_decode(sha256_hex, s_sha_expected)) {
        ESP_LOGW(TAG, "rejecting sha256 (need 64 hex chars)");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA update partition — partition table lacks ota_0/ota_1?");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_len == 0 || image_len > part->size) {
        ESP_LOGW(TAG, "rejecting image_len %" PRIu32 " (slot %s holds %" PRIu32 ")",
                 image_len, part->label, part->size);
        return ESP_ERR_INVALID_ARG;
    }

    /* Brick-guard: if the running image is STILL pending-verify (fresh OTA,
     * no supervision tick yet — e.g. this command arrived within seconds of
     * MQTT CONNECT), confirm it NOW: an authenticated fw.update received
     * over MQTT is itself proof of a healthy uplink. Without this the
     * download would overwrite the only other bootable slot while rollback
     * is still armed — a rollback then boots a half-written image and
     * leaves BOTH slots unbootable. */
    fw_ota_mark_uplink_healthy();
    if (s_pending_verify) {
        /* mark-valid failed (otadata write error) — refuse rather than
         * clobber the rollback slot with rollback still armed. The next
         * healthy supervision tick retries the mark. */
        ESP_LOGE(TAG, "running image still PENDING_VERIFY — refusing update");
        return ESP_ERR_INVALID_STATE;
    }

    strcpy(s_url, url);              /* length checked above */
    s_image_len   = image_len;
    s_update_part = part;
    s_in_progress = true;            /* set BEFORE the task runs — freezes the
                                      * modem uplink watchdog immediately */

    if (xTaskCreate(ota_task, "fw_ota", 8192, NULL, 3, NULL) != pdPASS) {
        s_in_progress = false;
        ESP_LOGE(TAG, "fw_ota task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --- WS-9 downlink hook -------------------------------------------------- */

/* Full inner WUPS RESP frame (dst = the REQ's src, result byte at
 * payload[0] — the service/panel cmd/response convention) into out[cap]. */
static uint16_t encode_resp_frame(uint8_t *out, size_t cap,
                                  uint8_t dst, uint8_t seq, uint8_t result)
{
    const uint16_t total = WUPS_FRAMING_BYTES + 1;
    if (cap < total) return 0;
    out[0] = WUPS_SYNC1;
    out[1] = WUPS_SYNC2;
    out[2] = dst;
    out[3] = WUPS_ADDR_ESP32;
    out[4] = WUPS_CLASS_NET;
    out[5] = WUPS_OP_NET_FW_UPDATE;
    out[6] = WUPS_FLAG_RESP;
    out[7] = seq;
    out[8] = 1;                       /* LEN_L */
    out[9] = 0;                       /* LEN_H */
    out[10] = result;
    uint8_t a, b;
    wups_fletcher8(out + 2, 9, &a, &b);
    out[11] = a;
    out[12] = b;
    out[13] = WUPS_END1;
    out[14] = WUPS_END2;
    return total;
}

bool fw_ota_try_handle_downlink(const uint8_t *frame, size_t frame_len)
{
    if (!frame || frame_len < WUPS_FRAMING_BYTES) return false;
    if (frame[0] != WUPS_SYNC1 || frame[1] != WUPS_SYNC2) return false;
    if (frame[4] != WUPS_CLASS_NET || frame[5] != WUPS_OP_NET_FW_UPDATE) {
        return false;                 /* not ours — forward as usual */
    }

    /* From here the frame is consumed: even a malformed fw.update must
     * never reach the RP2040 (this op is ESP32-local by definition). */
    const uint8_t src   = frame[3];
    const uint8_t flags = frame[6];
    const uint8_t seq   = frame[7];
    uint8_t result = WUPS_FW_UPDATE_RESULT_BAD_REQ;

    if (!(flags & WUPS_FLAG_REQ)) {
        ESP_LOGW(TAG, "fw.update without REQ flag — dropping");
        return true;
    }

    const uint16_t plen = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
    wups_net_fw_update_v1_hdr_t hdr;
    if ((size_t)WUPS_FRAMING_BYTES + plen > frame_len ||
        plen < sizeof(hdr)) {
        ESP_LOGW(TAG, "fw.update payload too short (%u)", (unsigned)plen);
        goto reply;
    }
    memcpy(&hdr, frame + WUPS_HEADER_BYTES, sizeof(hdr));
    if (hdr.version != 1 ||
        (size_t)hdr.url_len + sizeof(hdr) > plen ||
        hdr.url_len == 0) {
        ESP_LOGW(TAG, "fw.update malformed (version=%u url_len=%u plen=%u)",
                 hdr.version, hdr.url_len, (unsigned)plen);
        goto reply;
    }

    {
        char url[FW_OTA_URL_MAX + 1];
        char sha_hex[65];
        size_t ul = hdr.url_len;
        if (ul > FW_OTA_URL_MAX) ul = FW_OTA_URL_MAX;  /* can't happen; belt */
        memcpy(url, frame + WUPS_HEADER_BYTES + sizeof(hdr), ul);
        url[ul] = '\0';
        memcpy(sha_hex, hdr.sha256_hex, 64);
        sha_hex[64] = '\0';

        if (!modem_ppp_is_up()) {
            /* Shouldn't normally happen (the command arrived over MQTT-over-
             * PPP), but the link can drop between receipt and handling. */
            ESP_LOGW(TAG, "fw.update while PPP is down — refusing");
            result = WUPS_FW_UPDATE_RESULT_NO_NET;
        } else {
            esp_err_t err = fw_ota_request(url, sha_hex, hdr.image_len);
            switch (err) {
            case ESP_OK:                result = WUPS_FW_UPDATE_RESULT_OK;      break;
            case ESP_ERR_INVALID_STATE: result = WUPS_FW_UPDATE_RESULT_BUSY;    break;
            default:                    result = WUPS_FW_UPDATE_RESULT_BAD_REQ; break;
            }
        }
    }

reply:
    {
        uint8_t resp[WUPS_FRAMING_BYTES + 1];
        uint16_t n = encode_resp_frame(resp, sizeof(resp), src, seq, result);
        const char *topic = mqtt_topic_cmd_response();
        if (n && topic[0]) {
            (void)mqtt_publish_raw(topic, resp, n, /*qos=*/1, /*retain=*/0);
        }
        ESP_LOGI(TAG, "fw.update ACKed (seq=%u result=%u)", seq, result);
    }
    return true;
}

/* --- rollback ------------------------------------------------------------ */

static const char *ota_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "?";
    }
}

void fw_ota_boot_log(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(run, &st);
    s_pending_verify = (err == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    ESP_LOGI(TAG, "running from '%s' @0x%" PRIx32 " (state=%s)",
             run->label, run->address,
             err == ESP_OK ? ota_state_name(st) : esp_err_to_name(err));
    if (s_pending_verify) {
        ESP_LOGW(TAG, "image is PENDING_VERIFY — will roll back unless the "
                      "uplink is healthy within %d s", FW_OTA_VERIFY_WINDOW_S);
    }
}

void fw_ota_mark_uplink_healthy(void)
{
    if (!s_pending_verify || s_marked_valid) return;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        s_marked_valid   = true;
        s_pending_verify = false;
        ESP_LOGW(TAG, "uplink healthy — OTA image marked VALID (rollback cancelled)");
    } else {
        /* Leave BOTH flags untouched: a transient otadata write failure is
         * retried on the next healthy tick, and the rollback tick stays
         * armed as the net. */
        ESP_LOGE(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
    }
}

void fw_ota_rollback_tick(void)
{
    if (!s_pending_verify || s_marked_valid) return;
    /* Belt-and-braces: never roll back while a download is writing the
     * previous slot — rebooting into a half-written image would leave both
     * slots unbootable. (fw_ota_request refuses to start while pending-
     * verify, so this should be unreachable.) */
    if (s_in_progress) return;
    if (esp_timer_get_time() < (int64_t)FW_OTA_VERIFY_WINDOW_S * 1000000) return;
    ESP_LOGE(TAG, "no healthy uplink %d s after boot on a PENDING_VERIFY "
                  "image — rolling back to the previous slot",
             FW_OTA_VERIFY_WINDOW_S);
    vTaskDelay(pdMS_TO_TICKS(100));   /* flush the log line over USB-CDC */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    /* unreached on success; if it failed there is no previous valid image —
     * keep running (better degraded than boot-looping). */
    s_marked_valid = true;
    ESP_LOGE(TAG, "rollback failed — no valid previous image? staying up");
}
