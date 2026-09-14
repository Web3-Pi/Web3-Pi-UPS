/* Tests the production queue, not a mirror of its scheduling implementation. */
#include "mqtt_dispatch_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static mqtt_dispatch_request_t request(const char *topic, bool critical, uint32_t key)
{
    mqtt_dispatch_request_t r = {
        .topic = topic, .payload = "x", .payload_len = 1,
        .qos = critical ? 1 : 0, .critical = critical, .coalesce_key = key
    };
    return r;
}

static uint64_t submit(mqtt_dispatch_queue_t *q, mqtt_dispatch_request_t r)
{
    uint64_t token = 0;
    assert(mqtt_dispatch_submit(q, &r, &token, NULL) == MQTT_DISPATCH_ACCEPTED);
    assert(token != 0);
    return token;
}

static mqtt_dispatch_claim_t claim(mqtt_dispatch_queue_t *q, uint64_t now)
{
    mqtt_dispatch_claim_t c;
    assert(mqtt_dispatch_claim(q, now, &c));
    return c;
}

static void sent(mqtt_dispatch_queue_t *q, mqtt_dispatch_claim_t c)
{
    assert(mqtt_dispatch_finish(q, &c, MQTT_DISPATCH_SENT, 0, 0));
}

static void test_copy_validation(void)
{
    mqtt_dispatch_queue_t q;
    mqtt_dispatch_init(&q);
    char topic[] = "t/test";
    uint8_t payload[] = {0, 1, 0, 255};
    mqtt_dispatch_request_t r = request(topic, false, 0);
    r.payload = payload; r.payload_len = sizeof(payload);
    uint64_t first = submit(&q, r);
    memset(topic, 'z', sizeof(topic) - 1); memset(payload, 9, sizeof(payload));
    mqtt_dispatch_claim_t c = claim(&q, 0);
    const uint8_t expected[] = {0, 1, 0, 255};
    assert(c.item.token == first && strcmp(c.item.topic, "t/test") == 0);
    assert(c.item.payload_len == 4 && memcmp(c.item.payload, expected, 4) == 0);
    sent(&q, c);

    r = request("retained", false, 0);
    r.payload = NULL; r.payload_len = 0; r.retain = true;
    submit(&q, r); c = claim(&q, 0);
    assert(c.item.retain && c.item.payload_len == 0); sent(&q, c);

    char max_topic[64]; memset(max_topic, 't', 63); max_topic[63] = '\0';
    uint8_t max_payload[256]; memset(max_payload, 0xa5, sizeof(max_payload));
    r = request(max_topic, false, 0); r.payload = max_payload; r.payload_len = 256;
    submit(&q, r); c = claim(&q, 0);
    assert(strlen(c.item.topic) == 63 && c.item.payload_len == 256);
    assert(memcmp(c.item.payload, max_payload, 256) == 0); sent(&q, c);
    r.payload_len = 257;
    uint64_t token = 12, replaced = 13;
    assert(mqtt_dispatch_submit(&q, &r, &token, &replaced) == MQTT_DISPATCH_TOO_LARGE);
    assert(token == 0 && replaced == 0);
    char long_topic[65]; memset(long_topic, 'a', 64); long_topic[64] = '\0';
    r = request(long_topic, false, 0);
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_TOO_LARGE);
    r = request("valid", false, 0); r.payload = NULL;
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    r = request("", false, 0);
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    r.topic = NULL;
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    r = request("valid", false, 0); r.qos = 3;
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    r.qos = -1;
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    assert(mqtt_dispatch_submit(&q, NULL, NULL, NULL) == MQTT_DISPATCH_INVALID);
    assert(mqtt_dispatch_submit(NULL, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    r.qos = 2; submit(&q, r); c = claim(&q, 0); assert(c.item.qos == 2); sent(&q, c);
    q.last_token = UINT64_MAX;
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_INVALID);
    puts("PASS copied binary ownership, empty retained payload, size and argument bounds");
}

