#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pure state machine: caller serializes every access with a short application
 * lock. Never hold that lock across an MQTT SDK call.
 *
 * Contract: CONFIG_MQTT_MSG_ID_INCREMENTAL=y, clean MQTT sessions, and one SDK
 * owner. Every outgoing QoS>0 enqueue/subscribe/unsubscribe attempt reserves a
 * packet before entering the SDK and reports its result afterward. Valid
 * inbound packet acknowledgements and SDK retransmissions allocate no new ID.
 * A client must not be replaced without reinitializing this guard.
 */
#define MQTT_PACKET_GUARD_BUDGET 60000u
#define MQTT_PACKET_GUARD_EARLY_ACKS 8u

typedef enum {
    MQTT_PROBE_IDLE,
    MQTT_PROBE_ARMING,
    MQTT_PROBE_PENDING,
    MQTT_PROBE_ACKED,
    MQTT_PROBE_RETIRED,
    MQTT_PROBE_FAILED,
    MQTT_PROBE_EXPIRED,
} mqtt_packet_probe_state_t;

typedef struct {
    uint16_t packet_id;
    uint64_t received_ms;
} mqtt_packet_early_ack_t;

typedef struct {
    uint64_t generation;
    uint32_t attempts;
    uint32_t attempts_since_id;
    uint64_t operation_serial;
    uint64_t completed_operation_serial;
    int completed_packet_id;
    uint16_t last_packet_id;
    bool have_last_packet_id;
    bool connected;
    bool operation_pending;
    bool unsafe;
    bool reset_armed;
    bool reset_boundary_seen;

    mqtt_packet_probe_state_t probe_state;
    uint64_t probe_token;
    uint64_t probe_generation;
    uint64_t probe_operation_serial;
    uint64_t probe_started_ms;
    uint32_t probe_timeout_ms;
    uint16_t probe_packet_id;
    uint8_t early_ack_count;
    mqtt_packet_early_ack_t early_acks[MQTT_PACKET_GUARD_EARLY_ACKS];
    uint64_t proof_at_ms;
    uint64_t proof_generation;
    bool proof_valid;
} mqtt_packet_guard_t;

void mqtt_packet_guard_init(mqtt_packet_guard_t *guard);

/* Reserve BEFORE an ID-generating SDK call, report AFTER it. Failed calls also
 * consume the conservative budget (the SDK may have allocated their ID).
 * result returns false if the ID/order contract has become ambiguous. */
bool mqtt_packet_guard_packet_begin(mqtt_packet_guard_t *guard);
bool mqtt_packet_guard_packet_result(mqtt_packet_guard_t *guard, int packet_id);

/* Once armed, no new IDs may be allocated. outbox_empty must be a fresh SDK
 * observation by the sole owner with no concurrent publication. The budget is
 * renewed only after a socket boundary AND a subsequent CONNECTED callback.
 * If already DISCONNECTED, that known boundary suffices. */
bool mqtt_packet_guard_arm_reset(mqtt_packet_guard_t *guard, bool outbox_empty);
void mqtt_packet_guard_on_disconnected(mqtt_packet_guard_t *guard);
void mqtt_packet_guard_on_connected(mqtt_packet_guard_t *guard);

/* Begin after packet_begin, before the SDK call. started_ms is the planned
 * probe time, so SDK admission delay counts against the timeout. Exactly one
 * probe is active. token must be nonzero and locally unique. */
bool mqtt_packet_guard_probe_begin(mqtt_packet_guard_t *guard, uint64_t token,
                                   uint64_t started_ms, uint32_t timeout_ms);
/* Call after packet_result, for the same operation. Returns true only when a
 * previously buffered early ACK now establishes fresh proof. */
bool mqtt_packet_guard_probe_result(mqtt_packet_guard_t *guard, uint64_t token,
                                    int packet_id, uint64_t now_ms);
/* Stamp generation and time at CALLBACK RECEIPT, not when a queued event is
 * drained. Returns true exactly once for a newly accepted proof. */
bool mqtt_packet_guard_on_puback(mqtt_packet_guard_t *guard, uint64_t generation,
                                 int packet_id, uint64_t received_ms);
void mqtt_packet_guard_probe_expire(mqtt_packet_guard_t *guard, uint64_t now_ms);
/* Freeze, cancellation, queue-event overflow and mode changes retire the
 * current probe and its proof. Reconnect already does this automatically. */
void mqtt_packet_guard_probe_retire(mqtt_packet_guard_t *guard);
