/* Fault tests for verbatim esp-mqtt SDK excerpts and the real SDK outbox.
 * The runner generates sdk_abort_excerpts.inc directly from the selected SDK.
 * Only transport, clock, RTOS and message construction are controlled fakes.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_outbox.h"
#include "mqtt_service.h"

#define MQTT_MSG_TYPE_PUBLISH 3
#define MQTT_PROTOCOL_V_5 5
#define MQTT_STATE_CONNECTED 1
#define MQTT_STATE_WAIT_RECONNECT 2
#define MQTT_STATE_INIT 3
#define MQTT_STATE_BOUNDED_CONNECT 4
#define pdMS_TO_TICKS(ms) (ms)
#define MQTT_EVENT_DISCONNECTED 1
#define DISCONNECT_BIT 2
#define MQTT_POLL_READ_TIMEOUT_MS 1000
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

typedef struct {
    struct { uint8_t *data; size_t length; } outbound_message;
    struct { int keepalive; int protocol_ver; } information;
} connection_t;
typedef struct {
    bool bounded_service;
    int network_timeout_ms, reconnect_timeout_ms, message_retransmit_timeout;
    int refresh_connection_after_ms;
} config_t;
typedef struct {
    config_t *config;
    struct {
        connection_t connection;
        uint16_t pending_msg_id;
        int pending_msg_type, pending_publish_qos, message_length;
        uint8_t *in_buffer;
        size_t in_buffer_read_len;
    } mqtt_state;
    mqtt_service_t service;
    struct { atomic_uint deadline_failures; } service_observation;
    void *nb;
    outbox_handle_t outbox;
    void *transport;
    int state, status_bits, wait_timeout_ms;
    struct { int event_id; } event;
    bool wait_for_ping_resp;
    uint64_t keepalive_tick, reconnect_tick, refresh_connection_tick;
} client_t;
typedef client_t *esp_mqtt_client_handle_t;

static uint64_t now_ms, last_retransmit;
static bool closed, build_failure, ping_build_failure, partial_write;
static int fail_write_at, fail_write_result, writes, closed_writes, closes;
static int events, transport_errors, ping_builds, pubrel_builds, polls;
static int packet_counters, lock_depth, receive_result, disconnect_request;
static int checks, failures;
static const char *scenario;
static uint8_t control_packet[4];

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #c); } } while (0)
#define MQTT_API_LOCK(client) ((void)(client), ++lock_depth)
#define MQTT_API_UNLOCK(client) do { (void)(client); assert(lock_depth > 0); --lock_depth; } while (0)

static uint64_t platform_tick_get_ms(void) { return now_ms; }
static int esp_transport_write(void *transport, const char *data, int length, int timeout)
{
    (void)transport; (void)data;
    writes++;
    if (closed) {
        closed_writes++;
        now_ms += (uint64_t)timeout; /* models the observed empty-fd poll timeout */
        return 0;
    }
    if (writes == fail_write_at) {
        now_ms += (uint64_t)timeout;
        return fail_write_result;
    }
    if (partial_write && length > 1) return 1;
    return length;
}
static void esp_transport_close(void *transport) { (void)transport; closes++; closed = true; }
static int esp_transport_poll_read(void *transport, int timeout)
{
    (void)transport; (void)timeout; polls++; CHECK(!closed); return 0;
}
static int max_poll_timeout(esp_mqtt_client_handle_t client, int timeout) { (void)client; return timeout; }
static void esp_mqtt_dispatch_event_with_msgid(esp_mqtt_client_handle_t client)
{
    CHECK(client->event.event_id == MQTT_EVENT_DISCONNECTED);
    CHECK(client->state == MQTT_STATE_WAIT_RECONNECT);
    CHECK(lock_depth > 0);
    events++;
}
static void esp_mqtt_client_dispatch_transport_error(esp_mqtt_client_handle_t client)
{ (void)client; transport_errors++; }
static unsigned xEventGroupWaitBits(int bits, int requested, bool clear, bool all, int timeout)
{ (void)bits; (void)clear; (void)all; (void)timeout; return disconnect_request ? (unsigned)requested : 0; }
static void send_disconnect_msg(esp_mqtt_client_handle_t client) { (void)client; }
static int mqtt_process_receive(esp_mqtt_client_handle_t client) { (void)client; return receive_result; }
static void mqtt_set_dup(uint8_t *data) { data[0] |= 8; }
static void mqtt_msg_pubrel(connection_t *connection, uint16_t id)
{
    (void)id; pubrel_builds++; control_packet[0] = 0x62;
    connection->outbound_message.data = control_packet;
    connection->outbound_message.length = build_failure ? 0 : sizeof(control_packet);
}
static void mqtt_msg_pingreq(connection_t *connection)
{
    ping_builds++; control_packet[0] = 0xc0;
    connection->outbound_message.data = control_packet;
    connection->outbound_message.length = ping_build_failure ? 0 : 2;
}
#ifdef MQTT_PROTOCOL_5
static void mqtt5_msg_pubrel(connection_t *connection, uint16_t id) { mqtt_msg_pubrel(connection, id); }
static void esp_mqtt5_increment_packet_counter(esp_mqtt_client_handle_t client) { (void)client; packet_counters++; }
#endif
static void esp_mqtt_abort_connection(esp_mqtt_client_handle_t client);
static esp_err_t esp_mqtt_client_ping(esp_mqtt_client_handle_t client);
/* This suite exercises the unchanged default mode, including both MQTT
 * versions. The opt-in service has its own parser/transport integration suite. */
