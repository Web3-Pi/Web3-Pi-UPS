/* Web3-Pi bounded MQTT service. SPDX-License-Identifier: Apache-2.0 */
#include "mqtt_service.h"
#include <stdlib.h>
#include <string.h>

bool mqtt_service_push(mqtt_service_t *s, const void *data, size_t length,
                       uint16_t id, uint8_t type, uint8_t qos, bool outbox,
                       uint64_t deadline_ms)
{
    if (!length || length > MQTT_SERVICE_TX_BYTES ||
        s->count >= MQTT_SERVICE_TX_FRAMES || length > MQTT_SERVICE_TX_BYTES - s->bytes) return false;
    mqtt_service_frame_t *f = malloc(sizeof(*f) + length);
    if (!f) return false;
    *f = (mqtt_service_frame_t){.length = length, .deadline_ms = deadline_ms,
                               .id = id, .type = type, .qos = qos, .outbox = outbox};
    memcpy(f->data, data, length);
    if (s->tail) s->tail->next = f;
    else s->head = f;
    s->tail = f;
    s->bytes += length;
    ++s->count;
    return true;
}

bool mqtt_service_contains(const mqtt_service_t *s, uint16_t id, uint8_t type)
{
    for (const mqtt_service_frame_t *f = s->head; f; f = f->next)
        if (f->outbox && f->id == id && f->type == type) return true;
    return false;
}

void mqtt_service_pop(mqtt_service_t *s)
{
    mqtt_service_frame_t *f = s->head;
    if (!f) return;
    s->head = f->next;
    if (!s->head) s->tail = NULL;
    s->bytes -= f->length;
    --s->count;
    free(f);
}

void mqtt_service_clear(mqtt_service_t *s)
{
    while (s->head) mqtt_service_pop(s);
    memset(s, 0, sizeof(*s));
}
