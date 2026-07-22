#include "wups_link.h"
#include "wups_proto.h"
#include "mqtt.h"
#include "identity.h"
#include "cmdauth.h"
#include "cmdauth_arkiv.h"
#include "fw_ota.h"
#include "arkiv_ack.h"
#include "arkiv_tlm.h"
#include "arkiv_event.h"
#include "oled_menu.h"
#include "backend_mode.h"
#include "http_backend.h"
#include "http_cfg.h"

#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "wups_link"

/* --- pin map ------------------------------------------------------------ */
/*
 * Confirmed with the LilyGo T-SIM7080G-S3 pinout (see docs/info.md and the
 * board image). All four pads sit on the same left header, contiguous-ish
 * after skipping GPIO3 (modem RI / strapping) and GPIO46 (strapping):
 *
 *   GPIO16 — CTS  (input,  ESP32  ← RP2040 RTS = GPIO23)
 *   GPIO17 — TX   (output, ESP32  → RP2040 RX  = GPIO21)
 *   GPIO18 — RX   (input,  ESP32  ← RP2040 TX  = GPIO20)
 *   GPIO8  — RTS  (output, ESP32  → RP2040 CTS = GPIO22)
 *
 * GPIO16 happens to be the default U0CTS — irrelevant here, we route via
 * the IO MUX to UART2.
 */
#define WUPS_UART_NUM        UART_NUM_2
#define WUPS_UART_TX_PIN     17
#define WUPS_UART_RX_PIN     18
#define WUPS_UART_CTS_PIN    16
#define WUPS_UART_RTS_PIN     8
#define WUPS_UART_BAUD       921600

/* RX driver buffer. Comfortably larger than one full frame (254 B) — gives
 * us slack across the ~50 ms RX-task tick. TX buffer = 0 means writes are
 * synchronous (fine: largest frame is 2.8 ms at 921600). */
#define WUPS_UART_RX_BUFSIZE 1024
#define WUPS_UART_TX_BUFSIZE 0

/* --- module state ------------------------------------------------------- */

static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_tx_seq;

/* Track 2 / ADR-0011 — trust-anchor REQ/RESP correlation. The claim
 * driver arms a nonce, sends ui.trust_prompt, then blocks on s_trust_sig;
 * the RX path matches ui.trust_result by nonce and releases it. Only one
 * prompt is ever in flight (single caller). */
static SemaphoreHandle_t s_trust_sig;
static volatile uint32_t s_trust_nonce;
static volatile uint8_t  s_trust_result;
static volatile bool     s_trust_armed;

/* Diagnostic counters — surfaced via wups_link_log_stats(). */
static volatile uint32_t s_frames_tx = 0;
static volatile uint32_t s_frames_rx = 0;
static volatile uint32_t s_bytes_tx  = 0;
static volatile uint32_t s_bytes_rx  = 0;
static volatile uint32_t s_rx_resync = 0;  /* SYNC2 mismatch — out-of-frame bytes seen */

typedef enum {
    WUPS_RX_SYNC1 = 0,
    WUPS_RX_SYNC2,
    WUPS_RX_DST,
    WUPS_RX_SRC,
    WUPS_RX_CLASS,
    WUPS_RX_OP,
    WUPS_RX_FLAGS,
    WUPS_RX_SEQ,
    WUPS_RX_LEN_L,
    WUPS_RX_LEN_H,
    WUPS_RX_PAYLOAD,
    WUPS_RX_CK_A,
    WUPS_RX_CK_B,
    WUPS_RX_END1,
    WUPS_RX_END2,
} wups_rx_state_t;

static struct {
    wups_rx_state_t state;
    uint8_t  dst, src, cls, op, flags, seq;
    uint16_t len;
    uint16_t pidx;
    uint8_t  payload[WUPS_MAX_PAYLOAD];
    uint8_t  rx_ck_a;
    uint8_t  exp_a, exp_b;
} s_rx;

static inline void rx_reset(void) { s_rx.state = WUPS_RX_SYNC1; }
static inline void rx_step(uint8_t b)
{
    s_rx.exp_a = (uint8_t)(s_rx.exp_a + b);
    s_rx.exp_b = (uint8_t)(s_rx.exp_b + s_rx.exp_a);
}

/* --- TX ---------------------------------------------------------------- */

