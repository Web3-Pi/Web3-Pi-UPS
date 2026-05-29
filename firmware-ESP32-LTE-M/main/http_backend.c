/*
 * HTTP-2 (§4.18a) — HTTP control-mode backend. See http_backend.h and
 * HTTP-1-design-note.md for the contract this implements.
 */

#include "http_backend.h"
#include "http_cfg.h"
#include "identity.h"
#include "wups_link.h"
#include "wups_proto.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
/* IDF 6.0 / mbedtls 4.x: the high-level mbedtls_md_* HMAC path fails at
 * runtime here, but the low-level mbedtls_sha256_* API works (same approach as
 * components/arkiv_crypto). legacy sha256.h moved under mbedtls/private/. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>

#define TAG "http_backend"

/* Cadence aligned with the existing ~30 s uplink rhythm (HTTP-1 §"Command
 * delivery"). Commands have up to one-cadence latency by design. */
#define HTTP_PERIOD_MS      30000
#define HTTP_SETTLE_MS      12000   /* let one power.status cycle land first */
#define HTTP_FRESHNESS_MS   90000   /* drop a cached slot older than this */
#define HTTP_TIMEOUT_MS     20000

#define HTTP_RESP_MAX       2048    /* response (commands) capture cap */
#define HTTP_BODY_MAX       1024    /* telemetry JSON we POST */
#define HTTP_URL_MAX        (HTTP_CFG_URL_MAX + 96)

#define ACK_ID_MAX          32      /* command id string cap (incl. NUL) */
#define ACK_PENDING_MAX     8
#define EXEC_RING_MAX       16

/* --- telemetry cache (mirrors arkiv_tlm) ------------------------------- */

#define TLM_MAX_INNER       64

typedef struct {
    bool     valid;
    int64_t  observed_us;
    uint16_t len;
    uint8_t  data[TLM_MAX_INNER];
} slot_t;

/* [0]=power.status [1]=host.status [2]=net.status */
static slot_t s_slots[3];
static SemaphoreHandle_t s_lock;

/* --- command bookkeeping (http task only — no lock) -------------------- */

static char s_pending[ACK_PENDING_MAX][ACK_ID_MAX];
static int  s_pending_n;

static char s_executed[EXEC_RING_MAX][ACK_ID_MAX];
static int  s_exec_idx;

static uint8_t s_cmd_seq;

/* --- response capture -------------------------------------------------- */

static char   s_resp[HTTP_RESP_MAX];
static size_t s_resp_len;
static char   s_resp_sig[65];   /* X-W3PUPS-Sig from the response, if any */

/* --- telemetry snoop --------------------------------------------------- */

static slot_t *slot_for(uint8_t cls, uint8_t op)
{
    if (cls == WUPS_CLASS_POWER && op == WUPS_OP_PWR_STATUS)  return &s_slots[0];
    if (cls == WUPS_CLASS_HOST  && op == WUPS_OP_HOST_STATUS) return &s_slots[1];
    if (cls == WUPS_CLASS_NET   && op == WUPS_OP_NET_STATUS)  return &s_slots[2];
    return NULL;
}

void http_backend_observe_telemetry_frame(const uint8_t *frame, uint16_t frame_len)
{
    if (!frame || frame_len < WUPS_HEADER_BYTES + 2) return;
    uint8_t cls = frame[4];
    uint8_t op  = frame[5];
    uint16_t inner_len = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
    if (WUPS_HEADER_BYTES + inner_len + 2 > frame_len) return;
    if (inner_len == 0 || inner_len > TLM_MAX_INNER) return;

    slot_t *s = slot_for(cls, op);
    if (!s) return;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s->data, frame + WUPS_HEADER_BYTES, inner_len);
    s->len         = inner_len;
    s->valid       = true;
    s->observed_us = esp_timer_get_time();
    if (s_lock) xSemaphoreGive(s_lock);
}

