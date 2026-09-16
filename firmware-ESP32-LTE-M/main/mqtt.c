#include "mqtt.h"
#include "identity.h"
#include "modem.h"
#include "backend_mode.h"
#include "fw_ota.h"
#include "wups_link.h"
#include "mqtt_packet_guard.h"
#include "mqtt_sdk_adapter.h"
#include "endpoints.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <inttypes.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

#if !CONFIG_MQTT_MSG_ID_INCREMENTAL
#error "MQTT health correlation requires incremental packet IDs (pinned esp-mqtt 1.0.0)"
#endif

#define TAG "mqtt"
#define TOPIC_BUF_LEN 48
#define AUTH_REFUSED_LATCH 3
#define MQTT_RETRY_NORMAL_MS 10000u
#define MQTT_BACKOFF_MIN_MS 30000u
#define MQTT_BACKOFF_MAX_MS 120000u
#define MQTT_FAIL_STREAK_LATCH 6
#define MQTT_RECONN_REJECTS_MAX 20
#define MQTT_OUTBOX_LIMIT (32u * 1024u)
#define MQTT_OUTBOX_NORMAL_LIMIT (24u * 1024u)
#define MQTT_ITEM_TTL_MS 3600000u
#define MQTT_SNAPSHOT_TTL_MS 120000u
#define MQTT_STATUS_MAX_AGE_MS 90000u
#define MQTT_REFRESH_INTERVAL_MS 120000u
#define MQTT_COMMAND_CAPACITY 4
#define MQTT_COMMAND_BYTES 512
#define MQTT_RECEIPTS 8

/* LTE-M timing profile. The bounded client's PINGRESP deadline is half
 * keepalive, so 600 s advertises a 300 s response window. Retransmission
 * of an unacknowledged QoS publication is a separate, shorter timer. */
#define MQTT_DELIVERY_TIMEOUT_MS 300000u
#define MQTT_KEEPALIVE_S 600
#define MQTT_RETRANSMIT_TIMEOUT_MS 5000

/* This lock protects only bounded application state. NEVER call the SDK,
 * wait, log, or execute commands while holding it. Published client and topic
 * storage live until reboot. Only the owner uses the SDK handle. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic(esp_mqtt_client_handle_t) s_client;
static atomic_bool s_ready, s_enabled, s_started, s_topics_ready, s_connected;
static atomic_uint s_last_connected_s, s_auth_refusals, s_conn_fail_streak;
static atomic_bool s_last_failure_auth;
static TaskHandle_t s_owner_task, s_command_task, s_monitor_task;
static QueueHandle_t s_commands;
static mqtt_dispatch_queue_t s_queue;
static mqtt_health_t s_health;
static mqtt_packet_guard_t s_guard;
static mqtt_health_token_t s_probe_token;
static mqtt_diagnostics_t s_diag;
static mqtt_data_cb_t s_data_handler;
static bool s_ota_active; /* latched even during a runtime-allocation retry */
static uint64_t s_last_refresh_ms;
static uint8_t s_net_status[MQTT_DISPATCH_PAYLOAD_CAPACITY];
static size_t s_net_status_len;
static uint64_t s_net_status_ms;
static struct { uint64_t token; mqtt_receipt_status_t status; } s_receipts[MQTT_RECEIPTS];

typedef struct {
    uint64_t generation;
    size_t topic_len, data_len;
    char topic[201];
    uint8_t data[MQTT_COMMAND_BYTES];
} mqtt_command_t;

static char s_topic_status[TOPIC_BUF_LEN];
static char s_topic_identify[TOPIC_BUF_LEN];
static char s_topic_telemetry[TOPIC_BUF_LEN];
static char s_topic_event[TOPIC_BUF_LEN];
static char s_topic_cmd_resp[TOPIC_BUF_LEN];
static char s_topic_cmd_req[TOPIC_BUF_LEN];
static const char k_lwt_offline[] = "{\"online\":false}";
static const char k_status_online[] = "{\"online\":true}";

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static void wake_owner(void) { if (s_owner_task) xTaskNotifyGive(s_owner_task); }
const char *mqtt_topic_telemetry(void) { return atomic_load(&s_topics_ready) ? s_topic_telemetry : ""; }
const char *mqtt_topic_event(void) { return atomic_load(&s_topics_ready) ? s_topic_event : ""; }
const char *mqtt_topic_cmd_response(void) { return atomic_load(&s_topics_ready) ? s_topic_cmd_resp : ""; }
const char *mqtt_topic_cmd_request(void) { return atomic_load(&s_topics_ready) ? s_topic_cmd_req : ""; }
bool mqtt_is_connected(void) { return atomic_load(&s_connected); }
bool mqtt_sdk_is_started(void) { return atomic_load(&s_started); }
uint32_t mqtt_last_connected_s(void) { return atomic_load(&s_last_connected_s); }
bool mqtt_auth_refused(void) { return atomic_load(&s_auth_refusals) >= AUTH_REFUSED_LATCH; }
uint32_t mqtt_auth_refusals(void) { return atomic_load(&s_auth_refusals); }
uint32_t mqtt_connect_fail_streak(void) { return atomic_load(&s_conn_fail_streak); }

