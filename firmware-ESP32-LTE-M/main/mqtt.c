#include "mqtt.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "secrets.h"

#define TAG "mqtt"

#define MQTT_TOPIC_TEST    "web3piups/test"

static esp_mqtt_client_handle_t s_client;
static char s_client_id[32];     /* "ups-XXXXXXXXXXXX" — last 6 bytes of MAC */
static char s_topic_status[64];  /* "web3piups/<id>/status" */
static char s_topic_cmd[64];     /* "web3piups/<id>/cmd"    */

static void log_event(int32_t event_id, esp_mqtt_event_handle_t evt)
{
    switch (event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "BEFORE_CONNECT");
        break;
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "CONNECTED to %s as %s", MQTT_BROKER_URI, MQTT_USERNAME);
        /* Subscribe to our command topic and to the shared test topic
         * (so we'll see our own publish echoed back). */
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_TEST, 0);
        ESP_LOGI(TAG, "subscribed: %s (qos1), %s (qos0)", s_topic_cmd, MQTT_TOPIC_TEST);

        /* Hello-world publish to the test topic. Useful as a "first byte
         * across" proof, plus we'll see it echoed back via the subscribe. */
        char hello[160];
        int n = snprintf(hello, sizeof(hello),
                         "{\"client\":\"%s\",\"msg\":\"hello over LTE-M PPP\"}",
                         s_client_id);
        esp_mqtt_client_publish(s_client, MQTT_TOPIC_TEST, hello, n, 0, 0);

        /* Status retained on the device topic so the broker remembers we
         * came online even after we disconnect briefly. */
        char status[96];
        n = snprintf(status, sizeof(status),
                     "{\"client\":\"%s\",\"online\":true}",
                     s_client_id);
        esp_mqtt_client_publish(s_client, s_topic_status, status, n, 1, 1);
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
        ESP_LOGI(TAG, "PUBLISHED msg_id=%d", evt->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "DATA topic=%.*s len=%d payload=%.*s",
                 evt->topic_len, evt->topic, evt->data_len,
                 evt->data_len, evt->data);
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
    /* Crank up TLS / MQTT logs so we can see exactly what handshake step
     * is failing (cert chain, SNI, hostname, …). Crank back to INFO once
     * we have a stable connection. */
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    /* Build a stable client ID from the MAC: "ups-aabbccddeeff". Brokers
     * disconnect old sessions when they see the same client ID, so a
     * device-unique ID prevents two boots from fighting each other. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id),
             "ups-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_topic_status, sizeof(s_topic_status),
             "web3piups/%s/status", s_client_id);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd),
             "web3piups/%s/cmd", s_client_id);

    /* LWT: if we go offline ungracefully, broker tells subscribers we're
     * down. Retained so anyone who reconnects sees the latest state. */
    char lwt[96];
    int lwt_len = snprintf(lwt, sizeof(lwt),
                           "{\"client\":\"%s\",\"online\":false}",
                           s_client_id);

    /*
     * TODO(security): re-enable TLS verification once broker.w3p.ovh ships
     * a real Let's Encrypt cert (currently EMQX presents its bundled
     * self-signed cert with CN=localhost). For now we accept ANY cert from
     * the broker — connection is still encrypted, but vulnerable to MITM.
     * Tracked in TODO.md ("MQTT TLS verification disabled").
     *
     * To restore: re-enable .broker.verification.crt_bundle_attach below
     * and remove .broker.verification.skip_cert_common_name_check.
     */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        /* .broker.verification.crt_bundle_attach = esp_crt_bundle_attach, */
        .broker.verification.skip_cert_common_name_check = true,
        .credentials.username = MQTT_USERNAME,
        .credentials.client_id = s_client_id,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = lwt,
            .msg_len = lwt_len,
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 60,
        .network.timeout_ms = 15000,
        /* Default MQTT task stack is 6 KB, which is too tight for mbedTLS
         * to do a Let's Encrypt handshake — it triggers stack-corruption
         * errors that surface as MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000).
         * 12 KB gives the X509 chain validation comfortable headroom. */
        .task.stack_size = 12 * 1024,
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

    ESP_LOGI(TAG, "starting client_id=%s broker=%s", s_client_id, MQTT_BROKER_URI);
    return esp_mqtt_client_start(s_client);
}