static void test_coalescing_and_inflight(void)
{
    mqtt_dispatch_queue_t q; mqtt_dispatch_init(&q);
    mqtt_dispatch_request_t r = request("telemetry", false, 11);
    uint64_t first = submit(&q, r), token, replaced;
    uint64_t other = submit(&q, request("other", false, 0));
    r.payload = "new"; r.payload_len = 3;
    assert(mqtt_dispatch_submit(&q, &r, &token, &replaced) == MQTT_DISPATCH_ACCEPTED);
    assert(token > other && replaced == first && q.counters.depth == 2);
    mqtt_dispatch_claim_t c = claim(&q, 0);
    assert(c.item.token == token && memcmp(c.item.payload, "new", 3) == 0);
    mqtt_dispatch_claim_t blocked;
    assert(!mqtt_dispatch_claim(&q, 0, &blocked));
    /* In-flight item cannot be overwritten even with the identical key. */
    uint64_t newer = submit(&q, r);
    assert(q.counters.depth == 3 && q.slots[c.slot].item.token == token);
    mqtt_dispatch_claim_t wrong = c; ++wrong.item.token;
    assert(!mqtt_dispatch_finish(&q, &wrong, MQTT_DISPATCH_SENT, 0, 0));
    sent(&q, c);
    assert(!mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_SENT, 0, 0));
    c = claim(&q, 0); assert(c.item.token == other); sent(&q, c);
    c = claim(&q, 0); assert(c.item.token == newer); sent(&q, c);

    r = request("shared", false, 1); submit(&q, r);
    r.coalesce_key = 2; submit(&q, r);
    r.coalesce_key = 1; r.retain = true; submit(&q, r);
    r.retain = false; r.qos = 1; submit(&q, r);
    r.qos = 0; r.topic = "different"; submit(&q, r);
    r = request("opaque", false, 0); uint64_t opaque1 = submit(&q, r);
    uint64_t opaque2 = submit(&q, r);
    r = request("critical", true, 7); uint64_t critical1 = submit(&q, r);
    uint64_t critical2 = submit(&q, r);
    assert(q.counters.depth == 9 && q.counters.coalesced == 1);
    c = claim(&q, 0); assert(c.item.token == critical1); sent(&q, c);
    c = claim(&q, 0); assert(c.item.token == critical2); sent(&q, c);
    for (int i = 0; i < 5; ++i) { c = claim(&q, 0); sent(&q, c); }
    c = claim(&q, 0); assert(c.item.token == opaque1); sent(&q, c);
    c = claim(&q, 0); assert(c.item.token == opaque2); sent(&q, c);
    puts("PASS keyed snapshot isolation, opaque/critical FIFO, in-flight and stale completion guards");
}

static void test_reservation_fairness(void)
{
    mqtt_dispatch_queue_t q; mqtt_dispatch_init(&q);
    for (unsigned i = 0; i < 24; ++i) submit(&q, request("normal", false, i + 1));
    mqtt_dispatch_request_t r = request("overflow", false, 0);
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_FULL);
    for (unsigned i = 0; i < 8; ++i) submit(&q, request("event", true, 0));
    r = request("critical-overflow", true, 0);
    assert(mqtt_dispatch_submit(&q, &r, NULL, NULL) == MQTT_DISPATCH_FULL);
    r = request("normal", false, 1); submit(&q, r); /* replacement fits at full capacity */
    mqtt_dispatch_stats_t stats; mqtt_dispatch_stats(&q, &stats);
    assert(stats.depth == 32 && stats.highwater == 32 && stats.full == 2);
    assert(stats.accepted == 33 && stats.coalesced == 1);

    mqtt_dispatch_init(&q);
    uint64_t normal = submit(&q, request("normal", false, 0));
    for (unsigned i = 0; i < 12; ++i) submit(&q, request("event", true, 0));
    for (unsigned i = 0; i < 8; ++i) {
        mqtt_dispatch_claim_t c = claim(&q, 0);
        assert(c.item.critical && c.item.token == normal + i + 1); sent(&q, c);
    }
    mqtt_dispatch_claim_t c = claim(&q, 0);
    assert(c.item.token == normal); sent(&q, c);
    for (unsigned i = 0; i < 4; ++i) { c = claim(&q, 0); assert(c.item.critical); sent(&q, c); }
    puts("PASS 8-slot critical reserve, full accounting, priority and bounded critical burst");
}