/* Called under s_lock, including completion of failed/expired receipts. */
static void receipt_finish(uint64_t token, mqtt_receipt_status_t status)
{
    for (unsigned i = 0; i < MQTT_RECEIPTS; ++i)
        if (s_receipts[i].token == token) s_receipts[i].status = status;
}

static int submit(const char *topic, const void *payload, size_t len,
                  int qos, int retain, bool critical, uint32_t key, uint64_t *receipt)
{
    if (receipt) *receipt = 0;
    if (!atomic_load(&s_ready) || backend_mode_get() != WUPS_BACKEND_MODE_MQTT ||
        retain < 0 || retain > 1) return -1;
    uint64_t now = now_ms();
    mqtt_dispatch_request_t request = {
        .topic = topic, .payload = payload, .payload_len = len, .qos = qos,
        .retain = retain, .critical = critical, .coalesce_key = key,
        .deadline_ms = now + (key ? MQTT_SNAPSHOT_TTL_MS : MQTT_ITEM_TTL_MS),
    };
    uint64_t token = 0, replaced = 0;
    int slot = -1;
    portENTER_CRITICAL(&s_lock);
    if (receipt) {
        for (unsigned i = 0; i < MQTT_RECEIPTS; ++i)
            if (!s_receipts[i].token) { slot = (int)i; break; }
        if (slot < 0) { portEXIT_CRITICAL(&s_lock); return -2; }
    }
    mqtt_dispatch_result_t rc = mqtt_dispatch_submit(&s_queue, &request, &token, &replaced);
    if (rc == MQTT_DISPATCH_ACCEPTED) {
        s_diag.last_submit_ms = now;
        if (replaced) receipt_finish(replaced, MQTT_RECEIPT_FAILED);
        if (receipt) {
            s_receipts[slot].token = token;
            s_receipts[slot].status = MQTT_RECEIPT_PENDING;
            *receipt = token;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (rc == MQTT_DISPATCH_ACCEPTED) { wake_owner(); return 0; }
    return rc == MQTT_DISPATCH_FULL ? -2 : rc == MQTT_DISPATCH_TOO_LARGE ? -3 : -1;
}

int mqtt_publish_raw(const char *topic, const void *payload, size_t len, int qos, int retain)
{ return submit(topic, payload, len, qos, retain, false, 0, NULL); }
int mqtt_publish_critical(const char *topic, const void *payload, size_t len, int qos, int retain)
{ return submit(topic, payload, len, qos, retain, true, 0, NULL); }
int mqtt_publish_snapshot(const char *topic, const void *payload, size_t len, int qos, int retain, uint32_t key)
{ return submit(topic, payload, len, qos, retain, false, key, NULL); }
int mqtt_publish_tracked(const char *topic, const void *payload, size_t len, int qos, int retain, uint64_t *receipt)
{ return receipt ? submit(topic, payload, len, qos, retain, true, 0, receipt) : -1; }

mqtt_receipt_status_t mqtt_receipt_take(uint64_t token)
{
    mqtt_receipt_status_t rc = MQTT_RECEIPT_UNKNOWN;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; token && i < MQTT_RECEIPTS; ++i) {
        if (s_receipts[i].token != token) continue;
        rc = s_receipts[i].status;
        if (rc != MQTT_RECEIPT_PENDING) s_receipts[i].token = 0;
        break;
    }
    portEXIT_CRITICAL(&s_lock);
    return rc;
}
void mqtt_receipt_forget(uint64_t token)
{
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < MQTT_RECEIPTS; ++i)
        if (s_receipts[i].token == token) s_receipts[i].token = 0;
    portEXIT_CRITICAL(&s_lock);
}

