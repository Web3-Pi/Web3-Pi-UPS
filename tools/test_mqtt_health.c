/* Host tests exercise the production pure-C module, with a controlled clock.
 * Run: python3 tools/test_mqtt_health.py */
#include "mqtt_health.h"

#include <inttypes.h>
#include <stdio.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __func__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static mqtt_health_t new_health(uint64_t now)
{
    mqtt_health_t h;
    CHECK(mqtt_health_init(&h, now, NULL));
    return h;
}

static mqtt_health_snapshot_t snapshot(mqtt_health_t *h, uint64_t now)
{
    mqtt_health_snapshot_t s;
    mqtt_health_poll(h, now, &s);
    return s;
}

static mqtt_health_token_t begin(mqtt_health_t *h, uint64_t now)
{
    mqtt_health_token_t token = {0};
    CHECK(mqtt_health_begin_probe(h, now, &token));
    CHECK(token.generation != 0 && token.sequence != 0);
    return token;
}

static void test_configuration_and_idle(void)
{
    mqtt_health_config_t cfg = mqtt_health_default_config();
    CHECK(cfg.probe_interval_ms == 120000);
    CHECK(cfg.probe_deadline_ms == 60000);
    CHECK(cfg.worker_stall_ms == 30000);
    mqtt_health_t h;
    CHECK(!mqtt_health_init(NULL, 0, NULL));
    cfg.probe_deadline_ms = 0;
    CHECK(!mqtt_health_init(&h, 0, &cfg));
    cfg = mqtt_health_default_config();
    cfg.probe_interval_ms = cfg.probe_deadline_ms - 1;
    CHECK(!mqtt_health_init(&h, 0, &cfg));
    cfg = mqtt_health_default_config();
    cfg.worker_stall_ms = 0;
    CHECK(!mqtt_health_init(&h, 0, &cfg));
    h = new_health(0);
    mqtt_health_snapshot_t s = snapshot(&h, UINT64_C(8640000000));
    CHECK(!s.connected && !s.probe_due && !s.proof_fresh);
    CHECK(!s.degraded && !s.worker_stalled && !s.worker_busy);
}

static void test_initial_grace_and_missing_scheduler(void)
{
    mqtt_health_t h = new_health(1000);
    mqtt_health_on_connected(&h, 1000);
    mqtt_health_snapshot_t s = snapshot(&h, 1000);
    CHECK(s.connected && s.probe_due && !s.proof_fresh && !s.degraded);
    CHECK(s.next_probe_due_ms == 1000 && s.probe_deadline_ms == 61000);
    s = snapshot(&h, 60999);
    CHECK(s.probe_due && !s.degraded && !s.proof_fresh);
    /* No begin/admission is necessary to detect the parked scheduler. */
    s = snapshot(&h, 61000);
    CHECK(s.degraded && !s.proof_fresh && !s.probe_due);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);
    CHECK(s.next_probe_due_ms == 121000);
    mqtt_health_token_t token;
    CHECK(!mqtt_health_begin_probe(&h, 61001, &token));
    s = snapshot(&h, 121000);
    CHECK(s.probe_due && s.degraded);
    token = begin(&h, 121000);
    CHECK(mqtt_health_probe_admitted(&h, token, 121001));
    CHECK(mqtt_health_probe_ack(&h, token, 121002));
    s = snapshot(&h, 121002);
    CHECK(s.proof_fresh && !s.degraded && !s.probe_pending);
}

static void test_ack_ordering_and_unknown_tokens(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    CHECK(!mqtt_health_begin_probe(&h, 0, NULL));
    mqtt_health_token_t token = begin(&h, 0);
    mqtt_health_token_t other = token;
    other.sequence++;
    CHECK(!mqtt_health_probe_admitted(&h, other, 1));
    CHECK(!mqtt_health_probe_ack(&h, other, 1));
    CHECK(!mqtt_health_probe_failed(&h, other, 1));
    other = token;
    other.generation++;
    CHECK(!mqtt_health_probe_ack(&h, other, 1));
    /* The adapter must buffer an early ACK until it records admission. */
    CHECK(!mqtt_health_probe_ack(&h, token, 2));
    CHECK(!snapshot(&h, 2).proof_fresh);
    CHECK(mqtt_health_probe_admitted(&h, token, 3));
    CHECK(!mqtt_health_probe_admitted(&h, token, 3));
    CHECK(!mqtt_health_begin_probe(&h, 3, &other));
    CHECK(mqtt_health_probe_ack(&h, token, 4));
    CHECK(!mqtt_health_probe_ack(&h, token, 5));
    CHECK(!mqtt_health_probe_admitted(&h, token, 5));
    CHECK(!mqtt_health_probe_failed(&h, token, 5));
    mqtt_health_snapshot_t s = snapshot(&h, 5);
    CHECK(s.proof_fresh && s.last_ack_ms == 4);
    CHECK(s.next_probe_due_ms == 120000);
}