#ifdef WUPS_CURRENT_SDK
static esp_err_t mqtt_bounded_write(esp_mqtt_client_handle_t client)
{ (void)client; assert(!"Legacy test entered bounded TX"); return ESP_FAIL; }
static esp_err_t mqtt_bounded_service(esp_mqtt_client_handle_t client)
{ (void)client; assert(!"Legacy test entered bounded service"); return ESP_FAIL; }
static void mqtt_transport_nb_close(void *nb) { (void)nb; }
static void vTaskDelay(int ticks) { (void)ticks; assert(!"Legacy test entered bounded delay"); }
#endif

#include "sdk_abort_excerpts.inc"

static config_t config;
static client_t client;
static void setup(const char *name, bool overdue)
{
    scenario = name;
    memset(&client, 0, sizeof(client));
    memset(&config, 0, sizeof(config));
    now_ms = 100000; last_retransmit = 1;
    closed = build_failure = ping_build_failure = partial_write = false;
    fail_write_at = fail_write_result = writes = closed_writes = closes = 0;
    events = transport_errors = ping_builds = pubrel_builds = polls = 0;
    packet_counters = lock_depth = disconnect_request = 0;
    receive_result = ESP_OK;
    config.network_timeout_ms = 15000;
    config.reconnect_timeout_ms = 10000;
    config.message_retransmit_timeout = 1000;
    config.refresh_connection_after_ms = overdue ? 100 : 0;
    client.config = &config;
    client.state = MQTT_STATE_CONNECTED;
    client.outbox = outbox_init();
    assert(client.outbox);
    client.mqtt_state.connection.information.keepalive = 60;
#ifdef MQTT_PROTOCOL_5
    client.mqtt_state.connection.information.protocol_ver = MQTT_PROTOCOL_V_5;
#else
    client.mqtt_state.connection.information.protocol_ver = 4;
#endif
    client.keepalive_tick = overdue ? 1 : now_ms;
}
static outbox_item_handle_t add_item(int id, int qos, pending_state_t pending)
{
    uint8_t packet[] = {0x30, 0x02, 0x61, 0x62};
    outbox_message_t message = {
        .data = packet, .len = sizeof(packet), .msg_id = id,
        .msg_qos = qos, .msg_type = MQTT_MSG_TYPE_PUBLISH,
    };
    outbox_item_handle_t item = outbox_enqueue(client.outbox, &message, 1);
    assert(item);
    assert(outbox_set_pending(client.outbox, id, pending) == ESP_OK);
    return item;
}
static void teardown(void)
{
    CHECK(lock_depth == 0);
    outbox_destroy(client.outbox);
}
static void failed_resend(pending_state_t pending, int qos, int write_result, bool build_fail, bool overdue)
{
    setup(build_fail ? "PUBREL-build" : pending == QUEUED ? "QUEUED-write" :
          pending == TRANSMITTED ? "TRANSMITTED-write" : "PUBREL-write", overdue);
    outbox_item_handle_t item = add_item(7, qos, pending);
    if (pending == TRANSMITTED) add_item(8, 2, ACKNOWLEDGED);
    uint64_t size_before = outbox_get_size(client.outbox);
    fail_write_at = 1;
    fail_write_result = write_result;
    build_failure = build_fail;
    run_connected_iteration(&client);
    CHECK(client.state == MQTT_STATE_WAIT_RECONNECT);
    CHECK(events == 1);
    CHECK(closes == 1);
    CHECK(closed_writes == 0);
    CHECK(writes == (build_fail ? 0 : 1));
    CHECK(ping_builds == 0);
    CHECK(polls == 0);
    CHECK(pubrel_builds == (pending == ACKNOWLEDGED ? 1 : 0));
    CHECK(packet_counters == 0);
    CHECK(now_ms == (build_fail ? 100000u : 115000u));
    CHECK(client.reconnect_tick == now_ms);
    CHECK(outbox_get_size(client.outbox) == size_before);
    CHECK(outbox_get(client.outbox, 7) == item);
    CHECK(outbox_item_get_pending(item) == pending);
    CHECK(transport_errors == (!build_fail && write_result < 0 ? 1 : 0));
    CHECK(!client.wait_for_ping_resp);
    CHECK(lock_depth == 0);

    /* A fresh transport retries the same stored item successfully. */
    closed = false; build_failure = false; fail_write_at = 0;
    client.state = MQTT_STATE_CONNECTED;
    client.keepalive_tick = now_ms;
    config.refresh_connection_after_ms = 0;
    last_retransmit = 1;
    int events_before_retry = events;
    run_connected_iteration(&client);
    CHECK(client.state == MQTT_STATE_CONNECTED);
    CHECK(events == events_before_retry);
    if (pending == QUEUED && qos == 0) CHECK(outbox_get(client.outbox, 7) == NULL);
    else {
        CHECK(outbox_get(client.outbox, 7) == item);
        CHECK(outbox_item_get_pending(item) == (pending == QUEUED ? TRANSMITTED : pending));
    }
    teardown();
}
static void successful_resend(pending_state_t pending, int qos)
{
    setup("success", false);
    outbox_item_handle_t item = add_item(7, qos, pending);
    partial_write = true; /* exercise the SDK's write loop, not just one write */
    run_connected_iteration(&client);
    CHECK(client.state == MQTT_STATE_CONNECTED);
    CHECK(events == 0 && closes == 0 && closed_writes == 0);
    CHECK(polls == 1);
    CHECK(writes == 4);
    CHECK(pubrel_builds == (pending == ACKNOWLEDGED ? 1 : 0));
    if (pending == QUEUED && qos == 0) {
        CHECK(outbox_get_size(client.outbox) == 0);
        CHECK(outbox_get(client.outbox, 7) == NULL);
    } else {
        CHECK(outbox_get_size(client.outbox) == 4);
        CHECK(outbox_get(client.outbox, 7) == item);
        CHECK(outbox_item_get_pending(item) == (pending == QUEUED ? TRANSMITTED : pending));
        if (pending == TRANSMITTED) {
            size_t length; uint16_t id; int type, stored_qos;
            uint8_t *data = outbox_item_get_data(item, &length, &id, &type, &stored_qos);
            CHECK((data[0] & 8) != 0);
        }
    }
#ifdef MQTT_PROTOCOL_5
    CHECK(packet_counters == (qos > 0 ? 1 : 0));
#endif
    teardown();
}
static void unrelated_paths(void)
{
    setup("keepalive-success", false);
    client.keepalive_tick = 1;
    run_connected_iteration(&client);
    CHECK(writes == 1 && ping_builds == 1 && client.wait_for_ping_resp);
    CHECK(events == 0 && polls == 1);
    teardown();
    setup("keepalive-failure", false);
    client.keepalive_tick = 1; fail_write_at = 1; fail_write_result = -1;
    run_connected_iteration(&client);
    CHECK(writes == 1 && events == 1 && closes == 1 && closed_writes == 0);
    CHECK(client.state == MQTT_STATE_WAIT_RECONNECT && polls == 0);
    teardown();
    setup("receive-failure", true);
    receive_result = ESP_FAIL; add_item(7, 1, QUEUED);
    run_connected_iteration(&client);
    CHECK(writes == 0 && events == 1 && closes == 1 && polls == 0);
    CHECK(outbox_get_size(client.outbox) == 4);
    teardown();
    setup("refresh-after-success", false);
    config.refresh_connection_after_ms = 100;
    add_item(7, 1, QUEUED);
    run_connected_iteration(&client);
    CHECK(writes == 1 && events == 1 && closes == 1 && polls == 0);
    CHECK(client.state == MQTT_STATE_INIT);
    teardown();
}
int main(void)
{
    for (int overdue = 0; overdue <= 1; overdue++) {
        for (int result = -1; result <= 0; result++) {
            for (int qos = 0; qos <= 2; qos++) failed_resend(QUEUED, qos, result, false, overdue);
            for (int qos = 1; qos <= 2; qos++) failed_resend(TRANSMITTED, qos, result, false, overdue);
            failed_resend(ACKNOWLEDGED, 2, result, false, overdue);
        }
        failed_resend(ACKNOWLEDGED, 2, 0, true, overdue);
    }
    for (int qos = 0; qos <= 2; qos++) successful_resend(QUEUED, qos);
    for (int qos = 1; qos <= 2; qos++) successful_resend(TRANSMITTED, qos);
    successful_resend(ACKNOWLEDGED, 2);
    unrelated_paths();
    printf("mqtt abort: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
