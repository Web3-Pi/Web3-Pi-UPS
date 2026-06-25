/*
 * ADR-0012 — backend-mode persistence + switch / factory reset machinery.
 *
 * Storage: writable default `nvs` partition, namespace `w3mode`:
 *   - `cur_mode`     u8   currently active mode (WUPS_BACKEND_MODE_*).
 *                          Default WUPS_BACKEND_MODE_MQTT if absent.
 *   - `prev_mode`    u8   mode in effect BEFORE the switch we just
 *                          performed. Set by request_switch(); read +
 *                          cleared at boot by emit_post_switch_confirm()
 *                          so we know whether to publish a post-switch
 *                          confirmation event after the new stack comes
 *                          up. Absent on a clean (non-switch) boot.
 *
 * Pre-switch hint event: built inline (we don't want a hard runtime
 * dependency from this module on the mqtt or arkiv_writer modules
 * beyond their public init APIs). The WUPS frame shape matches
 * common/protocol.h and apps/api/src/lib/wupsproto.ts (decodeModeChangedV1).
 */

#include "backend_mode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "../../common/protocol.h"
#include "identity.h"
#include "mqtt.h"
#include "arkiv_writer.h"
#include "cmdauth_arkiv.h"

#define TAG "backend_mode"

#define NS_MODE         "w3mode"
#define KEY_CUR_MODE    "cur_mode"
#define KEY_PREV_MODE   "prev_mode"

static wups_backend_mode_t s_cur_mode = WUPS_BACKEND_MODE_MQTT;

/* --- helpers -------------------------------------------------------------- */

const char *backend_mode_name(wups_backend_mode_t mode)
{
    switch (mode) {
        case WUPS_BACKEND_MODE_MQTT:    return "mqtt";
        case WUPS_BACKEND_MODE_ARKIV:   return "arkiv";
        case WUPS_BACKEND_MODE_HTTP:    return "http";
        case WUPS_BACKEND_MODE_UNKNOWN: return "unknown";
        default:                         return "invalid";
    }
}

static bool is_known_mode(uint8_t m)
{
    return m == WUPS_BACKEND_MODE_MQTT ||
           m == WUPS_BACKEND_MODE_ARKIV ||
           m == WUPS_BACKEND_MODE_HTTP;
}

/* Build the 4-byte mode_changed payload (matches
 * decodeModeChangedV1 in apps/api/src/lib/wupsproto.ts). */
static void build_mode_changed_payload(uint8_t out[4],
                                       wups_backend_mode_t from,
                                       wups_backend_mode_t to,
                                       bool pending)
{
    out[0] = 1;                                                /* version */
    out[1] = (uint8_t)from;
    out[2] = (uint8_t)to;
    out[3] = pending ? WUPS_MODE_CHANGED_FLAG_PENDING : 0u;
}

/* Assemble a full WUPS frame for MQTT publish. Same on-wire layout as
 * the RP2040↔ESP32 UART path (SYNC + 10-byte header + payload + 2-byte
 * Fletcher-8 + END). We don't reuse wups_link's send routine because
 * that one writes to UART — here we need bytes in a buffer. */
static uint16_t encode_mqtt_frame(uint8_t *out, size_t cap,
                                  uint8_t cls, uint8_t op,
                                  const uint8_t *payload, uint16_t payload_len)
{
    const uint16_t total = (uint16_t)(WUPS_HEADER_BYTES + payload_len + 4);
    if (total > cap) return 0;
    out[0] = WUPS_SYNC1;
    out[1] = WUPS_SYNC2;
    out[2] = WUPS_ADDR_RPI;       /* dst: panel-side consumer */
    out[3] = WUPS_ADDR_ESP32;     /* src */
    out[4] = cls;
    out[5] = op;
    out[6] = WUPS_FLAG_EVENT;
    out[7] = 0;                   /* seq (unused for events) */
    out[8] = (uint8_t)(payload_len & 0xFFu);
    out[9] = (uint8_t)((payload_len >> 8) & 0xFFu);
    if (payload_len) memcpy(out + WUPS_HEADER_BYTES, payload, payload_len);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < (int)(WUPS_HEADER_BYTES + payload_len); ++i) {
        a = (uint8_t)(a + out[i]);
        b = (uint8_t)(b + a);
    }
    out[WUPS_HEADER_BYTES + payload_len + 0] = a;
    out[WUPS_HEADER_BYTES + payload_len + 1] = b;
    out[WUPS_HEADER_BYTES + payload_len + 2] = WUPS_END1;
    out[WUPS_HEADER_BYTES + payload_len + 3] = WUPS_END2;
    return total;
}

