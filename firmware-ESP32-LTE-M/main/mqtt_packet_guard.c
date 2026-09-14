#include "mqtt_packet_guard.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if !defined(CONFIG_MQTT_MSG_ID_INCREMENTAL) || !CONFIG_MQTT_MSG_ID_INCREMENTAL
#error "MQTT packet identity guard requires incremental SDK packet IDs"
#endif
#endif

_Static_assert(MQTT_PACKET_GUARD_BUDGET > 0 &&
               MQTT_PACKET_GUARD_BUDGET < UINT16_MAX,
               "An identity epoch must not reuse a 16-bit MQTT packet ID");
_Static_assert(MQTT_PACKET_GUARD_EARLY_ACKS <= UINT8_MAX,
               "Early ACK count must fit its bounded counter");

static bool probe_active(const mqtt_packet_guard_t *g)
{
    return g->probe_state == MQTT_PROBE_ARMING ||
           g->probe_state == MQTT_PROBE_PENDING;
}

static bool within_deadline(const mqtt_packet_guard_t *g, uint64_t now_ms)
{
    return now_ms >= g->probe_started_ms &&
           now_ms - g->probe_started_ms < g->probe_timeout_ms;
}

static void fail_probe(mqtt_packet_guard_t *g)
{
    g->probe_state = MQTT_PROBE_FAILED;
    g->early_ack_count = 0;
    g->proof_valid = false;
}

static bool mark_unsafe(mqtt_packet_guard_t *g)
{
    g->unsafe = true;
    fail_probe(g);
    return false;
}

static bool accept_ack(mqtt_packet_guard_t *g, uint64_t received_ms)
{
    if (!within_deadline(g, received_ms)) {
        g->probe_state = MQTT_PROBE_EXPIRED;
        g->proof_valid = false;
        return false;
    }
    g->probe_state = MQTT_PROBE_ACKED;
    g->early_ack_count = 0;
    g->proof_at_ms = received_ms;
    g->proof_generation = g->generation;
    g->proof_valid = true;
    return true;
}

void mqtt_packet_guard_init(mqtt_packet_guard_t *g)
{
    memset(g, 0, sizeof(*g));
    g->completed_packet_id = -1;
}

void mqtt_packet_guard_probe_retire(mqtt_packet_guard_t *g)
{
    g->probe_state = MQTT_PROBE_RETIRED;
    g->early_ack_count = 0;
    g->proof_valid = false;
}

bool mqtt_packet_guard_packet_begin(mqtt_packet_guard_t *g)
{
    if (g->operation_pending || g->operation_serial == UINT64_MAX)
        return mark_unsafe(g);
    if (g->unsafe || g->reset_armed || g->attempts >= MQTT_PACKET_GUARD_BUDGET)
        return false;
    ++g->attempts;
    ++g->attempts_since_id;
    ++g->operation_serial;
    g->operation_pending = true;
    return true;
}

bool mqtt_packet_guard_packet_result(mqtt_packet_guard_t *g, int packet_id)
{
    if (!g->operation_pending)
        return mark_unsafe(g);
    g->operation_pending = false;
    g->completed_operation_serial = g->operation_serial;
    g->completed_packet_id = packet_id;
    if (g->unsafe)
        return false;
    /* A failed call may or may not have consumed one ID before failing. */
    if (packet_id < 0)
        return true;
    if (packet_id == 0 || packet_id > UINT16_MAX)
        return mark_unsafe(g);

    if (g->have_last_packet_id) {
        uint32_t distance = packet_id >= g->last_packet_id
            ? (uint32_t)packet_id - g->last_packet_id
            : UINT16_MAX - (uint32_t)g->last_packet_id + (uint32_t)packet_id;
        /* IDs advance over 1..65535 (zero is skipped). Within the guarded
         * epoch fewer than 65535 attempts are allowed, including failures.
         * Zero distance and an implausibly large jump are both ambiguous. */
        if (distance == 0 || distance > g->attempts_since_id)
            return mark_unsafe(g);
    }
    g->last_packet_id = (uint16_t)packet_id;
    g->have_last_packet_id = true;
    g->attempts_since_id = 0;
    return true;
}

bool mqtt_packet_guard_arm_reset(mqtt_packet_guard_t *g, bool outbox_empty)
{
    if (!outbox_empty || g->operation_pending)
        return false;
    if (!g->reset_armed) {
        g->reset_armed = true;
        g->reset_boundary_seen = !g->connected;
    }
    mqtt_packet_guard_probe_retire(g);
    return true;
}

