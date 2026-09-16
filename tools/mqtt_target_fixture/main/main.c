/* Isolated ESP32 TLS/lwIP fixture. Never connects to the real MQTT broker.
 * The embedded certificate/key are disposable local test fixtures only. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "mqtt_client.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#define PORT 18884
#define NETWORK_TIMEOUT_MS 3000
extern const unsigned char cert_start[] asm("_binary_fixture_cert_pem_start");
extern const unsigned char cert_end[] asm("_binary_fixture_cert_pem_end");
extern const unsigned char key_start[] asm("_binary_fixture_key_pem_start");
extern const unsigned char key_end[] asm("_binary_fixture_key_pem_end");
static const char *TAG = "MQTT_FIXTURE";
typedef enum { COALESCED, FRAGMENTED, TRICKLE, PREFIX_ONLY, MISSING_PING,
               DELAYED_PING, TX_PRESSURE, CANCEL_PRESSURE } test_kind_t;
typedef struct { const char *name; test_kind_t kind; unsigned duration_ms; } test_case_t;
static const test_case_t cases[] = {
    {"coalesced_publish_pingresp", COALESCED, 3400},
    {"fragmented_tls_record", FRAGMENTED, 1800},
    {"slow_positive_tls_trickle", TRICKLE, 5200},
    {"partial_tls_then_silence", PREFIX_ONLY, 5200},
    {"missing_pingresp", MISSING_PING, 3600},
    {"delayed_pingresp", DELAYED_PING, 3600},
    {"tx_backpressure_deadline", TX_PRESSURE, 5200},
    {"cancel_during_tx_pressure", CANCEL_PRESSURE, 350},
};
static atomic_int current_case, connected_count, disconnected_count, data_count;
static atomic_int broker_error, broker_acks, broker_pings;
static atomic_bool listener_ready, case_done, broker_finished;
static atomic_uint connected_at, disconnected_at, injection_started_at, first_ping_at;
static unsigned failures;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void pause_ms(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

typedef struct {
    int fd;
    test_kind_t kind;
    bool injecting;
    size_t sent;
} bio_t;

static int fixture_send(void *context, const unsigned char *buffer, size_t length)
{
    bio_t *bio = context;
    if (atomic_load(&case_done)) return MBEDTLS_ERR_NET_SEND_FAILED;
    size_t count = length;
    if (bio->injecting) {
        if (!bio->sent) atomic_store(&injection_started_at, now_ms());
        if (bio->kind == PREFIX_ONLY && bio->sent) {
            while (!atomic_load(&case_done)) pause_ms(10);
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        if (bio->sent) pause_ms(bio->kind == TRICKLE ? 180 : 20);
        size_t chunk = bio->kind == PREFIX_ONLY ? 7 : bio->kind == TRICKLE ? 16 : 24;
        if (count > chunk) count = chunk;
    }
    int rc = send(bio->fd, buffer, count, 0);
    if (rc > 0) { bio->sent += (size_t)rc; return rc; }
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int fixture_recv(void *context, unsigned char *buffer, size_t length)
{
    bio_t *bio = context;
    if (atomic_load(&case_done)) return MBEDTLS_ERR_NET_RECV_FAILED;
    int rc = recv(bio->fd, buffer, length, 0);
    if (rc >= 0) return rc;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int read_exact(esp_tls_t *tls, unsigned char *buffer, size_t length)
{
    size_t offset = 0;
    uint32_t began = now_ms();
    while (offset < length && !atomic_load(&case_done) && now_ms() - began < 12000) {
        int rc = esp_tls_conn_read(tls, buffer + offset, length - offset);
        if (rc > 0) { offset += rc; continue; }
        if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE || rc == ESP_TLS_ERR_SSL_TIMEOUT) {
            pause_ms(2); continue;
        }
        return -1;
    }
    return offset == length ? 0 : -1;
}

static int read_packet(esp_tls_t *tls, unsigned char *packet, size_t capacity, size_t *length)
{
    if (read_exact(tls, packet, 1)) return -1;
    size_t header = 1, remaining = 0, multiplier = 1;
    do {
        if (header == 5 || read_exact(tls, packet + header, 1)) return -1;
        unsigned char byte = packet[header++];
        remaining += (byte & 127) * multiplier;
        multiplier *= 128;
        if (!(byte & 128)) break;
    } while (true);
    if (remaining > capacity - header || read_exact(tls, packet + header, remaining)) return -1;
    *length = header + remaining;
    return packet[0] >> 4;
}

static int send_packet(esp_tls_t *tls, const unsigned char *packet, size_t length)
{
    size_t offset = 0;
    while (offset < length && !atomic_load(&case_done)) {
        int rc = esp_tls_conn_write(tls, packet + offset, length - offset);
        if (rc > 0) { offset += rc; continue; }
        if (rc == ESP_TLS_ERR_SSL_WANT_READ || rc == ESP_TLS_ERR_SSL_WANT_WRITE) {
            pause_ms(2); continue;
        }
        return -1;
    }
    return offset == length ? 0 : -1;
}

static size_t publish_packet(unsigned char *packet)
{
    const char topic[] = "fixture/cmd";
    size_t remaining = 2 + strlen(topic) + 2 + 400;
    size_t pos = 0;
    packet[pos++] = 0x32; /* QoS 1 */
    size_t encoded = remaining;
    do {
        unsigned char byte = encoded % 128; encoded /= 128;
        packet[pos++] = byte | (encoded ? 128 : 0);
    } while (encoded);
    packet[pos++] = 0; packet[pos++] = strlen(topic);
    memcpy(packet + pos, topic, strlen(topic)); pos += strlen(topic);
    packet[pos++] = 0; packet[pos++] = 42;
    memset(packet + pos, 'F', 400); pos += 400;
    return pos;
}

