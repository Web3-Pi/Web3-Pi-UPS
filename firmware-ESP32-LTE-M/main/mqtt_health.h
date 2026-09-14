#ifndef WUPS_MQTT_HEALTH_H
#define WUPS_MQTT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

/* Portable state machine; no heap, SDK calls or internal locking. The caller
 * serializes ALL access, including poll(), with a short firmware lock. Times
 * are monotonic milliseconds since boot. A stale supplied time is clamped to
 * the most recent observation, so a delayed callback cannot backdate proof.
 *
 * Tokens identify logical probes only. The adapter MUST resolve MQTT packet
 * ID reuse and early PUBACK ordering before calling probe_ack(). */
typedef struct {
    uint64_t generation;
    uint64_t sequence;
} mqtt_health_token_t;

typedef struct {
    uint64_t probe_interval_ms;
    uint64_t probe_deadline_ms;
    uint64_t worker_stall_ms;
} mqtt_health_config_t;

typedef enum {
    MQTT_HEALTH_FAILURE_NONE = 0,
    MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT,
    MQTT_HEALTH_FAILURE_ACK_TIMEOUT,
    MQTT_HEALTH_FAILURE_PROBE_FAILED,
    MQTT_HEALTH_FAILURE_EVENT_LOSS,
} mqtt_health_failure_t;

typedef struct {
    bool connected;
    bool ota_active;
    bool probe_due;
    bool probe_pending;
    bool probe_admitted;
    bool proof_fresh;
    bool degraded;               /* Initial grace is not degraded. */
    bool worker_busy;
    bool worker_stalled;         /* Diagnostic only; never requests a reset. */
    mqtt_health_failure_t failure;
    uint64_t generation;
    uint64_t next_probe_due_ms;
    uint64_t probe_deadline_ms;
    uint64_t last_ack_ms;        /* Historical only; consult proof_fresh. */
    uint64_t worker_checkpoint_ms;
    mqtt_health_token_t pending_token;
} mqtt_health_snapshot_t;

/* Public for static allocation. Fields are private to mqtt_health.c. */
typedef struct {
    mqtt_health_config_t config;
    uint64_t now_ms;
    uint64_t generation;
    uint64_t sequence;
    uint64_t next_probe_due_ms;
    uint64_t probe_deadline_ms;
    uint64_t last_ack_ms;
    uint64_t worker_checkpoint_ms;
    mqtt_health_token_t pending_token;
    mqtt_health_failure_t failure;
    bool connected;
    bool ota_active;
    bool probe_pending;
    bool probe_admitted;
    bool proof_valid;
    bool worker_busy;
} mqtt_health_t;

mqtt_health_config_t mqtt_health_default_config(void);
/* NULL config selects 120 s interval, 60 s deadline and 30 s worker timeout.
 * Returns false for zero durations or interval shorter than deadline. */
bool mqtt_health_init(mqtt_health_t *health, uint64_t now_ms,
                      const mqtt_health_config_t *config);
void mqtt_health_on_connected(mqtt_health_t *health, uint64_t now_ms);
void mqtt_health_on_disconnected(mqtt_health_t *health, uint64_t now_ms);

/* poll() alone expires a planned probe even if begin_probe() is never called.
 * Expiry is exclusive: now >= deadline is too late for admission or ACK.
 * A failed window schedules at most one new window, no earlier than the old
 * due + interval (or now if monitoring was delayed); no catch-up burst. */
void mqtt_health_poll(mqtt_health_t *health, uint64_t now_ms,
                      mqtt_health_snapshot_t *snapshot);
bool mqtt_health_begin_probe(mqtt_health_t *health, uint64_t now_ms,
                             mqtt_health_token_t *token);
bool mqtt_health_probe_admitted(mqtt_health_t *health,
                                mqtt_health_token_t token, uint64_t now_ms);
bool mqtt_health_probe_ack(mqtt_health_t *health, mqtt_health_token_t token,
                           uint64_t now_ms);
bool mqtt_health_probe_failed(mqtt_health_t *health, mqtt_health_token_t token,
                              uint64_t now_ms);
/* Lost correlation events invalidate proof and any active logical probe. */
void mqtt_health_on_event_loss(mqtt_health_t *health, uint64_t now_ms);

/* OTA transfer pauses probe scheduling, not the boot/rollback clock. Entering
 * it invalidates proof; resuming schedules an immediate new probe. This module
 * never confirms an image and never changes OTA rollback state. */
void mqtt_health_set_ota_active(mqtt_health_t *health, uint64_t now_ms,
                                bool active);

/* Call checkpoints only for actual executor progress, never a timer tick.
 * Repeating begin while busy does not renew the checkpoint. Idle is healthy
 * regardless of duration; a deliberate quiet producer is not supervised here. */
void mqtt_health_worker_begin(mqtt_health_t *health, uint64_t now_ms);
void mqtt_health_worker_checkpoint(mqtt_health_t *health, uint64_t now_ms);
void mqtt_health_worker_end(mqtt_health_t *health, uint64_t now_ms);

#endif