/* Copy a fresh slot's payload out under the lock. Returns len or 0. */
static uint16_t take_slot(int idx, uint8_t *out, size_t cap)
{
    uint16_t n = 0;
    int64_t cutoff = esp_timer_get_time() - (int64_t)HTTP_FRESHNESS_MS * 1000;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = &s_slots[idx];
    if (s->valid && s->observed_us >= cutoff && s->len <= cap) {
        memcpy(out, s->data, s->len);
        n = s->len;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return n;
}

/* --- HMAC -------------------------------------------------------------- */

/* HMAC-SHA256 over up to three message segments (any may be NULL/0), built on
 * the low-level mbedtls_sha256_* API (the mbedtls_md_* HMAC path fails at
 * runtime on this IDF/mbedtls). Same construction as arkiv_crypto's. */
#define HMAC_BLOCK 64
static void hmac_sha256_3(const uint8_t *key, size_t key_len,
                          const uint8_t *s1, size_t l1,
                          const uint8_t *s2, size_t l2,
                          const uint8_t *s3, size_t l3,
                          uint8_t out[32])
{
    uint8_t k_pad[HMAC_BLOCK];
    uint8_t tk[32];
    if (key_len > HMAC_BLOCK) {
        mbedtls_sha256_context kc;
        mbedtls_sha256_init(&kc);
        mbedtls_sha256_starts(&kc, 0);
        mbedtls_sha256_update(&kc, key, key_len);
        mbedtls_sha256_finish(&kc, tk);
        mbedtls_sha256_free(&kc);
        key = tk;
        key_len = 32;
    }

    mbedtls_sha256_context ctx;
    uint8_t inner[32];

    memset(k_pad, 0, sizeof k_pad);
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < HMAC_BLOCK; ++i) k_pad[i] ^= 0x36;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_pad, HMAC_BLOCK);
    if (s1 && l1) mbedtls_sha256_update(&ctx, s1, l1);
    if (s2 && l2) mbedtls_sha256_update(&ctx, s2, l2);
    if (s3 && l3) mbedtls_sha256_update(&ctx, s3, l3);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    memset(k_pad, 0, sizeof k_pad);
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < HMAC_BLOCK; ++i) k_pad[i] ^= 0x5c;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, k_pad, HMAC_BLOCK);
    mbedtls_sha256_update(&ctx, inner, 32);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void mac_to_hex(const uint8_t mac[32], char out_hex[65])
{
    for (int i = 0; i < 32; ++i) sprintf(out_hex + i * 2, "%02x", mac[i]);
    out_hex[64] = '\0';
}

/* hex(HMAC_SHA256(key, ts_str || nonce_hex || body)) → out_hex[65]. */
static esp_err_t sign_request(const uint8_t *key, size_t key_len,
                              const char *ts_str, const char *nonce_hex,
                              const char *body, size_t body_len,
                              char out_hex[65])
{
    uint8_t mac[32];
    hmac_sha256_3(key, key_len,
                  (const uint8_t *)ts_str, strlen(ts_str),
                  (const uint8_t *)nonce_hex, strlen(nonce_hex),
                  (const uint8_t *)body, body_len,
                  mac);
    mac_to_hex(mac, out_hex);
    return ESP_OK;
}

/* Verify the response signature: hex(HMAC_SHA256(key, req_nonce || resp_body))
 * must equal `provided_hex`. Binding to the request nonce ties the response to
 * this exact request, so a captured-and-replayed old response is rejected.
 * This authenticates the commands regardless of transport — TLS is then only
 * needed for confidentiality, not command integrity. */
static bool verify_response_sig(const uint8_t *key, size_t key_len,
                                const char *req_nonce,
                                const char *body, size_t body_len,
                                const char *provided_hex)
{
    if (!provided_hex || provided_hex[0] == '\0') return false;
    uint8_t mac[32];
    hmac_sha256_3(key, key_len,
                  (const uint8_t *)req_nonce, strlen(req_nonce),
                  (const uint8_t *)body, body_len,
                  NULL, 0,
                  mac);
    char want[65];
    mac_to_hex(mac, want);
    return strcasecmp(want, provided_hex) == 0;
}

