/* Portable integration of the production identity guard and health state
 * machine. The adapter below supplies only SDK completion/event ordering;
 * neither production state machine is copied or reimplemented here. */
#include "mqtt_health.h"
#include "mqtt_packet_guard.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    mqtt_packet_guard_t guard;
    mqtt_health_t health;
    mqtt_health_token_t current_token;
} probe_adapter_t;

static probe_adapter_t adapter(void)
{
    probe_adapter_t a;
    mqtt_packet_guard_init(&a.guard);
    assert(mqtt_health_init(&a.health, 0, NULL));
    a.current_token = (mqtt_health_token_t){0};
    mqtt_packet_guard_on_connected(&a.guard);
    mqtt_health_on_connected(&a.health, 0);
    return a;
}

static mqtt_health_snapshot_t snapshot(probe_adapter_t *a, uint64_t now)
{
    mqtt_health_snapshot_t s;
    mqtt_packet_guard_probe_expire(&a->guard, now);
    mqtt_health_poll(&a->health, now, &s);
    return s;
}

static void begin(probe_adapter_t *a, uint64_t now)
{
    mqtt_health_snapshot_t s = snapshot(a, now);
    assert(s.probe_due);
    assert(mqtt_health_begin_probe(&a->health, now, &a->current_token));
    assert(mqtt_packet_guard_packet_begin(&a->guard));
    assert(mqtt_packet_guard_probe_begin(&a->guard, a->current_token.sequence,
                                        s.next_probe_due_ms, 60000));
}

static bool finish(probe_adapter_t *a, int id, uint64_t now)
{
    bool identity_safe = mqtt_packet_guard_packet_result(&a->guard, id);
    if (!identity_safe || id <= 0) {
        (void)mqtt_packet_guard_probe_result(&a->guard,
                                             a->current_token.sequence, id, now);
        (void)mqtt_health_probe_failed(&a->health, a->current_token, now);
        return false;
    }
    (void)mqtt_health_probe_admitted(&a->health, a->current_token, now);
    if (mqtt_packet_guard_probe_result(&a->guard, a->current_token.sequence,
                                       id, now))
        return mqtt_health_probe_ack(&a->health, a->current_token,
                                      a->guard.proof_at_ms);
    return false;
}

static bool ack(probe_adapter_t *a, uint64_t captured_generation, int id,
                uint64_t received_ms)
{
    mqtt_packet_probe_state_t before = a->guard.probe_state;
    bool accepted = mqtt_packet_guard_on_puback(&a->guard, captured_generation,
                                                 id, received_ms);
    /* A false return also covers overflow, which must immediately revoke the
     * prior logical proof instead of waiting for the next probe deadline. */
    if (before != MQTT_PROBE_FAILED && a->guard.probe_state == MQTT_PROBE_FAILED)
        mqtt_health_on_event_loss(&a->health, received_ms);
    if (!accepted)
        return false;
    return mqtt_health_probe_ack(&a->health, a->current_token,
                                  a->guard.proof_at_ms);
}

static void disconnect(probe_adapter_t *a, uint64_t now)
{
    mqtt_packet_guard_on_disconnected(&a->guard);
    mqtt_health_on_disconnected(&a->health, now);
}

static void connect(probe_adapter_t *a, uint64_t now)
{
    mqtt_packet_guard_on_connected(&a->guard);
    mqtt_health_on_connected(&a->health, now);
}

static void test_early_ack_admission_and_failure(void)
{
    probe_adapter_t a = adapter();
    begin(&a, 1000);
    assert(!ack(&a, a.guard.generation, 600, 1001));
    assert(!snapshot(&a, 1001).proof_fresh);
    assert(finish(&a, 600, 1002));
    assert(snapshot(&a, 1002).proof_fresh);
    assert(!ack(&a, a.guard.generation, 600, 1003));

    begin(&a, 120000);
    assert(!ack(&a, a.guard.generation, 601, 120001));
    assert(!finish(&a, -1, 120002));
    mqtt_health_snapshot_t s = snapshot(&a, 120002);
    assert(!s.proof_fresh && s.degraded);
    assert(s.failure == MQTT_HEALTH_FAILURE_PROBE_FAILED);
}

