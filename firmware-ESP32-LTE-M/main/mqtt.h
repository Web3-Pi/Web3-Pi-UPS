#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "mqtt_health.h"
#include "mqtt_dispatch_queue.h"

/* Initialize before UART producers. Retry is safe after an allocation failure;
 * partial task creation is cleaned up before any task may enter the SDK. */
esp_err_t mqtt_runtime_init(void);
/* Fast idempotent request after PPP/identity readiness, NOT SDK completion. */
esp_err_t mqtt_client_start(void);
bool mqtt_sdk_is_started(void);

/* Copy opaque bytes without waiting for SDK/network. 0 = application accepted,
 * NOT msg_id, SDK admission, PUBACK or ingest. -1 = unavailable/invalid,
 * -2 = full, -3 = too large. Max topic 63 bytes, payload 256 bytes.
 * FIFO expires after 1 hour; known snapshots after 120 seconds. SDK outbox
 * expiry is separate. QoS/retain preserved, including empty retained payload. */
int mqtt_publish_raw(const char *topic, const void *payload, size_t payload_len,
                     int qos, int retain);
int mqtt_publish_critical(const char *topic, const void *payload, size_t payload_len,
                          int qos, int retain);
int mqtt_publish_snapshot(const char *topic, const void *payload, size_t payload_len,
                          int qos, int retain, uint32_t coalesce_key);
/* Eight concurrent receipts for SDK admission. Poll releases terminal results;
 * forgetting only releases the receipt, never cancels the queued message. */
typedef enum {
    MQTT_RECEIPT_UNKNOWN, MQTT_RECEIPT_PENDING,
    MQTT_RECEIPT_SDK_ACCEPTED, MQTT_RECEIPT_FAILED
} mqtt_receipt_status_t;
int mqtt_publish_tracked(const char *topic, const void *payload, size_t payload_len,
                         int qos, int retain, uint64_t *receipt);
mqtt_receipt_status_t mqtt_receipt_take(uint64_t receipt);
void mqtt_receipt_forget(uint64_t receipt);

/* Copies a fresh existing binary net.status, even before topics are ready.
 * A selected copy becomes a QoS1 probe, independent of snapshot coalescing. */
void mqtt_publish_net_status(const void *frame, size_t len);
bool mqtt_is_connected(void);
uint32_t mqtt_last_connected_s(void);
bool mqtt_auth_refused(void);
uint32_t mqtt_auth_refusals(void);
uint32_t mqtt_connect_fail_streak(void);
void mqtt_get_health(mqtt_health_snapshot_t *snapshot);
bool mqtt_publication_proof_fresh(void);
/* Every transfer transition invalidates proof; never alters rollback time. */
void mqtt_ota_state_changed(bool active);

typedef struct {
    mqtt_health_snapshot_t health;
    mqtt_dispatch_stats_t queue;
    uint64_t sdk_accepted, sdk_rejected, last_submit_ms, last_sdk_accept_ms;
    uint32_t outbox_bytes, commands_rejected, commands_stale;
} mqtt_diagnostics_t;
void mqtt_get_diagnostics(mqtt_diagnostics_t *snapshot);

/* Separate command worker, NEVER SDK task or outbound owner. Only complete
 * copied messages, FIFO within a generation. The handler still authenticates
 * commands and checks replay before execution. Borrowed data lives this call. */
typedef void (*mqtt_data_cb_t)(const char *topic, size_t topic_len,
                              const void *payload, size_t payload_len);
void mqtt_set_data_handler(mqtt_data_cb_t cb);
/* Immutable after publication; empty until identity is ready. */
const char *mqtt_topic_telemetry(void);
const char *mqtt_topic_event(void);
const char *mqtt_topic_cmd_response(void);
const char *mqtt_topic_cmd_request(void);