/* --- ack / dedup bookkeeping ------------------------------------------- */

static bool exec_seen(const char *id)
{
    for (int i = 0; i < EXEC_RING_MAX; ++i)
        if (s_executed[i][0] && strcmp(s_executed[i], id) == 0) return true;
    return false;
}

static void exec_add(const char *id)
{
    snprintf(s_executed[s_exec_idx], ACK_ID_MAX, "%s", id);
    s_exec_idx = (s_exec_idx + 1) % EXEC_RING_MAX;
}

static void ack_add(const char *id)
{
    for (int i = 0; i < s_pending_n; ++i)
        if (strcmp(s_pending[i], id) == 0) return;   /* already queued */
    if (s_pending_n >= ACK_PENDING_MAX) {
        ESP_LOGW(TAG, "ack queue full — dropping oldest");
        memmove(s_pending[0], s_pending[1], (ACK_PENDING_MAX - 1) * ACK_ID_MAX);
        s_pending_n--;
    }
    snprintf(s_pending[s_pending_n++], ACK_ID_MAX, "%s", id);
}

/* Drop the first `n` acks (they were carried in a POST the server accepted). */
static void ack_drop_front(int n)
{
    if (n <= 0) return;
    if (n > s_pending_n) n = s_pending_n;
    memmove(s_pending[0], s_pending[n], (s_pending_n - n) * ACK_ID_MAX);
    s_pending_n -= n;
}

/* --- command → WUPS frame --------------------------------------------- */

/* Encode a complete inner WUPS frame into out[cap]. Returns total bytes. */
static uint16_t encode_frame(uint8_t *out, size_t cap,
                             uint8_t dst, uint8_t src, uint8_t cls, uint8_t op,
                             uint8_t flags, uint8_t seq,
                             const uint8_t *payload, uint16_t plen)
{
    const uint16_t total = (uint16_t)(WUPS_HEADER_BYTES + plen + WUPS_TRAILER_BYTES);
    if (total > cap || plen > WUPS_MAX_PAYLOAD) return 0;
    out[0] = WUPS_SYNC1;
    out[1] = WUPS_SYNC2;
    out[2] = dst;
    out[3] = src;
    out[4] = cls;
    out[5] = op;
    out[6] = flags;
    out[7] = seq;
    out[8] = (uint8_t)(plen & 0xFF);
    out[9] = (uint8_t)((plen >> 8) & 0xFF);
    if (plen) memcpy(out + WUPS_HEADER_BYTES, payload, plen);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < (int)(WUPS_HEADER_BYTES + plen); ++i) {
        a = (uint8_t)(a + out[i]);
        b = (uint8_t)(b + a);
    }
    out[WUPS_HEADER_BYTES + plen + 0] = a;
    out[WUPS_HEADER_BYTES + plen + 1] = b;
    out[WUPS_HEADER_BYTES + plen + 2] = WUPS_END1;
    out[WUPS_HEADER_BYTES + plen + 3] = WUPS_END2;
    return total;
}

