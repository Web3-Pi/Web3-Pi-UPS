#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure bounded queue. The caller serializes EVERY operation, including
 * snapshots, with its own short lock. No SDK, task, lock or heap dependency.
 * Times are absolute monotonic milliseconds; zero deadline means no expiry. */
#define MQTT_DISPATCH_CAPACITY 32u
#define MQTT_DISPATCH_CRITICAL_RESERVE 8u
#define MQTT_DISPATCH_TOPIC_CAPACITY 64u
#define MQTT_DISPATCH_PAYLOAD_CAPACITY 256u
#define MQTT_DISPATCH_CRITICAL_BURST 8u

typedef enum {
    MQTT_DISPATCH_ACCEPTED,
    MQTT_DISPATCH_FULL,
    MQTT_DISPATCH_TOO_LARGE,
    MQTT_DISPATCH_INVALID
} mqtt_dispatch_result_t;

typedef struct {
    const char *topic;
    const void *payload;
    size_t payload_len;
    int qos;
    bool retain;
    bool critical;
    uint32_t coalesce_key; /* zero: opaque FIFO; critical items never coalesce */
    uint64_t deadline_ms;
} mqtt_dispatch_request_t;

typedef struct {
    char topic[MQTT_DISPATCH_TOPIC_CAPACITY];
    uint8_t payload[MQTT_DISPATCH_PAYLOAD_CAPACITY];
    size_t payload_len;
    int qos;
    bool retain;
    bool critical;
    uint32_t coalesce_key;
    uint64_t token; /* application receipt, never an MQTT packet ID */
    uint64_t deadline_ms;
} mqtt_dispatch_item_t;

typedef struct {
    uint64_t accepted; /* includes successful coalescing submissions */
    uint64_t coalesced;
    uint64_t full;
    uint64_t expired;
    uint64_t dropped; /* explicit owner drops; excludes expiry/coalescing */
    uint32_t depth; /* includes the single in-flight item */
    uint32_t highwater;
} mqtt_dispatch_stats_t;

typedef enum {
    MQTT_DISPATCH_SLOT_FREE,
    MQTT_DISPATCH_SLOT_QUEUED,
    MQTT_DISPATCH_SLOT_INFLIGHT
} mqtt_dispatch_slot_state_t;

typedef struct {
    mqtt_dispatch_item_t item;
    uint64_t order;
    uint64_t retry_after_ms;
    mqtt_dispatch_slot_state_t state;
} mqtt_dispatch_slot_t;

typedef struct {
    mqtt_dispatch_slot_t slots[MQTT_DISPATCH_CAPACITY];
    mqtt_dispatch_stats_t counters;
    uint64_t last_token;
    unsigned critical_streak;
} mqtt_dispatch_queue_t;

typedef struct {
    size_t slot;
    mqtt_dispatch_item_t item; /* complete owned copy, safe after unlocking */
} mqtt_dispatch_claim_t;

typedef enum {
    MQTT_DISPATCH_SENT, /* SDK accepted ownership, not broker confirmation */
    MQTT_DISPATCH_RETRY,
    MQTT_DISPATCH_DROP
} mqtt_dispatch_finish_t;

void mqtt_dispatch_init(mqtt_dispatch_queue_t *q);

/* Copies topic/payload. Output tokens are zero on rejection. On coalescing,
 * replaced_token identifies the superseded receipt; queue position remains
 * unchanged. At most 24 noncritical entries can occupy the 32-slot pool. */
mqtt_dispatch_result_t mqtt_dispatch_submit(
    mqtt_dispatch_queue_t *q, const mqtt_dispatch_request_t *request,
    uint64_t *token, uint64_t *replaced_token);

/* Remove expired QUEUED items and optionally return their receipt tokens.
 * expired_tokens, when non-NULL, must have MQTT_DISPATCH_CAPACITY elements.
 * Call before claim/admission to reclaim capacity and complete receipts.
 * An in-flight SDK call owns its lifetime until finish; it is not reclaimed. */
size_t mqtt_dispatch_expire(mqtt_dispatch_queue_t *q, uint64_t now_ms,
                            uint64_t *expired_tokens);

/* FIFO within each priority class, including delayed retries. A delayed head
 * blocks its class, not the other class. At most eight critical claims precede
 * an eligible noncritical head. Expired queued entries are skipped. */
bool mqtt_dispatch_claim(mqtt_dispatch_queue_t *q, uint64_t now_ms,
                         mqtt_dispatch_claim_t *claim);

/* Completion is guarded by BOTH slot and token. RETRY preserves queue order
 * and defers until retry_after_ms; if its deadline passed, it expires instead.
 * SENT removes even after deadline: SDK already accepted it; do not duplicate. */
bool mqtt_dispatch_finish(mqtt_dispatch_queue_t *q,
                          const mqtt_dispatch_claim_t *claim,
                          mqtt_dispatch_finish_t result, uint64_t now_ms,
                          uint64_t retry_after_ms);

void mqtt_dispatch_stats(const mqtt_dispatch_queue_t *q,
                         mqtt_dispatch_stats_t *stats);

#ifdef __cplusplus
}
#endif
