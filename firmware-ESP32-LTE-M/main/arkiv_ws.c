#include "arkiv_ws.h"
#include "arkiv_cfg.h"
#include "arkiv_rpc.h"
#include "arkiv_writer.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "arkiv_crypto/keccak256.h"

#define TAG "arkiv_ws"

/* WSS endpoint — same host as the HTTPS RPC. The self-hosted node's WS
 * lives behind a token path (`/ws/<token>`, enforced by the HAProxy in
 * front of the node): the proxy's JSON-RPC method deny-list cannot
 * inspect WS frames, so the unguessable path keeps internet scanners off
 * the socket. The token ships inside every firmware image, so it is
 * scan-protection, not a secret against a determined attacker (the RPC
 * itself is public and the threat model already tolerates a hostile
 * gateway). Value lives in the gitignored arkiv_ws_token.h — copy
 * arkiv_ws_token.h.example and fill in the deployment's token. Pinned
 * via the public CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) the rest
 * of the firmware uses for HTTPS/MQTTS. */
#include "arkiv_ws_token.h"
#define ARKIV_WS_URI               "wss://arkiv.web3pi.io/ws/" ARKIV_WS_TOKEN

/* Operator's safe ping cadence — 1NCE/SimBase may close idle PPP TCP
 * after ~20 min; 30 s is well under any reasonable idle timeout while
 * staying low-traffic (~10 B per side per ping → ~1.7 MB/mo, see §E). */
#define ARKIV_WS_PING_SEC          30
#define ARKIV_WS_PINGPONG_TIMEOUT_SEC  (ARKIV_WS_PING_SEC * 3)

/* Backoff on reconnect — start at 2 s, the client doubles internally. */
#define ARKIV_WS_RECONNECT_MS      2000

/* RX buffer for fragmented WS payloads (eth_subscription notifications
 * can run several hundred bytes — give comfortable headroom). */
#define ARKIV_WS_RX_BUF            4096
#define ARKIV_WS_TASK_STACK        6144
#define ARKIV_WS_TASK_PRIO         5

/* Worker queue — entity keys observed via subscription that need a
 * one-shot HTTPS fetch + verify. Doing the fetch on the WS client task
 * would block the next notification; do it on a dedicated worker so
 * the WS RX stays responsive. */
#define ARKIV_WS_WORK_QUEUE_LEN    8
#define ARKIV_WS_WORK_TASK_STACK   6144  /* TLS handshake + cJSON */

/* Event signatures we care about. v1 only listens for Created because cmd
 * lifecycle today never updates / extends entities (the panel writes one
 * `w3pups-cmd` per call). When `w3pups-cmd` lifecycle grows (e.g. extend
 * for retry), enable the matching sig here. */
static const char *EVT_SIG_CREATED = "ArkivEntityCreated(uint256,address,uint256,uint256)";

/* Hashed event topic0 = keccak256(signature). Computed once at start. */
static char s_topic0_created[2 + 64 + 1];

/* The padded owner-address topic. topic[2] for ArkivEntityCreated is the
 * indexed `address ownerAddress` parameter. We compute the 32-byte zero-
 * padded hex form once at start; the server filters on it directly. */
static char s_owner_topic[2 + 64 + 1];

static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_client;
static bool s_running;
static bool s_subscribed;
static char s_sub_id[80];

/* Multi-frame RX buffer (continuation frames). */
static char *s_rx_acc;
static size_t s_rx_acc_len;
static size_t s_rx_acc_cap;

/* Worker queue: each item is a heap-allocated entityKey string we own. */
static QueueHandle_t s_work_q;

static void hex_byte(uint8_t b, char *out)
{
    static const char H[] = "0123456789abcdef";
    out[0] = H[(b >> 4) & 0xF];
    out[1] = H[b & 0xF];
}

static void hash_sig_topic0(const char *signature, char out[2 + 64 + 1])
{
    uint8_t h[32];
    arkiv_keccak256((const uint8_t *)signature, strlen(signature), h);
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < 32; ++i) hex_byte(h[i], &out[2 + 2 * i]);
    out[66] = '\0';
}

static void addr_to_topic(const uint8_t addr[20], char out[2 + 64 + 1])
{
    out[0] = '0';
    out[1] = 'x';
    /* 12 leading zero bytes (= 24 hex 0s) then 20-byte address. */
    for (size_t i = 0; i < 12; ++i) {
        out[2 + 2 * i]     = '0';
        out[2 + 2 * i + 1] = '0';
    }
    for (size_t i = 0; i < 20; ++i) hex_byte(addr[i], &out[2 + 2 * (12 + i)]);
    out[66] = '\0';
}