/* --- channel emitters ----------------------------------------------------- */

/* Publish over MQTT (only meaningful when current mode is MQTT). */
static esp_err_t emit_via_mqtt(wups_backend_mode_t from,
                               wups_backend_mode_t to,
                               bool pending)
{
    uint8_t payload[4];
    build_mode_changed_payload(payload, from, to, pending);

    uint8_t frame[WUPS_HEADER_BYTES + 4 + 4];
    uint16_t n = encode_mqtt_frame(frame, sizeof frame,
                                   WUPS_CLASS_SYSTEM,
                                   WUPS_OP_SYS_MODE_CHANGED,
                                   payload, sizeof payload);
    if (!n) return ESP_FAIL;

    const char *topic = mqtt_topic_event();
    if (!topic || topic[0] == '\0') return ESP_FAIL;

    /* QoS 1 + retain=false: we want at-least-once delivery on the live
     * channel, but the event is a point-in-time signal — replaying a
     * stale "switching to arkiv" weeks later would be misleading. */
    int rc = mqtt_publish_raw(topic, frame, n, /* qos */ 1, /* retain */ false);
    return rc >= 0 ? ESP_OK : ESP_FAIL;
}

/* Submit a w3pups-event entity with class='mode_changed'. Only useful
 * when currently in Arkiv mode + claimed. */
static esp_err_t emit_via_arkiv(wups_backend_mode_t from,
                                wups_backend_mode_t to,
                                bool pending)
{
    if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) return ESP_ERR_INVALID_STATE;
    if (!arkiv_writer_ready()) return ESP_ERR_INVALID_STATE;

    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') iccid = "0";

    uint64_t seq = arkiv_writer_next_seq();
    const char *from_str = backend_mode_name(from);
    const char *to_str   = backend_mode_name(to);
    const char *pending_str = pending ? "1" : "0";

    /* `seq` is a numeric attr so the panel's ingest sorts it the same
     * way as other w3pups-event entities (apps/api/src/lib/arkiv/
     * ingest.ts → ingestEvents → numbers[Attr.SEQ]). */
    /* mode_changed is on the ADR-0013 no-encrypt allowlist: every bit of
     * semantics is in plaintext attributes (from/to/pending/class), so the
     * 4-byte body stays plaintext and we mark scheme=0 (self-describing
     * "unencrypted") — the reader dispatches on `scheme` and routes this to
     * applyModeChange untouched, never attempting to decrypt it. */
    arkiv_attr_t attrs[8] = {
        { .key = "type",      .value_str = "w3pups-event",   .is_numeric = false },
        { .key = "device_id", .value_str = iccid,            .is_numeric = false },
        { .key = "class",     .value_str = "mode_changed",   .is_numeric = false },
        { .key = "from",      .value_str = from_str,         .is_numeric = false },
        { .key = "to",        .value_str = to_str,           .is_numeric = false },
        { .key = "pending",   .value_str = pending_str,      .is_numeric = false },
        { .key = "seq",       .value_num = (int64_t)seq,     .is_numeric = true  },
        { .key = "scheme",    .value_num = 0,                .is_numeric = true  },
    };
    /* Tiny payload: same 4 bytes the MQTT path carries, so consumers
     * sharing decoding logic can parse either uniformly later. */
    uint8_t payload[4];
    build_mode_changed_payload(payload, from, to, pending);

    bool ok = arkiv_writer_enqueue_create_entity(
        "application/octet-stream", payload, sizeof payload,
        60 * 60,    /* 1 h TTL — mode events are sticky */
        attrs, sizeof(attrs) / sizeof(attrs[0]));
    return ok ? ESP_OK : ESP_FAIL;
}

/* Dispatch the mode_changed event over whichever channel is active.
 * Returns ESP_OK if anything was published, ESP_ERR_NOT_SUPPORTED for
 * HTTP (no channel to our backend in that mode), other errors otherwise. */
