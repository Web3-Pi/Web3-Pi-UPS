/* Executes the production packet/probe state machine with adversarial SDK
 * results and event orderings. Run: python3 tools/test_mqtt_packet_guard.py */
#include "mqtt_packet_guard.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static mqtt_packet_guard_t connected_guard(void)
{
    mqtt_packet_guard_t g;
    mqtt_packet_guard_init(&g);
    mqtt_packet_guard_on_connected(&g);
    assert(g.connected && g.generation == 1);
    return g;
}

static void packet(mqtt_packet_guard_t *g, int id)
{
    assert(mqtt_packet_guard_packet_begin(g));
    assert(mqtt_packet_guard_packet_result(g, id));
}

static void begin_probe(mqtt_packet_guard_t *g, uint64_t token, uint64_t now)
{
    assert(mqtt_packet_guard_packet_begin(g));
    assert(mqtt_packet_guard_probe_begin(g, token, now, 60000));
}

static void pending_probe(mqtt_packet_guard_t *g, uint64_t token, int id,
                          uint64_t now)
{
    begin_probe(g, token, now);
    assert(mqtt_packet_guard_packet_result(g, id));
    assert(!mqtt_packet_guard_probe_result(g, token, id, now + 1));
    assert(g->probe_state == MQTT_PROBE_PENDING);
}

static void test_fresh_and_early_ack(void)
{
    mqtt_packet_guard_t g = connected_guard();
    pending_probe(&g, 1, 500, 1000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 499, 1020));
    assert(mqtt_packet_guard_on_puback(&g, g.generation, 500, 1030));
    assert(g.proof_valid && g.proof_at_ms == 1030);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 500, 1040));

    begin_probe(&g, 2, 2000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 500, 2010));
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 501, 2020));
    assert(mqtt_packet_guard_packet_result(&g, 501));
    assert(mqtt_packet_guard_probe_result(&g, 2, 501, 2030));
    assert(g.proof_at_ms == 2020 && g.proof_generation == g.generation);
    assert(!mqtt_packet_guard_probe_result(&g, 2, 501, 2040));
}

static void test_deadlines_and_cancellation(void)
{
    mqtt_packet_guard_t g = connected_guard();
    pending_probe(&g, 1, 1, 1000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 1, 999));
    assert(mqtt_packet_guard_on_puback(&g, g.generation, 1, 60999));
    pending_probe(&g, 2, 2, 100000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 2, 160000));
    assert(g.probe_state == MQTT_PROBE_EXPIRED && !g.proof_valid);

    begin_probe(&g, 3, 200000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 3, 200010));
    assert(mqtt_packet_guard_packet_result(&g, 3));
    /* Even an early ACK cannot revive an operation completed after deadline. */
    assert(!mqtt_packet_guard_probe_result(&g, 3, 3, 260000));
    assert(g.probe_state == MQTT_PROBE_EXPIRED);

    pending_probe(&g, 4, 4, 300000);
    mqtt_packet_guard_probe_retire(&g);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 4, 300001));
    assert(!g.proof_valid);
    pending_probe(&g, 5, 5, 400000);
    mqtt_packet_guard_probe_expire(&g, 460000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 5, 400001));
}

static void test_failure_and_overflow(void)
{
    mqtt_packet_guard_t g = connected_guard();
    begin_probe(&g, 1, 1000);
    for (int i = 1; i <= (int)MQTT_PACKET_GUARD_EARLY_ACKS + 1; ++i)
        assert(!mqtt_packet_guard_on_puback(&g, g.generation, i, 1001));
    assert(g.probe_state == MQTT_PROBE_FAILED && !g.proof_valid);
    assert(mqtt_packet_guard_packet_result(&g, 9));
    assert(!mqtt_packet_guard_probe_result(&g, 1, 9, 1002));

    begin_probe(&g, 2, 2000);
    for (int i = 0; i < 100; ++i)
        assert(!mqtt_packet_guard_on_puback(&g, g.generation, 10, 2001));
    assert(g.early_ack_count == 1);
    assert(mqtt_packet_guard_packet_result(&g, 10));
    assert(mqtt_packet_guard_probe_result(&g, 2, 10, 2002));

    begin_probe(&g, 3, 3000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 11, 3001));
    assert(mqtt_packet_guard_packet_result(&g, -1));
    assert(!mqtt_packet_guard_probe_result(&g, 3, -1, 3002));
    assert(g.probe_state == MQTT_PROBE_FAILED && !g.proof_valid);
}

