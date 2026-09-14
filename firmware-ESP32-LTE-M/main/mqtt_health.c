#include "mqtt_health.h"

#include <stddef.h>
#include <string.h>

static uint64_t observe_time(mqtt_health_t *h, uint64_t now)
{
    if (now > h->now_ms) h->now_ms = now;
    return h->now_ms;
}

static uint64_t add_saturated(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static void clear_pending(mqtt_health_t *h)
{
    h->probe_pending = false;
    h->probe_admitted = false;
    h->pending_token = (mqtt_health_token_t){0};
}

static void schedule(mqtt_health_t *h, uint64_t due)
{
    h->next_probe_due_ms = due;
    h->probe_deadline_ms = add_saturated(due, h->config.probe_deadline_ms);
}

static void schedule_next(mqtt_health_t *h, uint64_t now)
{
    uint64_t due = add_saturated(h->next_probe_due_ms,
                                  h->config.probe_interval_ms);
    schedule(h, due < now ? now : due);
}

static void fail_window(mqtt_health_t *h, uint64_t now,
                        mqtt_health_failure_t reason)
{
    h->proof_valid = false;
    h->failure = reason;
    clear_pending(h);
    schedule_next(h, now);
}

static uint64_t advance(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    if (h->connected && !h->ota_active && now >= h->probe_deadline_ms) {
        fail_window(h, now, h->probe_admitted
                    ? MQTT_HEALTH_FAILURE_ACK_TIMEOUT
                    : MQTT_HEALTH_FAILURE_ADMISSION_TIMEOUT);
    }
    return now;
}

static bool matches(const mqtt_health_t *h, mqtt_health_token_t token)
{
    return h->connected && !h->ota_active && h->probe_pending &&
           token.generation == h->pending_token.generation &&
           token.sequence == h->pending_token.sequence;
}

mqtt_health_config_t mqtt_health_default_config(void)
{
    return (mqtt_health_config_t){120000, 60000, 30000};
}

bool mqtt_health_init(mqtt_health_t *h, uint64_t now,
                      const mqtt_health_config_t *config)
{
    if (!h) return false;
    mqtt_health_config_t cfg = config ? *config : mqtt_health_default_config();
    memset(h, 0, sizeof(*h));
    if (!cfg.probe_deadline_ms || !cfg.worker_stall_ms ||
        cfg.probe_interval_ms < cfg.probe_deadline_ms) return false;
    h->config = cfg;
    h->now_ms = now;
    return true;
}

void mqtt_health_on_connected(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    /* sequence never resets or wraps, even if generation saturates. */
    if (h->generation != UINT64_MAX) h->generation++;
    h->connected = true;
    h->proof_valid = false;
    h->failure = MQTT_HEALTH_FAILURE_NONE;
    clear_pending(h);
    schedule(h, now);
}

void mqtt_health_on_disconnected(mqtt_health_t *h, uint64_t now)
{
    (void)observe_time(h, now);
    h->connected = false;
    h->proof_valid = false;
    h->failure = MQTT_HEALTH_FAILURE_NONE;
    clear_pending(h);
    h->next_probe_due_ms = 0;
    h->probe_deadline_ms = 0;
}

void mqtt_health_poll(mqtt_health_t *h, uint64_t now,
                      mqtt_health_snapshot_t *out)
{
    now = advance(h, now);
    if (!out) return;
    *out = (mqtt_health_snapshot_t){
        .connected = h->connected,
        .ota_active = h->ota_active,
        .probe_due = h->connected && !h->ota_active && !h->probe_pending &&
                     now >= h->next_probe_due_ms && now < h->probe_deadline_ms,
        .probe_pending = h->probe_pending,
        .probe_admitted = h->probe_admitted,
        .proof_fresh = h->connected && !h->ota_active && h->proof_valid,
        .degraded = h->failure != MQTT_HEALTH_FAILURE_NONE,
        .worker_busy = h->worker_busy,
        .worker_stalled = h->worker_busy &&
                          now - h->worker_checkpoint_ms >= h->config.worker_stall_ms,
        .failure = h->failure,
        .generation = h->generation,
        .next_probe_due_ms = h->next_probe_due_ms,
        .probe_deadline_ms = h->probe_deadline_ms,
        .last_ack_ms = h->last_ack_ms,
        .worker_checkpoint_ms = h->worker_checkpoint_ms,
        .pending_token = h->pending_token,
    };
}

bool mqtt_health_begin_probe(mqtt_health_t *h, uint64_t now,
                             mqtt_health_token_t *token)
{
    now = advance(h, now);
    if (!token || !h->connected || h->ota_active || h->probe_pending ||
        now < h->next_probe_due_ms || now >= h->probe_deadline_ms ||
        h->sequence == UINT64_MAX) return false;
    h->sequence++;
    h->pending_token = (mqtt_health_token_t){h->generation, h->sequence};
    h->probe_pending = true;
    h->probe_admitted = false;
    *token = h->pending_token;
    return true;
}

bool mqtt_health_probe_admitted(mqtt_health_t *h, mqtt_health_token_t token,
                                uint64_t now)
{
    (void)advance(h, now);
    if (!matches(h, token) || h->probe_admitted) return false;
    h->probe_admitted = true;
    return true;
}

bool mqtt_health_probe_ack(mqtt_health_t *h, mqtt_health_token_t token,
                           uint64_t now)
{
    now = advance(h, now);
    if (!matches(h, token) || !h->probe_admitted) return false;
    h->last_ack_ms = now;
    h->proof_valid = true;
    h->failure = MQTT_HEALTH_FAILURE_NONE;
    clear_pending(h);
    schedule_next(h, now);
    return true;
}

bool mqtt_health_probe_failed(mqtt_health_t *h, mqtt_health_token_t token,
                              uint64_t now)
{
    now = advance(h, now);
    if (!matches(h, token)) return false;
    fail_window(h, now, MQTT_HEALTH_FAILURE_PROBE_FAILED);
    return true;
}

void mqtt_health_on_event_loss(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    h->proof_valid = false;
    clear_pending(h);
    if (h->connected && !h->ota_active) {
        h->failure = MQTT_HEALTH_FAILURE_EVENT_LOSS;
        schedule_next(h, now);
    }
}

void mqtt_health_set_ota_active(mqtt_health_t *h, uint64_t now, bool active)
{
    now = observe_time(h, now);
    if (h->ota_active == active) return;
    h->ota_active = active;
    h->proof_valid = false;
    h->failure = MQTT_HEALTH_FAILURE_NONE;
    clear_pending(h);
    if (h->connected && !active) schedule(h, now);
}

void mqtt_health_worker_begin(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    if (!h->worker_busy) {
        h->worker_busy = true;
        h->worker_checkpoint_ms = now;
    }
}

void mqtt_health_worker_checkpoint(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    if (h->worker_busy) h->worker_checkpoint_ms = now;
}

void mqtt_health_worker_end(mqtt_health_t *h, uint64_t now)
{
    now = observe_time(h, now);
    h->worker_busy = false;
    h->worker_checkpoint_ms = now;
}
