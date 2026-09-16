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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_msg.h"
#include "mqtt_outbox.h"
#include "mqtt_service.h"
#include "mqtt_config.h"

typedef struct { bool connected; } mqtt_transport_nb_t;

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
#define RECONNECT_BIT 4
#define portTICK_PERIOD_MS 1
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT 0
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -1
#define ERR_TCP_TRANSPORT_CONNECTION_FAILED -2
#define MQTT_STATE_BOUNDED_CONNECT 4
#define pdMS_TO_TICKS(ms) (ms)
typedef void *esp_transport_handle_t;

typedef struct {
    bool bounded_service, auto_reconnect;
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
    mqtt_service_t service;
    mqtt_transport_nb_t nb;
    struct {
        atomic_uint slice_started_ms, slice_completed_ms, max_lock_ms;
        atomic_uint last_progress_ms, rx_remaining_ms, tx_remaining_ms;
        atomic_uint tx_frames, tx_bytes, deadline_failures, operation;
    } service_observation;
    outbox_handle_t outbox;
    void *transport;
    int state, status_bits, wait_timeout_ms;
    struct {
        int event_id, msg_id, qos, dup, retain;
        int total_data_len, data_len, current_data_offset, topic_len;
        char *data, *topic;
    } event;
    bool wait_for_ping_resp, run;
    uint64_t keepalive_tick, reconnect_tick, refresh_connection_tick;
} client_t;
typedef client_t *esp_mqtt_client_handle_t;

static uint64_t now_ms, last_retransmit, incoming_ready_ms, first_data_ms;
static uint8_t incoming[16384], input_buffer[16384], output_buffer[1024];
static size_t incoming_len, incoming_pos;
static int writes, write_delays[16], delivered, disconnected, published, lock_depth;
static bool closed, tx_failure, ciphertext_ready, disconnect_request;
static uint64_t tx_available_ms;
static int tx_chunk, tx_cost_ms, rx_cost_ms;
static size_t visible_len;
static const char *retry_pointer;
static int retry_length;
static bool tls_write_suspended, tx_suspends_tls, tls_read_suspended;
static uint16_t next_incoming_id;
static int reconnect_wait_ticks;
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
{ (void)c; }
static unsigned xEventGroupWaitBits(int bits, int requested, bool clear, bool all, int timeout)
{
    (void)bits; (void)clear; (void)all;
    if (requested == RECONNECT_BIT) { assert(!lock_depth); reconnect_wait_ticks = timeout; }
    return disconnect_request ? (unsigned)requested : 0;
}
static unsigned xEventGroupGetBits(int bits) { (void)bits; return 0; }
static unsigned xEventGroupClearBits(int bits, unsigned mask) { (void)bits; (void)mask; return 0; }
static void send_disconnect_msg(esp_mqtt_client_handle_t c) { (void)c; }
static esp_err_t deliver_suback(esp_mqtt_client_handle_t c) { (void)c; return ESP_OK; }
static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t c);
static esp_err_t esp_mqtt_client_ping(esp_mqtt_client_handle_t c);
static esp_err_t mqtt_bounded_write(esp_mqtt_client_handle_t c);
static esp_err_t mqtt_bounded_service(esp_mqtt_client_handle_t c);
static void vTaskDelay(int ticks) { assert(ticks > 0 && !lock_depth); }

static int mqtt_transport_nb_read(mqtt_transport_nb_t *nb, char *data, int length, bool *started)
{
    (void)nb;
    *started = ciphertext_ready;
    if (closed) return -1;
    if (now_ms < incoming_ready_ms || incoming_pos >= visible_len) return 0;
    *started = true;
    size_t count = visible_len - incoming_pos;
    if (count > (size_t)length) count = (size_t)length;
    memcpy(data, incoming + incoming_pos, count);
    incoming_pos += count;
    now_ms += (uint64_t)rx_cost_ms;
    return (int)count;
}
static bool mqtt_transport_nb_can_read(const mqtt_transport_nb_t *nb)
{ (void)nb; return !tls_write_suspended; }
static bool mqtt_transport_nb_can_write(const mqtt_transport_nb_t *nb)
{ (void)nb; return !tls_read_suspended; }
static int mqtt_transport_nb_write(mqtt_transport_nb_t *nb, const char *data, int length)
{
    (void)nb; (void)data;
    assert(!closed);
    if (retry_pointer) assert(data == retry_pointer && length == retry_length);
    writes++;
    if (tx_failure) return -1;
    if (now_ms < tx_available_ms) {
        retry_pointer = data;
        retry_length = length;
        tls_write_suspended = tx_suspends_tls;
        return 0;
    }
    retry_pointer = NULL;
    tls_write_suspended = false;
    now_ms += (uint64_t)tx_cost_ms;
    return tx_chunk > 0 && tx_chunk < length ? tx_chunk : length;
}
static void mqtt_transport_nb_close(mqtt_transport_nb_t *nb)
{ nb->connected = false; closed = true; }

