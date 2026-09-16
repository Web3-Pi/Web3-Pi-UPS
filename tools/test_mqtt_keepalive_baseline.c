/* Regression baseline from the issue #15 controlled reproduction.
 * It deliberately checks the old failure before testing the fixed service.
 * Controlled host boundaries around verbatim esp-mqtt 1.0.0 functions.
 * Protocol encoding/parsing, outbox and CONNECTED switch arm are real SDK code.
 * Transport delays/availability and callbacks are controlled by this harness.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_msg.h"
#include "mqtt_outbox.h"

#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGV(...) ((void)0)
#define MQTT_STATE_CONNECTED 1
#define MQTT_STATE_WAIT_RECONNECT 2
#define MQTT_STATE_INIT 3
#define MQTT_EVENT_DISCONNECTED 1
#define MQTT_EVENT_DATA 2
#define MQTT_EVENT_PUBLISHED 3
#define MQTT_EVENT_UNSUBSCRIBED 4
#define DISCONNECT_BIT 2
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT -100
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -101
typedef void *esp_transport_handle_t;

typedef struct {
    int network_timeout_ms, reconnect_timeout_ms, message_retransmit_timeout;
    int refresh_connection_after_ms;
} config_t;
typedef struct {
    config_t *config;
    struct {
        mqtt_connection_t connection;
        uint16_t pending_msg_id;
        int pending_msg_type, pending_publish_qos, message_length;
        uint8_t *in_buffer;
        size_t in_buffer_read_len, in_buffer_length;
    } mqtt_state;
    outbox_handle_t outbox;
    void *transport;
    int state, status_bits, wait_timeout_ms;
    struct {
        int event_id, msg_id, qos, dup, retain;
        int total_data_len, data_len, current_data_offset, topic_len;
        char *data, *topic;
    } event;
    bool wait_for_ping_resp;
    uint64_t keepalive_tick, reconnect_tick, refresh_connection_tick;
} client_t;
typedef client_t *esp_mqtt_client_handle_t;

static uint64_t now_ms, last_retransmit, incoming_ready_ms, first_data_ms;
static uint8_t incoming[4096], input_buffer[1024], output_buffer[1024];
static size_t incoming_len, incoming_pos;
static int writes, write_delays[16], delivered, disconnected, published, lock_depth;
static bool closed;
static config_t config;
static client_t client;

#define MQTT_API_LOCK(client) ((void)(client), ++lock_depth)
#define MQTT_API_UNLOCK(client) do { (void)(client); assert(lock_depth > 0); --lock_depth; } while (0)
static uint64_t platform_tick_get_ms(void) { return now_ms; }
static int esp_transport_write(void *transport, const char *data, int length, int timeout)
{
    (void)transport; (void)data;
    assert(!closed && writes < 16);
    assert(write_delays[writes] < timeout);
    now_ms += (uint64_t)write_delays[writes++];
    return length;
}
static int esp_transport_read(void *transport, char *buffer, int length, int timeout)
{
    (void)transport; (void)timeout;
    if (closed) return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    if (now_ms < incoming_ready_ms || incoming_pos == incoming_len)
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    size_t count = incoming_len - incoming_pos;
    if (count > (size_t)length) count = (size_t)length;
    memcpy(buffer, incoming + incoming_pos, count);
    incoming_pos += count;
    return (int)count;
}
static void esp_transport_close(void *transport) { (void)transport; closed = true; }
static int esp_transport_poll_read(void *transport, int timeout)
{
    (void)transport; (void)timeout;
    assert(!closed);
    return now_ms >= incoming_ready_ms && incoming_pos < incoming_len;
}
static int max_poll_timeout(esp_mqtt_client_handle_t c, int timeout) { (void)c; return timeout; }
static esp_err_t esp_mqtt_dispatch_event(esp_mqtt_client_handle_t c)
{
    if (c->event.event_id == MQTT_EVENT_DATA) {
        if (!delivered) first_data_ms = now_ms;
        delivered++;
        assert(c->event.data_len == c->event.total_data_len);
    }
    return ESP_OK;
}
static void esp_mqtt_dispatch_event_with_msgid(esp_mqtt_client_handle_t c)
{
    if (c->event.event_id == MQTT_EVENT_DISCONNECTED) disconnected++;
    if (c->event.event_id == MQTT_EVENT_PUBLISHED) published++;
}
static void esp_mqtt_client_dispatch_transport_error(esp_mqtt_client_handle_t c)
{ (void)c; assert(!"Unexpected transport error"); }
static unsigned xEventGroupWaitBits(int bits, int requested, bool clear, bool all, int timeout)
{ (void)bits; (void)requested; (void)clear; (void)all; (void)timeout; return 0; }
static void send_disconnect_msg(esp_mqtt_client_handle_t c) { (void)c; }
static esp_err_t deliver_suback(esp_mqtt_client_handle_t c) { (void)c; return ESP_OK; }
static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t c);
static esp_err_t esp_mqtt_client_ping(esp_mqtt_client_handle_t c);

#include "sdk_keepalive_excerpts.inc"

static void setup(uint64_t tick, bool waiting)
{
    memset(&client, 0, sizeof(client));
    memset(&config, 0, sizeof(config));
    memset(write_delays, 0, sizeof(write_delays));
    now_ms = tick; last_retransmit = tick; incoming_ready_ms = 0; first_data_ms = 0;
    incoming_len = incoming_pos = 0;
    writes = delivered = disconnected = published = lock_depth = 0;
    closed = false;
    config.network_timeout_ms = 15000;
    config.reconnect_timeout_ms = 10000;
    config.message_retransmit_timeout = 1000;
    client.config = &config;
    client.state = MQTT_STATE_CONNECTED;
    client.wait_for_ping_resp = waiting;
    client.mqtt_state.connection.buffer = output_buffer;
    client.mqtt_state.connection.buffer_length = sizeof(output_buffer);
    client.mqtt_state.connection.information.keepalive = 60;
    client.mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    client.mqtt_state.in_buffer = input_buffer;
    client.mqtt_state.in_buffer_length = sizeof(input_buffer);
    client.outbox = outbox_init();
    assert(client.outbox);
}
static void finish(void) { assert(!lock_depth); outbox_destroy(client.outbox); }
static void append_bytes(const uint8_t *data, size_t len)
{
    assert(incoming_len + len <= sizeof(incoming));
    memcpy(incoming + incoming_len, data, len); incoming_len += len;
}
static void append_pingresp(void) { const uint8_t packet[] = {0xd0, 0}; append_bytes(packet, 2); }
static void append_command(size_t payload_size)
{
    uint8_t buffer[1024], payload[400] = {0};
    mqtt_connection_t connection = {.buffer=buffer, .buffer_length=sizeof(buffer)};
    uint16_t id;
    assert(payload_size <= sizeof(payload));
    mqtt_message_t *message = mqtt_msg_publish(&connection,
        "audit/device/cmd/request", (char *)payload, (int)payload_size, 1, 0, &id);
    assert(message->length);
    append_bytes(message->data, message->length);
}
static void queued_publication(pending_state_t pending)
{
    const uint8_t packet[] = {0x32, 0x06, 0, 1, 't', 0, 7, 'x'};
    outbox_message_t message = {.data=(uint8_t *)packet, .len=sizeof(packet), .msg_id=7,
        .msg_type=MQTT_MSG_TYPE_PUBLISH, .msg_qos=1};
    assert(outbox_enqueue(client.outbox, &message, now_ms));
    assert(outbox_set_pending(client.outbox, 7, pending) == ESP_OK);
}

int main(void)
{
    /* Real MQTT parser accepts the 250-byte PUBLISH payload within its buffer. */
    setup(1000, false); append_command(250); run_connected_iteration(&client);
    assert(delivered == 1 && !closed && client.event.data_len == 250);
    puts("CONTROL: 250-byte PUBLISH payload delivered in one DATA event"); finish();

    /* Identical coalesced input with enough deadline remaining is healthy. */
    setup(59000, true); append_command(110); append_pingresp(); write_delays[0] = 200;
    run_connected_iteration(&client); run_connected_iteration(&client);
    assert(delivered == 1 && !closed && !client.wait_for_ping_resp && incoming_pos == incoming_len);
    puts("CONTROL: PUBLISH followed by buffered PINGRESP before deadline stays connected"); finish();

    /* No loss: both complete MQTT packets already available before deadline.
     * The successful PUBACK write crosses it. SDK discards the unread PINGRESP. */
    setup(59900, true); append_command(110); append_pingresp(); write_delays[0] = 200;
    run_connected_iteration(&client);
    assert(delivered == 1 && disconnected == 1 && closed && incoming_len - incoming_pos == 2);
    printf("REPRODUCED: command dispatched at %llu ms; disconnected at %llu ms; unread PINGRESP=%zu bytes\n",
        (unsigned long long)first_data_ms, (unsigned long long)now_ms, incoming_len - incoming_pos);
    finish();

    /* Same deadline, write cost and packets; putting PINGRESP first avoids abort. */
    setup(59900, true); append_pingresp(); append_command(110); write_delays[0] = 200;
    run_connected_iteration(&client); run_connected_iteration(&client);
    assert(delivered == 1 && !closed && !client.wait_for_ping_resp && incoming_pos == incoming_len);
    puts("CONTROL: PINGRESP first avoids disconnect with the same 200-ms successful write"); finish();

    /* A just-processed genuine MQTT PUBACK does not reset the PING deadline. */
    setup(60010, true); queued_publication(TRANSMITTED);
    const uint8_t ack[] = {0x40, 2, 0, 7}; append_bytes(ack, sizeof(ack));
    run_connected_iteration(&client);
    assert(published == 1 && outbox_get_size(client.outbox) == 0 && closed);
    puts("REPRODUCED: valid PUBACK processed and outbox cleared, then No-PING disconnect"); finish();

    /* The deadline is tied to the older tick, not successful PING write completion. */
    setup(30000, false); write_delays[0] = 14000;
    run_connected_iteration(&client);
    assert(now_ms == 44000 && client.wait_for_ping_resp && !closed);
    now_ms = 60000; run_connected_iteration(&client);
    assert(closed);
    puts("REPRODUCED: PING write completes at 44000 ms; disconnect at 60000 ms (16 s later)"); finish();

    /* Incoming command waits while the same task performs two successful writes. */
    setup(30000, false); queued_publication(QUEUED);
    append_command(110); incoming_ready_ms = 30001;
    write_delays[0] = 13000; write_delays[1] = 9000;
    run_connected_iteration(&client);
    assert(now_ms == 52000 && !delivered && !closed && writes == 2);
    run_connected_iteration(&client);
    assert(delivered == 1 && first_data_ms == 52000 && !closed);
    puts("REPRODUCED: ready command waits 21999 ms behind successful 13-s and 9-s writes"); finish();
    /* A genuinely missing response must still cause a finite disconnect. */
    setup(30000, false); run_connected_iteration(&client);
    assert(client.wait_for_ping_resp && !closed);
    now_ms = 59999; run_connected_iteration(&client); assert(!closed);
    now_ms = 60000; run_connected_iteration(&client); assert(closed && disconnected == 1);
    puts("CONTROL: missing PINGRESP still reaches a finite disconnect deadline"); finish();
    puts("ALL SCENARIO ASSERTIONS PASSED; controlled transport and clock");
    return 0;
}
