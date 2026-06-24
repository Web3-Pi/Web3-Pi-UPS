#include "arkiv_rpc.h"
#include "arkiv_cfg.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "identity.h"
#include "cmdauth_arkiv.h"
#include "wups_link.h"
#include "wups_proto.h"
#include "arkiv_ack.h"
#include "arkiv_ws.h"

#define TAG "arkiv_rpc"

/* Single hex-nibble decode; defined with the other hex helpers below. */
static int hexnib(char c);

/* --- HTTP JSON-RPC ---------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} resp_ctx_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_ctx_t *c = (resp_ctx_t *)e->user_data;
    if (!c || e->data_len <= 0) return ESP_OK;
    size_t room = c->cap - 1 - c->len;
    if (room == 0) return ESP_OK; /* truncate — parser will reject */
    size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
    memcpy(c->buf + c->len, e->data, n);
    c->len += n;
    c->buf[c->len] = '\0';
    return ESP_OK;
}

/* POST a JSON-RPC body, collect the response into resp (NUL-terminated).
 * Returns ESP_OK only on HTTP 2xx with a non-empty body. */
static esp_err_t rpc_post(const char *body, char *resp, size_t resp_cap)
{
    resp_ctx_t ctx = { .buf = resp, .cap = resp_cap, .len = 0 };
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url           = ARKIV_RPC_URL,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = ARKIV_HTTP_TIMEOUT_MS,
        .event_handler = http_evt,
        .user_data     = &ctx,
        /* HTTPS to the public Braga RPC gateway: trust via the ESP-IDF
         * mbedTLS cert bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y), the
         * same anchor mqtt.c uses for mqtts. Without this the TLS
         * handshake fails as ESP_ERR_HTTP_CONNECT even though PPP +
         * plain HTTP + MQTTS are all up (plan §4.7: integrity-protected
         * single gateway, availability deliberately not). */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return ESP_FAIL;
    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_post_field(cl, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(cl);
    int status = err == ESP_OK ? esp_http_client_get_status_code(cl) : -1;
    esp_http_client_cleanup(cl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rpc transport err: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300 || ctx.len == 0) {
        ESP_LOGW(TAG, "rpc http=%d len=%u", status, (unsigned)ctx.len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t arkiv_eth_block_number(uint64_t *out_block)
{
    static char resp[256];
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_blockNumber\",\"params\":[]}";
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    esp_err_t rc = ESP_FAIL;
    if (cJSON_IsString(r) && r->valuestring) {
        *out_block = strtoull(r->valuestring, NULL, 0); /* 0x-hex */
        rc = ESP_OK;
    }
    cJSON_Delete(root);
    return rc;
}

/* --- nonce / gas / send (P4 writer plumbing) ------------------------- */

/* Extract a 0x-hex uint64 from a JSON-RPC `result` field. */
static esp_err_t parse_result_uint(const char *resp, uint64_t *out)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    esp_err_t rc = ESP_FAIL;
    if (cJSON_IsString(r) && r->valuestring) {
        *out = strtoull(r->valuestring, NULL, 0);
        rc = ESP_OK;
    }
    cJSON_Delete(root);
    return rc;
}

static void addr_to_hex(const uint8_t addr[20], char out[2 + 40 + 1])
{
    static const char H[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (size_t i = 0; i < 20; ++i) {
        out[2 + 2 * i]     = H[addr[i] >> 4];
        out[2 + 2 * i + 1] = H[addr[i] & 0x0F];
    }
    out[42] = '\0';
}

esp_err_t arkiv_eth_get_tx_count(const uint8_t addr[20], uint64_t *out_nonce)
{
    if (!addr || !out_nonce) return ESP_ERR_INVALID_ARG;
    char addr_hex[43];
    addr_to_hex(addr, addr_hex);
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_getTransactionCount\","
        "\"params\":[\"%s\",\"pending\"]}", addr_hex);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_FAIL;
    static char resp[256];
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return ESP_FAIL;
    return parse_result_uint(resp, out_nonce);
}

/* Parse a JSON-RPC `result` 0x-hex quantity into a uint64, failing closed on
 * overflow (a value too large to be a small-GLM test balance). Distinct from
 * parse_result_uint's strtoull, which silently saturates and would report a
 * wrapped figure as a real balance. */
static esp_err_t parse_result_wei(const char *resp, uint64_t *out)
{
    cJSON *root = cJSON_Parse(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    esp_err_t rc = ESP_FAIL;
    if (cJSON_IsString(r) && r->valuestring) {
        const char *h = r->valuestring;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        while (*h == '0') ++h;            /* skip leading zeros */
        uint64_t v = 0;
        rc = ESP_OK;
        for (; *h; ++h) {
            int nib = hexnib(*h);
            if (nib < 0) { rc = ESP_FAIL; break; }
            if (v > (UINT64_MAX >> 4)) { rc = ESP_ERR_INVALID_SIZE; break; }
            v = (v << 4) | (uint64_t)nib;
        }
        if (rc == ESP_OK) *out = v;
    }
    cJSON_Delete(root);
    return rc;
}

esp_err_t arkiv_eth_get_balance(const uint8_t addr[20], uint64_t *out_wei)
{
    if (!addr || !out_wei) return ESP_ERR_INVALID_ARG;
    char addr_hex[43];
    addr_to_hex(addr, addr_hex);
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_getBalance\","
        "\"params\":[\"%s\",\"latest\"]}", addr_hex);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_FAIL;
    static char resp[256];
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return ESP_FAIL;
    return parse_result_wei(resp, out_wei);
}

esp_err_t arkiv_eth_gas_price(uint64_t *out_wei)
{
    if (!out_wei) return ESP_ERR_INVALID_ARG;
    static char resp[256];
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_gasPrice\",\"params\":[]}";
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return ESP_FAIL;
    return parse_result_uint(resp, out_wei);
}

esp_err_t arkiv_eth_send_raw_tx(const uint8_t *raw, size_t raw_len,
                                char *out_hash, size_t out_hash_cap)
{
    if (!raw || raw_len == 0) return ESP_ERR_INVALID_ARG;
    if (out_hash && out_hash_cap > 0) out_hash[0] = '\0';

    /* Hex-encode the raw bytes into a stack buffer. A typical CreateOp tx
     * is well under 2 KB; keep the cap generous but bounded. */
    static char tx_hex[4096];
    if (2 + raw_len * 2 + 1 > sizeof(tx_hex)) return ESP_ERR_INVALID_SIZE;
    static const char H[] = "0123456789abcdef";
    tx_hex[0] = '0'; tx_hex[1] = 'x';
    for (size_t i = 0; i < raw_len; ++i) {
        tx_hex[2 + 2 * i]     = H[raw[i] >> 4];
        tx_hex[2 + 2 * i + 1] = H[raw[i] & 0x0F];
    }
    tx_hex[2 + raw_len * 2] = '\0';

    /* Body: minimal JSON wrapper; tx hex is the bulk. */
    static char body[4200];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_sendRawTransaction\","
        "\"params\":[\"%s\"]}", tx_hex);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_FAIL;
    static char resp[512];
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    esp_err_t rc = ESP_FAIL;
    if (cJSON_IsString(r) && r->valuestring) {
        if (out_hash && out_hash_cap > 0) {
            size_t want = strlen(r->valuestring);
            size_t copy = want < out_hash_cap - 1 ? want : out_hash_cap - 1;
            memcpy(out_hash, r->valuestring, copy);
            out_hash[copy] = '\0';
        }
        rc = ESP_OK;
    } else {
        /* Surface the JSON-RPC error so we can see "intrinsic gas too low",
         * "nonce too low", "insufficient funds" etc. in the serial log. */
        cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *m = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
        if (cJSON_IsString(m)) {
            ESP_LOGW(TAG, "sendRawTransaction error: %s", m->valuestring);
        } else {
            ESP_LOGW(TAG, "sendRawTransaction: no result and no error");
        }
    }
    cJSON_Delete(root);
    return rc;
}

esp_err_t arkiv_rpc_query(const char *filter, char *resp, size_t resp_cap)
{
    if (!filter || !resp || resp_cap == 0) return ESP_ERR_INVALID_ARG;
    char body[640];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"arkiv_query\",\"params\":["
        "\"%s\","
        "{\"includeData\":{\"key\":true,\"attributes\":true,\"payload\":true,"
        "\"contentType\":false,\"owner\":true,\"creator\":true,"
        "\"expiration\":false},\"resultsPerPage\":\"0xa\"}]}", filter);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_FAIL;
    return rpc_post(body, resp, resp_cap);
}

/* --- helpers ---------------------------------------------------------- */

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode `hex` (optionally 0x-prefixed) into out[0..out_len). Returns bytes
 * written, or -1 on malformed input / size mismatch. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t n = strlen(hex);
    if (n != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hexnib(hex[2 * i]), lo = hexnib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)out_len;
}

