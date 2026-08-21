#include "mqtt.h"
#include "identity.h"
#include "modem.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "endpoints.h"

#define TAG "mqtt"

/* Topic buffers — sized for a 20-digit ICCID. The longest subtopic suffix is
 * "/cmd/response" (13 chars), plus the "t/" prefix and 20-char ICCID gives
 * 35 chars + NUL. 48 leaves room for spec drift. */
#define TOPIC_BUF_LEN 48

static esp_mqtt_client_handle_t s_client;

/* MISC-9: link state drives publish-vs-enqueue in mqtt_publish_raw(). Set
 * from the esp-mqtt event task; read from the wups_link task. */
static volatile bool s_connected;

/* esp_timer second of the last CONNECTED event — uplink-health telemetry for
 * the modem's post-PPP watchdog (u32 so the cross-task read is atomic). */
static volatile uint32_t s_last_connected_s;

/* 0.8.7 — consecutive CONNACK auth refusals (rc=4 bad credentials / rc=5
 * not authorized). See mqtt.h. Written from the esp-mqtt event task, read
 * from the modem supervisor. */
#define AUTH_REFUSED_LATCH 3
static volatile uint32_t s_auth_refusals;

/* 0.8.7 — consecutive failed connect attempts of ANY kind (auth refusal or
 * transport error), cleared only by a successful CONNECT. Drives the same
 * 30→120 s backoff for a broker that is down/half-up: those attempts cost a
 * full TLS handshake each, exactly like refused ones. */
static volatile uint32_t s_conn_fail_streak;

static char s_topic_status[TOPIC_BUF_LEN];     /* t/{iccid}/status     (LWT + online retained) */
static char s_topic_identify[TOPIC_BUF_LEN];   /* t/{iccid}/identify   (retained on connect) */
static char s_topic_telemetry[TOPIC_BUF_LEN];  /* t/{iccid}/telemetry  (per-frame uplink) */
static char s_topic_event[TOPIC_BUF_LEN];      /* t/{iccid}/event      (state-change uplink) */
static char s_topic_cmd_resp[TOPIC_BUF_LEN];   /* t/{iccid}/cmd/response */
static char s_topic_cmd_req[TOPIC_BUF_LEN];    /* c/{iccid}/cmd/request — downlink */

/* LWT bytes — must outlive esp_mqtt_client_init() since the config struct
 * stores pointers, not copies. */
static const char k_lwt_offline[] = "{\"online\":false}";
static const char k_status_online[] = "{\"online\":true}";

static mqtt_data_cb_t s_data_handler;

/* Allow callers (e.g. wups_link) to know our topic strings so they can publish
 * raw bytes onto telemetry / event / cmd_response without hardcoding the
 * format. */
const char *mqtt_topic_telemetry(void)   { return s_topic_telemetry; }
const char *mqtt_topic_event(void)       { return s_topic_event; }
const char *mqtt_topic_cmd_response(void){ return s_topic_cmd_resp; }
const char *mqtt_topic_cmd_request(void) { return s_topic_cmd_req; }

/* Uplink-health accessors — see mqtt.h. */
bool mqtt_is_connected(void)         { return s_connected; }
uint32_t mqtt_last_connected_s(void) { return s_last_connected_s; }
bool mqtt_auth_refused(void)         { return s_auth_refusals >= AUTH_REFUSED_LATCH; }
uint32_t mqtt_auth_refusals(void)    { return s_auth_refusals; }
uint32_t mqtt_connect_fail_streak(void) { return s_conn_fail_streak; }

esp_err_t mqtt_client_request_reconnect(void)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    return esp_mqtt_client_reconnect(s_client);
}

/* Last-resort revive for a dead esp-mqtt task (transport re-alloc OOM makes
 * the task self-delete; reconnect() then fails forever). esp_mqtt_client_
 * start() is component-legal from INIT/DISCONNECTED — i.e. exactly when the
 * task is actually gone — and refuses (harmlessly) otherwise. */
esp_err_t mqtt_client_revive(void)
{
    if (!s_client) return ESP_ERR_INVALID_STATE;
    return esp_mqtt_client_start(s_client);
}