bool arkiv_ws_subscribed(void)
{
    /* The poll_task in arkiv_rpc.c calls this on every loop iteration to
     * pick its cadence — it starts BEFORE `arkiv_ws_start` runs (main.c
     * defers WS start to the heartbeat loop, after ARKIV_CLAIMED + owner
     * are known). So `s_lock` may legitimately be NULL here on a fresh
     * boot; treat that as "not subscribed" and fall through. */
    if (!s_lock) return false;
    bool v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_subscribed;
    xSemaphoreGive(s_lock);
    return v;
}

/* Build the eth_subscribe request body. Returns a heap string the caller
 * must free; NULL on OOM. */
static char *build_subscribe_body(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", 1);
    cJSON_AddStringToObject(root, "method", "eth_subscribe");

    cJSON *params = cJSON_CreateArray();
    cJSON_AddItemToArray(params, cJSON_CreateString("logs"));

    cJSON *filter = cJSON_CreateObject();
    cJSON_AddStringToObject(filter, "address", ARKIV_STORAGE_CONTRACT_HEX);

    cJSON *topics = cJSON_CreateArray();
    /* topic[0]: OR-array of event signatures. v1 = ArkivEntityCreated only. */
    cJSON *t0 = cJSON_CreateArray();
    cJSON_AddItemToArray(t0, cJSON_CreateString(s_topic0_created));
    cJSON_AddItemToArray(topics, t0);
    /* topic[1]: entityKey (any) — null means "match all". */
    cJSON_AddItemToArray(topics, cJSON_CreateNull());
    /* topic[2]: padded owner address. */
    cJSON_AddItemToArray(topics, cJSON_CreateString(s_owner_topic));
    cJSON_AddItemToObject(filter, "topics", topics);
    cJSON_AddItemToArray(params, filter);
    cJSON_AddItemToObject(root, "params", params);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

static char *build_unsubscribe_body(const char *sub_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", 2);
    cJSON_AddStringToObject(root, "method", "eth_unsubscribe");
    cJSON *params = cJSON_CreateArray();
    cJSON_AddItemToArray(params, cJSON_CreateString(sub_id));
    cJSON_AddItemToObject(root, "params", params);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Enqueue an observed entity key for the worker task to fetch + verify. */
static void enqueue_entity_key(const char *key)
{
    if (!s_work_q) return;
    char *copy = strdup(key);
    if (!copy) {
        ESP_LOGW(TAG, "OOM dropping notification (key=%s)", key);
        return;
    }
    if (xQueueSend(s_work_q, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "work queue full — dropping key=%s", key);
        free(copy);
    }
}

/* Dispatch a fully-assembled WS text frame. Either a reply to our
 * eth_subscribe (id=1) or an eth_subscription notification. */
static void dispatch_ws_message(const char *buf, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed (%u B)", (unsigned)len);
        return;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsNumber(id) && id->valueint == 1) {
        if (cJSON_IsString(result)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strncpy(s_sub_id, result->valuestring, sizeof(s_sub_id) - 1);
            s_sub_id[sizeof(s_sub_id) - 1] = '\0';
            s_subscribed = true;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "subscribed id=%s", result->valuestring);
        } else {
            cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
            cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
            ESP_LOGW(TAG, "eth_subscribe error: %s",
                     cJSON_IsString(msg) ? msg->valuestring : "(no message)");
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (cJSON_IsString(method) && strcmp(method->valuestring, "eth_subscription") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *sub = cJSON_GetObjectItemCaseSensitive(params, "subscription");
        cJSON *log = cJSON_GetObjectItemCaseSensitive(params, "result");
        if (cJSON_IsString(sub) && cJSON_IsObject(log)) {
            /* Verify the subscription_id matches ours — guards against
             * stale subs after a reconnect that's still flushing. */
            bool ours = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ours = strcmp(sub->valuestring, s_sub_id) == 0;
            xSemaphoreGive(s_lock);
            if (!ours) { cJSON_Delete(root); return; }

            cJSON *topics = cJSON_GetObjectItemCaseSensitive(log, "topics");
            if (cJSON_IsArray(topics) && cJSON_GetArraySize(topics) >= 2) {
                cJSON *t1 = cJSON_GetArrayItem(topics, 1);  /* entityKey */
                if (cJSON_IsString(t1)) {
                    enqueue_entity_key(t1->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "ws connected");
        /* Subscription is per-connection — every reconnect needs a fresh
         * eth_subscribe. Also trigger a catch-up HTTPS poll so we don't
         * miss events that landed while the socket was down. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_subscribed = false;
        s_sub_id[0] = '\0';
        xSemaphoreGive(s_lock);

        char *body = build_subscribe_body();
        if (body) {
            esp_websocket_client_send_text(s_client, body, strlen(body),
                                           pdMS_TO_TICKS(5000));
            free(body);
        }
        /* Catch-up: post a sentinel into the worker queue that triggers
         * a full `arkiv_query(filter)` once. Empty string = "do a full
         * poll" (cheap: a few KB once, not per-cycle). */
        char *catch = strdup("");
        if (catch && s_work_q && xQueueSend(s_work_q, &catch, 0) != pdTRUE) {
            free(catch);
        }
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (!ev) break;
        if (ev->op_code == 0x8 /* close */) break;
        if (ev->op_code != 0x1 /* text */ && ev->op_code != 0x0 /* cont */) break;

        if (ev->payload_offset == 0) s_rx_acc_len = 0;
        if (ev->data_len > 0) {
            if (s_rx_acc_len + (size_t)ev->data_len + 1 > s_rx_acc_cap) {
                ESP_LOGW(TAG, "rx buf overflow (%u+%d > %u) — dropping frame",
                         (unsigned)s_rx_acc_len, ev->data_len, (unsigned)s_rx_acc_cap);
                s_rx_acc_len = 0;
                break;
            }
            memcpy(s_rx_acc + s_rx_acc_len, ev->data_ptr, ev->data_len);
            s_rx_acc_len += ev->data_len;
            s_rx_acc[s_rx_acc_len] = '\0';
        }
        bool complete = (ev->payload_len > 0) &&
            ((size_t)(ev->payload_offset + ev->data_len) >= (size_t)ev->payload_len);
        if (complete && s_rx_acc_len > 0) {
            dispatch_ws_message(s_rx_acc, s_rx_acc_len);
            s_rx_acc_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ws disconnected (auto-reconnect armed)");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_subscribed = false;
        s_sub_id[0] = '\0';
        xSemaphoreGive(s_lock);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws error");
        break;
    default:
        break;
    }
}

/* Worker task — pulls observed entity keys off the queue and asks
 * arkiv_rpc to fetch + verify them. Empty-string sentinel = full catch-up
 * poll (`arkiv_query` by type + device_id, same body the legacy poll uses). */
static void ws_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        char *key = NULL;
        if (xQueueReceive(s_work_q, &key, portMAX_DELAY) != pdTRUE) continue;
        if (!key) continue;
        if (key[0] == '\0') {
            /* Catch-up sweep — same filter the legacy poll uses, runs
             * once after each (re)connect. */
            arkiv_rpc_poll_once();
        } else {
            arkiv_rpc_fetch_and_verify_by_key(key);
        }
        free(key);
    }
}

esp_err_t arkiv_ws_start(const uint8_t owner_addr[20])
{
    if (!owner_addr) return ESP_ERR_INVALID_ARG;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);

    if (!s_rx_acc) {
        s_rx_acc = (char *)malloc(ARKIV_WS_RX_BUF);
        if (!s_rx_acc) return ESP_ERR_NO_MEM;
        s_rx_acc_cap = ARKIV_WS_RX_BUF;
    }
    if (!s_work_q) {
        s_work_q = xQueueCreate(ARKIV_WS_WORK_QUEUE_LEN, sizeof(char *));
        if (!s_work_q) return ESP_ERR_NO_MEM;
        xTaskCreate(ws_worker_task, "arkiv_ws_wk", ARKIV_WS_WORK_TASK_STACK,
                    NULL, ARKIV_WS_TASK_PRIO, NULL);
    }

    hash_sig_topic0(EVT_SIG_CREATED, s_topic0_created);
    addr_to_topic(owner_addr, s_owner_topic);

    esp_websocket_client_config_t cfg = {0};
    cfg.uri                    = ARKIV_WS_URI;
    cfg.crt_bundle_attach      = esp_crt_bundle_attach;
    cfg.disable_auto_reconnect = false;
    cfg.reconnect_timeout_ms   = ARKIV_WS_RECONNECT_MS;
    cfg.network_timeout_ms     = 10000;
    cfg.ping_interval_sec      = ARKIV_WS_PING_SEC;
    cfg.pingpong_timeout_sec   = ARKIV_WS_PINGPONG_TIMEOUT_SEC;
    cfg.task_stack             = ARKIV_WS_TASK_STACK;
    cfg.task_prio              = ARKIV_WS_TASK_PRIO;
    cfg.buffer_size            = ARKIV_WS_RX_BUF;

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) return ESP_FAIL;
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  ws_event_handler, NULL);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Arkiv WS subscriber started (uri=%s)", ARKIV_WS_URI);
    return ESP_OK;
}

void arkiv_ws_stop(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_websocket_client_handle_t cl = s_client;
    char sub[sizeof(s_sub_id)];
    strncpy(sub, s_sub_id, sizeof(sub));
    s_running = false;
    s_subscribed = false;
    s_sub_id[0] = '\0';
    s_client = NULL;
    xSemaphoreGive(s_lock);

    if (cl) {
        if (sub[0] && esp_websocket_client_is_connected(cl)) {
            char *body = build_unsubscribe_body(sub);
            if (body) {
                esp_websocket_client_send_text(cl, body, strlen(body),
                                               pdMS_TO_TICKS(500));
                free(body);
            }
        }
        esp_websocket_client_close(cl, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(cl);
    }
}