/* Decode an unknown-length 0x-prefixed hex string into out[0..out_cap),
 * writing the byte length into *out_len. Returns 0 on success, -1 on bad
 * input. Used for the entity `value` (canonical WUPS frame, variable
 * length) — Arkiv ships it as `"0x…"`, NOT base64. */
static int hex_decode_var(const char *hex, uint8_t *out, size_t out_cap,
                          size_t *out_len)
{
    if (!hex || !out_len) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t n = strlen(hex);
    if ((n & 1) != 0) return -1;       /* must be byte-aligned */
    size_t bytes = n / 2;
    if (bytes == 0 || bytes > out_cap) return -1;
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hexnib(hex[2 * i]), lo = hexnib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = bytes;
    return 0;
}

static const char *str_attr(const cJSON *arr, const char *key)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "key");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value");
        if (cJSON_IsString(k) && cJSON_IsString(v) &&
            strcmp(k->valuestring, key) == 0) {
            return v->valuestring;
        }
    }
    return NULL;
}

static uint64_t num_attr(const cJSON *arr, const char *key)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "key");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "value");
        if (!cJSON_IsString(k) || strcmp(k->valuestring, key) != 0) continue;
        if (cJSON_IsNumber(v)) return (uint64_t)v->valuedouble;
        if (cJSON_IsString(v) && v->valuestring) {
            return strtoull(v->valuestring, NULL, 0); /* dec or 0x-hex */
        }
    }
    return 0;
}