static void test_reconnect_retains_outbox_identity(void)
{
    mqtt_packet_guard_t g = connected_guard();
    pending_probe(&g, 1, 100, 1000);
    uint64_t old_generation = g.generation;
    mqtt_packet_guard_on_disconnected(&g);
    assert(!mqtt_packet_guard_arm_reset(&g, false));
    mqtt_packet_guard_on_connected(&g);
    assert(g.attempts == 1 && !g.proof_valid);
    pending_probe(&g, 2, 101, 2000);
    /* The retained old packet may legitimately ACK on the new connection. */
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 100, 2001));
    assert(!mqtt_packet_guard_on_puback(&g, old_generation, 101, 2001));
    assert(mqtt_packet_guard_on_puback(&g, g.generation, 101, 2002));

    begin_probe(&g, 3, 3000);
    mqtt_packet_guard_on_disconnected(&g); /* between enqueue and its return */
    mqtt_packet_guard_on_connected(&g);
    assert(mqtt_packet_guard_packet_result(&g, 102));
    assert(!mqtt_packet_guard_probe_result(&g, 3, 102, 3001));
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 102, 3002));
}

static void test_incremental_failures_wrap_and_collisions(void)
{
    mqtt_packet_guard_t g = connected_guard();
    packet(&g, 65533);
    packet(&g, -2); /* failed before assigning an ID */
    packet(&g, 65534);
    packet(&g, -1); /* failed after assigning 65535 */
    packet(&g, 1);
    packet(&g, 2);
    assert(!g.unsafe);
    begin_probe(&g, 1, 1000);
    assert(!mqtt_packet_guard_on_puback(&g, g.generation, 1, 1001));
    assert(!mqtt_packet_guard_packet_result(&g, 1)); /* forced collision */
    assert(g.unsafe && !g.proof_valid);
    assert(!mqtt_packet_guard_probe_result(&g, 1, 1, 1002));
    assert(!mqtt_packet_guard_packet_begin(&g));
    mqtt_packet_guard_on_disconnected(&g);
    mqtt_packet_guard_on_connected(&g);
    assert(g.unsafe); /* ordinary reconnect is not an identity reset */
    assert(mqtt_packet_guard_arm_reset(&g, true));
    mqtt_packet_guard_on_disconnected(&g);
    mqtt_packet_guard_on_connected(&g);
    assert(!g.unsafe && g.attempts == 0);
    packet(&g, 1); /* safe reuse after drained outbox + fresh clean socket */

    g = connected_guard();
    packet(&g, 50);
    assert(mqtt_packet_guard_packet_begin(&g));
    assert(!mqtt_packet_guard_packet_result(&g, 50)); /* immediate collision */
    g = connected_guard();
    packet(&g, 50);
    assert(mqtt_packet_guard_packet_begin(&g));
    assert(!mqtt_packet_guard_packet_result(&g, 52)); /* unowned allocation */
}

static uint16_t sdk_next_id(uint16_t *last)
{
    ++*last;
    if (*last == 0)
        ++*last;
    return *last;
}