static int json_int(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

/* Translate a JSON command into a WUPS payload (cls/op + bytes). Mirrors the
 * panel's encodeCommand (Web3-Pi-UPS-Panel/apps/api/src/lib/commands.ts) so
 * the firmware sees byte-identical frames regardless of backend. Returns the
 * payload length and sets cls/op via the out-pointers; -1 if the command name
 * is unsupported. */
static int build_cmd_payload(const char *cmd, const cJSON *args,
                             uint8_t *cls, uint8_t *op,
                             uint8_t *pl, size_t pl_cap)
{
    if (strcmp(cmd, "host.shutdown") == 0 || strcmp(cmd, "host.reset") == 0) {
        /* wups_host_shutdown_v1_t { u8 version, u8 reason, u16 delay_s } */
        if (pl_cap < 4) return -1;
        int delay_s = json_int(args, "delay_s", 5);
        if (delay_s < 0) delay_s = 0;
        if (delay_s > 0xFFFF) delay_s = 0xFFFF;
        pl[0] = 1;
        pl[1] = 2;                          /* reason = remote_cmd */
        pl[2] = (uint8_t)(delay_s & 0xFF);
        pl[3] = (uint8_t)((delay_s >> 8) & 0xFF);
        *cls = WUPS_CLASS_HOST;
        *op  = (cmd[5] == 's') ? WUPS_OP_HOST_SHUTDOWN : WUPS_OP_HOST_RESET;
        return 4;
    }
    if (strcmp(cmd, "power.cycle") == 0) {
        /* wups_power_cycle_v1_t { u8 version, u8 reserved, u16 off_ms } */
        if (pl_cap < 4) return -1;
        int off_ms = json_int(args, "off_ms", 1500);
        if (off_ms < 0) off_ms = 0;
        if (off_ms > 60000) off_ms = 60000;
        pl[0] = 1;
        pl[1] = 0;
        pl[2] = (uint8_t)(off_ms & 0xFF);
        pl[3] = (uint8_t)((off_ms >> 8) & 0xFF);
        *cls = WUPS_CLASS_POWER;
        *op  = WUPS_OP_PWR_CYCLE;
        return 4;
    }
    if (strcmp(cmd, "ui.beep") == 0) {
        /* wups_ui_beep_v1_t { u8 version, u8 reserved, u16 freq_hz, u16 dur_ms } */
        if (pl_cap < 6) return -1;
        int freq = json_int(args, "freq_hz", 0);
        int dur  = json_int(args, "dur_ms", 0);
        pl[0] = 1;
        pl[1] = 0;
        pl[2] = (uint8_t)(freq & 0xFF);
        pl[3] = (uint8_t)((freq >> 8) & 0xFF);
        pl[4] = (uint8_t)(dur & 0xFF);
        pl[5] = (uint8_t)((dur >> 8) & 0xFF);
        *cls = WUPS_CLASS_UI;
        *op  = WUPS_OP_UI_BEEP;
        return 6;
    }
    if (strcmp(cmd, "ui.display_msg") == 0) {
        /* wups_ui_display_msg_v1_hdr_t { u8 ver, u8 line, u8 text_len, u8 rsv }
         * + ASCII text */
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "text");
        const char *text = cJSON_IsString(t) ? t->valuestring : "";
        size_t tl = strlen(text);
        if (tl > 64) tl = 64;
        if (pl_cap < 4 + tl) return -1;
        pl[0] = 1;
        pl[1] = (uint8_t)json_int(args, "line", 0);
        pl[2] = (uint8_t)tl;
        pl[3] = 0;
        memcpy(pl + 4, text, tl);
        *cls = WUPS_CLASS_UI;
        *op  = WUPS_OP_UI_DISPLAY_MSG;
        return (int)(4 + tl);
    }
    return -1;
}

/* Build the WUPS frame for `cmd` and inject it to the RP2040 as a net.downlink
 * EVENT — exactly the wrapper on_mqtt_data() produces, so the RP2040 deframes
 * and re-routes it through its normal dispatcher. Returns true if sent. */