static void broker_task(void *unused)
{
    (void)unused;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PORT),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
        ESP_LOGE(TAG, "FIXTURE_LISTENER_FAILED errno=%d", errno);
        atomic_store(&broker_error, 1);
        atomic_store(&listener_ready, true);
        vTaskDelete(NULL);
        return;
    }
    atomic_store(&listener_ready, true);
    static unsigned char packet[32768];
    unsigned char response[512];
    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { atomic_store(&broker_error, 2); break; }
        struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        esp_tls_t *tls = esp_tls_init();
        esp_tls_cfg_server_t server = {.servercert_buf = cert_start,
            .servercert_bytes = cert_end - cert_start, .serverkey_buf = key_start,
            .serverkey_bytes = key_end - key_start};
        if (!tls || esp_tls_server_session_create(&server, fd, tls)) {
            ESP_LOGE(TAG, "FIXTURE_SERVER_HANDSHAKE_FAILED case=%u", index);
            atomic_store(&broker_error, 3);
            if (tls) esp_tls_server_session_delete(tls); else close(fd);
            atomic_store(&broker_finished, true);
            continue;
        }
        timeout = (struct timeval){.tv_sec = 0, .tv_usec = 100000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        bio_t bio = {.fd = fd, .kind = cases[index].kind};
        mbedtls_ssl_set_bio(esp_tls_get_ssl_context(tls), &bio, fixture_send, fixture_recv, NULL);
        size_t length;
        if (read_packet(tls, packet, sizeof(packet), &length) != 1) {
            atomic_store(&broker_error, 4);
        } else {
            const unsigned char connack[] = {0x20, 2, 0, 0};
            if (send_packet(tls, connack, sizeof(connack))) atomic_store(&broker_error, 5);
        }
        if (bio.kind == FRAGMENTED || bio.kind == TRICKLE || bio.kind == PREFIX_ONLY) {
            pause_ms(50);
            bio.sent = 0; /* injection offsets exclude the already sent CONNACK record */
            bio.injecting = true;
            size_t count = publish_packet(response);
            int sent = send_packet(tls, response, count);
            bio.injecting = false;
            if (sent && bio.kind == FRAGMENTED) atomic_store(&broker_error, 6);
        }
        if (bio.kind == TX_PRESSURE || bio.kind == CANCEL_PRESSURE) {
            while (!atomic_load(&case_done)) pause_ms(10); /* deliberately stop TCP application reads */
        } else {
            while (!atomic_load(&case_done)) {
                int type = read_packet(tls, packet, sizeof(packet), &length);
                if (type < 0) break;
                if (type == 12) {
                    if (!atomic_load(&first_ping_at)) atomic_store(&first_ping_at, now_ms());
                    atomic_fetch_add(&broker_pings, 1);
                    if (bio.kind == MISSING_PING) continue;
                    if (bio.kind == DELAYED_PING) pause_ms(700);
                    size_t count = bio.kind == COALESCED ? publish_packet(response) : 0;
                    response[count++] = 0xd0; response[count++] = 0;
                    if (send_packet(tls, response, count)) break;
                } else if (type == 4) {
                    atomic_fetch_add(&broker_acks, 1);
                } else if (type == 14) break;
            }
        }
        esp_tls_server_session_delete(tls);
        atomic_store(&broker_finished, true);
    }
    close(listener);
    vTaskDelete(NULL);
}

