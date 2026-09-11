#include "arkiv_rpc.h"
#include "arkiv_cfg.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "identity.h"
#include "cmdauth_arkiv.h"
#include "wups_link.h"
#include "wups_proto.h"
#include "arkiv_ack.h"
#include "arkiv_ws.h"
#include "fw_ota.h"

#define TAG "arkiv_rpc"

/* Single hex-nibble decode; defined with the other hex helpers below. */
static int hexnib(char c);

/* Monotonic (esp_timer) second-stamp of the last successful JSON-RPC
 * round-trip. 0 = none yet this boot. Volatile: written from the caller's
 * task, read from modem.c's supervision task. */
static volatile uint32_t s_last_rpc_ok_s;

/* --- HTTP JSON-RPC ---------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;   /* body exceeded cap-1: what's in buf is unusable */
} resp_ctx_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_ctx_t *c = (resp_ctx_t *)e->user_data;
    if (!c || e->data_len <= 0) return ESP_OK;
    size_t room = c->cap - 1 - c->len;
    size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
    /* Flag the overflow instead of silently cutting the JSON: a cut page
     * used to fail the parse and blind a whole cmd sweep with no trace
     * (issue #9). Keep draining so the client sees the full response. */
    if (n < (size_t)e->data_len) c->truncated = true;
    if (n == 0) return ESP_OK;
    memcpy(c->buf + c->len, e->data, n);
    c->len += n;
    c->buf[c->len] = '\0';
    return ESP_OK;
}

/* POST a JSON-RPC body, collect the response into resp (NUL-terminated).
 * Returns ESP_OK only on HTTP 2xx with a non-empty body that fit;
 * ESP_ERR_INVALID_SIZE when the 2xx body exceeded resp_cap (resp holds a
 * truncated prefix the caller MUST NOT parse — it still counts as a
 * successful round-trip for the uplink-health stamp); ESP_FAIL / transport
 * codes otherwise. */
static esp_err_t rpc_post(const char *body, char *resp, size_t resp_cap)
{
    resp_ctx_t ctx = { .buf = resp, .cap = resp_cap, .len = 0, .truncated = false };
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
    /* Uplink-health stamp: a completed HTTPS round-trip with a 2xx body is
     * proof the LTE data path works end-to-end. modem.c's watchdog uses
     * this instead of the WS-subscription state (a refused/404 WS — e.g. a
     * placeholder-token build — must not trip modem resets while RPC
     * traffic flows; field incident 2026-08-16). */
    s_last_rpc_ok_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (ctx.truncated) {
        ESP_LOGW(TAG, "rpc response exceeded %u B — unusable", (unsigned)resp_cap);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

uint32_t arkiv_rpc_last_success_s(void) { return s_last_rpc_ok_s; }

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
         * "nonce too low", "insufficient funds" etc. in the serial log. An
         * error OBJECT is normally the node's verdict on THIS tx — permanent,
         * so the writer must not retry it: ESP_ERR_INVALID_RESPONSE. The two
         * codes a gateway/proxy emits on its own behalf (-32603 internal
         * error, -32005 limit exceeded / rate limit) say nothing about the
         * tx and stay transient (ESP_FAIL). Anything else (no result, no
         * error) is a garbled reply → ESP_FAIL, transient. */
        cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *m = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
        cJSON *c = e ? cJSON_GetObjectItemCaseSensitive(e, "code") : NULL;
        if (cJSON_IsObject(e)) {
            int code = cJSON_IsNumber(c) ? (int)c->valuedouble : 0;
            bool transient = (code == -32603 || code == -32005);
            ESP_LOGW(TAG, "sendRawTransaction error %d: %s%s", code,
                     cJSON_IsString(m) ? m->valuestring : "(no message)",
                     transient ? " (transient)" : "");
            rc = transient ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGW(TAG, "sendRawTransaction: no result and no error");
        }
    }
    cJSON_Delete(root);
    return rc;
}