void mqtt_publish_net_status(const void *frame, size_t len)
{
    if (!atomic_load(&s_ready) || !frame || !len || len > sizeof(s_net_status)) return;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_net_status, frame, len);
    s_net_status_len = len;
    s_net_status_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);
    const char *topic = mqtt_topic_telemetry();
    if (topic[0]) (void)mqtt_publish_snapshot(topic, frame, len, 0, 0, 0xE0000301u);
    wake_owner();
}
void mqtt_set_data_handler(mqtt_data_cb_t cb)
{
    portENTER_CRITICAL(&s_lock);
    s_data_handler = cb;
    portEXIT_CRITICAL(&s_lock);
}
void mqtt_ota_state_changed(bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_ota_active = active;
    if (atomic_load(&s_ready)) {
        mqtt_health_set_ota_active(&s_health, now_ms(), active);
        mqtt_packet_guard_probe_retire(&s_guard);
    }
    portEXIT_CRITICAL(&s_lock);
    /* The OTA caller may hold its claim mux. No scheduler calls here; owner
     * observes this within its 100 ms poll, monitor within one second. */
}
void mqtt_get_health(mqtt_health_snapshot_t *out)
{
    if (!out) return;
    if (!atomic_load(&s_ready)) { memset(out, 0, sizeof(*out)); return; }
    portENTER_CRITICAL(&s_lock);
    mqtt_health_poll(&s_health, now_ms(), out);
    portEXIT_CRITICAL(&s_lock);
}
bool mqtt_recovery_try_commit(const mqtt_health_snapshot_t *expected,
                              bool (*commit)(void *), void *context)
{
    if (!expected || !commit) return false;
    portENTER_CRITICAL(&s_lock);
    mqtt_health_snapshot_t current = {0};
    if (atomic_load(&s_ready)) mqtt_health_poll(&s_health, now_ms(), &current);
    bool eligible = !s_ota_active && !mqtt_auth_refused() && !current.ota_active && !current.connected &&
        !current.proof_fresh && !current.worker_stalled &&
        current.generation == expected->generation &&
        current.last_ack_ms == expected->last_ack_ms;
    bool accepted = eligible && commit(context);
    portEXIT_CRITICAL(&s_lock);
    return accepted;
}
bool mqtt_publication_proof_fresh(void)
{
    if (!atomic_load(&s_ready) || fw_ota_in_progress()) return false;
    mqtt_health_snapshot_t state;
    mqtt_get_health(&state);
    return state.proof_fresh;
}
void mqtt_get_diagnostics(mqtt_diagnostics_t *out)
{
    if (!out) return;
    if (!atomic_load(&s_ready)) { memset(out, 0, sizeof(*out)); return; }
    portENTER_CRITICAL(&s_lock);
    *out = s_diag;
    mqtt_dispatch_stats(&s_queue, &out->queue);
    mqtt_health_poll(&s_health, now_ms(), &out->health);
    portEXIT_CRITICAL(&s_lock);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t evt = data;
    uint64_t now = now_ms();
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        portENTER_CRITICAL(&s_lock);
        mqtt_packet_guard_on_connected(&s_guard);
        mqtt_health_on_connected(&s_health, now);
        if (s_guard.unsafe) mqtt_health_on_event_loss(&s_health, now);
        atomic_store(&s_connected, true);
        atomic_store(&s_auth_refusals, 0);
        atomic_store(&s_conn_fail_streak, 0);
        atomic_store(&s_last_failure_auth, false);
        atomic_store(&s_last_connected_s, (unsigned)(now / 1000));
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "CONNECTED; bootstrap requested");
        wake_owner();
        break;
    case MQTT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_lock);
        atomic_store(&s_connected, false);
        mqtt_packet_guard_on_disconnected(&s_guard);
        mqtt_health_on_disconnected(&s_health, now);
        portEXIT_CRITICAL(&s_lock);
        modem_notify_mqtt_down();
        wake_owner();
        break;
    case MQTT_EVENT_PUBLISHED:
        portENTER_CRITICAL(&s_lock);
        if (mqtt_packet_guard_on_puback(&s_guard, s_guard.generation, evt->msg_id, now))
            (void)mqtt_health_probe_ack(&s_health, s_probe_token, now);
        else if (s_guard.probe_state == MQTT_PROBE_FAILED)
            (void)mqtt_health_probe_failed(&s_health, s_probe_token, now);
        portEXIT_CRITICAL(&s_lock);
        break;
    case MQTT_EVENT_DATA: {
        /* SDK fragments are never executed piecemeal. WAE1 authenticated
         * commands fit this bound; zero-copy pointers cannot escape callback. */
        mqtt_command_t cmd = {0};
        bool valid = evt->topic && evt->topic_len > 0 && evt->topic_len < (int)sizeof(cmd.topic) &&
            evt->data_len >= 0 && evt->data_len <= MQTT_COMMAND_BYTES &&
            (!evt->data_len || evt->data) && evt->current_data_offset == 0 &&
            evt->data_len == evt->total_data_len;
        if (valid) {
            cmd.topic_len = (size_t)evt->topic_len;
            cmd.data_len = (size_t)evt->data_len;
            memcpy(cmd.topic, evt->topic, cmd.topic_len);
            cmd.topic[cmd.topic_len] = 0;
            if (cmd.data_len) memcpy(cmd.data, evt->data, cmd.data_len);
            portENTER_CRITICAL(&s_lock);
            cmd.generation = s_guard.generation;
            portEXIT_CRITICAL(&s_lock);
            valid = xQueueSend(s_commands, &cmd, 0) == pdTRUE;
        }
        if (!valid) {
            portENTER_CRITICAL(&s_lock);
            ++s_diag.commands_rejected;
            portEXIT_CRITICAL(&s_lock);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (evt->error_handle) {
            portENTER_CRITICAL(&s_lock);
            int type = evt->error_handle->error_type;
            int rc = evt->error_handle->connect_return_code;
            atomic_store(&s_last_failure_auth,
                type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME || rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED));
            if (type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME || rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED))
                atomic_fetch_add(&s_auth_refusals, 1);
            else if (type == MQTT_ERROR_TYPE_TCP_TRANSPORT) atomic_store(&s_auth_refusals, 0);
            portEXIT_CRITICAL(&s_lock);
            if (!mqtt_is_connected() && (type == MQTT_ERROR_TYPE_TCP_TRANSPORT || type == MQTT_ERROR_TYPE_CONNECTION_REFUSED))
                atomic_fetch_add(&s_conn_fail_streak, 1);
            ESP_LOGW(TAG, "error type=%d connack=%d tls=0x%x socket=%d", type, rc,
                     evt->error_handle->esp_tls_last_esp_err, evt->error_handle->esp_transport_sock_errno);
        }
        break;
    default: break;
    }
}
static esp_err_t sdk_start(void)
{
    /* Re-entrant for the supervisor's failed-start retry (0.8.7): if init
     * already succeeded but start failed, just try starting again — never
     * re-init (that would leak the old handle and its registration). */
    esp_mqtt_client_handle_t client = atomic_load_explicit(&s_client, memory_order_acquire);
    if (client) {
        return esp_mqtt_client_start(client);
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
    if (!atomic_load(&s_topics_ready)) {
        snprintf(s_topic_status,    sizeof s_topic_status,    "t/%s/status",       iccid);
        snprintf(s_topic_identify,  sizeof s_topic_identify,  "t/%s/identify",     iccid);
        snprintf(s_topic_telemetry, sizeof s_topic_telemetry, "t/%s/telemetry",    iccid);
        snprintf(s_topic_event,     sizeof s_topic_event,     "t/%s/event",        iccid);
        snprintf(s_topic_cmd_resp,  sizeof s_topic_cmd_resp,  "t/%s/cmd/response", iccid);
        snprintf(s_topic_cmd_req,   sizeof s_topic_cmd_req,   "c/%s/cmd/request",  iccid);

        atomic_store_explicit(&s_topics_ready, true, memory_order_release);
    }

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
        .session.keepalive   = MQTT_KEEPALIVE_S,
        .session.message_retransmit_timeout = MQTT_RETRANSMIT_TIMEOUT_MS,
        .network.timeout_ms  = MQTT_DELIVERY_TIMEOUT_MS,
        /* Incremental TLS/MQTT I/O, with finite partial-frame deadlines and
         * RX service before keepalive decisions (issues #15/#16). */
        .network.bounded_service = true,
        /* 0.8.7: the SDK owner owns the retry schedule (normal 10 s
         * cadence, 30→120 s backoff while the broker refuses credentials —
         * each attempt is a full TLS handshake, ~40 MB/day at the built-in
         * fixed 10 s timer). After a failure the client idles in
         * WAIT_RECONNECT until the owner calls reconnect(). */
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

    client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_event failed: %s", esp_err_to_name(err));
        /* Keep the re-entrant guard's invariant: s_client non-NULL implies
         * init AND handler registration succeeded. A handler-less client
         * that later connects would be invisible to the whole firmware
         * (s_connected never set, no subscribe) — destroy and let the
         * supervisor's retry rebuild from scratch. */
        esp_mqtt_client_destroy(client);
        return err;
    }

    atomic_store_explicit(&s_client, client, memory_order_release);
    ESP_LOGI(TAG, "starting iccid=%s broker=%s", iccid, MQTT_BROKER_URI);
    return esp_mqtt_client_start(client);
}