static void test_admission_and_ack_deadlines(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    mqtt_health_token_t token = begin(&h, 59000);
    /* Deadline is planned due + 60 s, not begin/admission + 60 s. */
    CHECK(!mqtt_health_probe_admitted(&h, token, 60000));
    mqtt_health_snapshot_t s = snapshot(&h, 60000);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);
    CHECK(!s.proof_fresh && !s.probe_pending);

    h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    token = begin(&h, 59000);
    CHECK(mqtt_health_probe_admitted(&h, token, 59998));
    CHECK(mqtt_health_probe_ack(&h, token, 59999));
    CHECK(snapshot(&h, 59999).proof_fresh);

    h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    token = begin(&h, 0);
    CHECK(mqtt_health_probe_admitted(&h, token, 0));
    CHECK(!mqtt_health_probe_ack(&h, token, 60000));
    s = snapshot(&h, 60000);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_ACK_TIMEOUT);
    CHECK(!s.proof_fresh && !s.probe_pending);
    CHECK(!mqtt_health_probe_ack(&h, token, 60001));

    /* A backdated callback cannot restore proof after the deadline expired. */
    CHECK(!mqtt_health_probe_ack(&h, token, 59999));
    CHECK(!snapshot(&h, 59999).proof_fresh);
}

static void test_periodic_freshness_without_producer_traffic(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    mqtt_health_token_t token = begin(&h, 0);
    CHECK(mqtt_health_probe_admitted(&h, token, 1));
    CHECK(mqtt_health_probe_ack(&h, token, 10));
    mqtt_health_snapshot_t s = snapshot(&h, 119999);
    CHECK(s.proof_fresh && !s.probe_due && !s.degraded);
    s = snapshot(&h, 120000);
    CHECK(s.proof_fresh && s.probe_due && !s.degraded);
    token = begin(&h, 120000);
    CHECK(mqtt_health_probe_admitted(&h, token, 120001));
    CHECK(snapshot(&h, 179999).proof_fresh);
    s = snapshot(&h, 180000);
    CHECK(!s.proof_fresh && s.degraded);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_ACK_TIMEOUT);
    CHECK(!s.worker_stalled);  /* Idle executor/producer is not a stall. */

    /* No normal telemetry is required: successful probes maintain proof. */
    token = begin(&h, 240000);
    CHECK(mqtt_health_probe_admitted(&h, token, 240001));
    CHECK(mqtt_health_probe_ack(&h, token, 240010));
    s = snapshot(&h, 240010);
    CHECK(s.proof_fresh && !s.degraded && !s.worker_stalled);
}

static void test_reconnect_invalidates_old_tokens_and_proof(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    mqtt_health_token_t old = begin(&h, 0);
    CHECK(mqtt_health_probe_admitted(&h, old, 1));
    mqtt_health_on_disconnected(&h, 2);
    CHECK(!mqtt_health_probe_ack(&h, old, 3));
    mqtt_health_snapshot_t s = snapshot(&h, 3);
    CHECK(!s.connected && !s.proof_fresh && !s.probe_pending && !s.degraded);
    mqtt_health_on_connected(&h, 4);
    mqtt_health_token_t current = begin(&h, 4);
    CHECK(current.generation != old.generation);
    CHECK(current.sequence != old.sequence);
    CHECK(!mqtt_health_probe_admitted(&h, old, 4));
    CHECK(!mqtt_health_probe_ack(&h, old, 4));
    CHECK(mqtt_health_probe_admitted(&h, current, 5));
    CHECK(mqtt_health_probe_ack(&h, current, 6));
    CHECK(snapshot(&h, 6).proof_fresh);
    mqtt_health_on_connected(&h, 7);
    s = snapshot(&h, 7);
    CHECK(s.connected && s.probe_due && !s.proof_fresh && !s.degraded);
    CHECK(!mqtt_health_probe_ack(&h, current, 7));
}

static void test_explicit_failure_event_loss_and_delayed_poll(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    mqtt_health_token_t token = begin(&h, 0);
    CHECK(mqtt_health_probe_failed(&h, token, 100));
    mqtt_health_snapshot_t s = snapshot(&h, 100);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_PROBE_FAILED);
    CHECK(s.degraded && !s.proof_fresh && !s.probe_due);
    CHECK(!mqtt_health_probe_failed(&h, token, 100));
    token = begin(&h, 120000);
    CHECK(mqtt_health_probe_admitted(&h, token, 120001));
    CHECK(mqtt_health_probe_ack(&h, token, 120002));
    mqtt_health_on_event_loss(&h, 120003);
    s = snapshot(&h, 120003);
    CHECK(s.failure == MQTT_HEALTH_FAILURE_EVENT_LOSS);
    CHECK(s.degraded && !s.proof_fresh);

    /* A missed hour makes one new window due, not a burst of old work. */
    s = snapshot(&h, 3600000);
    CHECK(s.probe_due && s.degraded);
    CHECK(s.next_probe_due_ms == 3600000);
    token = begin(&h, 3600000);
    mqtt_health_token_t extra;
    CHECK(!mqtt_health_begin_probe(&h, 3600000, &extra));
    mqtt_health_on_event_loss(&h, 3600001);
    CHECK(!mqtt_health_probe_admitted(&h, token, 3600002));
    CHECK(!mqtt_health_probe_ack(&h, token, 3600002));
}

