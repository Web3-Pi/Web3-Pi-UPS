/*
 * OTA-1 — firmware update engine. See fw_ota.h for the three paths.
 *
 * Path 1 (self-OTA over LTE): WS-9 net.fw_update REQ → validate + ACK
 * (result byte on t/{iccid}/cmd/response) → download task streams the image
 * over HTTPS (esp_https_ota advanced API, cert bundle, redirects followed)
 * into the passive OTA slot → SHA-256 read-back of the written bytes is
 * compared against the commanded digest → esp_https_ota_finish flips the
 * boot partition → reboot. The freshly booted image is PENDING_VERIFY
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) until the uplink proves healthy.
 *
 * Path 2 (fw_xfer receiver, Workbench over USB): net.fw_xfer_begin/data/end
 * REQs land here from wups_link's dispatch; esp_ota_* writes the passive
 * slot, END(commit=1) runs the SAME read-back verify, then set-boot +
 * reboot. The rebooted image goes through the identical PENDING_VERIFY /
 * mark-valid-on-healthy-uplink rollback machinery — full safety net.
 *
 * Path 3 (RP2040 relay over LTE): net.fw_update target=RP2040 → relay task
 * streams the image over HTTPS chunk-by-chunk and drives the fw_xfer sender
 * over the UART link (strict stop-and-wait), hashing the stream on the fly;
 * END(commit=1) is only sent when the digest matches the commanded sha256.
 */

#include "fw_ota.h"
#include "arkiv_ack.h"
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
#include "freertos/semphr.h"
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

/* Relay (path 3) knobs. The HTTP read buffer is sliced into ≤236-byte
 * (WUPS_FW_XFER_CHUNK) stop-and-wait frames; per-chunk RESP timeout is 5 s
 * with one retry (the RP2040 stalls its UART RX while erasing flash). END
 * (commit=1) gets a single, longer wait — the RP2040 verifies + applies the
 * whole staged image before answering. Image cap is loose on purpose: the
 * RP2040's own BEGIN validation (staging area size) is authoritative, and
 * BEGIN is sent before the first HTTP byte, so an oversized image fails
 * fast with zero LTE bytes spent. */
#define FW_RELAY_HTTP_BUF          2048
#define FW_RELAY_RESP_TIMEOUT_MS   5000
#define FW_RELAY_END_TIMEOUT_MS    30000
#define FW_RELAY_HELLO_TIMEOUT_MS  90000
#define FW_RELAY_IMAGE_MAX         (2u * 1024u * 1024u)   /* RP2040 flash */

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

/* fw_xfer receiver session (path 2). Guarded by s_xfer_lock — REQs run in
 * the wups_rx task, the idle guard on the heartbeat task. */
static SemaphoreHandle_t      s_xfer_lock;
static bool                   s_xfer_open;
static esp_ota_handle_t       s_xfer_handle;
static const esp_partition_t *s_xfer_part;
static uint32_t               s_xfer_image_len;
static uint32_t               s_xfer_received;
static uint8_t                s_xfer_sha[32];
static int64_t                s_xfer_last_us;

/* fw_xfer sender stop-and-wait correlation (path 3). Armed by the relay
 * task before each REQ; released by fw_ota_xfer_on_resp (wups_rx task). */
static SemaphoreHandle_t s_resp_sig;
static volatile bool     s_resp_armed;
static volatile uint8_t  s_resp_op;
static volatile uint8_t  s_resp_seq;
static volatile uint8_t  s_resp_result;

/* RP2040 reboot detector (path 3) — bumped on every system.hello from the
 * RP2040; the relay task snapshots + polls it across END(commit=1). */
static volatile uint32_t s_rp2040_hello_count;

/* Claim arbitration: fw_ota_request runs in the esp-mqtt task, the fw_xfer
 * receiver in the wups_rx task — check-and-set must be atomic. */
static portMUX_TYPE s_claim_mux = portMUX_INITIALIZER_UNLOCKED;

bool fw_ota_in_progress(void) { return s_in_progress; }