/* Forward a verified bare WUPS frame to the RP2040 hub as net.downlink —
 * the SAME wrapping the MQTT path uses (Decision C parity). topic =
 * c/<iccid>/cmd/request so the RP2040 routes it exactly as before. */
static void forward_to_rp2040(const char *iccid,
                              const uint8_t *frame, size_t frame_len)
{
    char topic[64];
    int tl = snprintf(topic, sizeof(topic), "c/%s/cmd/request", iccid);
    if (tl <= 0 || tl >= (int)sizeof(topic)) return;
    size_t total = sizeof(wups_net_downlink_v1_hdr_t) + (size_t)tl + frame_len;
    if (total > WUPS_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "downlink too big (%u), dropping", (unsigned)total);
        return;
    }
    uint8_t buf[WUPS_MAX_PAYLOAD];
    wups_net_downlink_v1_hdr_t hdr = {
        .version = 1, .qos = 0, .retain = 0,
        .topic_len = (uint8_t)tl, .payload_len = (uint16_t)frame_len,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), topic, (size_t)tl);
    memcpy(buf + sizeof(hdr) + tl, frame, frame_len);
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_NET, WUPS_OP_NET_DOWNLINK,
                   WUPS_FLAG_EVENT, buf, (uint16_t)total);
}

/* --- poll ------------------------------------------------------------- */