static void send_frame_full(uint8_t dst, uint8_t src, uint8_t cls, uint8_t op,
                            uint8_t flags, uint8_t seq,
                            const void *payload, uint16_t payload_len)
{
    if (payload_len > WUPS_MAX_PAYLOAD) return;

    uint8_t header[10];
    header[0] = WUPS_SYNC1;
    header[1] = WUPS_SYNC2;
    header[2] = dst;
    header[3] = src;
    header[4] = cls;
    header[5] = op;
    header[6] = flags;
    header[7] = seq;
    header[8] = (uint8_t)(payload_len & 0xFFu);
    header[9] = (uint8_t)((payload_len >> 8) & 0xFFu);

    /* Fletcher-8 over DST..LEN_H..payload (sync and end marker excluded). */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 10; ++i) { a = (uint8_t)(a + header[i]); b = (uint8_t)(b + a); }
    const uint8_t *p = (const uint8_t *)payload;
    for (uint16_t i = 0; i < payload_len; ++i) { a = (uint8_t)(a + p[i]); b = (uint8_t)(b + a); }
    uint8_t trailer[4] = { a, b, WUPS_END1, WUPS_END2 };

    /* Serialize at frame granularity so concurrent senders (RX task,
     * MQTT data callback, hello at boot) don't interleave. */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(WUPS_UART_NUM, (const char *)header, 10);
    if (payload_len) uart_write_bytes(WUPS_UART_NUM, (const char *)payload, payload_len);
    uart_write_bytes(WUPS_UART_NUM, (const char *)trailer, 4);
    xSemaphoreGive(s_tx_mutex);

    s_frames_tx++;
    s_bytes_tx += (uint32_t)(14 + payload_len);
}

void wups_link_log_stats(void)
{
    ESP_LOGI(TAG, "stats: tx=%lu (%lu B) rx=%lu (%lu B) resync=%lu",
             (unsigned long)s_frames_tx, (unsigned long)s_bytes_tx,
             (unsigned long)s_frames_rx, (unsigned long)s_bytes_rx,
             (unsigned long)s_rx_resync);
}

void wups_link_send_seq(uint8_t dst, uint8_t cls, uint8_t op,
                        uint8_t flags, uint8_t seq,
                        const void *payload, uint16_t payload_len)
{
    send_frame_full(dst, WUPS_ADDR_ESP32, cls, op, flags, seq, payload, payload_len);
}

void wups_link_send(uint8_t dst, uint8_t cls, uint8_t op, uint8_t flags,
                    const void *payload, uint16_t payload_len)
{
    /* Race on s_tx_seq across multiple sender threads is benign: SEQ is
     * only used for human-readable correlation between REQ and RESP at
     * higher layers, not for transport correctness. */
    send_frame_full(dst, WUPS_ADDR_ESP32, cls, op, flags, s_tx_seq++,
                    payload, payload_len);
}

uint16_t wups_link_render_frame(uint8_t *out, size_t cap,
                                uint8_t dst, uint8_t cls, uint8_t op,
                                uint8_t flags,
                                const void *payload, uint16_t payload_len)
{
    if (!out || payload_len > WUPS_MAX_PAYLOAD) return 0;
    const uint16_t total = (uint16_t)(WUPS_FRAMING_BYTES + payload_len);
    if (cap < total) return 0;

    out[0] = WUPS_SYNC1;
    out[1] = WUPS_SYNC2;
    out[2] = dst;
    out[3] = WUPS_ADDR_ESP32;
    out[4] = cls;
    out[5] = op;
    out[6] = flags;
    out[7] = s_tx_seq++;   /* same benign race as wups_link_send */
    out[8] = (uint8_t)(payload_len & 0xFFu);
    out[9] = (uint8_t)((payload_len >> 8) & 0xFFu);
    if (payload_len) memcpy(out + WUPS_HEADER_BYTES, payload, payload_len);

    /* Fletcher-8 over DST..LEN_H..payload — sync and end marker excluded,
     * exactly as send_frame_full computes it. */
    uint8_t a, b;
    wups_fletcher8(out + 2, (size_t)(8 + payload_len), &a, &b);
    out[WUPS_HEADER_BYTES + payload_len + 0] = a;
    out[WUPS_HEADER_BYTES + payload_len + 1] = b;
    out[WUPS_HEADER_BYTES + payload_len + 2] = WUPS_END1;
    out[WUPS_HEADER_BYTES + payload_len + 3] = WUPS_END2;
    return total;
}

