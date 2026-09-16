/* Web3-Pi bounded MQTT service. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_SERVICE_SLICE_MS 10u
#define MQTT_SERVICE_IDLE_MS 10u
#define MQTT_SERVICE_RX_PACKETS 8u
#define MQTT_SERVICE_PING_GRACE_MS 200u
#define MQTT_SERVICE_PING_GRACE_PACKETS 64u
#define MQTT_SERVICE_TX_BYTES (32u * 1024u)
#define MQTT_SERVICE_TX_FRAMES (MQTT_SERVICE_PING_GRACE_PACKETS + 4u)
#define MQTT_SERVICE_NORMAL_TX_FRAMES 2u
#define MQTT_SERVICE_CONTROL_RESERVE 512u
#define MQTT_SERVICE_RX_BYTES (16u * 1024u)
#define MQTT_SERVICE_WRITE_CHUNK 1024u

typedef struct mqtt_service_frame {
    struct mqtt_service_frame *next;
    uint64_t deadline_ms;
    size_t length, offset, retry_length;
    uint16_t id;
    uint8_t type, qos;
    bool outbox;
    uint8_t data[];
} mqtt_service_frame_t;

typedef struct {
    mqtt_service_frame_t *head, *tail;
    size_t bytes;
    unsigned count;
    uint64_t rx_deadline_ms, connect_deadline_ms, last_tx_ms;
    uint64_t ping_sent_ms, ping_deadline_ms, ping_grace_deadline_ms;
    uint64_t last_retransmit_ms;
    unsigned ping_grace_packets;
    unsigned connect_phase;
    bool ping_queued, rx_progress;
} mqtt_service_t;

/* Own a copy, so the encoder/outbox may be changed or freed independently.
 * Deadline includes time queued. Positive progress never extends it. */
bool mqtt_service_push(mqtt_service_t *s, const void *data, size_t length,
                       uint16_t id, uint8_t type, uint8_t qos, bool outbox,
                       uint64_t deadline_ms);
bool mqtt_service_contains(const mqtt_service_t *s, uint16_t id, uint8_t type);
void mqtt_service_pop(mqtt_service_t *s);
void mqtt_service_clear(mqtt_service_t *s);