static void poll_once(const char *iccid)
{
    static char resp[ARKIV_RPC_RESP_CAP];

    /* arkiv_query: params[0]=filter, params[1]=options. device_id is a
     * digits-only ICCID (identity.c validated it) so it is filter-safe. */
    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"arkiv_query\",\"params\":["
        "\"%s = \\\"%s\\\" && %s = \\\"%s\\\"\","
        "{\"includeData\":{\"key\":true,\"attributes\":true,\"payload\":true,"
        "\"contentType\":false,\"owner\":true,\"creator\":true,"
        "\"expiration\":false},\"resultsPerPage\":\"0xa\"}]}",
        ARKIV_ATTR_TYPE, ARKIV_CMD_ENTITY_TYPE,
        ARKIV_ATTR_DEVICE_ID, iccid);
    if (n <= 0 || n >= (int)sizeof(body)) return;
    if (rpc_post(body, resp, sizeof(resp)) != ESP_OK) return;

    cJSON *root = cJSON_Parse(resp);
    if (!root) { ESP_LOGW(TAG, "rpc json parse failed"); return; }
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *data = result
        ? cJSON_GetObjectItemCaseSensitive(result, "data") : NULL;
    if (!cJSON_IsArray(data)) { cJSON_Delete(root); return; }

    /* Pick the freshest entity (max seq); cmdauth_arkiv enforces strict
     * monotonicity + writer/epoch (and is fail-closed until P2-3). */
    const cJSON *best = NULL;
    uint64_t best_seq = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, data) {
        const cJSON *na = cJSON_GetObjectItemCaseSensitive(e, "numericAttributes");
        uint64_t seq = num_attr(na, ARKIV_ATTR_SEQ);
        if (seq > best_seq) { best_seq = seq; best = e; }
    }
    if (!best) { cJSON_Delete(root); return; }

    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(best, "stringAttributes");
    const cJSON *na = cJSON_GetObjectItemCaseSensitive(best, "numericAttributes");
    const cJSON *writer = cJSON_GetObjectItemCaseSensitive(best, "creator");
    if (!cJSON_IsString(writer))
        writer = cJSON_GetObjectItemCaseSensitive(best, "owner");
    /* Entity payload comes back as the `value` field, NOT `payload` — and
     * as a 0x-prefixed hex string, NOT base64 (Arkiv RPC contract; see
     * RpcEntity in @arkiv-network/sdk). The earlier base64 path silently
     * dropped every cmd because Arkiv returns no `payload` key at all. */
    const cJSON *value     = cJSON_GetObjectItemCaseSensitive(best, "value");
    const char *sig_hex    = str_attr(sa, ARKIV_ATTR_SIG);
    const char *command_id = str_attr(sa, ARKIV_ATTR_COMMAND_ID);

    uint8_t writer_b[20], sig_b[64];
    uint8_t frame[WUPS_MAX_PAYLOAD];
    size_t frame_len = 0;

    /* Per-field validation with explicit logs — when this drops, we want
     * to know WHICH attribute looked wrong, not just that *something* did. */
    if (!cJSON_IsString(writer)) {
        ESP_LOGW(TAG, "cmd seq=%llu: writer missing/non-string — dropping",
                 (unsigned long long)best_seq);
        cJSON_Delete(root); return;
    }
    if (!cJSON_IsString(value) || !value->valuestring) {
        ESP_LOGW(TAG, "cmd seq=%llu: entity value missing — dropping",
                 (unsigned long long)best_seq);
        cJSON_Delete(root); return;
    }
    if (!sig_hex) {
        ESP_LOGW(TAG, "cmd seq=%llu: sig attribute missing — dropping",
                 (unsigned long long)best_seq);
        cJSON_Delete(root); return;
    }
    if (!command_id || strlen(command_id) != ARKIV_COMMAND_ID_LEN) {
        ESP_LOGW(TAG, "cmd seq=%llu: command_id missing or wrong length "
                 "(got %u, want %d) — dropping", (unsigned long long)best_seq,
                 command_id ? (unsigned)strlen(command_id) : 0,
                 ARKIV_COMMAND_ID_LEN);
        cJSON_Delete(root); return;
    }
    if (hex_decode(writer->valuestring, writer_b, 20) != 20) {
        ESP_LOGW(TAG, "cmd seq=%llu: writer not 20-byte hex — dropping",
                 (unsigned long long)best_seq);
        cJSON_Delete(root); return;
    }
    if (hex_decode(sig_hex, sig_b, 64) != 64) {
        ESP_LOGW(TAG, "cmd seq=%llu: sig not 64-byte hex (len=%u) — dropping",
                 (unsigned long long)best_seq, (unsigned)strlen(sig_hex));
        cJSON_Delete(root); return;
    }
    if (hex_decode_var(value->valuestring, frame, sizeof(frame), &frame_len) != 0) {
        ESP_LOGW(TAG, "cmd seq=%llu: entity value not decodable hex (len=%u) "
                 "— dropping", (unsigned long long)best_seq,
                 (unsigned)strlen(value->valuestring));
        cJSON_Delete(root); return;
    }

    arkiv_cmd_t cmd = {
        .epoch      = (uint32_t)num_attr(na, ARKIV_ATTR_EPOCH),
        .counter    = best_seq,
        .block      = 0, /* MVP: seq is the strict baseline (§4.4 TODO) */
        .device_id  = iccid,
        .command_id = command_id,
        .frame      = frame,
        .frame_len  = frame_len,
        .sig        = sig_b,
    };
    memcpy(cmd.writer, writer_b, 20);

    const uint8_t *vframe = NULL;
    size_t vlen = 0;
    if (cmdauth_arkiv_check(&cmd, &vframe, &vlen)) {
        ESP_LOGI(TAG, "Arkiv command verified (seq=%llu) → RP2040",
                 (unsigned long long)best_seq);
        /* Track the inner WUPS SEQ → command_id mapping BEFORE forwarding,
         * so when the RP2040's cmd/response RESP arrives (with the same
         * SEQ echoed back) we can divert it into a w3pups-ack entity
         * instead of MQTT (P4 §4.6 — closes the loop the panel UI watches). */
        if (vlen >= WUPS_HEADER_BYTES) {
            arkiv_ack_track_pending(vframe[7] /* SEQ offset */, command_id);
        }
        forward_to_rp2040(iccid, vframe, vlen);
    }
    cJSON_Delete(root);
}