static esp_err_t emit_mode_changed(wups_backend_mode_t from,
                                   wups_backend_mode_t to,
                                   bool pending)
{
    switch (s_cur_mode) {
        case WUPS_BACKEND_MODE_MQTT:
            return emit_via_mqtt(from, to, pending);
        case WUPS_BACKEND_MODE_ARKIV:
            return emit_via_arkiv(from, to, pending);
        case WUPS_BACKEND_MODE_HTTP:
            /* No connection to our backend in HTTP mode; the user-hosted
             * server is the device's only sink. We deliberately do
             * nothing here — the panel side will already have got the
             * pre-switch hint from the previous channel. */
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

/* --- public API ----------------------------------------------------------- */

esp_err_t backend_mode_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_MODE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Brand-new device or factory-reset: stay on the MQTT default
         * without writing anything yet — the first request_switch()
         * will materialise the namespace. */
        s_cur_mode = WUPS_BACKEND_MODE_MQTT;
        ESP_LOGI(TAG, "backend_mode: no NVS entry — default %s",
                 backend_mode_name(s_cur_mode));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s — assuming default %s",
                 NS_MODE, esp_err_to_name(err),
                 backend_mode_name(WUPS_BACKEND_MODE_MQTT));
        s_cur_mode = WUPS_BACKEND_MODE_MQTT;
        return err;
    }
    uint8_t raw = WUPS_BACKEND_MODE_MQTT;
    err = nvs_get_u8(h, KEY_CUR_MODE, &raw);
    nvs_close(h);
    if (err == ESP_OK && is_known_mode(raw)) {
        s_cur_mode = (wups_backend_mode_t)raw;
    } else {
        s_cur_mode = WUPS_BACKEND_MODE_MQTT;
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "unknown stored mode %u — falling back to %s",
                     (unsigned)raw, backend_mode_name(s_cur_mode));
        }
    }
    ESP_LOGI(TAG, "active mode: %s", backend_mode_name(s_cur_mode));
    return ESP_OK;
}

wups_backend_mode_t backend_mode_get(void)
{
    return s_cur_mode;
}

esp_err_t backend_mode_request_switch(wups_backend_mode_t new_mode)
{
    if (!is_known_mode((uint8_t)new_mode)) return ESP_ERR_INVALID_ARG;
    if (new_mode == s_cur_mode) {
        /* Deliberately re-selecting the ACTIVE mode from the OLED menu is
         * treated as a clean "restart in this mode" — the menu already showed
         * a "switching…" screen, so a true reboot makes that honest (and is a
         * handy way to force a fresh reconnect). No mode change → no
         * mode_changed, and prev_mode is left as-is (the post-switch confirm
         * path no-ops on prev == cur, or on a stale/invalid marker). */
        ESP_LOGW(TAG, "re-selected current mode %s — restarting in place",
                 backend_mode_name(new_mode));
        vTaskDelay(pdMS_TO_TICKS(100));   /* flush the log line over USB-CDC */
        esp_restart();
        return ESP_OK;  /* unreached */
    }
    ESP_LOGI(TAG, "switch %s → %s requested",
             backend_mode_name(s_cur_mode), backend_mode_name(new_mode));

    /* (1) Pre-switch hint over the CURRENT channel. Best-effort but
     *     logged on failure — without it the panel won't see the
     *     switch happen until/unless the new channel reports back. */
    esp_err_t emit_err = emit_mode_changed(s_cur_mode, new_mode, /* pending */ true);
    if (emit_err != ESP_OK) {
        ESP_LOGW(TAG, "pre-switch hint not delivered (%s) — proceeding with switch anyway",
                 esp_err_to_name(emit_err));
    } else {
        /* Give the MQTT/Arkiv stack time to actually flush the frame
         * over PPP+TLS and (for MQTT QoS 1) collect a PUBACK from the
         * broker before we yank the network rug from under it. 500 ms
         * was too tight — packets ended up in the client's local queue
         * but never on the wire, so the panel never saw the pre-switch
         * hint. 2 s is conservative but the user-visible switch already
         * takes ~10 s end-to-end (modem boot dominates) so this doesn't
         * regress UX. */
        ESP_LOGI(TAG, "pre-switch hint emitted — flushing for 2 s before reboot");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* (2) Persist the new mode + previous mode (so the post-switch
     *     confirm path knows what to announce after reboot). */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_MODE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        esp_err_t e1 = nvs_set_u8(h, KEY_CUR_MODE, (uint8_t)new_mode);
        esp_err_t e2 = nvs_set_u8(h, KEY_PREV_MODE, (uint8_t)s_cur_mode);
        esp_err_t ec = nvs_commit(h);
        nvs_close(h);
        if (e1 != ESP_OK || e2 != ESP_OK || ec != ESP_OK) {
            ESP_LOGE(TAG, "NVS persist failed (cur=%s prev=%s commit=%s) — "
                          "rebooting will fall back to default",
                     esp_err_to_name(e1), esp_err_to_name(e2),
                     esp_err_to_name(ec));
        }
    } else {
        ESP_LOGE(TAG, "nvs_open RW failed (%s) — rebooting anyway",
                 esp_err_to_name(err));
    }

    /* (3) Reboot. We don't try to tear down subsystems cleanly — the
     *     reboot does that for free, and a partial teardown here could
     *     deadlock on a stuck client. ADR-0012 §4. */
    ESP_LOGW(TAG, "rebooting into %s mode", backend_mode_name(new_mode));
    vTaskDelay(pdMS_TO_TICKS(100));   /* let the log line flush over USB-CDC */
    esp_restart();
    return ESP_OK;  /* unreached */
}

