#include "mqtt_dispatch_queue.h"

#include <string.h>

static bool expired(const mqtt_dispatch_item_t *item, uint64_t now_ms)
{
    return item->deadline_ms != 0 && item->deadline_ms <= now_ms;
}

static void release_slot(mqtt_dispatch_queue_t *q, size_t index)
{
    memset(&q->slots[index], 0, sizeof(q->slots[index]));
    --q->counters.depth;
}

void mqtt_dispatch_init(mqtt_dispatch_queue_t *q)
{
    if (q) memset(q, 0, sizeof(*q));
}

mqtt_dispatch_result_t mqtt_dispatch_submit(
    mqtt_dispatch_queue_t *q, const mqtt_dispatch_request_t *request,
    uint64_t *token, uint64_t *replaced_token)
{
    if (token) *token = 0;
    if (replaced_token) *replaced_token = 0;
    if (!q || !request || !request->topic || request->topic[0] == '\0' ||
        (!request->payload && request->payload_len != 0) ||
        request->qos < 0 || request->qos > 2 || q->last_token == UINT64_MAX)
        return MQTT_DISPATCH_INVALID;

    size_t topic_len = 0;
    while (topic_len < MQTT_DISPATCH_TOPIC_CAPACITY && request->topic[topic_len])
        ++topic_len;
    if (topic_len == MQTT_DISPATCH_TOPIC_CAPACITY ||
        request->payload_len > MQTT_DISPATCH_PAYLOAD_CAPACITY)
        return MQTT_DISPATCH_TOO_LARGE;

    size_t free_slot = MQTT_DISPATCH_CAPACITY;
    size_t replacement = MQTT_DISPATCH_CAPACITY;
    unsigned noncritical = 0;
    for (size_t i = 0; i < MQTT_DISPATCH_CAPACITY; ++i) {
        const mqtt_dispatch_slot_t *slot = &q->slots[i];
        if (slot->state == MQTT_DISPATCH_SLOT_FREE) {
            if (free_slot == MQTT_DISPATCH_CAPACITY) free_slot = i;
            continue;
        }
        if (!slot->item.critical) ++noncritical;
        if (slot->state == MQTT_DISPATCH_SLOT_QUEUED && !request->critical &&
            !slot->item.critical && request->coalesce_key != 0 &&
            slot->item.coalesce_key == request->coalesce_key &&
            slot->item.qos == request->qos && slot->item.retain == request->retain &&
            strcmp(slot->item.topic, request->topic) == 0)
            replacement = i;
    }

    size_t index = replacement == MQTT_DISPATCH_CAPACITY ? free_slot : replacement;
    if (index == MQTT_DISPATCH_CAPACITY ||
        (replacement == MQTT_DISPATCH_CAPACITY && !request->critical &&
         noncritical >= MQTT_DISPATCH_CAPACITY - MQTT_DISPATCH_CRITICAL_RESERVE)) {
        ++q->counters.full;
        return MQTT_DISPATCH_FULL;
    }

    mqtt_dispatch_slot_t *slot = &q->slots[index];
    if (replacement != MQTT_DISPATCH_CAPACITY) {
        if (replaced_token) *replaced_token = slot->item.token;
        ++q->counters.coalesced;
    } else {
        ++q->counters.depth;
        if (q->counters.depth > q->counters.highwater)
            q->counters.highwater = q->counters.depth;
        slot->order = q->last_token + 1;
        slot->retry_after_ms = 0;
    }
    memset(&slot->item, 0, sizeof(slot->item));
    memcpy(slot->item.topic, request->topic, topic_len + 1);
    if (request->payload_len)
        memcpy(slot->item.payload, request->payload, request->payload_len);
    slot->item.payload_len = request->payload_len;
    slot->item.qos = request->qos;
    slot->item.retain = request->retain;
    slot->item.critical = request->critical;
    slot->item.coalesce_key = request->coalesce_key;
    slot->item.deadline_ms = request->deadline_ms;
    slot->item.token = ++q->last_token;
    slot->state = MQTT_DISPATCH_SLOT_QUEUED;
    ++q->counters.accepted;
    if (token) *token = slot->item.token;
    return MQTT_DISPATCH_ACCEPTED;
}