/* SDK calls below are reachable ONLY from owner_task. Callback and producers
 * never take its mutex. A stuck call leaves worker_busy visible to monitor. */
static void sdk_begin(void)
{
    portENTER_CRITICAL(&s_lock);
    mqtt_health_worker_begin(&s_health, now_ms());
    portEXIT_CRITICAL(&s_lock);
}
static void sdk_end(void)
{
    portENTER_CRITICAL(&s_lock);
    mqtt_health_worker_end(&s_health, now_ms());
    portEXIT_CRITICAL(&s_lock);
}
static int sdk_outbox_bytes(esp_mqtt_client_handle_t client)
{
    sdk_begin();
    int n = esp_mqtt_client_get_outbox_size(client);
    sdk_end();
    portENTER_CRITICAL(&s_lock);
    s_diag.outbox_bytes = n < 0 ? MQTT_OUTBOX_LIMIT : (uint32_t)n;
    portEXIT_CRITICAL(&s_lock);
    return n;
}
static bool outbox_room(esp_mqtt_client_handle_t client, size_t len, const char *topic, bool critical)
{
    int used = sdk_outbox_bytes(client);
    /* Conservative full packet overhead, including topic length and packet ID.
     * One owner means no competing application admission between check/call. */
    size_t limit = critical ? MQTT_OUTBOX_LIMIT : MQTT_OUTBOX_NORMAL_LIMIT;
    return used >= 0 && (size_t)used + len + strlen(topic) + 16 <= limit;
}
static bool reserve_packet(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ok = mqtt_packet_guard_packet_begin(&s_guard);
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
static void complete_packet(int id)
{
    portENTER_CRITICAL(&s_lock);
    if (!mqtt_packet_guard_packet_result(&s_guard, id))
        mqtt_health_on_event_loss(&s_health, now_ms());
    portEXIT_CRITICAL(&s_lock);
}

static int sdk_enqueue(esp_mqtt_client_handle_t client, const char *topic,
                       const void *payload, size_t len, int qos, int retain,
                       bool critical, const mqtt_health_snapshot_t *probe)
{
    if (!outbox_room(client, len, topic, critical)) return -2;
    if (qos && !reserve_packet()) return -2;
    bool armed = true;
    if (probe) {
        portENTER_CRITICAL(&s_lock);
        armed = mqtt_health_begin_probe(&s_health, now_ms(), &s_probe_token) &&
            mqtt_packet_guard_probe_begin(&s_guard, s_probe_token.sequence,
                                           probe->next_probe_due_ms, MQTT_DELIVERY_TIMEOUT_MS);
        if (!armed) {
            (void)mqtt_health_probe_failed(&s_health, s_probe_token, now_ms());
            (void)mqtt_packet_guard_packet_result(&s_guard, -1);
        }
        portEXIT_CRITICAL(&s_lock);
        if (!armed) return -1;
    }
    sdk_begin();
    /* In esp-mqtt len==0 invokes strlen(data): use a known empty string,
     * never the uninitialized bytes beyond an empty queue payload. */
    int rc = esp_mqtt_client_enqueue(client, topic, len ? payload : "", (int)len,
                                     qos, retain, true);
    sdk_end();
    if (qos) complete_packet(rc);
    portENTER_CRITICAL(&s_lock);
    uint64_t now = now_ms();
    if (rc >= 0) {
        ++s_diag.sdk_accepted;
        s_diag.last_sdk_accept_ms = now;
    } else ++s_diag.sdk_rejected;
    if (probe) {
        if (rc >= 0) (void)mqtt_health_probe_admitted(&s_health, s_probe_token, now);
        if (mqtt_packet_guard_probe_result(&s_guard, s_probe_token.sequence, rc, now))
            (void)mqtt_health_probe_ack(&s_health, s_probe_token, now);
        else if (rc < 0 || s_guard.probe_state == MQTT_PROBE_FAILED)
            (void)mqtt_health_probe_failed(&s_health, s_probe_token, now);
    }
    portEXIT_CRITICAL(&s_lock);
    return rc;
}

/* Returns true while ordinary traffic must wait for a safe ID epoch. Never
 * clear an epoch just because MQTT reconnected: retained outbox IDs still
 * exist. Stop allocating, drain ALL entries, then cross a socket boundary. */
static bool service_packet_epoch(esp_mqtt_client_handle_t client)
{
    portENTER_CRITICAL(&s_lock);
    bool needed = s_guard.unsafe || s_guard.attempts >= MQTT_PACKET_GUARD_BUDGET || s_guard.reset_armed;
    bool armed = s_guard.reset_armed;
    portEXIT_CRITICAL(&s_lock);
    if (!needed) return false;
    if (!armed && sdk_outbox_bytes(client) == 0) {
        portENTER_CRITICAL(&s_lock);
        armed = mqtt_packet_guard_arm_reset(&s_guard, true);
        if (armed) mqtt_health_on_event_loss(&s_health, now_ms());
        portEXIT_CRITICAL(&s_lock);
    }
    return true;
}

/* Adapter calls this only after the old SDK task finished transport/outbox
 * cleanup. Run before start: a CONNECTED callback may precede its return. */
static void on_sdk_stopped(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    atomic_store(&s_connected, false);
    mqtt_packet_guard_on_disconnected(&s_guard);
    mqtt_health_on_disconnected(&s_health, now_ms());
    /* The pinned SDK exit path already emptied the outbox before STOPPED. */
    (void)mqtt_packet_guard_arm_reset(&s_guard, true);
    portEXIT_CRITICAL(&s_lock);
}

/* Coalesced control uses current health/epoch state plus idempotent enable.
 * Backoff belongs here and advances on actual SDK success, never submit. */
static void owner_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* runtime init commit barrier */
    uint64_t next_attempt = 0, bootstrap_generation = 0, next_bootstrap = 0;
    uint32_t backoff = MQTT_BACKOFF_MIN_MS;
    uint32_t ppp_generation = 0;
    bool auth_retry = false;
    unsigned rejects = 0, bootstrap_step = 0;
    for (;;) {
        uint64_t now = now_ms(), expired[MQTT_DISPATCH_CAPACITY];
        portENTER_CRITICAL(&s_lock);
        size_t n_expired = mqtt_dispatch_expire(&s_queue, now, expired);
        for (size_t i = 0; i < n_expired; ++i) receipt_finish(expired[i], MQTT_RECEIPT_FAILED);
        portEXIT_CRITICAL(&s_lock);
        uint32_t current_ppp = modem_ppp_generation();
        if (!atomic_load(&s_enabled) || !current_ppp) goto idle;
        if (current_ppp != ppp_generation) {
            ppp_generation = current_ppp;
            /* A new interface invalidates transport backoff, not a broker's
             * authentication refusal. Do not let repeated start wakeups or
             * repeated GOT_IP notifications bypass either policy. */
            if (!auth_retry && !atomic_load(&s_last_failure_auth)) {
                next_attempt = now;
                backoff = MQTT_BACKOFF_MIN_MS;
                rejects = 0;
                atomic_store(&s_conn_fail_streak, 0);
                ESP_LOGI(TAG, "new PPP generation=%" PRIu32 "; transport retry expedited", current_ppp);
            }
        }
        if (!atomic_load(&s_started)) {
            if (now < next_attempt) goto idle;
            sdk_begin();
            esp_err_t rc = sdk_start();
            sdk_end();
            if (rc == ESP_OK) atomic_store(&s_started, true);
            else ESP_LOGW(TAG, "SDK start failed: %s", esp_err_to_name(rc));
            next_attempt = now_ms() + MQTT_RETRY_NORMAL_MS;
            goto idle;
        }
        esp_mqtt_client_handle_t client = atomic_load_explicit(&s_client, memory_order_acquire);
        bool epoch_wait = service_packet_epoch(client);
        now = now_ms();
        portENTER_CRITICAL(&s_lock);
        mqtt_health_snapshot_t control_health;
        mqtt_health_poll(&s_health, now, &control_health);
        /* Re-evaluate health at execution, rather than replaying a stale
         * failure after a new probe/OTA transition restored the connection.
         * Packet epoch rotation is an independent mandatory reason. */
        bool refresh = (s_guard.reset_armed || control_health.degraded) &&
            control_health.connected && !control_health.ota_active &&
            (!s_last_refresh_ms || now - s_last_refresh_ms >= MQTT_REFRESH_INTERVAL_MS);
        portEXIT_CRITICAL(&s_lock);
        if (refresh && mqtt_is_connected() && !fw_ota_in_progress()) {
            /* Disconnect asks the SDK task to close the socket; it does not
             * stop/delete the task, purge outbox or destroy a live client. */
            s_last_refresh_ms = now_ms(); /* owner-only, actual attempt */
            sdk_begin();
            esp_err_t rc = esp_mqtt_client_disconnect(client);
            sdk_end();
            ESP_LOGW(TAG, "bounded MQTT refresh request: %s", esp_err_to_name(rc));
            next_attempt = now_ms() + MQTT_RETRY_NORMAL_MS;
            goto idle;
        }
        if (!mqtt_is_connected()) {
            if (now >= next_attempt) {
                sdk_begin();
                esp_err_t rc = esp_mqtt_client_reconnect(client);
                sdk_end();
                uint32_t delay = MQTT_RETRY_NORMAL_MS;
                if (rc == ESP_OK) {
                    rejects = 0;
                    if (mqtt_auth_refused() || mqtt_connect_fail_streak() >= MQTT_FAIL_STREAK_LATCH) {
                        delay = backoff;
                        backoff = backoff * 2 > MQTT_BACKOFF_MAX_MS ? MQTT_BACKOFF_MAX_MS : backoff * 2;
                    }
                } else if (++rejects >= MQTT_RECONN_REJECTS_MAX) {
                    sdk_begin();
                    esp_err_t revived = mqtt_sdk_revive_stopped(client, on_sdk_stopped, NULL);
                    sdk_end();
                    ESP_LOGW(TAG, "SDK revive after %u rejected reconnects: %s", rejects, esp_err_to_name(revived));
                    rejects = 0;
                }
                next_attempt = now_ms() + delay;
                auth_retry = atomic_load(&s_last_failure_auth);
            }
            goto idle;
        }
        backoff = MQTT_BACKOFF_MIN_MS;
        auth_retry = false;
        rejects = 0;
        next_attempt = now + MQTT_RETRY_NORMAL_MS;
        if (epoch_wait) goto idle;
        mqtt_health_snapshot_t health;
        mqtt_get_health(&health);
        if (bootstrap_generation != health.generation) {
            bootstrap_generation = health.generation;
            bootstrap_step = 0;
            next_bootstrap = 0;
        }
        if (bootstrap_step < 3) {
            if (now < next_bootstrap) goto idle;
            int rc = -2;
            if (bootstrap_step == 0) {
                if (outbox_room(client, 0, s_topic_cmd_req, true) && reserve_packet()) {
                    sdk_begin();
                    rc = esp_mqtt_client_subscribe(client, s_topic_cmd_req, 1);
                    sdk_end();
                    complete_packet(rc);
                }
            } else if (bootstrap_step == 1) {
                rc = sdk_enqueue(client, s_topic_status, k_status_online, sizeof(k_status_online)-1, 1, 1, true, NULL);
            } else {
                char body[160];
                int n = snprintf(body, sizeof(body), "{\"imei\":\"%s\",\"fw\":\"%s\",\"hw\":\"%s\"}",
                                  identity_imei(), identity_fw_version(), identity_hw_version());
                if (n > 0 && (size_t)n < sizeof(body))
                    rc = sdk_enqueue(client, s_topic_identify, body, (size_t)n, 1, 1, true, NULL);
            }
            if (rc >= 0) ++bootstrap_step;
            else next_bootstrap = now_ms() + 1000;
            goto idle;
        }
        /* An existing, recent status frame forms the probe. This cache is
         * independent of the app queue, so coalescing cannot retire a probe. */
        if (health.probe_due && !fw_ota_in_progress()) {
            uint8_t frame[MQTT_DISPATCH_PAYLOAD_CAPACITY];
            size_t len = 0;
            portENTER_CRITICAL(&s_lock);
            if (s_net_status_len && now >= s_net_status_ms && now - s_net_status_ms <= MQTT_STATUS_MAX_AGE_MS) {
                len = s_net_status_len;
                memcpy(frame, s_net_status, len);
            }
            portEXIT_CRITICAL(&s_lock);
            if (len) (void)sdk_enqueue(client, s_topic_telemetry, frame, len, 1, 0, true, &health);
        }
        mqtt_dispatch_claim_t claim;
        portENTER_CRITICAL(&s_lock);
        bool have = mqtt_dispatch_claim(&s_queue, now_ms(), &claim);
        portEXIT_CRITICAL(&s_lock);
        if (have) {
            int rc = sdk_enqueue(client, claim.item.topic, claim.item.payload, claim.item.payload_len,
                                 claim.item.qos, claim.item.retain, claim.item.critical, NULL);
            now = now_ms();
            portENTER_CRITICAL(&s_lock);
            (void)mqtt_dispatch_finish(&s_queue, &claim, rc >= 0 ? MQTT_DISPATCH_SENT : MQTT_DISPATCH_RETRY, now, now + 1000);
            if (rc >= 0) receipt_finish(claim.item.token, MQTT_RECEIPT_SDK_ACCEPTED);
            else if (claim.item.deadline_ms && now >= claim.item.deadline_ms)
                receipt_finish(claim.item.token, MQTT_RECEIPT_FAILED);
            portEXIT_CRITICAL(&s_lock);
        }
idle:
        /* Maximum ten application admissions/s without a producer wake-up;
         * retries have individual due times and never spin under outbox full. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

static void command_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    mqtt_command_t cmd;
    for (;;) {
        if (xQueueReceive(s_commands, &cmd, portMAX_DELAY) != pdTRUE) continue;
        portENTER_CRITICAL(&s_lock);
        bool current = s_guard.connected && cmd.generation == s_guard.generation;
        mqtt_data_cb_t cb = s_data_handler;
        if (!current) ++s_diag.commands_stale;
        portEXIT_CRITICAL(&s_lock);
        if (current && cb && backend_mode_get() == WUPS_BACKEND_MODE_MQTT)
            cb(cmd.topic, cmd.topic_len, cmd.data, cmd.data_len);
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint64_t last_log = 0;
    bool previous_stalled = false, previous_degraded = false;
    for (;;) {
        uint64_t now = now_ms(), expired[MQTT_DISPATCH_CAPACITY];
        portENTER_CRITICAL(&s_lock);
        mqtt_packet_guard_probe_expire(&s_guard, now);
        mqtt_health_snapshot_t health;
        mqtt_health_poll(&s_health, now, &health);
        size_t n = mqtt_dispatch_expire(&s_queue, now, expired);
        for (size_t i = 0; i < n; ++i) receipt_finish(expired[i], MQTT_RECEIPT_FAILED);
        portEXIT_CRITICAL(&s_lock);
        if (health.degraded || health.worker_stalled) wake_owner();
        if (!last_log || now - last_log >= 60000 || previous_stalled != health.worker_stalled || previous_degraded != health.degraded) {
            mqtt_diagnostics_t d;
            mqtt_get_diagnostics(&d);
            /* UART age is separate evidence, not proof that Orange or the
             * broker failed. Quiet/OTA sources never trigger a modem reset. */
            ESP_LOGI(TAG, "health connected=%d proof=%d degraded=%d cause=%d worker_stall=%d "
                     "queue=%u/%u full=%" PRIu64 " expired=%" PRIu64 " coalesced=%" PRIu64 " "
                     "sdk_ok=%" PRIu64 " sdk_fail=%" PRIu64 " outbox=%u cmd_drop=%u stale=%u uart_age_s=%u",
                     health.connected, health.proof_fresh, health.degraded, health.failure, health.worker_stalled,
                     (unsigned)d.queue.depth, MQTT_DISPATCH_CAPACITY, d.queue.full, d.queue.expired, d.queue.coalesced,
                     d.sdk_accepted, d.sdk_rejected, (unsigned)d.outbox_bytes,
                     (unsigned)d.commands_rejected, (unsigned)d.commands_stale, (unsigned)wups_link_frame_age_s());
            ESP_LOGI(TAG, "memory heap_free=%u heap_min=%u stack_free owner=%u cmd=%u monitor=%u",
                     (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)uxTaskGetStackHighWaterMark(s_owner_task),
                     (unsigned)uxTaskGetStackHighWaterMark(s_command_task),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            /* This client remains alive after publication in s_client. The
             * observation API only loads atomics, so a blocked SDK task does
             * not prevent this independent monitor from reporting its state. */
            esp_mqtt_client_handle_t client = atomic_load_explicit(&s_client, memory_order_acquire);
            esp_mqtt_service_status_t service;
            if (client && esp_mqtt_client_get_service_status(client, &service) == ESP_OK) {
                ESP_LOGI(TAG, "service op=%u lock_max_ms=%u slice_start=%u slice_end=%u progress=%u "
                         "rx_left_ms=%u tx_left_ms=%u tx_frames=%u tx_bytes=%u deadlines=%u",
                         (unsigned)service.operation, (unsigned)service.max_lock_ms,
                         (unsigned)service.slice_started_ms, (unsigned)service.slice_completed_ms,
                         (unsigned)service.last_progress_ms, (unsigned)service.rx_remaining_ms,
                         (unsigned)service.tx_remaining_ms, (unsigned)service.tx_frames,
                         (unsigned)service.tx_bytes, (unsigned)service.deadline_failures);
            }
            last_log = now;
            previous_stalled = health.worker_stalled;
            previous_degraded = health.degraded;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t mqtt_runtime_init(void)
{
    if (atomic_load(&s_ready)) return ESP_OK;
    /* Sole boot caller, before producers. Failed attempts leave no live task
     * past its notification barrier, and can be retried by the main loop. */
    mqtt_dispatch_init(&s_queue);
    mqtt_packet_guard_init(&s_guard);
    mqtt_health_config_t health_config = mqtt_health_default_config();
    /* The health state machine requires interval >= deadline. */
    health_config.probe_interval_ms = MQTT_DELIVERY_TIMEOUT_MS;
    health_config.probe_deadline_ms = MQTT_DELIVERY_TIMEOUT_MS;
    if (!mqtt_health_init(&s_health, now_ms(), &health_config)) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "MQTT timing: network_ms=%u keepalive_s=%u ping_response_ms=%u "
             "probe_interval_ms=%u probe_deadline_ms=%u retransmit_ms=%u",
             (unsigned)MQTT_DELIVERY_TIMEOUT_MS, (unsigned)MQTT_KEEPALIVE_S,
             (unsigned)(MQTT_KEEPALIVE_S * 500u),
             (unsigned)health_config.probe_interval_ms,
             (unsigned)health_config.probe_deadline_ms,
             (unsigned)MQTT_RETRANSMIT_TIMEOUT_MS);
    s_commands = xQueueCreate(MQTT_COMMAND_CAPACITY, sizeof(mqtt_command_t));
    if (!s_commands) return ESP_ERR_NO_MEM;
    if (xTaskCreate(owner_task, "mqtt_owner", 6144, NULL, 5, &s_owner_task) != pdPASS ||
        xTaskCreate(command_task, "mqtt_commands", 8192, NULL, 5, &s_command_task) != pdPASS ||
        xTaskCreate(monitor_task, "mqtt_health", 3072, NULL, 5, &s_monitor_task) != pdPASS) {
        if (s_owner_task) vTaskDelete(s_owner_task);
        if (s_command_task) vTaskDelete(s_command_task);
        if (s_monitor_task) vTaskDelete(s_monitor_task);
        s_owner_task = s_command_task = s_monitor_task = NULL;
        vQueueDelete(s_commands);
        s_commands = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    mqtt_health_set_ota_active(&s_health, now_ms(), s_ota_active);
    atomic_store(&s_ready, true);
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_owner_task);
    xTaskNotifyGive(s_command_task);
    xTaskNotifyGive(s_monitor_task);
    return ESP_OK;
}
esp_err_t mqtt_client_start(void)
{
    if (!atomic_load(&s_ready) || backend_mode_get() != WUPS_BACKEND_MODE_MQTT)
        return ESP_ERR_INVALID_STATE;
    atomic_store(&s_enabled, true);
    wake_owner();
    return ESP_OK;
}