/* Atomically claim the single "an update is running" slot shared by all
 * three paths. Release is a plain `s_in_progress = false` (single owner). */
static bool claim_in_progress(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_claim_mux);
    if (!s_in_progress) {
        s_in_progress = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_claim_mux);
    return ok;
}

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
 * also proves the flash writes themselves landed intact. Shared by the LTE
 * self-OTA (path 1) and the fw_xfer receiver (path 2). */
static bool verify_written_image(const esp_partition_t *part, uint32_t len,
                                 const uint8_t expected[32])
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
    if (memcmp(digest, expected, sizeof(digest)) != 0) {
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
    if (!verify_written_image(s_update_part, s_image_len, s_sha_expected)) {
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

/* --- RP2040 relay (path 3) ----------------------------------------------- */

static const char *xfer_result_name(uint8_t r)
{
    switch (r) {
    case WUPS_FW_XFER_OK:           return "ok";
    case WUPS_FW_XFER_BAD_REQ:      return "bad_req";
    case WUPS_FW_XFER_BUSY:         return "busy";
    case WUPS_FW_XFER_SEQ_MISMATCH: return "seq_mismatch";
    case WUPS_FW_XFER_FLASH_ERR:    return "flash_err";
    case WUPS_FW_XFER_VERIFY_FAIL:  return "verify_fail";
    case 0xFF:                      return "link timeout";
    default:                        return "?";
    }
}

/* Send one fw_xfer REQ to the RP2040 and block for its RESP result byte.
 * STRICTLY stop-and-wait (protocol.h: the RP2040 stalls its UART RX while
 * erasing/programming flash, so pipelined frames would be lost). One retry
 * on timeout; NB a retry whose first copy WAS processed comes back as
 * SEQ_MISMATCH and correctly aborts the transfer — the protocol trades that
 * rare restart for receiver simplicity. Returns the result byte, 0xFF on
 * double timeout. Relay-task context only (single in-flight REQ). */
static uint8_t xfer_send_req(uint8_t op, const void *payload, uint16_t len,
                             uint32_t timeout_ms)
{
    static uint8_t seq;   /* own counter — RESP is matched by op+seq */
    for (int attempt = 0; attempt < 2; ++attempt) {
        seq++;
        xSemaphoreTake(s_resp_sig, 0);        /* drain a stale late RESP */
        s_resp_op     = op;
        s_resp_seq    = seq;
        s_resp_result = 0xFF;
        s_resp_armed  = true;                 /* arm BEFORE sending */
        wups_link_send_seq(WUPS_ADDR_RP2040, WUPS_CLASS_NET, op,
                           WUPS_FLAG_REQ, seq, payload, len);
        if (xSemaphoreTake(s_resp_sig, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return s_resp_result;
        }
        s_resp_armed = false;
        ESP_LOGW(TAG, "fw_xfer op=0x%02x seq=%u RESP timeout (attempt %d/2)",
                 op, seq, attempt + 1);
    }
    return 0xFF;
}

/* Best-effort session abort on the RP2040 (END commit=0). Short timeout,
 * result ignored — the receiver's own 30 s idle guard is the backstop. */
static void relay_send_abort(void)
{
    wups_net_fw_xfer_end_v1_t end = { .version = 1, .commit = 0, .reserved = {0, 0} };
    (void)xfer_send_req(WUPS_OP_NET_FW_XFER_END, &end, sizeof(end), 2000);
}

/* Stream the image from s_url and push it into the RP2040's staging area as
 * stop-and-wait fw_xfer chunks. Never buffers the whole image (heap is
 * ~270 KB, images 130-250 KB): each HTTP read is hashed + relayed and then
 * reused. Side effect on the bus: RP2040 flash erase stalls its UART0 RX,
 * so some of the CH32X's 1 Hz power.status frames are dropped during the
 * transfer — acceptable, the RP2040's staleness guard tolerates ~5 s. */
static void relay_task(void *arg)
{
    (void)arg;
    esp_http_client_handle_t http = NULL;
    uint8_t *buf = NULL;
    bool begun = false;                /* RP2040 session open → abort on fail */
    const char *stage = "begin";
    const char *detail = "";
    uint8_t r;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);         /* init early — freed on every exit */
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)FW_OTA_TIMEOUT_S * 1000000;

    ESP_LOGW(TAG, "RP2040 relay start: %s (%" PRIu32 " B)", s_url, s_image_len);
    ota_ui_banner("FW UPDATE");
    emit_status("{\"fw_update\":\"started\",\"len\":%" PRIu32
                ",\"target\":\"rp2040\"}", s_image_len);

    /* 1) Open the receiver session BEFORE downloading — a rejected/oversized
     *    image fails fast with zero LTE bytes spent. */
    {
        wups_net_fw_xfer_begin_v1_t begin = {
            .version   = 1,
            .target    = WUPS_FW_TARGET_RP2040,
            .reserved  = {0, 0},
            .image_len = s_image_len,
        };
        memcpy(begin.sha256, s_sha_expected, sizeof(begin.sha256));
        r = xfer_send_req(WUPS_OP_NET_FW_XFER_BEGIN, &begin, sizeof(begin),
                          FW_RELAY_RESP_TIMEOUT_MS);
        if (r != WUPS_FW_XFER_OK) {
            detail = xfer_result_name(r);
            goto fail;
        }
        begun = true;
    }

    /* 2) HTTPS download, streamed. Same trust anchors as the self path
     *    (https:// enforced in fw_ota_request, cert bundle); redirects are
     *    followed manually — the open/read API doesn't auto-follow. */
    stage = "download";
    {
        esp_http_client_config_t http_cfg = {
            .url               = s_url,
            .timeout_ms        = FW_OTA_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 2048,
            .keep_alive_enable = true,
        };
        http = esp_http_client_init(&http_cfg);
        if (!http) {
            detail = "http init failed";
            goto fail;
        }
        int status = 0;
        for (int hop = 0; hop < 4; ++hop) {
            esp_err_t err = esp_http_client_open(http, 0);
            if (err != ESP_OK) {
                detail = esp_err_to_name(err);
                goto fail;
            }
            (void)esp_http_client_fetch_headers(http);
            status = esp_http_client_get_status_code(http);
            if (status != 301 && status != 302 && status != 303 &&
                status != 307 && status != 308) {
                break;
            }
            if (esp_http_client_set_redirection(http) != ESP_OK) {
                detail = "bad redirect";
                goto fail;
            }
            esp_http_client_close(http);
        }
        if (status != 200) {
            ESP_LOGE(TAG, "http status %d", status);
            detail = "http status";
            goto fail;
        }
    }

    buf = malloc(FW_RELAY_HTTP_BUF);
    if (!buf) {
        detail = "no heap";
        goto fail;
    }
    mbedtls_sha256_starts(&sha, 0);

    /* 3) Stream → hash → chunk → stop-and-wait relay. */
    {
        uint32_t sent = 0;
        int last_step_pct = 0;
        while (sent < s_image_len) {
            if (esp_timer_get_time() >= deadline_us) {
                stage = "timeout";
                detail = "OTA time cap exceeded";
                goto fail;
            }
            uint32_t want = s_image_len - sent;
            if (want > FW_RELAY_HTTP_BUF) want = FW_RELAY_HTTP_BUF;
            int n = esp_http_client_read(http, (char *)buf, (int)want);
            if (n < 0) {
                detail = "http read error";
                goto fail;
            }
            if (n == 0) {
                ESP_LOGE(TAG, "stream ended at %" PRIu32 "/%" PRIu32 " B",
                         sent, s_image_len);
                detail = "short stream";
                goto fail;
            }
            mbedtls_sha256_update(&sha, buf, (size_t)n);

            for (int off = 0; off < n; ) {
                uint16_t chunk = (uint16_t)(n - off);
                if (chunk > WUPS_FW_XFER_CHUNK) chunk = WUPS_FW_XFER_CHUNK;
                uint8_t fbuf[WUPS_MAX_PAYLOAD];
                wups_net_fw_xfer_data_v1_hdr_t dh = { .offset = sent };
                memcpy(fbuf, &dh, sizeof(dh));
                memcpy(fbuf + sizeof(dh), buf + off, chunk);
                r = xfer_send_req(WUPS_OP_NET_FW_XFER_DATA, fbuf,
                                  (uint16_t)(sizeof(dh) + chunk),
                                  FW_RELAY_RESP_TIMEOUT_MS);
                if (r != WUPS_FW_XFER_OK) {
                    detail = xfer_result_name(r);
                    goto fail;
                }
                sent += chunk;
                off  += chunk;
            }

            int pct = (int)(((int64_t)sent * 100) / (int64_t)s_image_len);
            if (pct >= last_step_pct + 25 && pct < 100) {
                last_step_pct = pct - (pct % 25);
                emit_status("{\"fw_update\":\"progress\",\"pct\":%d,"
                            "\"target\":\"rp2040\"}", last_step_pct);
            }
        }
    }

    /* 4) The stream digest must match the commanded sha256 BEFORE we let the
     *    RP2040 commit — never commit garbage remotely. (The RP2040 runs its
     *    own staged-flash read-back against the BEGIN digest too.) */
    stage = "verify";
    {
        uint8_t digest[32];
        mbedtls_sha256_finish(&sha, digest);
        if (memcmp(digest, s_sha_expected, sizeof(digest)) != 0) {
            ESP_LOGE(TAG, "stream SHA-256 MISMATCH — aborting remote session");
            detail = "sha256 mismatch";
            goto fail;
        }
    }
    emit_status("{\"fw_update\":\"verifying\",\"target\":\"rp2040\"}");

    /* 5) Commit. Snapshot the hello counter FIRST — a fast RP2040 reboot
     *    must not be missed. The END RESP wait is long: the RP2040 verifies
     *    + applies the whole staged image before answering. */
    stage = "commit";
    {
        uint32_t hello0 = s_rp2040_hello_count;
        wups_net_fw_xfer_end_v1_t end = { .version = 1, .commit = 1,
                                          .reserved = {0, 0} };
        r = xfer_send_req(WUPS_OP_NET_FW_XFER_END, &end, sizeof(end),
                          FW_RELAY_END_TIMEOUT_MS);
        if (r != WUPS_FW_XFER_OK) {
            begun = false;             /* session is spent either way */
            detail = xfer_result_name(r);
            goto fail;
        }
        emit_status("{\"fw_update\":\"rebooting\",\"target\":\"rp2040\"}");

        /* 6) Watch for the RP2040's boot hello (new image alive). */
        stage = "hello";
        const int64_t hello_deadline =
            esp_timer_get_time() + (int64_t)FW_RELAY_HELLO_TIMEOUT_MS * 1000;
        bool hello_seen = false;
        while (esp_timer_get_time() < hello_deadline) {
            if (s_rp2040_hello_count != hello0) {
                hello_seen = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (!hello_seen) {
            begun = false;
            detail = "no hello after commit";
            goto fail;
        }
        ESP_LOGW(TAG, "RP2040 relay complete — new image said hello");
        emit_status("{\"fw_update\":\"done\",\"target\":\"rp2040\"}");
    }

    esp_http_client_cleanup(http);
    free(buf);
    mbedtls_sha256_free(&sha);
    ota_ui_banner(NULL);
    s_in_progress = false;   /* clear LAST — modem watchdog resumes now */
    vTaskDelete(NULL);
    return;                  /* unreached */

fail:
    ESP_LOGE(TAG, "RP2040 relay failed at %s: %s", stage, detail);
    if (begun) relay_send_abort();
    if (http) esp_http_client_cleanup(http);
    free(buf);
    mbedtls_sha256_free(&sha);
    emit_status("{\"fw_update\":\"error\",\"stage\":\"%s\",\"detail\":\"%s\","
                "\"target\":\"rp2040\"}", stage, detail);
    ota_ui_banner(NULL);
    s_in_progress = false;   /* clear LAST — modem watchdog resumes now */
    vTaskDelete(NULL);
}

/* --- public API ---------------------------------------------------------- */

esp_err_t fw_ota_request(const char *url, const char *sha256_hex,
                         uint32_t image_len, uint8_t target)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "update already in progress — rejecting");
        return ESP_ERR_INVALID_STATE;
    }
    if (target != WUPS_FW_TARGET_ESP32 && target != WUPS_FW_TARGET_RP2040) {
        ESP_LOGW(TAG, "rejecting unknown fw target %u", target);
        return ESP_ERR_INVALID_ARG;
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

    const esp_partition_t *part = NULL;
    if (target == WUPS_FW_TARGET_ESP32) {
        part = esp_ota_get_next_update_partition(NULL);
        if (!part) {
            ESP_LOGE(TAG, "no OTA update partition — partition table lacks ota_0/ota_1?");
            return ESP_ERR_NOT_FOUND;
        }
        if (image_len == 0 || image_len > part->size) {
            ESP_LOGW(TAG, "rejecting image_len %" PRIu32 " (slot %s holds %" PRIu32 ")",
                     image_len, part->label, part->size);
            return ESP_ERR_INVALID_ARG;
        }

        /* Brick-guard: if the running image is STILL pending-verify (fresh
         * OTA, no supervision tick yet — e.g. this command arrived within
         * seconds of MQTT CONNECT), confirm it NOW: an authenticated
         * fw.update received over MQTT is itself proof of a healthy uplink.
         * Without this the download would overwrite the only other bootable
         * slot while rollback is still armed — a rollback then boots a
         * half-written image and leaves BOTH slots unbootable. */
        fw_ota_mark_uplink_healthy();
        if (s_pending_verify) {
            /* mark-valid failed (otadata write error) — refuse rather than
             * clobber the rollback slot with rollback still armed. The next
             * healthy supervision tick retries the mark. */
            ESP_LOGE(TAG, "running image still PENDING_VERIFY — refusing update");
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        /* RP2040 relay: the image never touches our flash — the RP2040's
         * own BEGIN validation is authoritative on size, this is a sanity
         * cap only (its whole flash is 2 MB). No brick-guard needed either:
         * our slots stay untouched. */
        if (image_len == 0 || image_len > FW_RELAY_IMAGE_MAX) {
            ESP_LOGW(TAG, "rejecting relay image_len %" PRIu32, image_len);
            return ESP_ERR_INVALID_ARG;
        }
    }

    strcpy(s_url, url);              /* length checked above */
    s_image_len   = image_len;
    s_update_part = part;            /* NULL for the relay path */

    /* Claim BEFORE the task runs — freezes the modem uplink watchdog
     * immediately. Atomic: the fw_xfer receiver (wups_rx task) races us. */
    if (!claim_in_progress()) {
        ESP_LOGW(TAG, "update already in progress — rejecting");
        return ESP_ERR_INVALID_STATE;
    }

    TaskFunction_t entry = (target == WUPS_FW_TARGET_RP2040) ? relay_task
                                                             : ota_task;
    if (xTaskCreate(entry, "fw_ota", 8192, NULL, 3, NULL) != pdPASS) {
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

        /* Target dispatch: 0 (legacy encoders) and WUPS_FW_TARGET_ESP32 are
         * the self-OTA; WUPS_FW_TARGET_RP2040 relays the image over the
         * fw_xfer UART link. Anything else is rejected by fw_ota_request. */
        uint8_t target = (hdr.target == 0) ? WUPS_FW_TARGET_ESP32 : hdr.target;

        if (!modem_ppp_is_up()) {
            /* Shouldn't normally happen (the command arrived over MQTT-over-
             * PPP), but the link can drop between receipt and handling. */
            ESP_LOGW(TAG, "fw.update while PPP is down — refusing");
            result = WUPS_FW_UPDATE_RESULT_NO_NET;
        } else {
            esp_err_t err = fw_ota_request(url, sha_hex, hdr.image_len, target);
            switch (err) {
            case ESP_OK:                result = WUPS_FW_UPDATE_RESULT_OK;      break;
            case ESP_ERR_INVALID_STATE: result = WUPS_FW_UPDATE_RESULT_BUSY;    break;
            default:                    result = WUPS_FW_UPDATE_RESULT_BAD_REQ; break;
            }
        }
    }

reply:
    {
        /* Arkiv-issued command (arkiv_rpc tracked SEQ -> command_id before
         * intercepting): ACK as a w3pups-ack entity — this mode has no
         * broker, the MQTT publish below would be a silent no-op and the
         * panel row would sit "pending" until timeout. */
        if (arkiv_ack_has_pending(seq)) {
            uint8_t pl = result;
            (void)arkiv_ack_emit(seq, &pl, 1);
        } else {
            uint8_t resp[WUPS_FRAMING_BYTES + 1];
            uint16_t n = encode_resp_frame(resp, sizeof(resp), src, seq, result);
            const char *topic = mqtt_topic_cmd_response();
            if (n && topic[0]) {
                (void)mqtt_publish_raw(topic, resp, n, /*qos=*/1, /*retain=*/0);
            }
        }
        ESP_LOGI(TAG, "fw.update ACKed (seq=%u result=%u)", seq, result);
    }
    return true;
}

/* --- fw_xfer receiver (path 2 — Workbench over USB) ---------------------- */

/* Abort the open receive session and release the update slot. Caller holds
 * s_xfer_lock. */
static void xfer_abort_locked(const char *why)
{
    if (!s_xfer_open) return;
    ESP_LOGW(TAG, "fw_xfer abort at %" PRIu32 "/%" PRIu32 " B: %s",
             s_xfer_received, s_xfer_image_len, why);
    esp_ota_abort(s_xfer_handle);
    s_xfer_open = false;
    ota_ui_banner(NULL);
    s_in_progress = false;   /* modem watchdog resumes */
}

static uint8_t xfer_begin(const uint8_t *payload, uint16_t len)
{
    wups_net_fw_xfer_begin_v1_t req;
    if (len < sizeof(req)) return WUPS_FW_XFER_BAD_REQ;
    memcpy(&req, payload, sizeof(req));
    if (req.version != 1) return WUPS_FW_XFER_BAD_REQ;
    if (req.target != WUPS_FW_TARGET_ESP32) {
        ESP_LOGW(TAG, "fw_xfer BEGIN for target %u — not us", req.target);
        return WUPS_FW_XFER_BAD_REQ;
    }

    /* BEGIN implicitly aborts a session in progress (protocol.h) … */
    if (s_xfer_open) xfer_abort_locked("BEGIN while open");
    /* … but never the LTE download task's claim. */
    if (s_in_progress) return WUPS_FW_XFER_BUSY;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "no OTA update partition");
        return WUPS_FW_XFER_FLASH_ERR;
    }
    if (req.image_len == 0 || req.image_len > part->size) {
        ESP_LOGW(TAG, "fw_xfer image_len %" PRIu32 " vs slot %" PRIu32,
                 req.image_len, part->size);
        return WUPS_FW_XFER_BAD_REQ;
    }

    /* Same brick-guard as the LTE path: writing the passive slot while the
     * RUNNING image is still pending-verify would arm a rollback into a
     * half-written image. A local push is deliberate operator action
     * (physical access is this path's auth model), so confirming the
     * running image first is the right call — it then becomes the rollback
     * target for the image we're about to stage. */
    fw_ota_mark_uplink_healthy();
    if (s_pending_verify) {
        ESP_LOGE(TAG, "running image still PENDING_VERIFY — refusing fw_xfer");
        return WUPS_FW_XFER_BUSY;
    }

    if (!claim_in_progress()) return WUPS_FW_XFER_BUSY;

    /* Sequential-writes mode erases sector-by-sector during DATA instead of
     * the whole slot here — BEGIN answers fast (the sender's RESP timeout
     * is 5 s) and each per-chunk stall stays small. */
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_xfer_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_in_progress = false;
        return WUPS_FW_XFER_FLASH_ERR;
    }
    s_xfer_part      = part;
    s_xfer_image_len = req.image_len;
    s_xfer_received  = 0;
    memcpy(s_xfer_sha, req.sha256, sizeof(s_xfer_sha));
    s_xfer_open = true;

    ESP_LOGW(TAG, "fw_xfer session open: %" PRIu32 " B -> %s",
             req.image_len, part->label);
    ota_ui_banner("FW UPDATE");
    emit_status("{\"fw_update\":\"started\",\"len\":%" PRIu32
                ",\"slot\":\"%s\",\"via\":\"usb\"}",
                req.image_len, part->label);
    return WUPS_FW_XFER_OK;
}