size_t mqtt_dispatch_expire(mqtt_dispatch_queue_t *q, uint64_t now_ms,
                            uint64_t *expired_tokens)
{
    if (!q) return 0;
    size_t count = 0;
    for (size_t i = 0; i < MQTT_DISPATCH_CAPACITY; ++i) {
        if (q->slots[i].state != MQTT_DISPATCH_SLOT_QUEUED ||
            !expired(&q->slots[i].item, now_ms)) continue;
        if (expired_tokens) expired_tokens[count] = q->slots[i].item.token;
        ++count;
        ++q->counters.expired;
        release_slot(q, i);
    }
    return count;
}

bool mqtt_dispatch_claim(mqtt_dispatch_queue_t *q, uint64_t now_ms,
                         mqtt_dispatch_claim_t *claim)
{
    if (!q || !claim) return false;
    size_t critical = MQTT_DISPATCH_CAPACITY, other = MQTT_DISPATCH_CAPACITY;
    for (size_t i = 0; i < MQTT_DISPATCH_CAPACITY; ++i) {
        const mqtt_dispatch_slot_t *slot = &q->slots[i];
        if (slot->state == MQTT_DISPATCH_SLOT_INFLIGHT) return false;
        if (slot->state != MQTT_DISPATCH_SLOT_QUEUED || expired(&slot->item, now_ms)) continue;
        size_t *candidate = slot->item.critical ? &critical : &other;
        if (*candidate == MQTT_DISPATCH_CAPACITY || slot->order < q->slots[*candidate].order)
            *candidate = i;
    }
    /* Retry may delay the head, but must never let a younger FIFO item
     * overtake it. The other priority class can still make progress. */
    if (critical != MQTT_DISPATCH_CAPACITY && q->slots[critical].retry_after_ms > now_ms)
        critical = MQTT_DISPATCH_CAPACITY;
    if (other != MQTT_DISPATCH_CAPACITY && q->slots[other].retry_after_ms > now_ms)
        other = MQTT_DISPATCH_CAPACITY;
    size_t index;
    if (critical != MQTT_DISPATCH_CAPACITY &&
        (other == MQTT_DISPATCH_CAPACITY || q->critical_streak < MQTT_DISPATCH_CRITICAL_BURST)) {
        index = critical;
        if (q->critical_streak < MQTT_DISPATCH_CRITICAL_BURST) ++q->critical_streak;
    } else {
        index = other;
        if (index == MQTT_DISPATCH_CAPACITY) return false;
        q->critical_streak = 0;
    }
    q->slots[index].state = MQTT_DISPATCH_SLOT_INFLIGHT;
    claim->slot = index;
    claim->item = q->slots[index].item;
    return true;
}

bool mqtt_dispatch_finish(mqtt_dispatch_queue_t *q,
                          const mqtt_dispatch_claim_t *claim,
                          mqtt_dispatch_finish_t result, uint64_t now_ms,
                          uint64_t retry_after_ms)
{
    if (!q || !claim || claim->slot >= MQTT_DISPATCH_CAPACITY ||
        result < MQTT_DISPATCH_SENT || result > MQTT_DISPATCH_DROP) return false;
    mqtt_dispatch_slot_t *slot = &q->slots[claim->slot];
    if (slot->state != MQTT_DISPATCH_SLOT_INFLIGHT || slot->item.token != claim->item.token)
        return false;
    if (result == MQTT_DISPATCH_RETRY && !expired(&slot->item, now_ms)) {
        slot->state = MQTT_DISPATCH_SLOT_QUEUED;
        slot->retry_after_ms = retry_after_ms;
        return true;
    }
    if (result == MQTT_DISPATCH_RETRY) ++q->counters.expired;
    else if (result == MQTT_DISPATCH_DROP) ++q->counters.dropped;
    release_slot(q, claim->slot);
    return true;
}

void mqtt_dispatch_stats(const mqtt_dispatch_queue_t *q,
                         mqtt_dispatch_stats_t *stats)
{
    if (q && stats) *stats = q->counters;
}