/* --- trust-anchor (Track 2 / ADR-0011 §10.1/§10.4) --------------------- */

void wups_link_trust_prompt(uint8_t mode, uint8_t confirm_secs,
                            uint32_t nonce, const char *text)
{
    size_t tl = text ? strlen(text) : 0;
    const size_t cap = WUPS_MAX_PAYLOAD - sizeof(wups_ui_trust_prompt_v1_hdr_t);
    if (tl > cap) {
        ESP_LOGW(TAG, "trust_prompt text %u > cap %u — truncating",
                 (unsigned)tl, (unsigned)cap);
        tl = cap;
    }

    uint8_t buf[WUPS_MAX_PAYLOAD];
    wups_ui_trust_prompt_v1_hdr_t hdr = {
        .version      = 1,
        .mode         = mode,
        .confirm_secs = confirm_secs,
        .text_len     = (uint8_t)tl,
        .nonce        = nonce,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    if (tl) memcpy(buf + sizeof(hdr), text, tl);

    /* Arm before sending so a fast RP2040 reply can't beat the wait. A
     * display-only prompt (confirm_secs == 0) expects no result, so don't
     * arm — keeps a stale nonce from swallowing a later real result.
     *
     * ADR-0012 — also don't arm for menu prompts (mode == 2): the menu
     * state machine has its own (lighter) result handler in
     * oled_menu_on_trust_result(), and the RX dispatch in
     * deliver_local_frame() already hands the result over there when
     * `s_trust_armed` is false. Arming here would swallow the exit
     * gesture into the claim-flow path, leaving `oled_menu.S.active`
     * stuck true and blocking the next menu open. */
    if (confirm_secs > 0 && mode != WUPS_TRUST_PROMPT_MODE_MENU) {
        s_trust_nonce  = nonce;
        s_trust_result = 1;                 /* default = timeout */
        s_trust_armed  = true;
        xSemaphoreTake(s_trust_sig, 0);     /* drain any stale signal */
    }
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_UI, WUPS_OP_UI_TRUST_PROMPT,
                   WUPS_FLAG_REQ, buf,
                   (uint16_t)(sizeof(hdr) + tl));
}

esp_err_t wups_link_trust_wait(uint32_t nonce, uint32_t timeout_ms,
                               uint8_t *result_out)
{
    if (!s_trust_armed || s_trust_nonce != nonce) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_trust_sig, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_trust_armed = false;
        return ESP_ERR_TIMEOUT;
    }
    if (result_out) *result_out = s_trust_result;
    return ESP_OK;
}

/* --- dispatch ---------------------------------------------------------- */