static uint8_t xfer_data(const uint8_t *payload, uint16_t len)
{
    if (!s_xfer_open) return WUPS_FW_XFER_BAD_REQ;
    if (len < sizeof(wups_net_fw_xfer_data_v1_hdr_t)) return WUPS_FW_XFER_BAD_REQ;

    wups_net_fw_xfer_data_v1_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    const uint8_t *chunk = payload + sizeof(hdr);
    uint16_t chunk_len = (uint16_t)(len - sizeof(hdr));

    if (hdr.offset != s_xfer_received) {
        /* Contiguity violation — per protocol.h the sender must restart
         * from BEGIN, so drop the session rather than hold a stale one. */
        xfer_abort_locked("offset mismatch");
        return WUPS_FW_XFER_SEQ_MISMATCH;
    }
    if ((uint64_t)hdr.offset + chunk_len > s_xfer_image_len) {
        xfer_abort_locked("write past image_len");
        return WUPS_FW_XFER_BAD_REQ;
    }
    if (chunk_len) {
        esp_err_t err = esp_ota_write(s_xfer_handle, chunk, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            xfer_abort_locked("flash write");
            return WUPS_FW_XFER_FLASH_ERR;
        }
        s_xfer_received += chunk_len;
    }
    return WUPS_FW_XFER_OK;
}