#include "sdk_keepalive_excerpts.inc"

static void setup(uint64_t tick, bool waiting)
{
    memset(&client, 0, sizeof(client));
    memset(&config, 0, sizeof(config));
    memset(write_delays, 0, sizeof(write_delays));
    now_ms = tick; last_retransmit = tick; incoming_ready_ms = 0; first_data_ms = 0;
    incoming_len = incoming_pos = visible_len = 0;
    writes = delivered = disconnected = published = lock_depth = 0;
    tx_available_ms = 0;
    tx_chunk = tx_cost_ms = rx_cost_ms = 0;
    closed = tx_failure = ciphertext_ready = disconnect_request = false;
    retry_pointer = NULL;
    retry_length = 0;
    tls_write_suspended = tx_suspends_tls = tls_read_suspended = false;
    next_incoming_id = 0;
    reconnect_wait_ticks = -1;
    config.bounded_service = true;
    config.network_timeout_ms = 15000;
    config.reconnect_timeout_ms = 10000;
    config.message_retransmit_timeout = 1000;
    client.config = &config;
    client.state = MQTT_STATE_CONNECTED;
    client.run = true;
    client.nb.connected = true;
    client.wait_for_ping_resp = waiting;
    client.service.last_tx_ms = waiting ? 30000 : 0;
    if (waiting) {
        client.service.ping_sent_ms = 30000;
        client.service.ping_deadline_ms = 60000;
    }
    client.mqtt_state.connection.buffer = output_buffer;
    client.mqtt_state.connection.buffer_length = sizeof(output_buffer);
    client.mqtt_state.connection.information.keepalive = 60;
    client.mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    client.mqtt_state.in_buffer = input_buffer;
    client.mqtt_state.in_buffer_length = sizeof(input_buffer);
    client.outbox = outbox_init();
    assert(client.outbox);
}

static void finish(void)
{
    assert(!lock_depth);
    mqtt_service_clear(&client.service);
    outbox_destroy(client.outbox);
}

static void append_bytes(const uint8_t *data, size_t length)
{
    assert(incoming_len + length <= sizeof(incoming));
    memcpy(incoming + incoming_len, data, length);
    incoming_len += length;
    visible_len = incoming_len;
}

static void append_pingresp(void)
{
    const uint8_t packet[] = {0xd0, 0};
    append_bytes(packet, sizeof(packet));
}

static void append_command(int qos)
{
    uint8_t buffer[512], payload[110] = {0};
    mqtt_connection_t connection = {.buffer = buffer, .buffer_length = sizeof(buffer),
                                    .last_message_id = next_incoming_id};
    uint16_t id;
    mqtt_message_t *message = mqtt_msg_publish(&connection, "audit/device/cmd/request",
                                             (char *)payload, sizeof(payload), qos, 0, &id);
    assert(message->length);
    next_incoming_id = connection.last_message_id;
    append_bytes(message->data, message->length);
}

static outbox_item_handle_t queued_publication(int qos)
{
    uint8_t packet[] = {0x32, 0x06, 0, 1, 't', 0, 7, 'x'};
    packet[0] = (uint8_t)(0x30 | (qos << 1));
    outbox_message_t message = {.data = packet, .len = sizeof(packet), .msg_id = 7,
                               .msg_type = MQTT_MSG_TYPE_PUBLISH, .msg_qos = qos};
    outbox_item_handle_t item = outbox_enqueue(client.outbox, &message, now_ms);
    assert(item);
    assert(outbox_set_pending(client.outbox, 7, QUEUED) == ESP_OK);
    return item;
}

static void run_steps(unsigned count)
{
    for (unsigned i = 0; i < count && !closed; ++i) run_connected_iteration(&client);
}