static bool dispatch_command(const char *cmd, const cJSON *args)
{
    uint8_t pl[80];
    uint8_t cls = 0, op = 0;
    int plen = build_cmd_payload(cmd, args, &cls, &op, pl, sizeof(pl));
    if (plen < 0) {
        ESP_LOGW(TAG, "unsupported command '%s' — ignoring", cmd);
        return false;
    }

    /* Inner frame: dst=BROADCAST, src=RPI, flags=REQ — identical to the
     * panel's downlink frames (routes/devices.ts encodeFrame). */
    uint8_t inner[WUPS_MAX_FRAME];
    uint16_t inner_n = encode_frame(inner, sizeof(inner),
                                    WUPS_ADDR_BROADCAST, WUPS_ADDR_RPI,
                                    cls, op, WUPS_FLAG_REQ, s_cmd_seq++,
                                    pl, (uint16_t)plen);
    if (!inner_n) return false;

    /* Wrap in net.downlink: hdr + topic + inner frame. */
    static const char topic[] = "http";
    const uint8_t topic_len = (uint8_t)(sizeof(topic) - 1);
    uint8_t buf[WUPS_MAX_PAYLOAD];
    wups_net_downlink_v1_hdr_t hdr = {
        .version     = 1,
        .qos         = 0,
        .retain      = 0,
        .topic_len   = topic_len,
        .payload_len = inner_n,
    };
    size_t total = sizeof(hdr) + topic_len + inner_n;
    if (total > sizeof(buf)) return false;
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), topic, topic_len);
    memcpy(buf + sizeof(hdr) + topic_len, inner, inner_n);

    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_NET, WUPS_OP_NET_DOWNLINK,
                   WUPS_FLAG_EVENT, buf, (uint16_t)total);
    ESP_LOGI(TAG, "dispatched '%s' (cls=0x%02x op=0x%02x, %d B payload)",
             cmd, cls, op, plen);
    return true;
}

/* --- telemetry JSON ---------------------------------------------------- */

/* Build the telemetry JSON body. Returns length, 0 on failure. */
static size_t build_body(char *out, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;

    time_t now = time(NULL);
    cJSON_AddNumberToObject(root, "ts", (double)now);
    cJSON_AddStringToObject(root, "fw_ver", identity_fw_version());
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));

    uint8_t raw[TLM_MAX_INNER];

    /* power.status → wups_power_status_v1_t */
    if (take_slot(0, raw, sizeof(raw)) >= sizeof(wups_power_status_v1_t)) {
        wups_power_status_v1_t p;
        memcpy(&p, raw, sizeof(p));
        cJSON *o = cJSON_AddObjectToObject(root, "power");
        cJSON_AddNumberToObject(o, "charge_state", p.charge_state);
        cJSON_AddNumberToObject(o, "vbus_in_mv",   p.vbus_in_mV);
        cJSON_AddNumberToObject(o, "vbus_out_mv",  p.vbus_out_mV);
        cJSON_AddNumberToObject(o, "ibus_out_ma",  p.ibus_out_mA);
        cJSON_AddNumberToObject(o, "vbat_mv",      p.vbat_mV);
        cJSON_AddNumberToObject(o, "ibat_ma",      p.ibat_mA);
        cJSON_AddNumberToObject(o, "temp_dc",      p.temp_dC);
        cJSON_AddNumberToObject(o, "faults",       p.faults);
    }

    /* host.status → wups_host_status_v1_t */
    if (take_slot(1, raw, sizeof(raw)) >= sizeof(wups_host_status_v1_t)) {
        wups_host_status_v1_t h;
        memcpy(&h, raw, sizeof(h));
        cJSON *o = cJSON_AddObjectToObject(root, "host");
        cJSON_AddNumberToObject(o, "eth_state",   h.eth_client_state);
        cJSON_AddNumberToObject(o, "cpu_temp_dc", h.cpu_temp_dC);
        cJSON_AddNumberToObject(o, "mem_pct",     h.mem_used_pct);
        cJSON_AddNumberToObject(o, "disk_pct",    h.disk_used_pct);
        cJSON_AddNumberToObject(o, "load_x100",   h.load_avg_x100);
        cJSON_AddNumberToObject(o, "uptime_s",    h.uptime_s);
    }

    /* net.status → wups_net_status_v1_t */
    if (take_slot(2, raw, sizeof(raw)) >= sizeof(wups_net_status_v1_t)) {
        wups_net_status_v1_t n;
        memcpy(&n, raw, sizeof(n));
        cJSON *o = cJSON_AddObjectToObject(root, "net");
        cJSON_AddNumberToObject(o, "state",    n.state);
        cJSON_AddNumberToObject(o, "rssi_dbm", n.rssi_dBm);
        cJSON_AddNumberToObject(o, "rsrp_dbm", n.rsrp_dBm);
        cJSON_AddNumberToObject(o, "rsrq_db",  n.rsrq_dB);
        cJSON_AddNumberToObject(o, "bytes_tx", n.bytes_tx);
        cJSON_AddNumberToObject(o, "bytes_rx", n.bytes_rx);
    }

    /* acks — command ids completed since the previous POST. */
    cJSON *acks = cJSON_AddArrayToObject(root, "acks");
    for (int i = 0; i < s_pending_n; ++i)
        cJSON_AddItemToArray(acks, cJSON_CreateString(s_pending[i]));

    bool ok = cJSON_PrintPreallocated(root, out, (int)cap, false);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "telemetry JSON did not fit in %u B", (unsigned)cap);
        return 0;
    }
    return strlen(out);
}

