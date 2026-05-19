#include "mqtt.h"
#include "identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "secrets.h"

#define TAG "mqtt"

/* Topic buffers — sized for a 20-digit ICCID. The longest subtopic suffix is
 * "/cmd/response" (13 chars), plus the "t/" prefix and 20-char ICCID gives
 * 35 chars + NUL. 48 leaves room for spec drift. */
#define TOPIC_BUF_LEN 48

static esp_mqtt_client_handle_t s_client;

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
        ESP_LOGW(TAG, "DISCONNECTED");
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
    return esp_mqtt_client_publish(s_client, topic,
                                   (const char *)payload, (int)payload_len,
                                   qos, retain);
}

void mqtt_set_data_handler(mqtt_data_cb_t cb)
{
    s_data_handler = cb;
}