static void handle_net_publish(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(wups_net_publish_v1_hdr_t)) {
        ESP_LOGW(TAG, "net.publish too short: %u", (unsigned)len);
        return;
    }
    wups_net_publish_v1_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.version != 1) {
        ESP_LOGW(TAG, "net.publish version=%u (expected 1)", hdr.version);
        return;
    }
    size_t need = sizeof(wups_net_publish_v1_hdr_t) + hdr.topic_len + hdr.payload_len;
    if (need > len) {
        ESP_LOGW(TAG, "net.publish length mismatch: header wants %u, frame has %u",
                 (unsigned)need, (unsigned)len);
        return;
    }
    if (hdr.topic_len == 0) {
        ESP_LOGW(TAG, "net.publish has empty topic");
        return;
    }

    /* The caller (RP2040) is SIM-agnostic — it doesn't know our ICCID, so
     * it sends relative subtopics like "telemetry" / "event" / "cmd/response"
     * and trusts the ESP32 to prepend the per-device prefix `t/{iccid}/`
     * (ADR-0004). Absolute "t/..." / "c/..." topics pass through verbatim
     * as an escape hatch. */
    char rel[201];
    if (hdr.topic_len >= sizeof(rel)) {
        ESP_LOGW(TAG, "net.publish topic too long: %u", hdr.topic_len);
        return;
    }
    memcpy(rel, payload + sizeof(hdr), hdr.topic_len);
    rel[hdr.topic_len] = '\0';
    const uint8_t *mqtt_payload = payload + sizeof(hdr) + hdr.topic_len;

    bool absolute = (hdr.topic_len >= 2) &&
                    ((rel[0] == 't' || rel[0] == 'c') && rel[1] == '/');
    char topic[64];
    if (absolute) {
        if (hdr.topic_len + 1 > sizeof(topic)) {
            ESP_LOGW(TAG, "net.publish absolute topic too long: %u", hdr.topic_len);
            return;
        }
        memcpy(topic, rel, hdr.topic_len + 1);
    } else {
        const char *iccid = identity_iccid();
        if (!iccid || iccid[0] == '\0') {
            ESP_LOGW(TAG, "net.publish before ICCID known — dropping (%s)", rel);
            return;
        }
        int n = snprintf(topic, sizeof(topic), "t/%s/%s", iccid, rel);
        if (n < 0 || (size_t)n >= sizeof(topic)) {
            ESP_LOGW(TAG, "net.publish topic synthesis overflow (rel=%s)", rel);
            return;
        }
    }

    /* Track 2 / ADR-0011 P4 §4.6 — snoop telemetry frames on the way to
     * MQTT so the Arkiv telemetry emitter has an up-to-date snapshot per
     * class (power.status / host.status / net.status). We pass the WHOLE
     * frame including the WUPS header; arkiv_tlm classifies by header
     * and copies only the inner payload. No-op in MQTT mode (the cache
     * just stays warm; the periodic emit task is self-gated on
     * cmdauth_arkiv_claim_state()). */
    if (!absolute && strcmp(rel, "telemetry") == 0 &&
        cmdauth_arkiv_claim_state() == ARKIV_CLAIMED) {
        arkiv_tlm_observe_frame(mqtt_payload, hdr.payload_len);
        /* fall through — the MQTT publish still happens; in arkiv mode
         * the backend dispatcher already drops these (Slice #3 dispatcher
         * gate), but the snoop must not interfere with the existing flow. */
    }
    /* HTTP-2 (§4.18a) — in HTTP control mode, snoop the telemetry frame on
     * its way past so the periodic POST carries a fresh power/host/net
     * snapshot. There is no MQTT broker in this mode, so the publish below
     * just no-ops (mqtt_publish_raw returns -1); the cache is the real sink. */
    if (!absolute && strcmp(rel, "telemetry") == 0 &&
        backend_mode_get() == WUPS_BACKEND_MODE_HTTP) {
        http_backend_observe_telemetry_frame(mqtt_payload, hdr.payload_len);
    }

    if (!absolute && strcmp(rel, "event") == 0 &&
        cmdauth_arkiv_claim_state() == ARKIV_CLAIMED) {
        /* Immediate submit — events are rare + time-sensitive. The function
         * returns quickly on non-event payloads (e.g. system.log frames are
         * also routed through "event" today). */
        arkiv_event_observe_frame(mqtt_payload, hdr.payload_len);
        /* fall through to MQTT — backend dispatcher drops it in arkiv mode. */
    }

    /* Track 2 / ADR-0011 P4 §4.6 — divert cmd/response to a w3pups-ack
     * entity when this RESP is for an Arkiv-issued command. The arkiv_rpc
     * poll stamped (SEQ → command_id) on accept; if a pending entry exists
     * we publish on chain instead of on the MQTT broker so the panel sees
     * a single, on-chain ACK source for Paranoic-mode commands.
     *
     * Match by RELATIVE subtopic only — absolute topics are an escape
     * hatch (e.g. early bring-up before ICCID known) and shouldn't be
     * silently reinterpreted. The cmd RESP payload here is the full WUPS
     * frame the panel needs to surface in `commands.result` anyway. */
    if (!absolute && strcmp(rel, "cmd/response") == 0 &&
        cmdauth_arkiv_claim_state() == ARKIV_CLAIMED &&
        hdr.payload_len >= WUPS_HEADER_BYTES) {
        uint8_t inner_seq = mqtt_payload[7]; /* SEQ offset in WUPS header */
        if (arkiv_ack_has_pending(inner_seq)) {
            /* Result code lives at the START of the inner payload (the
             * Track 1 convention the MQTT dispatcher already uses). */
            uint16_t inner_len = (uint16_t)mqtt_payload[8] | ((uint16_t)mqtt_payload[9] << 8);
            const uint8_t *inner = mqtt_payload + WUPS_HEADER_BYTES;
            size_t inner_avail = (size_t)hdr.payload_len - WUPS_HEADER_BYTES;
            if (inner_len > inner_avail) inner_len = (uint16_t)inner_avail;
            if (arkiv_ack_emit(inner_seq, inner, inner_len)) {
                ESP_LOGI(TAG, "cmd/response diverted to w3pups-ack (seq=%u)",
                         (unsigned)inner_seq);
                return;
            }
            ESP_LOGW(TAG, "w3pups-ack emit failed (seq=%u) — fallback to MQTT",
                     (unsigned)inner_seq);
            /* Fall through to MQTT so the dev unit's Track 0/1 path still
             * has a chance — backend will drop it in arkiv mode anyway
             * (Slice #1 dispatcher gate), but we avoid losing the ACK
             * silently on a one-off submit failure. */
        }
    }

    int rc = mqtt_publish_raw(topic, mqtt_payload, hdr.payload_len,
                              hdr.qos, hdr.retain);
    if (rc < 0) {
        ESP_LOGW(TAG, "mqtt publish %s rc=%d (mqtt not connected?)", topic, rc);
    } else {
        ESP_LOGI(TAG, "mqtt publish %s len=%u qos=%u retain=%u msg_id=%d",
                 topic, hdr.payload_len, hdr.qos, hdr.retain, rc);
    }
}