esp_err_t arkiv_eth_tx_known(const uint8_t hash[32], bool *out_known)
{
    if (!hash || !out_known) return ESP_ERR_INVALID_ARG;
    *out_known = false;
    static const char H[] = "0123456789abcdef";
    char hash_hex[2 + 64 + 1];
    hash_hex[0] = '0'; hash_hex[1] = 'x';
    for (size_t i = 0; i < 32; ++i) {
        hash_hex[2 + 2 * i]     = H[hash[i] >> 4];
        hash_hex[2 + 2 * i + 1] = H[hash[i] & 0x0F];
    }
    hash_hex[66] = '\0';
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_getTransactionByHash\","
        "\"params\":[\"%s\"]}", hash_hex);
    if (n <= 0 || n >= (int)sizeof(body)) return ESP_FAIL;
    /* Sized for the two shapes we care about: an unknown hash answers
     * `"result":null` (39 B, probed 2026-09-11); a known tx is a full
     * object (hash, blockHash, from, to, r, s, input = the whole CreateOp
     * RLP, ...) — 557 B on this node even with an EMPTY input — that never
     * fits, so an over-cap 2xx body IS the "known" answer and costs no
     * parse. A tx that did fit would parse as an object below anyway. */
    static char resp[512];
    esp_err_t rc = rpc_post(body, resp, sizeof(resp));
    if (rc == ESP_ERR_INVALID_SIZE) { *out_known = true; return ESP_OK; }
    if (rc != ESP_OK) return ESP_FAIL;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return ESP_FAIL;
    cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    rc = ESP_FAIL;
    if (cJSON_IsNull(r))        { *out_known = false; rc = ESP_OK; }
    else if (cJSON_IsObject(r)) { *out_known = true;  rc = ESP_OK; }
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

/* Sweep body; defined after its helpers below. The prototype sits here so
 * the helper block reads top-down — keep it: host harnesses slice this
 * file from the first occurrence of this prototype's text (its name must
 * not be spelled out in any comment above it). */
static void poll_once(const char *iccid, bool *out_more);

/* Serialises sweeps. poll_task (5 s / 5 min cadence) and arkiv_ws.c's
 * worker both run poll_once(), which parses into ONE static response
 * buffer and gathers into ONE static candidate array. Created in
 * arkiv_poll_start(), which main.c runs at boot before either consumer
 * task exists (the WS subscriber starts lazily from the heartbeat loop,
 * after ARKIV_CLAIMED). A NULL lock therefore means the poll task was
 * never started and the WS worker is the only caller, so running unlocked
 * is still race-free — no lazy create-under-critical-section needed. */
static SemaphoreHandle_t s_poll_lock;

/* Outcome of one w3pups-cmd entity in a sweep. */
typedef enum {
    CMD_DISPATCHED,    /* verified, counter advanced, frame → RP2040       */
    CMD_INVALID,       /* malformed entity — skipped, counter untouched    */
    CMD_AUTH_REJECTED, /* cmdauth_arkiv_check refused — counter untouched  */
    CMD_LOCAL_OP,      /* verified + consumed on the ESP32 (OTA / reset):
                        * the sweep stops — the device is about to reboot
                        * or is busy flashing; the caller re-arms a sweep
                        * for whatever was left behind (see out_more)      */
} cmd_outcome_t;

/* Candidate gathered for this sweep, kept ascending by seq. */
typedef struct {
    uint64_t     seq;
    const cJSON *e;
} cmd_cand_t;

/* Candidate bound. The gather may see ARKIV_CMD_MAX_PAGES × PAGE_N
 * entities but only the LOWEST seqs can be dispatched this sweep; anything
 * evicted from the array is still pending above the baseline and the next
 * sweep comes back for it. The PAGE_N headroom over MAX_PER_SWEEP keeps a
 * page of rejected (foreign / malformed) entities from starving valid ones. */
#define CMD_CAND_MAX (ARKIV_CMD_MAX_PER_SWEEP + ARKIV_CMD_PAGE_N)

/* Insertion-sort `seq` into `cand` (ascending). When the array is full a
 * newcomer only enters below the current highest, which is evicted;
 * `*evicted` tells the sweep it has not seen everything above what it
 * dispatches. */