void mqtt_packet_guard_on_disconnected(mqtt_packet_guard_t *g)
{
    g->connected = false;
    if (g->reset_armed)
        g->reset_boundary_seen = true;
    mqtt_packet_guard_probe_retire(g);
}

void mqtt_packet_guard_on_connected(mqtt_packet_guard_t *g)
{
    /* A duplicate CONNECTED without a socket boundary cannot renew identity. */
    if (g->connected || g->generation == UINT64_MAX) {
        (void)mark_unsafe(g);
        return;
    }
    g->connected = true;
    ++g->generation;
    mqtt_packet_guard_probe_retire(g);
    if (g->reset_armed && g->reset_boundary_seen && !g->operation_pending) {
        g->attempts = 0;
        g->attempts_since_id = 0;
        g->have_last_packet_id = false;
        g->unsafe = false;
        g->reset_armed = false;
        g->reset_boundary_seen = false;
        g->completed_packet_id = -1;
    }
}

bool mqtt_packet_guard_probe_begin(mqtt_packet_guard_t *g, uint64_t token,
                                   uint64_t started_ms, uint32_t timeout_ms)
{
    if (!g->connected || !g->operation_pending || g->unsafe || g->reset_armed ||
        probe_active(g) || token == 0 || token == g->probe_token || timeout_ms == 0)
        return false;
    g->probe_state = MQTT_PROBE_ARMING;
    g->probe_token = token;
    g->probe_generation = g->generation;
    g->probe_operation_serial = g->operation_serial;
    g->probe_started_ms = started_ms;
    g->probe_timeout_ms = timeout_ms;
    g->probe_packet_id = 0;
    g->early_ack_count = 0;
    return true;
}

bool mqtt_packet_guard_probe_result(mqtt_packet_guard_t *g, uint64_t token,
                                    int packet_id, uint64_t now_ms)
{
    if (g->probe_state != MQTT_PROBE_ARMING || token != g->probe_token)
        return false;
    if (g->unsafe || !g->connected || g->probe_generation != g->generation ||
        g->operation_pending ||
        g->completed_operation_serial != g->probe_operation_serial ||
        g->completed_packet_id != packet_id || packet_id <= 0 ||
        packet_id > UINT16_MAX) {
        fail_probe(g);
        return false;
    }
    if (!within_deadline(g, now_ms)) {
        g->probe_state = MQTT_PROBE_EXPIRED;
        g->early_ack_count = 0;
        g->proof_valid = false;
        return false;
    }
    g->probe_packet_id = (uint16_t)packet_id;
    g->probe_state = MQTT_PROBE_PENDING;
    for (uint8_t i = 0; i < g->early_ack_count; ++i) {
        if (g->early_acks[i].packet_id == packet_id)
            return accept_ack(g, g->early_acks[i].received_ms);
    }
    g->early_ack_count = 0;
    return false;
}

bool mqtt_packet_guard_on_puback(mqtt_packet_guard_t *g, uint64_t generation,
                                 int packet_id, uint64_t received_ms)
{
    if (!g->connected || g->unsafe || !probe_active(g) ||
        generation != g->generation || generation != g->probe_generation ||
        packet_id <= 0 || packet_id > UINT16_MAX)
        return false;
    /* A queued ACK captured before this attempt cannot establish its proof. */
    if (received_ms < g->probe_started_ms)
        return false;
    if (!within_deadline(g, received_ms)) {
        mqtt_packet_guard_probe_expire(g, received_ms);
        return false;
    }
    if (g->probe_state == MQTT_PROBE_PENDING) {
        if (packet_id != g->probe_packet_id)
            return false;
        return accept_ack(g, received_ms);
    }
    for (uint8_t i = 0; i < g->early_ack_count; ++i) {
        if (g->early_acks[i].packet_id == packet_id)
            return false; /* Retain first receipt; duplicate ACK adds no proof. */
    }
    if (g->early_ack_count == MQTT_PACKET_GUARD_EARLY_ACKS) {
        fail_probe(g); /* Bounded storage exhausted: evidence is ambiguous. */
        return false;
    }
    g->early_acks[g->early_ack_count++] = (mqtt_packet_early_ack_t) {
        .packet_id = (uint16_t)packet_id,
        .received_ms = received_ms,
    };
    return false;
}

void mqtt_packet_guard_probe_expire(mqtt_packet_guard_t *g, uint64_t now_ms)
{
    if (probe_active(g) && now_ms >= g->probe_started_ms &&
        !within_deadline(g, now_ms)) {
        g->probe_state = MQTT_PROBE_EXPIRED;
        g->early_ack_count = 0;
        g->proof_valid = false;
    }
}