void backend_mode_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset — regen device wallet + erasing writable nvs partition + rebooting");

    /* Roll the device's Arkiv wallet too. ak_dev_priv lives in the `prov`
     * partition, which nvs_flash_erase() below does NOT touch — so without
     * this the device wallet would survive a factory reset and only the
     * standalone "Regen Wallet" item could change it. Rolling it here makes
     * factory reset a complete return to "as-new": no on-chain link to the
     * prior owner's (possibly funded / used) wallet. The fresh key lands in
     * `prov` and is picked up on the next boot, which starts UNCLAIMED. The
     * wallet is a disposable gas-payer (the owner funds it at claim time), so
     * regenerating it costs nothing. Non-fatal on failure — we still wipe. */
    esp_err_t rerr = cmdauth_arkiv_regenerate_wallet();
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: device-wallet regen failed: %s — "
                      "wiping nvs anyway", esp_err_to_name(rerr));
    }

    /* Erase the entire writable NVS partition. This blows away:
     *   - w3mode/cur_mode + prev_mode (this module)
     *   - w3arkiv/owner_pub + key_epoch + last_ctr + cur_block + claim_state
     *     (cmdauth_arkiv state — device returns to ARKIV_UNCLAIMED)
     *   - any other writable runtime state added in the future
     * The read-only `prov` partition is untouched, so the per-device
     * MQTT secret + Arkiv device key survive — exactly what we want for
     * "factory reset" (return to first-boot, not to un-provisioned). */
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s — rebooting anyway",
                 esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void backend_mode_emit_post_switch_confirm(void)
{
    /* Read prev_mode; if set, this boot is a post-switch boot and we
     * should announce ourselves on the NEW channel. Idempotent — we
     * only clear the NVS marker AFTER a successful emit so that a
     * caller polling us from a heartbeat loop can retry while the
     * channel comes up. */
    nvs_handle_t rh;
    if (nvs_open(NS_MODE, NVS_READONLY, &rh) != ESP_OK) return;
    uint8_t prev = WUPS_BACKEND_MODE_UNKNOWN;
    esp_err_t err = nvs_get_u8(rh, KEY_PREV_MODE, &prev);
    nvs_close(rh);
    if (err != ESP_OK) return;
    if (!is_known_mode(prev) || prev == s_cur_mode) {
        /* Stale / invalid marker — silently clear so we don't keep
         * doing the NVS read on every heartbeat tick. */
        nvs_handle_t wh;
        if (nvs_open(NS_MODE, NVS_READWRITE, &wh) == ESP_OK) {
            nvs_erase_key(wh, KEY_PREV_MODE);
            nvs_commit(wh);
            nvs_close(wh);
        }
        return;
    }

    esp_err_t emit_err = emit_mode_changed((wups_backend_mode_t)prev,
                                           s_cur_mode,
                                           /* pending */ false);
    if (emit_err != ESP_OK) {
        /* Quiet log — caller is polling us from the heartbeat loop, so
         * a "not ready yet" emit is expected for the first several ticks
         * after a mode switch (PPP+MQTT or PPP+Arkiv WS startup). */
        ESP_LOGD(TAG, "post-switch confirm deferred (%s)", esp_err_to_name(emit_err));
        return;
    }

    ESP_LOGI(TAG, "post-switch confirm sent: %s -> %s",
             backend_mode_name((wups_backend_mode_t)prev),
             backend_mode_name(s_cur_mode));
    /* Success — clear the marker so we don't re-emit on the next tick or
     * the next clean boot. */
    nvs_handle_t wh;
    if (nvs_open(NS_MODE, NVS_READWRITE, &wh) == ESP_OK) {
        nvs_erase_key(wh, KEY_PREV_MODE);
        nvs_commit(wh);
        nvs_close(wh);
    }
}