static void test_ready_response_order(void)
{
    for (int reverse = 0; reverse < 2; ++reverse) {
        setup(59999, true);
        if (reverse) append_pingresp();
        append_command(1);
        if (!reverse) append_pingresp();
        tx_cost_ms = 2; /* a successful write may cross the old deadline */
        run_steps(3);
        if (!(delivered == 1 && !closed && !client.wait_for_ping_resp))
            fprintf(stderr, "ready order=%d delivered=%d closed=%d waiting=%d time=%llu pos=%zu/%zu deadline=%llu queue=%u\n",
                    reverse, delivered, closed, client.wait_for_ping_resp, (unsigned long long)now_ms,
                    incoming_pos, incoming_len, (unsigned long long)client.service.ping_deadline_ms,
                    client.service.count);
        assert(delivered == 1 && !closed && !client.wait_for_ping_resp);
        assert(incoming_pos == incoming_len && !client.service.ping_deadline_ms);
        finish();
    }
    setup(59000, true);
    append_command(1); append_pingresp();
    run_steps(3);
    assert(delivered == 1 && !closed && !client.wait_for_ping_resp);
    finish();
    puts("PASS: both packet orders and ample-time control preserve the ready PINGRESP");
}

static void test_response_behind_multiple_packets(void)
{
    setup(60000, true);
    client.service.last_tx_ms = 59900; /* unrelated TX does not answer the pending PING */
    const unsigned packets = MQTT_SERVICE_RX_PACKETS * 2 + 1;
    for (unsigned i = 0; i < packets; ++i) append_command(0);
    append_pingresp();
    run_steps(packets + 2);
    assert(delivered == (int)packets && !closed && !client.wait_for_ping_resp);
    assert(incoming_pos == incoming_len);
    finish();
    puts("PASS: pre-timeout grace services several RX slices before the waiting PINGRESP");
}

static void test_ping_send_completion_deadline(void)
{
    setup(30000, false);
    tx_available_ms = 44000;
    run_connected_iteration(&client);
    assert(!closed && !client.wait_for_ping_resp && !client.service.ping_deadline_ms);
    now_ms = 43999;
    run_connected_iteration(&client);
    assert(!closed && !client.wait_for_ping_resp && !client.service.ping_deadline_ms);
    now_ms = 44000;
    run_connected_iteration(&client);
    assert(!closed && client.wait_for_ping_resp);
    assert(client.service.ping_sent_ms == 44000 && client.service.ping_deadline_ms == 74000);
    now_ms = 60000;
    run_connected_iteration(&client);
    assert(!closed);
    append_pingresp();
    run_connected_iteration(&client);
    assert(!closed && !client.wait_for_ping_resp);
    finish();

    setup(30000, false);
    tx_failure = true;
    run_connected_iteration(&client);
    assert(closed && disconnected == 1 && !client.wait_for_ping_resp);
    assert(!client.service.ping_sent_ms && !client.service.ping_deadline_ms);
    finish();
    puts("PASS: only completed PING transmission starts the response deadline");
}

static void test_unrelated_input_cannot_hide_timeout(void)
{
    setup(60000, true);
    for (unsigned i = 0; i < MQTT_SERVICE_PING_GRACE_PACKETS + MQTT_SERVICE_RX_PACKETS; ++i)
        append_command(0);
    for (unsigned i = 0; i < MQTT_SERVICE_PING_GRACE_MS + 2 && !closed; ++i) {
        run_connected_iteration(&client);
        ++now_ms;
    }
    assert(closed && disconnected == 1);
    assert(now_ms <= 60000 + MQTT_SERVICE_PING_GRACE_MS + 2);
    finish();

    setup(60000, true);
    run_connected_iteration(&client);
    now_ms = 60000 + MQTT_SERVICE_PING_GRACE_MS;
    run_connected_iteration(&client);
    assert(closed && disconnected == 1);
    finish();
    puts("PASS: no response and continuous unrelated packets retain a finite failure bound");
}

static void test_partial_input_state_and_deadline(void)
{
    setup(1000, false);
    append_command(1);
    visible_len = 1;
    run_connected_iteration(&client);
    assert(!closed && !delivered && client.mqtt_state.in_buffer_read_len == 1);
    now_ms = 1001;
    run_connected_iteration(&client);
    assert(!closed && !delivered && client.mqtt_state.in_buffer_read_len == 1);
    visible_len = incoming_len;
    run_connected_iteration(&client);
    assert(!closed && delivered == 1);
    finish();

    setup(1000, false);
    ciphertext_ready = true; /* partial TLS record, no authenticated app bytes yet */
    run_connected_iteration(&client);
    assert(!closed && !delivered);
    uint64_t deadline = client.service.rx_deadline_ms;
    assert(deadline == 16000);
    for (now_ms = 2000; now_ms < deadline; now_ms += 1000) {
        run_connected_iteration(&client);
        assert(!closed && client.service.rx_deadline_ms == deadline);
    }
    run_connected_iteration(&client);
    assert(closed && disconnected == 1);
    finish();
    puts("PASS: partial MQTT state survives slices; slow TLS progress does not reset its deadline");
}