static uint8_t xfer_end(const uint8_t *payload, uint16_t len, bool *reboot)
{
    *reboot = false;
    if (!s_xfer_open) return WUPS_FW_XFER_BAD_REQ;

    wups_net_fw_xfer_end_v1_t req;
    if (len < sizeof(req)) return WUPS_FW_XFER_BAD_REQ;   /* session stays */
    memcpy(&req, payload, sizeof(req));
    if (req.version != 1) return WUPS_FW_XFER_BAD_REQ;

    if (!req.commit) {
        xfer_abort_locked("END commit=0");
        return WUPS_FW_XFER_OK;
    }
    if (s_xfer_received != s_xfer_image_len) {
        xfer_abort_locked("commit with incomplete image");
        return WUPS_FW_XFER_BAD_REQ;
    }

    /* The handle is consumed by esp_ota_end either way. */
    s_xfer_open = false;
    esp_err_t err = esp_ota_end(s_xfer_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_ui_banner(NULL);
        s_in_progress = false;
        return (err == ESP_ERR_OTA_VALIDATE_FAILED) ? WUPS_FW_XFER_VERIFY_FAIL
                                                    : WUPS_FW_XFER_FLASH_ERR;
    }

    /* Same read-back SHA-256 as the LTE path, against the BEGIN digest. */
    emit_status("{\"fw_update\":\"verifying\",\"via\":\"usb\"}");
    if (!verify_written_image(s_xfer_part, s_xfer_image_len, s_xfer_sha)) {
        ota_ui_banner(NULL);
        s_in_progress = false;
        return WUPS_FW_XFER_VERIFY_FAIL;
    }

    err = esp_ota_set_boot_partition(s_xfer_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        ota_ui_banner(NULL);
        s_in_progress = false;
        return WUPS_FW_XFER_FLASH_ERR;
    }

    /* Rollback is armed exactly like the LTE path: set-boot marks the slot
     * ESP_OTA_IMG_NEW, the bootloader flips it to PENDING_VERIFY, and the
     * healthy-uplink supervision either confirms or rolls back. */
    *reboot = true;
    return WUPS_FW_XFER_OK;
}