/* HTTP-2 (§4.18a) — net.config REQ from the RPi host. Persists the HTTP
 * control-mode endpoint (or device_id override) in NVS so a fielded unit can
 * be re-pointed without re-flashing. Replies with a net.config RESP carrying
 * a 1-byte result. Applies in any backend mode; takes effect when the device
 * is (re)booted into HTTP mode. */
static void handle_net_config(uint8_t src, uint8_t seq,
                              const uint8_t *payload, uint16_t len)
{
    wups_net_config_result_v1_t resp = { .version = 1, .item = 0, .result = 1, .reserved = 0 };

    if (len < sizeof(wups_net_config_v1_hdr_t)) {
        ESP_LOGW(TAG, "net.config too short: %u", (unsigned)len);
        goto reply;
    }
    wups_net_config_v1_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    resp.item = hdr.item;
    if (hdr.version != 1) {
        ESP_LOGW(TAG, "net.config version=%u (expected 1)", hdr.version);
        goto reply;
    }
    if ((size_t)sizeof(hdr) + hdr.value_len > len) {
        ESP_LOGW(TAG, "net.config length mismatch (value_len=%u)", hdr.value_len);
        goto reply;
    }

    char value[HTTP_CFG_URL_MAX + 1];
    if (hdr.value_len > HTTP_CFG_URL_MAX) {
        ESP_LOGW(TAG, "net.config value too long: %u", hdr.value_len);
        goto reply;
    }
    memcpy(value, payload + sizeof(hdr), hdr.value_len);
    value[hdr.value_len] = '\0';

    esp_err_t err = ESP_ERR_INVALID_ARG;
    switch (hdr.item) {
        case WUPS_NET_CONFIG_HTTP_URL:
            err = http_cfg_set_url(value);
            ESP_LOGI(TAG, "net.config HTTP_URL -> '%s' (%s)",
                     value, esp_err_to_name(err));
            break;
        case WUPS_NET_CONFIG_DEVICE_ID:
            err = http_cfg_set_device_id(value);
            ESP_LOGI(TAG, "net.config DEVICE_ID -> '%s' (%s)",
                     value, esp_err_to_name(err));
            break;
        default:
            ESP_LOGW(TAG, "net.config unknown item %u", hdr.item);
            break;
    }
    resp.result = (err == ESP_OK) ? 0 : 1;

reply:
    wups_link_send_seq(src, WUPS_CLASS_NET, WUPS_OP_NET_CONFIG,
                       WUPS_FLAG_RESP, seq, &resp, sizeof(resp));
}