static void test_outbox_completion_and_copy_ownership(void)
{
    for (int qos = 0; qos <= 1; ++qos) {
        setup(1000, false);
        outbox_item_handle_t item = queued_publication(qos);
        tx_chunk = 1;
        run_connected_iteration(&client);
        assert(!closed && client.service.head && outbox_get(client.outbox, 7) == item);
        assert(outbox_item_get_pending(item) == QUEUED);
        for (unsigned i = 0; i < 16 && client.service.head; ++i) run_connected_iteration(&client);
        assert(!closed && !client.service.head);
        if (qos == 0) assert(outbox_get_size(client.outbox) == 0);
        else {
            assert(outbox_item_get_pending(item) == TRANSMITTED);
            const uint8_t ack[] = {0x40, 2, 0, 7};
            append_bytes(ack, sizeof(ack));
            run_connected_iteration(&client);
            assert(published == 1 && outbox_get_size(client.outbox) == 0 && !closed);
        }
        finish();
    }
    mqtt_service_t service = {0};
    uint8_t payload[] = {1, 2, 3};
    assert(mqtt_service_push(&service, payload, sizeof(payload), 7, 3, 1, true, 1234));
    payload[0] = 99;
    assert(service.head->data[0] == 1 && service.head->deadline_ms == 1234);
    mqtt_service_clear(&service);
    assert(!service.head && !service.tail && !service.bytes && !service.count);
    puts("PASS: outbox completion follows actual TX and queued frames own their bytes");
}

static void test_backpressure_fairness_and_cancellation(void)
{
    setup(1000, false);
    queued_publication(1);
    tx_available_ms = 14000;
    append_command(1);
    incoming_ready_ms = 1001;
    run_connected_iteration(&client);
    assert(!closed && !delivered && client.service.head);
    now_ms = 1001;
    run_connected_iteration(&client);
    assert(!closed && delivered == 1 && first_data_ms == 1001);
    assert(client.service.head->deadline_ms == 16000);
    now_ms = 13999;
    run_connected_iteration(&client);
    assert(!closed && client.service.head->deadline_ms == 16000);
    now_ms = 14000;
    run_steps(3);
    assert(!closed && !client.service.head);
    finish();

    setup(1000, false);
    outbox_item_handle_t item = queued_publication(1);
    tx_chunk = 1;
    run_connected_iteration(&client);
    for (now_ms = 4000; now_ms < 16000; now_ms += 3000) {
        run_connected_iteration(&client);
        assert(!closed && client.service.head->deadline_ms == 16000);
        assert(outbox_item_get_pending(item) == QUEUED);
    }
    run_connected_iteration(&client);
    assert(closed && disconnected == 1 && outbox_get(client.outbox, 7) == item);
    assert(outbox_item_get_pending(item) == QUEUED);
    finish();

    setup(1000, false);
    queued_publication(1);
    tx_available_ms = 14000;
    run_connected_iteration(&client);
    int previous_writes = writes;
    disconnect_request = true;
    run_connected_iteration(&client);
    assert(closed && disconnected == 1 && writes == previous_writes);
    assert(!client.service.head && !client.service.ping_queued && !client.service.rx_deadline_ms);
    finish();
    puts("PASS: TX backpressure yields to RX/cancellation and partial writes retain an aggregate deadline");
}

static void test_ping_grace_after_suspended_tls_write(void)
{
    setup(59999, true);
    queued_publication(1);
    tx_available_ms = 61000;
    tx_suspends_tls = true;
    run_connected_iteration(&client); /* TLS write returns WANT before the response deadline */
    assert(!closed && tls_write_suspended);
    now_ms = 60001;
    append_pingresp();
    run_connected_iteration(&client);
    assert(!closed && incoming_pos == 0);
    now_ms = 61000;
    run_connected_iteration(&client);
    assert(!closed && !tls_write_suspended);
    run_connected_iteration(&client);
    assert(!closed && !client.wait_for_ping_resp && incoming_pos == incoming_len);
    finish();

    setup(60000, true);
    queued_publication(1);
    tx_available_ms = 61000;
    tx_suspends_tls = true;
    run_connected_iteration(&client);
    assert(!closed && client.service.ping_grace_deadline_ms && writes == 0);
    now_ms = 60001;
    append_pingresp();
    run_connected_iteration(&client);
    assert(!closed && !client.wait_for_ping_resp && incoming_pos == incoming_len);
    finish();
    puts("PASS: a ready PINGRESP gets service after a suspended TLS write completes");
}