static void test_admission_and_ack_deadlines(void)
{
    probe_adapter_t a = adapter();
    begin(&a, 59000); /* deadline remains 60000, not begin + 60000 */
    assert(!finish(&a, 1, 59001));
    assert(!ack(&a, a.guard.generation, 1, 60000));
    mqtt_health_snapshot_t s = snapshot(&a, 60000);
    assert(!s.proof_fresh && s.failure == MQTT_HEALTH_FAILURE_ACK_TIMEOUT);

    begin(&a, 120000);
    assert(!ack(&a, a.guard.generation, 2, 120001));
    s = snapshot(&a, 180000); /* worker never reports SDK admission */
    assert(s.failure == MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);
    assert(!finish(&a, 2, 180001));
    assert(!snapshot(&a, 180001).proof_fresh);
}

static void test_old_ack_around_reconnect(void)
{
    probe_adapter_t a = adapter();
    begin(&a, 1);
    assert(!finish(&a, 50, 2));
    uint64_t first_generation = a.guard.generation;
    mqtt_health_token_t first_token = a.current_token;
    disconnect(&a, 10);
    assert(!ack(&a, first_generation, 50, 11));
    connect(&a, 20);
    begin(&a, 21);
    assert(!ack(&a, a.guard.generation, 50, 22)); /* retained old outbox ACK */
    assert(!finish(&a, 51, 23));
    assert(!ack(&a, first_generation, 51, 24)); /* queued old-generation event */
    assert(!mqtt_health_probe_ack(&a.health, first_token, 24));
    assert(!snapshot(&a, 24).proof_fresh);
    assert(ack(&a, a.guard.generation, 51, 25));
    assert(snapshot(&a, 25).proof_fresh);

    begin(&a, 120020);
    assert(!ack(&a, a.guard.generation, 52, 120021));
    disconnect(&a, 120022); /* early ACK followed by disconnect before result */
    connect(&a, 120023);
    assert(!finish(&a, 52, 120024));
    assert(!snapshot(&a, 120024).proof_fresh);
}

static void test_expired_probe_wrap_and_qos2_completion(void)
{
    probe_adapter_t a = adapter();
    /* Other ID generators share the same sequence: subscriptions, QoS1 and
     * QoS2 publishes. Their PUBLISHED/PUBCOMP events must not prove a probe. */
    assert(mqtt_packet_guard_packet_begin(&a.guard));
    assert(mqtt_packet_guard_packet_result(&a.guard, 65534));
    begin(&a, 1);
    assert(!finish(&a, 65535, 2));
    assert(!ack(&a, a.guard.generation, 65534, 3)); /* unrelated QoS2 completion */
    assert(!snapshot(&a, 60000).proof_fresh);
    begin(&a, 120000);
    assert(!finish(&a, 1, 120001)); /* natural 65535 -> 1 transition */
    assert(!ack(&a, a.guard.generation, 65535, 120002));
    assert(ack(&a, a.guard.generation, 1, 120003));
    assert(snapshot(&a, 120003).proof_fresh);
}

static void test_budget_requires_drained_outbox_and_socket(void)
{
    probe_adapter_t a = adapter();
    for (unsigned id = 1; id <= MQTT_PACKET_GUARD_BUDGET; ++id) {
        assert(mqtt_packet_guard_packet_begin(&a.guard));
        assert(mqtt_packet_guard_packet_result(&a.guard, (int)id));
    }
    assert(!mqtt_packet_guard_packet_begin(&a.guard));
    /* A retained SUBSCRIBE counts as nonempty just like a publish. */
    assert(!mqtt_packet_guard_arm_reset(&a.guard, false));
    disconnect(&a, 10);
    connect(&a, 20);
    assert(a.guard.attempts == MQTT_PACKET_GUARD_BUDGET);
    assert(!mqtt_packet_guard_packet_begin(&a.guard));
    mqtt_health_snapshot_t s = snapshot(&a, 60020);
    assert(s.failure == MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);

    uint64_t old_generation = a.guard.generation;
    assert(mqtt_packet_guard_arm_reset(&a.guard, true));
    assert(!mqtt_packet_guard_packet_begin(&a.guard));
    disconnect(&a, 60021);
    connect(&a, 60022);
    assert(a.guard.attempts == 0);
    begin(&a, 60023);
    assert(!finish(&a, 1, 60024));
    assert(!ack(&a, old_generation, 1, 60025));
    assert(ack(&a, a.guard.generation, 1, 60026));
    assert(snapshot(&a, 60026).proof_fresh);
}