void fw_ota_xfer_on_req(uint8_t op, uint8_t src, uint8_t seq,
                        const uint8_t *payload, uint16_t len)
{
    uint8_t result = WUPS_FW_XFER_BAD_REQ;
    bool reboot = false;

    if (!s_xfer_lock) {                      /* boot ordering safety net */
        wups_link_send_seq(src, WUPS_CLASS_NET, op, WUPS_FLAG_RESP, seq,
                           &result, 1);
        return;
    }

    xSemaphoreTake(s_xfer_lock, portMAX_DELAY);
    switch (op) {
    case WUPS_OP_NET_FW_XFER_BEGIN: result = xfer_begin(payload, len);        break;
    case WUPS_OP_NET_FW_XFER_DATA:  result = xfer_data(payload, len);         break;
    case WUPS_OP_NET_FW_XFER_END:   result = xfer_end(payload, len, &reboot); break;
    default:                                                                  break;
    }
    s_xfer_last_us = esp_timer_get_time();
    xSemaphoreGive(s_xfer_lock);

    /* Every REQ gets its RESP — including the committed END, sent BEFORE
     * the reboot below (uart TX is synchronous, the frame is out before
     * esp_restart). */
    wups_link_send_seq(src, WUPS_CLASS_NET, op, WUPS_FLAG_RESP, seq,
                       &result, 1);

    if (reboot) {
        ESP_LOGW(TAG, "fw_xfer commit OK — rebooting into %s (rollback armed)",
                 s_xfer_part->label);
        emit_status("{\"fw_update\":\"rebooting\",\"via\":\"usb\"}");
        /* QoS-flush: give esp-mqtt time to push the status over PPP+TLS
         * (same rationale as the LTE path's pre-reboot delay). */
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

void fw_ota_xfer_on_resp(uint8_t op, uint8_t seq,
                         const uint8_t *payload, uint16_t len)
{
    if (!s_resp_armed || op != s_resp_op || seq != s_resp_seq || len < 1) return;
    s_resp_result = payload[0];
    s_resp_armed  = false;
    if (s_resp_sig) xSemaphoreGive(s_resp_sig);
}

void fw_ota_xfer_tick(void)
{
    if (!s_xfer_open || !s_xfer_lock) return;
    /* Zero-timeout take: if the lock is held the session is mid-frame right
     * now, i.e. not idle — check again on the next heartbeat. */
    if (xSemaphoreTake(s_xfer_lock, 0) != pdTRUE) return;
    bool aborted = false;
    if (s_xfer_open &&
        esp_timer_get_time() - s_xfer_last_us >
            (int64_t)WUPS_FW_XFER_IDLE_TIMEOUT_S * 1000000) {
        xfer_abort_locked("idle timeout");
        aborted = true;
    }
    xSemaphoreGive(s_xfer_lock);
    /* QoS-1 publish OUTSIDE the lock: with the lock held this could park the
     * heartbeat task on the esp-mqtt API for up to network.timeout_ms while
     * rx_task waits on the same lock at portMAX_DELAY for the next fw_xfer
     * frame — wedging the whole RP2040 link. */
    if (aborted) {
        emit_status("{\"fw_update\":\"error\",\"stage\":\"xfer\","
                    "\"detail\":\"idle timeout\",\"via\":\"usb\"}");
    }
}

void fw_ota_notify_rp2040_hello(void)
{
    s_rp2040_hello_count++;
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
    /* Created here — app_main calls this once, BEFORE wups_link_init brings
     * the UART up, so no fw_xfer frame can ever race the creation. */
    if (!s_xfer_lock) s_xfer_lock = xSemaphoreCreateMutex();
    if (!s_resp_sig)  s_resp_sig  = xSemaphoreCreateBinary();

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