static void test_qos0_expiry_during_queued_tx(void)
{
    setup(29000, false);
    client.mqtt_state.connection.information.keepalive = 0;
    uint8_t first_data[] = {0x30, 2, 'a', 'A'}, second_data[] = {0x30, 2, 'b', 'B'};
    outbox_message_t message = {.data = first_data, .len = sizeof(first_data), .msg_id = 0,
                               .msg_type = MQTT_MSG_TYPE_PUBLISH, .msg_qos = 0};
    outbox_item_handle_t first = outbox_enqueue(client.outbox, &message, 1000);
    message.data = second_data;
    outbox_item_handle_t second = outbox_enqueue(client.outbox, &message, 25000);
    assert(first && second);
    tx_available_ms = 35000;
    run_connected_iteration(&client);
    assert(client.service.head && outbox_get(client.outbox, 0) == first);
    now_ms = 32001; /* first item has expired; its copied TX is still pending */
    mqtt_delete_expired_messages(&client);
    assert(outbox_get(client.outbox, 0) == first && outbox_get_size(client.outbox) == 8);
    now_ms = 35000;
    run_connected_iteration(&client);
    assert(!closed && !client.service.head && outbox_get(client.outbox, 0) == second);
    assert(outbox_get_size(client.outbox) == 4);
    size_t length; uint16_t id; int type, qos;
    uint8_t *stored = outbox_item_get_data(second, &length, &id, &type, &qos);
    assert(length == sizeof(second_data) && !memcmp(stored, second_data, length));
    now_ms = 56001;
    mqtt_delete_expired_messages(&client);
    assert(outbox_get_size(client.outbox) == 0);
    finish();
    puts("PASS: QoS0 expiry cannot make a pending TX completion delete the next id-zero item");
}

static void test_pingresp_with_preexisting_control_backlog(void)
{
    setup(59900, true);
    client.service.last_tx_ms = 59900;
    tx_available_ms = 65000; /* no socket readiness; SSL_write has not suspended RX */
    for (unsigned i = 0; i < MQTT_SERVICE_NORMAL_TX_FRAMES; ++i) append_command(1);
    run_connected_iteration(&client);
    assert(!closed && client.service.count == MQTT_SERVICE_NORMAL_TX_FRAMES);
    /* Reach the backlog through normal protocol handling, then prove normal
     * RX preserves the additional control slots reserved for PING grace. */
    append_command(1);
    size_t before = incoming_pos;
    now_ms = 59901;
    run_connected_iteration(&client);
    assert(!closed && incoming_pos == before && delivered == MQTT_SERVICE_NORMAL_TX_FRAMES);
    assert(client.service.count == MQTT_SERVICE_NORMAL_TX_FRAMES);
    now_ms = 60000;
    for (unsigned i = 1; i < MQTT_SERVICE_PING_GRACE_PACKETS - 1; ++i) append_command(1);
    append_pingresp();
    run_steps(MQTT_SERVICE_PING_GRACE_PACKETS);
    assert(!closed && delivered == MQTT_SERVICE_NORMAL_TX_FRAMES + MQTT_SERVICE_PING_GRACE_PACKETS - 1);
    assert(!client.wait_for_ping_resp);
    assert(incoming_pos == incoming_len);
    finish();
    puts("PASS: a preexisting control backlog cannot hide a buffered PINGRESP during grace");
}

static void test_reconnect_wait_cancellation_bound(void)
{
    for (int bounded = 0; bounded <= 1; ++bounded) {
        setup(1000, false);
        config.bounded_service = bounded;
        client.state = MQTT_STATE_WAIT_RECONNECT;
        client.wait_timeout_ms = 10000;
        run_wait_reconnect_iteration(&client);
        assert(!lock_depth && client.state == MQTT_STATE_WAIT_RECONNECT);
        assert(reconnect_wait_ticks == (bounded ? MQTT_SERVICE_IDLE_MS : 5000));
        finish();
    }
    puts("PASS: bounded reconnect wait yields after 10 ms; default mode preserves its 5 s backoff wait");
}

int main(void)
{
    test_ready_response_order();
    test_response_behind_multiple_packets();
    test_ping_send_completion_deadline();
    test_unrelated_input_cannot_hide_timeout();
    test_partial_input_state_and_deadline();
    test_outbox_completion_and_copy_ownership();
    test_backpressure_fairness_and_cancellation();
    test_ping_grace_after_suspended_tls_write();
    test_qos0_expiry_during_queued_tx();
    test_pingresp_with_preexisting_control_backlog();
    test_reconnect_wait_cancellation_bound();
    puts("ALL BOUNDED MQTT SERVICE SCENARIOS PASSED");
    return 0;
}