static void test_ota_freeze_and_full_token_mapping(void)
{
    probe_adapter_t a = adapter();
    begin(&a, 1);
    assert(!finish(&a, 20, 2));
    mqtt_health_token_t old_token = a.current_token;
    mqtt_health_set_ota_active(&a.health, 3, true);
    mqtt_packet_guard_probe_retire(&a.guard);
    assert(!ack(&a, a.guard.generation, 20, 4));
    assert(!mqtt_health_probe_ack(&a.health, old_token, 4));
    mqtt_health_snapshot_t s = snapshot(&a, 1000000);
    assert(s.ota_active && !s.probe_due && !s.proof_fresh && !s.degraded);

    mqtt_health_set_ota_active(&a.health, 1000001, false);
    begin(&a, 1000002);
    assert(a.current_token.sequence != old_token.sequence);
    assert(!finish(&a, 21, 1000003));
    assert(!ack(&a, a.guard.generation, 20, 1000004));
    assert(!mqtt_health_probe_ack(&a.health, old_token, 1000004));
    mqtt_health_token_t wrong_generation = a.current_token;
    ++wrong_generation.generation;
    assert(!mqtt_health_probe_ack(&a.health, wrong_generation, 1000004));
    assert(ack(&a, a.guard.generation, 21, 1000005));
    assert(snapshot(&a, 1000005).proof_fresh);

    /* Freezing an already proven image also invalidates proof. This module
     * cannot approve OTA; callers must explicitly require fresh proof. */
    mqtt_health_set_ota_active(&a.health, 1000006, true);
    mqtt_packet_guard_probe_retire(&a.guard);
    assert(!snapshot(&a, 1000006).proof_fresh);
}

static void test_event_loss_cannot_become_health(void)
{
    probe_adapter_t a = adapter();
    begin(&a, 1);
    assert(!finish(&a, 1, 2));
    assert(ack(&a, a.guard.generation, 1, 3));
    assert(snapshot(&a, 3).proof_fresh);
    mqtt_health_on_event_loss(&a.health, 4);
    mqtt_packet_guard_probe_retire(&a.guard);
    assert(!ack(&a, a.guard.generation, 1, 5));
    assert(!snapshot(&a, 5).proof_fresh);

    begin(&a, 240000);
    for (int id = 1; id <= (int)MQTT_PACKET_GUARD_EARLY_ACKS + 1; ++id)
        assert(!ack(&a, a.guard.generation, id, 240001));
    assert(a.guard.probe_state == MQTT_PROBE_FAILED);
    assert(!finish(&a, 2, 240002));
    assert(!snapshot(&a, 240002).proof_fresh);
    assert(!snapshot(&a, 300000).proof_fresh);

    a = adapter();
    begin(&a, 1);
    assert(!finish(&a, 1, 2));
    assert(ack(&a, a.guard.generation, 1, 3));
    begin(&a, 120000);
    assert(snapshot(&a, 120000).proof_fresh); /* previous proof still valid */
    for (int id = 1; id <= (int)MQTT_PACKET_GUARD_EARLY_ACKS + 1; ++id)
        assert(!ack(&a, a.guard.generation, id, 120001));
    mqtt_health_snapshot_t s = snapshot(&a, 120001);
    assert(!s.proof_fresh && s.failure == MQTT_HEALTH_FAILURE_EVENT_LOSS);
    assert(!finish(&a, 2, 120002));
}

int main(void)
{
    test_early_ack_admission_and_failure();
    test_admission_and_ack_deadlines();
    test_old_ack_around_reconnect();
    test_expired_probe_wrap_and_qos2_completion();
    test_budget_requires_drained_outbox_and_socket();
    test_ota_freeze_and_full_token_mapping();
    test_event_loss_cannot_become_health();
    puts("mqtt probe integration: 7 scenario groups passed (production guard + health)");
    return 0;
}