static void test_retry_expiry(void)
{
    mqtt_dispatch_queue_t q; mqtt_dispatch_init(&q);
    mqtt_dispatch_request_t r = request("older", false, 0); r.deadline_ms = 100;
    uint64_t first = submit(&q, r);
    mqtt_dispatch_claim_t c = claim(&q, 1);
    assert(mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_RETRY, 2, 10));
    uint64_t second = submit(&q, request("newer", false, 0));
    assert(!mqtt_dispatch_claim(&q, 3, &c)); /* younger FIFO must not overtake */
    uint64_t urgent = submit(&q, request("critical", true, 0));
    c = claim(&q, 4); assert(c.item.token == urgent); sent(&q, c);
    assert(!mqtt_dispatch_claim(&q, 9, &c));
    c = claim(&q, 10); assert(c.item.token == first);
    assert(mqtt_dispatch_expire(&q, 100, NULL) == 0); /* in-flight retains ownership */
    assert(mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_RETRY, 100, 110));
    assert(q.counters.expired == 1 && q.counters.depth == 1);
    c = claim(&q, 100); assert(c.item.token == second); sent(&q, c);

    /* Critical events also retain FIFO across a failed SDK admission. */
    uint64_t critical_first = submit(&q, request("event1", true, 0));
    uint64_t critical_second = submit(&q, request("event2", true, 0));
    c = claim(&q, 100); assert(c.item.token == critical_first);
    assert(mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_RETRY, 100, 150));
    assert(!mqtt_dispatch_claim(&q, 149, &c));
    c = claim(&q, 150); assert(c.item.token == critical_first); sent(&q, c);
    c = claim(&q, 150); assert(c.item.token == critical_second); sent(&q, c);

    r.deadline_ms = 200; first = submit(&q, r);
    uint64_t expired_tokens[MQTT_DISPATCH_CAPACITY] = {0};
    assert(mqtt_dispatch_expire(&q, 199, expired_tokens) == 0);
    assert(!mqtt_dispatch_claim(&q, 200, &c));
    assert(mqtt_dispatch_expire(&q, 200, expired_tokens) == 1);
    assert(expired_tokens[0] == first);

    r.deadline_ms = 300; submit(&q, r); c = claim(&q, 299);
    assert(mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_SENT, 301, 0));
    assert(q.counters.expired == 2); /* accepted by SDK is never re-enqueued */
    r.deadline_ms = 0; submit(&q, r); c = claim(&q, UINT64_MAX);
    assert(mqtt_dispatch_finish(&q, &c, MQTT_DISPATCH_DROP, UINT64_MAX, 0));
    assert(q.counters.dropped == 1 && q.counters.depth == 0);
    puts("PASS deferred retry, deadline boundary, in-flight expiry and distinct drop counters");
}

static void check_invariants(const mqtt_dispatch_queue_t *q)
{
    unsigned depth = 0, noncritical = 0, inflight = 0;
    for (unsigned i = 0; i < MQTT_DISPATCH_CAPACITY; ++i) {
        const mqtt_dispatch_slot_t *s = &q->slots[i];
        if (s->state == MQTT_DISPATCH_SLOT_FREE) continue;
        ++depth; noncritical += !s->item.critical;
        inflight += s->state == MQTT_DISPATCH_SLOT_INFLIGHT;
        assert(s->item.token != 0 && s->item.token <= q->last_token);
        for (unsigned j = i + 1; j < MQTT_DISPATCH_CAPACITY; ++j)
            if (q->slots[j].state != MQTT_DISPATCH_SLOT_FREE)
                assert(s->item.token != q->slots[j].item.token);
    }
    assert(depth == q->counters.depth && depth <= MQTT_DISPATCH_CAPACITY);
    assert(noncritical <= 24 && inflight <= 1);
    assert(q->counters.highwater >= depth && q->counters.highwater <= 32);
    assert(q->critical_streak <= 8);
}

static void test_pressure(void)
{
    mqtt_dispatch_queue_t q; mqtt_dispatch_init(&q);
    uint32_t random = 0x789abcdu;
    for (uint64_t now = 1; now <= 25000; ++now) {
        random = random * 1664525u + 1013904223u;
        mqtt_dispatch_request_t r = request("mixed", (random & 8u) != 0, random % 7);
        r.deadline_ms = now + 20 + random % 80;
        uint64_t token, replaced;
        mqtt_dispatch_result_t result = mqtt_dispatch_submit(&q, &r, &token, &replaced);
        assert(result == MQTT_DISPATCH_ACCEPTED || result == MQTT_DISPATCH_FULL);
        mqtt_dispatch_expire(&q, now, NULL);
        if ((random & 3u) == 0) {
            mqtt_dispatch_claim_t c;
            if (mqtt_dispatch_claim(&q, now, &c)) {
                mqtt_dispatch_finish_t how = (mqtt_dispatch_finish_t)((random >> 8) % 3);
                assert(mqtt_dispatch_finish(&q, &c, how, now, now + 13));
            }
        }
        check_invariants(&q);
    }
    mqtt_dispatch_expire(&q, UINT64_MAX, NULL);
    assert(q.counters.depth == 0 && q.counters.full > 0 && q.counters.expired > 0);
    puts("PASS 25000 mixed pressure/retry/coalesce/expiry cycles with bounded storage invariants");
}

int main(void)
{
    test_copy_validation();
    test_coalescing_and_inflight();
    test_reservation_fairness();
    test_retry_expiry();
    test_pressure();
    printf("ALL PASSED; static queue size: %zu bytes\n", sizeof(mqtt_dispatch_queue_t));
    return 0;
}
