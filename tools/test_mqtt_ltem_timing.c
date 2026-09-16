/* Reuse the controlled transport fixture; source excerpts remain actual SDK C. */
#define main baseline_suite_main
#include "test_mqtt_keepalive.c"
#undef main

static void setup300(uint64_t tick)
{
    setup(tick, false);
    config.network_timeout_ms = 300000;
    client.mqtt_state.connection.information.keepalive = 600;
    client.service.last_tx_ms = tick;
    config.message_retransmit_timeout = 5000;
}
static void late_puback(void)
{
    setup300(1000);
    queued_publication(1);
    run_connected_iteration(&client);
    assert(outbox_item_get_pending(outbox_get(client.outbox, 7)) == TRANSMITTED);
    now_ms = 201000;
    run_connected_iteration(&client);
    assert(!closed && !published);
    const uint8_t ack[] = {0x40, 2, 0, 7};
    append_bytes(ack, sizeof(ack));
    now_ms = 300999;
    run_connected_iteration(&client);
    assert(!closed && published == 1 && !outbox_get(client.outbox, 7));
    finish();
    puts("PASS bounded300: ordinary PUBACK after299999ms completes real outbox publication");
}
static void partial_rx(void)
{
    for (unsigned expired = 0; expired < 2; ++expired) {
        setup300(1000);
        append_command(0);
        visible_len = 1;
        run_connected_iteration(&client);
        assert(client.service.rx_deadline_ms == 301000 && !delivered);
        now_ms = 201000;
        run_connected_iteration(&client);
        assert(!closed && client.service.rx_deadline_ms == 301000);
        now_ms = expired ? 301000 : 300999;
        visible_len = incoming_len;
        run_connected_iteration(&client);
        if (expired) assert(closed && disconnected == 1 && !delivered);
        else assert(!closed && delivered == 1 && !client.service.rx_deadline_ms);
        finish();
    }
    puts("PASS bounded300: partial RX survives200s/completes299999ms; at300000ms expires");
}
static void queued_tx(void)
{
    for (unsigned expired = 0; expired < 2; ++expired) {
        setup300(1000);
        queued_publication(1);
        tx_available_ms = expired ? 301000 : 300999;
        run_connected_iteration(&client);
        assert(client.service.head && client.service.head->deadline_ms == 301000);
        now_ms = 201000;
        run_connected_iteration(&client);
        assert(!closed && client.service.head->deadline_ms == 301000);
        now_ms = expired ? 301000 : 300999;
        int previous_writes = writes;
        run_connected_iteration(&client);
        if (expired) assert(closed && disconnected == 1 && writes == previous_writes);
        else assert(!closed && !client.service.head && writes == previous_writes + 1);
        finish();
    }
    puts("PASS bounded300: queued TX survives200s/completes299999ms; at300000ms aborts before write");
}
static void ping_completion(void)
{
    for (unsigned missing = 0; missing < 2; ++missing) {
        setup300(300000);
        client.service.last_tx_ms = 0;
        tx_available_ms = 310000;
        run_connected_iteration(&client);
        assert(client.service.ping_queued && !client.wait_for_ping_resp);
        assert(!client.service.ping_deadline_ms && client.service.head->deadline_ms == 600000);
        now_ms = 309999;
        run_connected_iteration(&client);
        assert(!closed && !client.service.ping_deadline_ms);
        now_ms = 310000;
        run_connected_iteration(&client);
        assert(client.wait_for_ping_resp && client.service.ping_sent_ms == 310000);
        assert(client.service.ping_deadline_ms == 610000);
        now_ms = 510000;
        run_connected_iteration(&client);
        assert(!closed && client.wait_for_ping_resp);
        if (!missing) {
            now_ms = 609999;
            append_pingresp();
            run_connected_iteration(&client);
            assert(!closed && !client.wait_for_ping_resp && !client.service.ping_deadline_ms);
        } else {
            now_ms = 610000;
            run_connected_iteration(&client);
            assert(!closed && client.service.ping_grace_deadline_ms == 610000 + MQTT_SERVICE_PING_GRACE_MS);
            now_ms = 610000 + MQTT_SERVICE_PING_GRACE_MS;
            run_connected_iteration(&client);
            assert(closed && disconnected == 1);
        }
        finish();
    }
    puts("PASS bounded300: PING response deadline=completed send+300000; missing response expires after200ms grace");
}
static void retry_5s(void)
{
    setup300(1000);
    queued_publication(1);
    run_connected_iteration(&client);
    assert(writes == 1 && !published && !closed);
    now_ms = 5999;
    run_connected_iteration(&client);
    assert(writes == 1 && !closed);
    now_ms = 6000;
    run_connected_iteration(&client);
    assert(writes == 2 && !closed && !published);
    const uint8_t ack[] = {0x40, 2, 0, 7};
    append_bytes(ack, sizeof(ack));
    now_ms = 6001;
    run_connected_iteration(&client);
    assert(published == 1 && !outbox_get(client.outbox, 7));
    now_ms = 11000;
    run_connected_iteration(&client);
    assert(writes == 2 && !closed);
    finish();
    puts("PASS LTE-M: no retry before5s; retransmit at5s; PUBACK removes message without waiting300s");
}
int main(void)
{
    retry_5s();
    late_puback();
    partial_rx();
    queued_tx();
    ping_completion();
    return 0;
}