static void mqtt_event(void *unused, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)unused; (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        atomic_fetch_add(&connected_count, 1);
        atomic_store(&connected_at, now_ms());
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        atomic_fetch_add(&disconnected_count, 1);
        atomic_store(&disconnected_at, now_ms());
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->total_data_len != 400 || event->data_len != 400) atomic_store(&broker_error, 7);
        for (int i = 0; i < event->data_len; ++i)
            if (event->data[i] != 'F') atomic_store(&broker_error, 8);
        atomic_fetch_add(&data_count, 1);
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "client error type=%d tls=%d stack=%d flags=%d", event->error_handle->error_type,
                 event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_tls_stack_err,
                 event->error_handle->esp_tls_cert_verify_flags);
    }
}

void app_main(void)
{
    /* Fixture certificate is freshly generated on the host. A future fixed
     * wall clock keeps validity checks enabled without internet/SNTP. */
    struct timeval wall = {.tv_sec = 1790000000}; /* 2026-09-21 UTC */
    settimeofday(&wall, NULL);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "FIXTURE_BEGIN transport=loopback TLS=1.2 network_timeout=%d", NETWORK_TIMEOUT_MS);
    if (xTaskCreate(broker_task, "fixture_broker", 16384, NULL, 5, NULL) != pdPASS) abort();
    while (!atomic_load(&listener_ready)) pause_ms(5);
    if (atomic_load(&broker_error)) abort();
    for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        atomic_store(&current_case, index);
        atomic_store(&connected_count, 0); atomic_store(&disconnected_count, 0);
        atomic_store(&data_count, 0); atomic_store(&broker_error, 0);
        atomic_store(&broker_acks, 0); atomic_store(&broker_pings, 0);
        atomic_store(&connected_at, 0); atomic_store(&disconnected_at, 0);
        atomic_store(&injection_started_at, 0); atomic_store(&case_done, false);
        atomic_store(&first_ping_at, 0);
        atomic_store(&broker_finished, false);
        bool isolated_operation = cases[index].kind == TRICKLE || cases[index].kind == PREFIX_ONLY ||
                                  cases[index].kind == TX_PRESSURE || cases[index].kind == CANCEL_PRESSURE;
        esp_mqtt_client_config_t config = {
            .broker.address.uri = "mqtts://127.0.0.1:18884",
            .broker.verification.certificate = (const char *)cert_start,
            .broker.verification.common_name = "fixture.local",
            .credentials.client_id = "isolated-loopback-fixture",
            .session.keepalive = isolated_operation ? 60 : 2,
            .network.timeout_ms = NETWORK_TIMEOUT_MS,
            .network.disable_auto_reconnect = true,
            .network.bounded_service = true,
            .task.stack_size = 12288,
            .outbox.limit = 32768,
        };
        esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
        if (!client) abort();
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL));
        ESP_LOGI(TAG, "CASE_BEGIN index=%u name=%s", index, cases[index].name);
        ESP_ERROR_CHECK(esp_mqtt_client_start(client));
        uint32_t started = now_ms();
        while (!atomic_load(&connected_count) && !atomic_load(&disconnected_count) && now_ms() - started < 10000)
            pause_ms(5);
        bool connected = atomic_load(&connected_count) == 1;
        bool pressure = cases[index].kind == TX_PRESSURE || cases[index].kind == CANCEL_PRESSURE;
        int admitted = 0;
        if (connected && pressure) {
            char *payload = malloc(30000);
            if (!payload) abort();
            memset(payload, 'B', 30000);
            admitted = esp_mqtt_client_enqueue(client, "fixture/bulk", payload, 30000, 1, 0, true);
            free(payload);
        }
        uint32_t observed = now_ms(), max_observer_call_us = 0;
        bool blocked_tx_observed = false;
        esp_mqtt_service_status_t status = {0};
        while (connected && !atomic_load(&disconnected_count) && now_ms() - observed < cases[index].duration_ms) {
            int64_t before = esp_timer_get_time();
            esp_mqtt_client_get_service_status(client, &status);
            if (pressure && status.tx_bytes >= 30000 && status.tx_frames > 0 &&
                status.tx_remaining_ms > 0 && status.tx_remaining_ms <= NETWORK_TIMEOUT_MS - 200)
                blocked_tx_observed = true;
            uint32_t cost = esp_timer_get_time() - before;
            if (cost > max_observer_call_us) max_observer_call_us = cost;
            pause_ms(5);
        }
        unsigned data = atomic_load(&data_count), disconnects = atomic_load(&disconnected_count);
        uint32_t failure_delay = atomic_load(&disconnected_at) - atomic_load(&connected_at);
        bool must_fail = cases[index].kind == TRICKLE || cases[index].kind == PREFIX_ONLY ||
                         cases[index].kind == MISSING_PING || cases[index].kind == TX_PRESSURE;
        bool ok = connected && !atomic_load(&broker_error) && (!pressure || admitted >= 0);
        ok &= status.max_lock_ms <= 1000 && max_observer_call_us <= 1000;
        if (must_fail) ok &= disconnects == 1 && failure_delay <= cases[index].duration_ms + 100;
        else ok &= disconnects == 0;
        if (cases[index].kind == COALESCED || cases[index].kind == FRAGMENTED)
            ok &= data > 0 && atomic_load(&broker_acks) > 0;
        if (cases[index].kind == TRICKLE || cases[index].kind == PREFIX_ONLY) {
            uint32_t injection = atomic_load(&injection_started_at);
            uint32_t elapsed = atomic_load(&disconnected_at) - injection;
            ok &= injection > 0 && data == 0 && elapsed >= 2800 && elapsed <= 3500;
        }
        if (pressure) ok &= blocked_tx_observed;
        if (cases[index].kind == TX_PRESSURE)
            ok &= failure_delay >= 2800 && failure_delay <= 3500;
        if (cases[index].kind == MISSING_PING) {
            uint32_t ping = atomic_load(&first_ping_at);
            uint32_t elapsed = atomic_load(&disconnected_at) - ping;
            ok &= ping > 0 && elapsed >= 950 && elapsed <= 1500;
        }
        if (cases[index].kind == DELAYED_PING || cases[index].kind == MISSING_PING)
            ok &= atomic_load(&broker_pings) > 0;
        int64_t stop_began = esp_timer_get_time();
        esp_err_t stopped = esp_mqtt_client_stop(client);
        uint32_t stop_ms = (esp_timer_get_time() - stop_began) / 1000;
        ok &= stopped == ESP_OK && stop_ms < 500;
        atomic_store(&case_done, true);
        uint32_t join_began = now_ms();
        while (!atomic_load(&broker_finished) && now_ms() - join_began < 2000) pause_ms(5);
        ok &= atomic_load(&broker_finished);
        ESP_ERROR_CHECK(esp_mqtt_client_destroy(client));
        bool heap_ok = heap_caps_check_integrity_all(true);
        ok &= heap_ok;
        unsigned free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        unsigned minimum_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "CASE_RESULT name=%s result=%s data=%u disconnects=%u fail_after_ms=%u stop_ms=%u max_lock_ms=%u observer_us=%u pings=%d pubacks=%d blocked_tx=%d broker_error=%d heap_ok=%d heap_free=%u heap_min=%u",
                 cases[index].name, ok ? "PASS" : "FAIL", data, disconnects, disconnects ? failure_delay : 0,
                 stop_ms, status.max_lock_ms, max_observer_call_us, atomic_load(&broker_pings),
                 atomic_load(&broker_acks), blocked_tx_observed, atomic_load(&broker_error),
                 heap_ok, free_heap, minimum_heap);
        if (!ok) ++failures;
        pause_ms(100);
    }
    ESP_LOGI(TAG, "FIXTURE_RESULT cases=%u failures=%u result=%s", (unsigned)(sizeof(cases) / sizeof(cases[0])),
             failures, failures ? "FAIL" : "PASS");
    while (true) pause_ms(1000);
}