static void cand_insert(cmd_cand_t *cand, unsigned *nc, bool *evicted,
                        uint64_t seq, const cJSON *e)
{
    if (*nc >= CMD_CAND_MAX) {
        if (seq >= cand[CMD_CAND_MAX - 1].seq) { *evicted = true; return; }
        *nc = CMD_CAND_MAX - 1;
        *evicted = true;
    }
    unsigned i = (*nc)++;
    while (i > 0 && cand[i - 1].seq > seq) { cand[i] = cand[i - 1]; --i; }
    cand[i].seq = seq;
    cand[i].e   = e;
}

/* One entity: field-by-field validation (explicit logs — when this drops,
 * we want to know WHICH attribute looked wrong), then cmdauth_arkiv_check
 * (which advances + persists last_ctr on success), ACK-mapping tracking,
 * and either an ESP32-local op or the forward to the RP2040. */
static cmd_outcome_t try_dispatch_entity(const cJSON *e, uint64_t seq,
                                         const char *iccid)
{
    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(e, "stringAttributes");
    const cJSON *na = cJSON_GetObjectItemCaseSensitive(e, "numericAttributes");
    const cJSON *writer = cJSON_GetObjectItemCaseSensitive(e, "creator");
    if (!cJSON_IsString(writer))
        writer = cJSON_GetObjectItemCaseSensitive(e, "owner");
    /* Entity payload comes back as the `value` field, NOT `payload` — and
     * as a 0x-prefixed hex string, NOT base64 (Arkiv RPC contract; see
     * RpcEntity in @arkiv-network/sdk). The earlier base64 path silently
     * dropped every cmd because Arkiv returns no `payload` key at all. */
    const cJSON *value     = cJSON_GetObjectItemCaseSensitive(e, "value");
    const char *sig_hex    = str_attr(sa, ARKIV_ATTR_SIG);
    const char *command_id = str_attr(sa, ARKIV_ATTR_COMMAND_ID);

    uint8_t writer_b[20], sig_b[64];
    uint8_t frame[WUPS_MAX_PAYLOAD];
    size_t frame_len = 0;

    if (!cJSON_IsString(writer)) {
        ESP_LOGW(TAG, "cmd seq=%llu: writer missing/non-string — dropping",
                 (unsigned long long)seq);
        return CMD_INVALID;
    }
    if (!cJSON_IsString(value) || !value->valuestring) {
        ESP_LOGW(TAG, "cmd seq=%llu: entity value missing — dropping",
                 (unsigned long long)seq);
        return CMD_INVALID;
    }
    if (!sig_hex) {
        ESP_LOGW(TAG, "cmd seq=%llu: sig attribute missing — dropping",
                 (unsigned long long)seq);
        return CMD_INVALID;
    }
    if (!command_id || strlen(command_id) != ARKIV_COMMAND_ID_LEN) {
        ESP_LOGW(TAG, "cmd seq=%llu: command_id missing or wrong length "
                 "(got %u, want %d) — dropping", (unsigned long long)seq,
                 command_id ? (unsigned)strlen(command_id) : 0,
                 ARKIV_COMMAND_ID_LEN);
        return CMD_INVALID;
    }
    if (hex_decode(writer->valuestring, writer_b, 20) != 20) {
        ESP_LOGW(TAG, "cmd seq=%llu: writer not 20-byte hex — dropping",
                 (unsigned long long)seq);
        return CMD_INVALID;
    }
    if (hex_decode(sig_hex, sig_b, 64) != 64) {
        ESP_LOGW(TAG, "cmd seq=%llu: sig not 64-byte hex (len=%u) — dropping",
                 (unsigned long long)seq, (unsigned)strlen(sig_hex));
        return CMD_INVALID;
    }
    if (hex_decode_var(value->valuestring, frame, sizeof(frame), &frame_len) != 0) {
        ESP_LOGW(TAG, "cmd seq=%llu: entity value not decodable hex (len=%u) "
                 "— dropping", (unsigned long long)seq,
                 (unsigned)strlen(value->valuestring));
        return CMD_INVALID;
    }

    arkiv_cmd_t cmd = {
        .epoch      = (uint32_t)num_attr(na, ARKIV_ATTR_EPOCH),
        .counter    = seq,
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
    if (!cmdauth_arkiv_check(&cmd, &vframe, &vlen)) return CMD_AUTH_REJECTED;

    ESP_LOGI(TAG, "Arkiv command verified (seq=%llu) → RP2040",
             (unsigned long long)seq);
    /* Track the inner WUPS SEQ → command_id mapping BEFORE forwarding,
     * so when the RP2040's cmd/response RESP arrives (with the same
     * SEQ echoed back) we can divert it into a w3pups-ack entity
     * instead of MQTT (P4 §4.6 — closes the loop the panel UI watches). */
    if (vlen >= WUPS_HEADER_BYTES) {
        arkiv_ack_track_pending(vframe[7] /* SEQ offset */, command_id);
    }
    /* ESP32-local ops — MQTT-downlink-path parity (wups_link.c):
     * fw.update and system.reset execute on the ESP32 itself and ACK
     * through the tracker above (w3pups-ack); they must never reach
     * the RP2040, which would drop them and the command would time
     * out after the owner already paid signature + gas. */
    if (fw_ota_try_handle_downlink(vframe, vlen)) {
        return CMD_LOCAL_OP;   /* consumed — OTA kicked off (or refused), ACK emitted */
    }
    if (wups_link_try_sys_reset(vframe, vlen)) {
        return CMD_LOCAL_OP;   /* consumed — reset scheduled (or refused), ACK emitted */
    }
    forward_to_rp2040(iccid, vframe, vlen);
    return CMD_DISPATCHED;
}

/* One sweep: every pending w3pups-cmd above the replay baseline, oldest
 * first, at most ARKIV_CMD_MAX_PER_SWEEP dispatches. Before issue #9 this
 * picked the single max-seq entity and validated only that one — with two
 * pending commands the older one was replay-rejected forever, and any
 * higher-seq entity by a foreign writer shadowed the valid one.
 *
 * GATHER, THEN DISPATCH. Node semantics probed read-only on
 * arkiv.web3pi.io 2026-09-11 (the pinned Braga-era arkiv-op-geth build):
 *   - arkiv_query lists entities NEWEST FIRST; `orderBy` is accepted and
 *     silently ignored (asc/desc/any shape return the same order);
 *   - the numeric clauses `seq > N`, `seq < N`, `seq <= N` are honoured;
 *   - the response `cursor` (0x-hex) is also present on a full LAST page,
 *     so it cannot tell "more exist" apart from "page full".
 * A full page of PAGE_N entities is therefore the NEWEST subset of what is
 * pending, and dispatching it would advance the baseline past any older
 * command still on a later page — the issue #9 failure class again, merely
 * bounded to > PAGE_N pending. So the sweep walks DOWN the seq axis with
 * the window `seq > last_ctr && seq < floor` (floor = the lowest seq of the
 * previous page) until a short page proves the window exhausted, keeping
 * the lowest CMD_CAND_MAX candidates, and only then dispatches them
 * ascending. No server-side pagination state is involved, and a node that
 * ignored a clause makes no progress instead of looping (the window is
 * re-checked client-side). If the ARKIV_CMD_MAX_PAGES budget runs out
 * before a short page — more than MAX_PAGES × PAGE_N pending owner-signed
 * commands inside the panel's ≤ 5 min row window — the lowest gathered
 * are dispatched anyway and a WARN names the hazard: waiting would not make
 * the unseen older ones visible either, and the panel times them out the
 * same way.
 *
 * `*out_more`: the sweep made progress AND left pending commands behind
 * (dispatch cap, local op, evicted candidates, or an incomplete gather).
 * The callers re-arm on it instead of waiting for the next cadence. A sweep
 * that dispatched nothing never reports more, so a page of foreign junk
 * cannot spin them. Any RPC failure mid-gather aborts the sweep WITHOUT
 * dispatching — a page that failed may hold the oldest command. */
static void poll_once(const char *iccid, bool *out_more)
{
    /* Statics: one sweep at a time (s_poll_lock) and ~1 KB less stack on
     * the two 6 KB consumer tasks. */
    static char       resp[ARKIV_RPC_RESP_CAP];
    static char       body[512];
    static cmd_cand_t cand[CMD_CAND_MAX];
    cJSON   *roots[ARKIV_CMD_MAX_PAGES] = {0};
    unsigned nc = 0, pages = 0, seen = 0, dispatched = 0, rejected = 0;
    bool     evicted = false, complete = false, aborted = false;
    const uint64_t last_ctr = cmdauth_arkiv_last_ctr();
    uint64_t floor = UINT64_MAX;   /* exclusive upper bound of the next window */

    *out_more = false;

    /* Phase 1 — gather. */
    while (pages < ARKIV_CMD_MAX_PAGES) {
        char window[48] = "";
        if (floor != UINT64_MAX) {
            snprintf(window, sizeof(window), " && %s < %llu",
                     ARKIV_ATTR_SEQ, (unsigned long long)floor);
        }
        /* arkiv_query: params[0]=filter, params[1]=options. device_id is a
         * digits-only ICCID (identity.c validated it) so it is filter-safe. */
        int n = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"arkiv_query\",\"params\":["
            "\"%s = \\\"%s\\\" && %s = \\\"%s\\\" && %s > %llu%s\","
            "{\"includeData\":{\"key\":true,\"attributes\":true,\"payload\":true,"
            "\"contentType\":false,\"owner\":true,\"creator\":true,"
            "\"expiration\":false},\"resultsPerPage\":\"" ARKIV_CMD_PAGE "\"}]}",
            ARKIV_ATTR_TYPE, ARKIV_CMD_ENTITY_TYPE,
            ARKIV_ATTR_DEVICE_ID, iccid,
            ARKIV_ATTR_SEQ, (unsigned long long)last_ctr, window);
        if (n <= 0 || n >= (int)sizeof(body)) { aborted = true; break; }
        pages++;
        esp_err_t rc = rpc_post(body, resp, sizeof(resp));
        if (rc == ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "arkiv_query response exceeded %u B — sweep skipped "
                          "(page too large?)", (unsigned)ARKIV_RPC_RESP_CAP);
        }
        if (rc != ESP_OK) { aborted = true; break; }

        cJSON *root = cJSON_Parse(resp);
        if (!root) { ESP_LOGW(TAG, "rpc json parse failed"); aborted = true; break; }
        roots[pages - 1] = root;   /* strings live in the tree; resp is reused */
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const cJSON *data = result
            ? cJSON_GetObjectItemCaseSensitive(result, "data") : NULL;
        if (!cJSON_IsArray(data)) { aborted = true; break; }

        unsigned page_n = 0, added = 0;
        uint64_t page_min = UINT64_MAX;
        const cJSON *e;
        cJSON_ArrayForEach(e, data) {
            page_n++;
            const cJSON *na = cJSON_GetObjectItemCaseSensitive(e, "numericAttributes");
            uint64_t seq = num_attr(na, ARKIV_ATTR_SEQ);
            /* Window re-check: consumed entities (≤ baseline) are never
             * re-validated, and a node ignoring `seq <` cannot loop us. */
            if (seq <= last_ctr || seq >= floor) continue;
            added++;
            if (seq < page_min) page_min = seq;
            cand_insert(cand, &nc, &evicted, seq, e);
        }
        seen += page_n;
        /* A short page is the complete remainder of the window. */
        if (page_n < ARKIV_CMD_PAGE_N) { complete = true; break; }
        /* Full page, nothing new: the node ignores the window — no progress. */
        if (added == 0) break;
        /* Nothing can sit between the baseline and baseline+1. */
        if (page_min <= last_ctr + 1) { complete = true; break; }
        floor = page_min;   /* narrow the window below everything seen */
    }

    /* Phase 2 — dispatch ascending. */
    unsigned i = 0;
    if (!aborted) {
        if (!complete && nc > 0) {
            ESP_LOGW(TAG, "cmd sweep: page budget (%d x %d) exhausted above seq "
                          "%llu — dispatching the lowest %u seen; older unseen "
                          "commands will be replay-rejected",
                     ARKIV_CMD_MAX_PAGES, ARKIV_CMD_PAGE_N,
                     (unsigned long long)last_ctr, nc);
        }
        bool stop = false;
        while (i < nc && !stop) {
            /* Re-read after every accept: a duplicate seq (two entities for
             * one command) is pre-skipped once the first advanced it. */
            if (cand[i].seq > cmdauth_arkiv_last_ctr()) {
                switch (try_dispatch_entity(cand[i].e, cand[i].seq, iccid)) {
                case CMD_DISPATCHED: dispatched++; break;
                case CMD_LOCAL_OP:   dispatched++; stop = true; break;
                default:             rejected++;   break;
                }
                if (dispatched >= ARKIV_CMD_MAX_PER_SWEEP) stop = true;
            }
            i++;
        }
    }
    for (unsigned r = 0; r < ARKIV_CMD_MAX_PAGES; ++r) cJSON_Delete(roots[r]);

    *out_more = dispatched > 0 && (i < nc || evicted || !complete);
    if (seen > 1 || rejected || *out_more || aborted) {
        ESP_LOGI(TAG, "cmd sweep: %u entities over %u page(s) — %u dispatched, "
                      "%u rejected, %u skipped%s", seen, pages, dispatched, rejected,
                 seen - dispatched - rejected,
                 aborted ? " (aborted — nothing consumed)"
                         : *out_more ? " (more pending — re-arming)" : "");
    }
}