void arkiv_rpc_poll_once(void)
{
    if (!cmdauth_arkiv_ready()) return;
    if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) return;
    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') return;
    poll_once(iccid);
}

void arkiv_rpc_fetch_and_verify_by_key(const char *entity_key)
{
    /* v1: ignore the specific key and run the standard freshest-by-seq
     * sweep. That's intentional — it's the same body the legacy poll
     * uses, well-tested, and a single WS event almost always corresponds
     * to the freshest entity. A future precision lookup by key can
     * skip the scan but it's not worth the extra path until we see a
     * scenario where the WS event arrives before the entity is queryable. */
    (void)entity_key;
    arkiv_rpc_poll_once();
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Dynamic interval: while the WS subscriber is healthy, this loop
         * is just a belt-and-suspenders sweep every 5 minutes; when the
         * WS drops we fall back to the original 5-second cadence so a
         * prolonged WS outage doesn't strand commands. The decision is
         * re-evaluated at the top of each loop iteration. */
        uint32_t delay_ms = arkiv_ws_subscribed()
                              ? ARKIV_POLL_INTERVAL_FALLBACK_MS
                              : ARKIV_POLL_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        arkiv_rpc_poll_once();
    }
}

void arkiv_poll_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* 6 KB stack: TLS + cJSON over a few-KB response. */
    xTaskCreate(poll_task, "arkiv_poll", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Arkiv poll task started (interval %d ms aggressive / "
                  "%d ms fallback while WS subscribed)",
             ARKIV_POLL_INTERVAL_MS, ARKIV_POLL_INTERVAL_FALLBACK_MS);
}