static void on_local_frame(uint8_t dst, uint8_t src, uint8_t cls, uint8_t op,
                           uint8_t flags, uint8_t seq,
                           const uint8_t *payload, uint16_t len,
                           uint8_t ck_a, uint8_t ck_b)
{
    (void)dst; (void)ck_a; (void)ck_b;

    if (cls == WUPS_CLASS_SYSTEM) {
        if (op == WUPS_OP_SYS_PING && (flags & WUPS_FLAG_REQ)) {
            wups_sys_pong_v1_t pong;
            pong.version    = 1;
            pong.reserved   = 0;
            pong.fw_version = (uint16_t)((1u << 8) | 0u); /* 1.0 */
            pong.uptime_ms  = (uint32_t)(esp_timer_get_time() / 1000);
            wups_link_send_seq(src, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_PING,
                               WUPS_FLAG_RESP, seq, &pong, sizeof(pong));
            return;
        }
        /* hello / log / status_query — ignored in v1 */
        return;
    }
    if (cls == WUPS_CLASS_NET) {
        if (op == WUPS_OP_NET_PUBLISH && (flags & WUPS_FLAG_REQ)) {
            handle_net_publish(payload, len);
            return;
        }
        if (op == WUPS_OP_NET_CONFIG && (flags & WUPS_FLAG_REQ)) {
            handle_net_config(src, seq, payload, len);
            return;
        }
        /* status / downlink / time_sync are outbound from us; drop. */
        return;
    }
    if (cls == WUPS_CLASS_UI) {
        /* Track 2 / ADR-0011 §10.1/§10.4 — trust-anchor RESPs from the
         * RP2040 OLED gate. Two consumers share this op:
         *   (a) cmdauth_arkiv's owner-binding flow (matches nonce via
         *       wups_link_trust_wait — semaphore-based blocking wait);
         *   (b) ADR-0012 oled_menu (mode=2 menu sessions match by nonce
         *       too, but use polling — the menu state machine pushes
         *       fresh prompts on each navigation and only needs the
         *       exit-gesture result asynchronously).
         * Try (a) first; if its nonce doesn't match, hand to (b). */
        if (op == WUPS_OP_UI_TRUST_RESULT && (flags & WUPS_FLAG_RESP) &&
            len >= sizeof(wups_ui_trust_result_v1_t)) {
            wups_ui_trust_result_v1_t r;
            memcpy(&r, payload, sizeof(r));
            if (r.version != 1) return;
            if (s_trust_armed && r.nonce == s_trust_nonce) {
                s_trust_result = r.result;
                s_trust_armed  = false;
                xSemaphoreGive(s_trust_sig);
            } else {
                /* Hand to the menu — it'll no-op if the nonce isn't its own. */
                oled_menu_on_trust_result(r.nonce, r.result);
            }
            return;
        }
        /* ADR-0012 — button events from the RP2040. The RP2040 only
         * broadcasts these while in menu mode (otherwise buttons are
         * its own local nav/power UI); the activation gesture
         * (button=0xFF, action=long) opens the menu. */
        if (op == WUPS_OP_UI_BUTTON_EVENT && (flags & WUPS_FLAG_EVENT) &&
            len >= sizeof(wups_ui_button_event_v1_t)) {
            wups_ui_button_event_v1_t e;
            memcpy(&e, payload, sizeof(e));
            if (e.version == 1) {
                oled_menu_on_button_event(e.button, e.action);
            }
            return;
        }
        return;
    }
    /* power / host — ESP32 is a dumb pipe to MQTT (CLAUDE.md). RP2040
     * decides what reaches the panel by issuing net.publish REQs; we don't
     * second-guess class semantics here. */
}

static void deliver_frame(void)
{
    /* Leaf node — accept frames addressed to us, broadcast, or internal
     * multicast; drop everything else (RP2040 router shouldn't be sending
     * us those, but defense in depth). */
    if (s_rx.dst != WUPS_ADDR_ESP32 &&
        s_rx.dst != WUPS_ADDR_BROADCAST &&
        s_rx.dst != WUPS_ADDR_INTERNAL) {
        return;
    }
    s_frames_rx++;
    /* exp_a / exp_b were updated alongside each rx_step() and matched
     * the on-wire CK_A/CK_B at the end of WUPS_RX_CK_B, so they're the
     * canonical checksum bytes for re-publish. */
    on_local_frame(s_rx.dst, s_rx.src, s_rx.cls, s_rx.op, s_rx.flags, s_rx.seq,
                   s_rx.payload, s_rx.len, s_rx.exp_a, s_rx.exp_b);
}

/* --- RX state machine -------------------------------------------------- */