/* --- response → commands ---------------------------------------------- */

static void handle_response(const char *json, size_t len,
                            const uint8_t *key, size_t key_len,
                            const char *req_nonce, const char *resp_sig)
{
    if (len == 0) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "response is not valid JSON — ignoring");
        return;
    }
    const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(root, "commands");
    /* Commands are only honoured if the response carries a valid signature
     * over (request nonce || body). This authenticates the command channel
     * independently of TLS, so plain HTTP cannot be used to inject commands.
     * No commands → no verification needed (nothing to execute). */
    if (cJSON_IsArray(cmds) && cJSON_GetArraySize(cmds) > 0 &&
        !verify_response_sig(key, key_len, req_nonce, json, len, resp_sig)) {
        ESP_LOGW(TAG, "response signature missing/invalid — ignoring %d command(s) "
                      "(is the server signing X-W3PUPS-Sig with the device secret?)",
                 cJSON_GetArraySize(cmds));
        cJSON_Delete(root);
        return;
    }
    if (cJSON_IsArray(cmds)) {
        const cJSON *c = NULL;
        cJSON_ArrayForEach(c, cmds) {
            const cJSON *jid  = cJSON_GetObjectItemCaseSensitive(c, "id");
            const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(c, "cmd");
            if (!cJSON_IsString(jid) || !cJSON_IsString(jcmd)) continue;
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(c, "args");

            if (exec_seen(jid->valuestring)) {
                /* Already applied — the server just hasn't seen our ack yet.
                 * Re-ack, do NOT re-execute (idempotent by id, HTTP-1). */
                ack_add(jid->valuestring);
                continue;
            }
            if (dispatch_command(jcmd->valuestring, args)) {
                exec_add(jid->valuestring);
                ack_add(jid->valuestring);
            }
        }
    }
    cJSON_Delete(root);
}

/* --- HTTP POST --------------------------------------------------------- */