static void test_ota_pause_requires_new_proof(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_on_connected(&h, 0);
    mqtt_health_token_t token = begin(&h, 0);
    CHECK(mqtt_health_probe_admitted(&h, token, 1));
    CHECK(mqtt_health_probe_ack(&h, token, 2));
    CHECK(snapshot(&h, 2).proof_fresh);
    mqtt_health_set_ota_active(&h, 3, true);
    mqtt_health_snapshot_t s = snapshot(&h, 3600000);
    CHECK(s.ota_active && !s.proof_fresh && !s.probe_due && !s.degraded);
    CHECK(s.last_ack_ms == 2);  /* No fabricated ACK or shifted proof time. */
    CHECK(!mqtt_health_probe_ack(&h, token, 3600000));
    CHECK(!mqtt_health_begin_probe(&h, 3600000, &token));
    mqtt_health_set_ota_active(&h, 3600000, false);
    s = snapshot(&h, 3600000);
    CHECK(!s.ota_active && s.probe_due && !s.proof_fresh && !s.degraded);
    CHECK(s.probe_deadline_ms == 3660000);
    /* Repeating the state does not renew the resume grace. */
    mqtt_health_set_ota_active(&h, 3659999, false);
    CHECK(snapshot(&h, 3660000).degraded);

    mqtt_health_on_connected(&h, 4000000);
    mqtt_health_token_t before_pause = begin(&h, 4000000);
    CHECK(mqtt_health_probe_admitted(&h, before_pause, 4000001));
    mqtt_health_set_ota_active(&h, 4000002, true);
    mqtt_health_on_disconnected(&h, 4000003);
    mqtt_health_on_connected(&h, 4000004);
    CHECK(!snapshot(&h, 5000000).probe_due);
    mqtt_health_set_ota_active(&h, 5000001, false);
    token = begin(&h, 5000001);
    CHECK(!mqtt_health_probe_ack(&h, before_pause, 5000001));
    CHECK(mqtt_health_probe_admitted(&h, token, 5000002));
    CHECK(mqtt_health_probe_ack(&h, token, 5000003));
    CHECK(snapshot(&h, 5000003).proof_fresh);
}

static void test_worker_progress_is_independent(void)
{
    mqtt_health_t h = new_health(0);
    mqtt_health_worker_begin(&h, 1000);
    CHECK(!snapshot(&h, 30999).worker_stalled);
    /* Repeated begin cannot act as an unrelated heartbeat. */
    mqtt_health_worker_begin(&h, 30999);
    mqtt_health_snapshot_t s = snapshot(&h, 31000);
    CHECK(s.worker_busy && s.worker_stalled);
    CHECK(!s.connected && !s.degraded); /* Distinct diagnosis, no reset action. */
    mqtt_health_set_ota_active(&h, 31001, true);
    CHECK(snapshot(&h, 31002).worker_stalled);
    mqtt_health_worker_checkpoint(&h, 31003);
    CHECK(!snapshot(&h, 31003).worker_stalled);
    CHECK(snapshot(&h, 61003).worker_stalled);
    mqtt_health_worker_end(&h, 61004);
    s = snapshot(&h, UINT64_C(10000000000));
    CHECK(!s.worker_busy && !s.worker_stalled);
}

static void test_64_bit_clock_and_saturation(void)
{
    const uint64_t base = UINT64_C(1) << 40;
    mqtt_health_t h = new_health(base);
    mqtt_health_on_connected(&h, base);
    mqtt_health_token_t token = begin(&h, base);
    CHECK(mqtt_health_probe_admitted(&h, token, base + 1));
    CHECK(mqtt_health_probe_ack(&h, token, base + 2));
    mqtt_health_snapshot_t s = snapshot(&h, base + 120000);
    CHECK(s.proof_fresh && s.probe_due);
    CHECK(s.probe_deadline_ms == base + 180000);

    h = new_health(UINT64_MAX - 1000);
    mqtt_health_on_connected(&h, UINT64_MAX - 1000);
    s = snapshot(&h, UINT64_MAX - 1);
    CHECK(s.probe_deadline_ms == UINT64_MAX);
    CHECK(s.probe_due && !s.degraded);
    s = snapshot(&h, UINT64_MAX);
    CHECK(!s.probe_due && s.degraded && !s.proof_fresh);
    CHECK(!mqtt_health_begin_probe(&h, UINT64_MAX, &token));
}

int main(void)
{
    test_configuration_and_idle();
    test_initial_grace_and_missing_scheduler();
    test_ack_ordering_and_unknown_tokens();
    test_admission_and_ack_deadlines();
    test_periodic_freshness_without_producer_traffic();
    test_reconnect_invalidates_old_tokens_and_proof();
    test_explicit_failure_event_loss_and_delayed_poll();
    test_ota_pause_requires_new_proof();
    test_worker_progress_is_independent();
    test_64_bit_clock_and_saturation();
    printf("mqtt_health: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