static void rx_byte(uint8_t b)
{
    switch (s_rx.state) {
    case WUPS_RX_SYNC1:
        if (b == WUPS_SYNC1) s_rx.state = WUPS_RX_SYNC2;
        break;
    case WUPS_RX_SYNC2:
        if (b == WUPS_SYNC2) {
            s_rx.exp_a = 0;
            s_rx.exp_b = 0;
            s_rx.pidx  = 0;
            s_rx.state = WUPS_RX_DST;
        } else {
            s_rx_resync++;
            rx_reset();
        }
        break;
    case WUPS_RX_DST:    s_rx.dst   = b; rx_step(b); s_rx.state = WUPS_RX_SRC;   break;
    case WUPS_RX_SRC:    s_rx.src   = b; rx_step(b); s_rx.state = WUPS_RX_CLASS; break;
    case WUPS_RX_CLASS:  s_rx.cls   = b; rx_step(b); s_rx.state = WUPS_RX_OP;    break;
    case WUPS_RX_OP:     s_rx.op    = b; rx_step(b); s_rx.state = WUPS_RX_FLAGS; break;
    case WUPS_RX_FLAGS:  s_rx.flags = b; rx_step(b); s_rx.state = WUPS_RX_SEQ;   break;
    case WUPS_RX_SEQ:    s_rx.seq   = b; rx_step(b); s_rx.state = WUPS_RX_LEN_L; break;
    case WUPS_RX_LEN_L:
        s_rx.len = b;
        rx_step(b);
        s_rx.state = WUPS_RX_LEN_H;
        break;
    case WUPS_RX_LEN_H:
        s_rx.len |= (uint16_t)((uint16_t)b << 8);
        rx_step(b);
        if (s_rx.len > WUPS_MAX_PAYLOAD) { rx_reset(); break; }
        s_rx.state = (s_rx.len == 0) ? WUPS_RX_CK_A : WUPS_RX_PAYLOAD;
        break;
    case WUPS_RX_PAYLOAD:
        s_rx.payload[s_rx.pidx++] = b;
        rx_step(b);
        if (s_rx.pidx >= s_rx.len) s_rx.state = WUPS_RX_CK_A;
        break;
    case WUPS_RX_CK_A:
        s_rx.rx_ck_a = b;
        s_rx.state = WUPS_RX_CK_B;
        break;
    case WUPS_RX_CK_B:
        if (s_rx.rx_ck_a == s_rx.exp_a && b == s_rx.exp_b) {
            s_rx.state = WUPS_RX_END1;
        } else {
            rx_reset();
        }
        break;
    case WUPS_RX_END1:
        s_rx.state = (b == WUPS_END1) ? WUPS_RX_END2 : WUPS_RX_SYNC1;
        break;
    case WUPS_RX_END2:
        if (b == WUPS_END2) deliver_frame();
        rx_reset();
        break;
    default:
        rx_reset();
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    while (1) {
        int n = uart_read_bytes(WUPS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n < 0) {
            ESP_LOGE(TAG, "uart_read_bytes err %d", n);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n > 0) s_bytes_rx += (uint32_t)n;
        for (int i = 0; i < n; ++i) rx_byte(buf[i]);
    }
}

/* --- MQTT inbound → net.downlink -------------------------------------- */

/* Forward an arriving MQTT message to RP2040 (hub) as a net.downlink event.
 * The frame carries the topic and the raw payload; RP2040 decides what to
 * do with it (route to CH32X for power commands, to itself for UI/system,
 * etc.). Runs in the MQTT client task context. */
static void on_mqtt_data(const char *topic, size_t topic_len,
                         const void *payload, size_t payload_len)
{
    if (topic_len > 200) {
        ESP_LOGW(TAG, "mqtt downlink: topic too long (%u), dropping",
                 (unsigned)topic_len);
        return;
    }

    /* WS-9 / ADR-0009: every downlink is a backend-signed command (the ESP32
     * only subscribes to c/{iccid}/cmd/request). Verify the WAE1 envelope and
     * forward ONLY the inner WUPS frame — the RP2040 sees exactly what it saw
     * before WS-9 (Decision C). Reject = drop, never forward unverified. */
    const uint8_t *frame = NULL;
    size_t frame_len = 0;
    if (!cmdauth_check_and_strip((const uint8_t *)payload, payload_len,
                                 &frame, &frame_len)) {
        ESP_LOGW(TAG, "mqtt downlink: command auth failed — dropping");
        return;
    }
    payload = frame;
    payload_len = frame_len;

    /* OTA-1 — fw.update (net.fw_update) is the one WS-9 command the ESP32
     * executes itself: intercept it AFTER envelope verification and never
     * forward it to the RP2040. The hook validates, ACKs on cmd/response
     * and kicks off the download task; true = frame consumed. */
    if (fw_ota_try_handle_downlink((const uint8_t *)payload, payload_len)) {
        return;
    }

    size_t total = sizeof(wups_net_downlink_v1_hdr_t) + topic_len + payload_len;
    if (total > WUPS_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "mqtt downlink: %u + %u + hdr exceeds %u, dropping",
                 (unsigned)topic_len, (unsigned)payload_len, WUPS_MAX_PAYLOAD);
        return;
    }

    uint8_t buf[WUPS_MAX_PAYLOAD];
    wups_net_downlink_v1_hdr_t hdr = {
        .version     = 1,
        .qos         = 0,
        .retain      = 0,
        .topic_len   = (uint8_t)topic_len,
        .payload_len = (uint16_t)payload_len,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), topic, topic_len);
    memcpy(buf + sizeof(hdr) + topic_len, payload, payload_len);

    /* DST=RP2040 — hub routes by application logic. (Old code used
     * WUPS_ADDR_RPI because RPi service was the only consumer; with the
     * panel-side migration RP2040 is now the dispatcher, so it gets the
     * downlink and forwards to CH32X / RPi / itself as appropriate.) */
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_NET, WUPS_OP_NET_DOWNLINK,
                   WUPS_FLAG_EVENT, buf, (uint16_t)total);
}