bool arkiv_rpc_poll_once(void)
{
    if (!cmdauth_arkiv_ready()) return false;
    if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) return false;
    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') return false;
    /* Whole sweep under the lock: the response buffer is shared and the
     * only callers (poll task, WS worker) have nothing better to do than
     * wait for the in-flight sweep — which dispatches their commands too. */
    bool more = false;
    if (s_poll_lock) xSemaphoreTake(s_poll_lock, portMAX_DELAY);
    poll_once(iccid, &more);
    if (s_poll_lock) xSemaphoreGive(s_poll_lock);
    return more;
}

bool arkiv_rpc_fetch_and_verify_by_key(const char *entity_key)
{
    /* Deliberately NOT a by-key lookup: the key is a wake-up signal and the
     * seq-ordered sweep is the only path that keeps the replay baseline
     * consistent (a by-key fetch of a newer entity would burn any older
     * pending seq, exactly the issue #9 failure). arkiv_ws.c coalesces a
     * burst of keys into one call, so this costs one sweep per burst. */
    (void)entity_key;
    return arkiv_rpc_poll_once();
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
        /* Re-arm while the sweep reports pending commands left behind
         * (dispatch cap, local op, page budget): the 5-min steady-state
         * cadence equals the panel's row window, so waiting for it would
         * time those commands out. Bounded so a persistent "more" cannot
         * starve the cadence. */
        for (unsigned k = 0; arkiv_rpc_poll_once() && k < ARKIV_CMD_MAX_RESWEEPS; ++k) {
            vTaskDelay(pdMS_TO_TICKS(ARKIV_CMD_RESWEEP_DELAY_MS));
        }
    }
}

void arkiv_poll_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* The sweep lock must exist before any consumer task does (see its
     * comment). Without it two sweeps could share the response buffer, so
     * an allocation failure keeps the fallback poll task down rather than
     * risk that — the WS worker still sweeps on its own. */
    if (!s_poll_lock) s_poll_lock = xSemaphoreCreateMutex();
    if (!s_poll_lock) {
        ESP_LOGE(TAG, "poll lock alloc failed — fallback poll task NOT started");
        return;
    }
    /* 6 KB stack: TLS + cJSON over a few-KB response. */
    xTaskCreate(poll_task, "arkiv_poll", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Arkiv poll task started (interval %d ms aggressive / "
                  "%d ms fallback while WS subscribed)",
             ARKIV_POLL_INTERVAL_MS, ARKIV_POLL_INTERVAL_FALLBACK_MS);
}