static void test_budget_never_reuses_live_id(void)
{
    mqtt_packet_guard_t g = connected_guard();
    uint16_t sdk_id = 0;
    bool live_ids[65536] = {false};
    for (unsigned epoch = 0; epoch < 4; ++epoch) {
        memset(live_ids, 0, sizeof(live_ids));
        for (unsigned i = 0; i < MQTT_PACKET_GUARD_BUDGET; ++i) {
            assert(mqtt_packet_guard_packet_begin(&g));
            /* Alternate failed-before, failed-after and successful allocation.
             * Keep every allocated ID outstanding to make reuse observable. */
            int result = -2;
            if (i % 7 != 0) {
                uint16_t id = sdk_next_id(&sdk_id);
                assert(!live_ids[id]);
                live_ids[id] = true;
                result = i % 11 == 0 ? -1 : id;
            }
            assert(mqtt_packet_guard_packet_result(&g, result));
            if (i == 12345) {
                mqtt_packet_guard_on_disconnected(&g);
                mqtt_packet_guard_on_connected(&g);
                assert(g.attempts == i + 1); /* no reset for retained outbox */
            }
        }
        assert(!mqtt_packet_guard_packet_begin(&g));
        assert(!mqtt_packet_guard_arm_reset(&g, false));
        mqtt_packet_guard_on_disconnected(&g);
        mqtt_packet_guard_on_connected(&g);
        assert(!mqtt_packet_guard_packet_begin(&g));
        assert(mqtt_packet_guard_arm_reset(&g, true));
        assert(!mqtt_packet_guard_packet_begin(&g));
        assert(g.attempts == MQTT_PACKET_GUARD_BUDGET);
        mqtt_packet_guard_on_disconnected(&g);
        mqtt_packet_guard_on_connected(&g);
        assert(g.attempts == 0);
    }
}

static void test_reset_and_misuse_fail_closed(void)
{
    mqtt_packet_guard_t g = connected_guard();
    assert(mqtt_packet_guard_packet_begin(&g));
    assert(!mqtt_packet_guard_arm_reset(&g, true));
    assert(mqtt_packet_guard_packet_result(&g, 10));
    mqtt_packet_guard_on_disconnected(&g);
    assert(mqtt_packet_guard_arm_reset(&g, true));
    assert(g.reset_boundary_seen);
    mqtt_packet_guard_on_connected(&g);
    assert(g.attempts == 0);

    pending_probe(&g, 1, 10, 1000);
    uint64_t old_generation = g.generation;
    mqtt_packet_guard_probe_retire(&g);
    assert(mqtt_packet_guard_arm_reset(&g, true));
    mqtt_packet_guard_on_disconnected(&g);
    mqtt_packet_guard_on_connected(&g);
    pending_probe(&g, 2, 10, 2000);
    assert(!mqtt_packet_guard_on_puback(&g, old_generation, 10, 2001));
    assert(mqtt_packet_guard_on_puback(&g, g.generation, 10, 2002));

    g = connected_guard();
    assert(mqtt_packet_guard_arm_reset(&g, true));
    mqtt_packet_guard_on_connected(&g); /* no actual disconnect */
    assert(g.unsafe && g.reset_armed && !g.proof_valid);
    g = connected_guard();
    assert(!mqtt_packet_guard_packet_result(&g, 1));
    assert(g.unsafe);
    g = connected_guard();
    assert(mqtt_packet_guard_packet_begin(&g));
    assert(!mqtt_packet_guard_packet_begin(&g));
    assert(g.unsafe);
    g = connected_guard();
    assert(mqtt_packet_guard_packet_begin(&g));
    assert(!mqtt_packet_guard_packet_result(&g, 0));
    g = connected_guard();
    g.generation = UINT64_MAX;
    mqtt_packet_guard_on_disconnected(&g);
    mqtt_packet_guard_on_connected(&g);
    assert(g.unsafe && !g.proof_valid);
}

int main(void)
{
    test_fresh_and_early_ack();
    test_deadlines_and_cancellation();
    test_failure_and_overflow();
    test_reconnect_retains_outbox_identity();
    test_incremental_failures_wrap_and_collisions();
    test_budget_never_reuses_live_id();
    test_reset_and_misuse_fail_closed();
    printf("mqtt packet guard: 7 scenario groups passed; 240000 guarded attempts; state=%zu bytes\n",
           sizeof(mqtt_packet_guard_t));
    return 0;
}