static esp_err_t resp_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && strcasecmp(evt->header_key, "X-W3PUPS-Sig") == 0 &&
            evt->header_value) {
            snprintf(s_resp_sig, sizeof(s_resp_sig), "%s", evt->header_value);
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t room = HTTP_RESP_MAX - 1 - s_resp_len;
        if (room) {
            size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
            memcpy(s_resp + s_resp_len, evt->data, n);
            s_resp_len += n;
            s_resp[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* One POST cycle. Returns the count of acks carried (to drop on success), or
 * -1 on a transport/auth failure (nothing dropped, retried next cadence). */
static int post_once(void)
{
    char url[HTTP_URL_MAX];
    char base[HTTP_CFG_URL_MAX + 1];
    char devid[HTTP_CFG_DEVID_MAX + 1];

    if (http_cfg_get_url(base, sizeof(base)) != ESP_OK) {
        ESP_LOGW(TAG, "no HTTP endpoint configured — set it from the host "
                      "(net.config) or HTTP_ENDPOINT_BASE in secrets.h");
        return -1;
    }
    if (http_cfg_get_device_id(devid, sizeof(devid)) != ESP_OK) {
        ESP_LOGW(TAG, "device_id unknown (ICCID not read yet?) — skipping POST");
        return -1;
    }
    snprintf(url, sizeof(url), "%s/api/v1/devices/%s/telemetry", base, devid);

    /* HMAC key = the OLED-shown HTTP secret (separate from the MQTT/Arkiv
     * per-device secret). Auto-generated on first use; re-rollable from the
     * OLED menu. The key is the ASCII bytes of the code. */
    char secret[HTTP_CFG_SECRET_CHARS + 1];
    if (http_cfg_get_secret(secret, sizeof(secret)) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP secret unavailable — cannot sign");
        return -1;
    }
    const uint8_t *key = (const uint8_t *)secret;
    const size_t key_len = strlen(secret);

    char body[HTTP_BODY_MAX];
    int acks_in_body = s_pending_n;
    size_t body_len = build_body(body, sizeof(body));
    if (body_len == 0) return -1;

    char ts_str[16];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)time(NULL));
    char nonce_hex[17];
    snprintf(nonce_hex, sizeof(nonce_hex), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    char sig_hex[65];
    if (sign_request(key, key_len, ts_str, nonce_hex, body, body_len, sig_hex) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC signing failed");
        return -1;
    }

    bool is_https = strncmp(url, "https://", 8) == 0;
    if (!is_https) {
        /* Plain HTTP is supported (HTTPS is optional). Telemetry + commands
         * are still HMAC-authenticated (request + response signatures), so
         * this is safe against forgery/injection; TLS only adds confidentiality
         * (hiding telemetry contents from on-path observers). */
        ESP_LOGI(TAG, "endpoint is plaintext HTTP — commands stay HMAC-authenticated; "
                      "add TLS only if you need telemetry confidentiality");
    }

    s_resp_len = 0;
    s_resp[0] = '\0';
    s_resp_sig[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = HTTP_TIMEOUT_MS,
        .event_handler = resp_evt,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-W3PUPS-Device", devid);
    esp_http_client_set_header(client, "X-W3PUPS-Ts", ts_str);
    esp_http_client_set_header(client, "X-W3PUPS-Nonce", nonce_hex);
    esp_http_client_set_header(client, "X-W3PUPS-Sig", sig_hex);
    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed: %s (url=%s)", esp_err_to_name(err), url);
        return -1;
    }
    ESP_LOGI(TAG, "POST %d (%u B body, %u B resp, acks=%d)",
             status, (unsigned)body_len, (unsigned)s_resp_len, acks_in_body);

    if (status < 200 || status >= 300) {
        if (status == 401) ESP_LOGW(TAG, "401 — signature/identity rejected");
        return -1;
    }

    /* 2xx: the server accepted the acks we carried; drop them. Then apply any
     * commands it returned (verified against the request nonce + secret). */
    handle_response(s_resp, s_resp_len, key, key_len, nonce_hex, s_resp_sig);
    return acks_in_body;
}

/* --- task -------------------------------------------------------------- */

static void http_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(HTTP_SETTLE_MS));
    ESP_LOGI(TAG, "HTTP control-mode backend running (period=%d ms)", HTTP_PERIOD_MS);
    for (;;) {
        int dropped = post_once();
        if (dropped > 0) ack_drop_front(dropped);
        vTaskDelay(pdMS_TO_TICKS(HTTP_PERIOD_MS));
    }
}

void http_backend_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    /* 10 KB stack: TLS handshake + cJSON + HMAC scratch. */
    xTaskCreate(http_task, "http_backend", 10 * 1024, NULL, 4, NULL);
}