static void publish_identify(void)
{
    /* {"imei":"...","fw":"...","hw":"..."} — retained, QoS 1.
     * Backend uses this for anti-clone IMEI lock (ADR-0002) and to populate
     * devices.fwVersionEsp32 / devices.hwRev. */
    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"imei\":\"%s\",\"fw\":\"%s\",\"hw\":\"%s\"}",
                     identity_imei(),
                     identity_fw_version(),
                     identity_hw_version());
    if (n < 0 || (size_t)n >= sizeof(body)) {
        ESP_LOGW(TAG, "identify JSON truncated, skipping publish");
        return;
    }
    int rc = esp_mqtt_client_publish(s_client, s_topic_identify, body, n,
                                     /*qos=*/1, /*retain=*/1);
    ESP_LOGI(TAG, "identify published rc=%d body=%s", rc, body);
}

static void log_event(int32_t event_id, esp_mqtt_event_handle_t evt)
{
    switch (event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "BEFORE_CONNECT");
        break;
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_auth_refusals = 0;
        s_conn_fail_streak = 0;
        s_last_connected_s = (uint32_t)(esp_timer_get_time() / 1000000);
        ESP_LOGI(TAG, "CONNECTED to %s as %s", MQTT_BROKER_URI, identity_iccid());

        /* Subscribe to the per-device downlink command topic. The broker
         * ACL (once strict mode is on) only allows the device to subscribe
         * to its own c/{iccid}/# subtree. */
        esp_mqtt_client_subscribe(s_client, s_topic_cmd_req, 1);
        ESP_LOGI(TAG, "subscribed: %s (qos1)", s_topic_cmd_req);

        /* Status retained "online": broker remembers we're up so a late
         * subscriber doesn't have to wait for the next message. */
        esp_mqtt_client_publish(s_client, s_topic_status,
                                k_status_online, sizeof(k_status_online) - 1,
                                /*qos=*/1, /*retain=*/1);

        /* Identify retained: IMEI/fw/hw triple for the anti-clone check. */
        publish_identify();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "DISCONNECTED");
        /* Wake the modem supervisor so the first reconnect attempt lands
         * ~10 s after the drop (matching the component's old timer), not
         * up to a full 30 s supervise tick later. */
        modem_notify_mqtt_down();
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "SUBSCRIBED msg_id=%d", evt->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "UNSUBSCRIBED msg_id=%d", evt->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "PUBLISHED msg_id=%d", evt->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "DATA topic=%.*s len=%d",
                 evt->topic_len, evt->topic, evt->data_len);
        if (s_data_handler) {
            s_data_handler(evt->topic, (size_t)evt->topic_len,
                           evt->data, (size_t)evt->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (evt->error_handle) {
            ESP_LOGE(TAG, "ERROR: type=%d connect_return_code=%d "
                          "esp_tls_last_err=0x%x esp_tls_stack_err=0x%x sock_errno=%d",
                     evt->error_handle->error_type,
                     evt->error_handle->connect_return_code,
                     evt->error_handle->esp_tls_last_esp_err,
                     evt->error_handle->esp_tls_stack_err,
                     evt->error_handle->esp_transport_sock_errno);
            /* 0.8.7 auth-refusal latch: a CONNACK refusal for credentials
             * (rc=4/5) proves TCP+TLS worked end to end — count it. Any
             * transport-level error means the network itself misbehaved,
             * which invalidates that proof — reset the streak. Other
             * refusal codes (protocol/id/server-unavailable) are neither:
             * leave the streak alone. */
            if (evt->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (evt->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                 evt->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)) {
                s_auth_refusals++;
                if (s_auth_refusals == AUTH_REFUSED_LATCH) {
                    ESP_LOGW(TAG, "broker keeps refusing our credentials (×%u) — "
                                  "unclaimed unit or rotated secret; network itself is fine",
                             (unsigned)s_auth_refusals);
                }
            } else if (evt->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                s_auth_refusals = 0;
            }
            if (evt->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED ||
                evt->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                if (!s_connected) s_conn_fail_streak++;
            }
        } else {
            ESP_LOGE(TAG, "ERROR (no error_handle)");
        }
        break;

    default:
        ESP_LOGI(TAG, "event id=%ld", event_id);
        break;
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    log_event(event_id, (esp_mqtt_event_handle_t)event_data);
}

esp_err_t mqtt_client_start(void)
{
    /* Re-entrant for the supervisor's failed-start retry (0.8.7): if init
     * already succeeded but start failed, just try starting again — never
     * re-init (that would leak the old handle and its registration). */
    if (s_client) {
        return esp_mqtt_client_start(s_client);
    }
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') {
        ESP_LOGE(TAG, "no ICCID — refusing to start MQTT");
        return ESP_ERR_INVALID_STATE;
    }
    const char *password = identity_mqtt_password_hex();
    if (!password || password[0] == '\0') {
        ESP_LOGE(TAG, "no MQTT password derived");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build all topic strings up-front. ADR-0004 split:
     *   t/{iccid}/...   uplink (device publishes, backend subscribes)
     *   c/{iccid}/...   downlink (backend publishes, device subscribes) */
    snprintf(s_topic_status,    sizeof s_topic_status,    "t/%s/status",       iccid);
    snprintf(s_topic_identify,  sizeof s_topic_identify,  "t/%s/identify",     iccid);
    snprintf(s_topic_telemetry, sizeof s_topic_telemetry, "t/%s/telemetry",    iccid);
    snprintf(s_topic_event,     sizeof s_topic_event,     "t/%s/event",        iccid);
    snprintf(s_topic_cmd_resp,  sizeof s_topic_cmd_resp,  "t/%s/cmd/response", iccid);
    snprintf(s_topic_cmd_req,   sizeof s_topic_cmd_req,   "c/%s/cmd/request",  iccid);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        /* TLS via the bundled root CA list — covers Let's Encrypt. */
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,

        /* Per-device credentials (Track 0 / WS-10; ADR-0005 superseded):
         * username = ICCID, password = the per-device secret read from the
         * `prov` NVS partition (see identity.c). Client ID = ICCID too so
         * two boots of the same device cleanly displace each other. */
        .credentials.client_id = iccid,
        .credentials.username  = iccid,
        .credentials.authentication.password = password,

        .session.last_will = {
            .topic   = s_topic_status,
            .msg     = k_lwt_offline,
            .msg_len = sizeof(k_lwt_offline) - 1,
            .qos     = 1,
            .retain  = 1,
        },
        .session.keepalive   = 60,
        .network.timeout_ms  = 15000,
        /* 0.8.7: the modem supervisor owns the retry schedule (normal 10 s
         * cadence, 30→120 s backoff while the broker refuses credentials —
         * each attempt is a full TLS handshake, ~40 MB/day at the built-in
         * fixed 10 s timer). After a failure the client idles in
         * WAIT_RECONNECT until mqtt_client_request_reconnect(). */
        .network.disable_auto_reconnect = true,
        /* MISC-9: bound the RAM outbox that buffers uplinks across LTE/MQTT
         * outages (see mqtt_publish_raw). WUPS frames are tens of bytes, so
         * 32 KB holds several hundred parked messages; when full, enqueue
         * returns -2 and the frame is dropped (bounded memory wins). */
        .outbox.limit        = 32 * 1024,
        /* 12 KB for mbedTLS X509 chain validation headroom (default 6 KB
         * triggers stack-corruption errors during the LE handshake). */
        .task.stack_size     = 12 * 1024,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_event failed: %s", esp_err_to_name(err));
        /* Keep the re-entrant guard's invariant: s_client non-NULL implies
         * init AND handler registration succeeded. A handler-less client
         * that later connects would be invisible to the whole firmware
         * (s_connected never set, no subscribe) — destroy and let the
         * supervisor's retry rebuild from scratch. */
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "starting iccid=%s broker=%s", iccid, MQTT_BROKER_URI);
    return esp_mqtt_client_start(s_client);
}