/* --- hello broadcast --------------------------------------------------- */

static void send_hello_bcast(void)
{
    wups_sys_hello_v1_t h;
    h.version       = 1;
    h.proto_version = WUPS_PROTO_VERSION;
    h.node_addr     = WUPS_ADDR_ESP32;
    h.reserved      = 0;
    h.fw_version    = (uint16_t)((1u << 8) | 0u);
    h.caps_classes  = WUPS_CAP_SYSTEM | WUPS_CAP_NET;
    h.build_id      = 0;
    wups_link_send(WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
                   WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, &h, sizeof(h));
}

/* --- public init ------------------------------------------------------- */

esp_err_t wups_link_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    s_trust_sig = xSemaphoreCreateBinary(); /* starts empty */
    if (!s_trust_sig) return ESP_ERR_NO_MEM;

    rx_reset();

    /* ADR-0012 / 2026-05-25: HW flow control disabled. The CTS/RTS handshake
     * misbehaved across ESP32-only reboots — RP2040 saw a stuck or pulsing
     * CTS from the booting ESP32 and either paused sends or sent into a
     * non-listening RX, leaving the deframer permanently desynced (resync
     * counter climbed every frame, telemetry only worked after a full
     * power-cycle). Without HW flow control we rely on the 4 KB RX FIFO
     * + per-frame Fletcher-8 checksum + deframer resync — the existing
     * defense-in-depth was already carrying the load, so dropping CTS/RTS
     * trades a soft "stop sending" hint for a more robust cold-start. */
    uart_config_t cfg = {
        .baud_rate           = WUPS_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(WUPS_UART_NUM, &cfg);
    if (err != ESP_OK) return err;

    /* TX + RX only; pass UART_PIN_NO_CHANGE for RTS/CTS so the pins stay
     * driven by their previous configuration (typically inputs with
     * pullups) and don't try to negotiate a phantom handshake. */
    err = uart_set_pin(WUPS_UART_NUM, WUPS_UART_TX_PIN, WUPS_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(WUPS_UART_NUM, WUPS_UART_RX_BUFSIZE,
                              WUPS_UART_TX_BUFSIZE, 0, NULL, 0);
    if (err != ESP_OK) return err;

    /* 4 KB stack: deframer + occasional mqtt_publish_raw call (which just
     * enqueues to the MQTT task). */
    BaseType_t ok = xTaskCreate(rx_task, "wups_rx", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    /* Hook MQTT inbound → net.downlink. The handler will fire once MQTT
     * has connected and starts receiving messages. */
    mqtt_set_data_handler(on_mqtt_data);

    ESP_LOGI(TAG, "UART2 up: TX=GPIO%d RX=GPIO%d @ %d, NO HW flow ctrl",
             WUPS_UART_TX_PIN, WUPS_UART_RX_PIN, WUPS_UART_BAUD);

    /* Announce ourselves to anyone listening on the bus (RP2040 hub will
     * receive immediately; RPi sees it after USB-CDC enumerates on its end). */
    send_hello_bcast();
    return ESP_OK;
}