int mqtt_publish_raw(const char *topic, const void *payload, size_t payload_len,
                     int qos, int retain)
{
    if (!s_client) {
        return -1;
    }
    if (s_connected) {
        int rc = esp_mqtt_client_publish(s_client, topic,
                                         (const char *)payload, (int)payload_len,
                                         qos, retain);
        if (rc >= 0) {
            return rc;
        }
        /* Race: DISCONNECTED landed between the flag check and the publish —
         * fall through and park the frame in the outbox instead. */
    }
    /* MISC-9: link down — enqueue into the esp-mqtt RAM outbox (store=true)
     * so the client flushes it on reconnect instead of dropping. Bounded by
     * .outbox.limit; entries expire after CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS.
     * Covers telemetry/events/cmd-responses generated during LTE dips. */
    int rc = esp_mqtt_client_enqueue(s_client, topic,
                                     (const char *)payload, (int)payload_len,
                                     qos, retain, /*store=*/true);
    if (rc >= 0) {
        ESP_LOGI(TAG, "link down — parked in outbox: %s len=%u (msg_id=%d)",
                 topic, (unsigned)payload_len, rc);
    } else {
        ESP_LOGW(TAG, "outbox enqueue failed rc=%d (%s): %s len=%u",
                 rc, rc == -2 ? "outbox full" : "error",
                 topic, (unsigned)payload_len);
    }
    return rc;
}

void mqtt_set_data_handler(mqtt_data_cb_t cb)
{
    s_data_handler = cb;
}
