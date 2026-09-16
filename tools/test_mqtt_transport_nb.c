#include "host.h"
#include "mqtt_transport_nb.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scripted API boundary, not a fake replacement for the adapter under test. */
static unsigned checks, live_jobs, live_tls, dns_calls, tls_calls, ssl_reads, ssl_writes;
static unsigned elapsed_ms, max_tls_wait_ms, socket_checks;
static bool queue_fail, dns_async, dns_failure, tcp_ready, read_ready, write_ready;
static int socket_error, read_result, write_results[8];
static unsigned write_count, write_index;
static void (*queued_callback)(void *);
static void *queued_arg, *dns_arg;
static dns_found_callback dns_callback;
static char dns_name[256], tls_name[256], tls_address[48];
static const unsigned char *pending_write;
static size_t pending_length;
static bool invalid_read_during_write;
static esp_tls_t *active_tls;
static unsigned posthandshake_warnings;

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

void *host_calloc(size_t n, size_t size)
{
    void *memory = calloc(n, size);
    if (memory) ++live_jobs;
    return memory;
}
void host_free(void *memory)
{
    if (memory) { CHECK(live_jobs > 0); --live_jobs; free(memory); }
}
void host_log(const char *tag, const char *message, ...)
{
    CHECK(strcmp(tag, "mqtt_nb") == 0);
    CHECK(strstr(message, "post-handshake output") != NULL);
    ++posthandshake_warnings;
}

err_t tcpip_try_callback(void (*callback)(void *), void *arg)
{
    if (queue_fail) return ERR_ARG;
    CHECK(!queued_callback);
    queued_callback = callback;
    queued_arg = arg;
    return ERR_OK;
}
err_t dns_gethostbyname(const char *name, ip_addr_t *address,
                      dns_found_callback callback, void *arg)
{
    ++dns_calls;
    CHECK(strlen(name) < sizeof(dns_name));
    strcpy(dns_name, name);
    if (dns_async) {
        CHECK(!dns_callback);
        dns_callback = callback;
        dns_arg = arg;
        return ERR_INPROGRESS;
    }
    strcpy(address->text, "192.0.2.19");
    return dns_failure ? ERR_ARG : ERR_OK;
}
char *ipaddr_ntoa_r(const ip_addr_t *address, char *text, int length)
{
    CHECK((size_t)length > strlen(address->text));
    strcpy(text, address->text);
    return text;
}
static void run_dns_begin(void)
{
    CHECK(queued_callback != NULL);
    void (*callback)(void *) = queued_callback;
    void *arg = queued_arg;
    queued_callback = NULL;
    queued_arg = NULL;
    callback(arg);
}
static void run_dns_done(void)
{
    CHECK(dns_callback != NULL);
    dns_found_callback callback = dns_callback;
    void *arg = dns_arg;
    dns_callback = NULL;
    dns_arg = NULL;
    ip_addr_t address = {.text = "192.0.2.19"};
    callback(dns_name, dns_failure ? NULL : &address, arg);
}

esp_tls_t *esp_tls_init(void)
{
    CHECK(active_tls == NULL);
    active_tls = calloc(1, sizeof(*active_tls));
    CHECK(active_tls != NULL);
    ++live_tls;
    active_tls->socket_fd = -1;
    active_tls->config.renegotiation = MBEDTLS_SSL_RENEGOTIATION_ENABLED;
    active_tls->ssl.config = &active_tls->config;
    return active_tls;
}

int host_select(int count, fd_set *reads, fd_set *writes, fd_set *exceptions,
                struct timeval *timeout)
{
    (void)exceptions;
    CHECK(count == 8 && active_tls != NULL);
    CHECK(timeout != NULL); /* IDF timeout=0 accidentally passes NULL. */
    unsigned delay = (unsigned)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    if (delay > max_tls_wait_ms) max_tls_wait_ms = delay;
    CHECK(delay <= 1);
    bool ready = active_tls->state != ESP_TLS_DONE ? tcp_ready :
        ((reads && read_ready) || (writes && write_ready));
    bool watched = (reads && FD_ISSET(7, reads)) || (writes && FD_ISSET(7, writes));
    if (!ready || !watched) {
        if (reads) FD_ZERO(reads);
        if (writes) FD_ZERO(writes);
        elapsed_ms += delay;
        return 0;
    }
    return 1;
}
int host_getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    CHECK(fd == 7 && level == SOL_SOCKET && option == SO_ERROR && *length == sizeof(int));
    ++socket_checks;
    *(int *)value = socket_error;
    return 0;
}

/* Reproduce the significant v6.0.2 behavior: CONNECTING select mutates saved
 * sets, including clearing them on timeout. The adapter must work around it. */
int esp_tls_conn_new_async(const char *host, int length, int port,
                          const esp_tls_cfg_t *cfg, esp_tls_t *tls)
{
    ++tls_calls;
    CHECK(tls == active_tls && port == 8883 && length == (int)strlen(host));
    CHECK(cfg->tls_version == ESP_TLS_VER_TLS_1_2);
    CHECK(cfg->timeout_ms > 0 && cfg->timeout_ms <= 1);
    strcpy(tls_address, host);
    CHECK(cfg->common_name != NULL);
    strcpy(tls_name, cfg->common_name);
    if (tls->state == ESP_TLS_INIT) {
        CHECK(cfg->non_block);
        tls->socket_fd = 7;
        tls->socket_nonblocking = true;
        FD_ZERO(&tls->rset);
        FD_SET(7, &tls->rset);
        tls->wset = tls->rset;
        tls->state = ESP_TLS_CONNECTING;
    }
    if (tls->state == ESP_TLS_CONNECTING) {
        if (cfg->non_block) {
            struct timeval timeout = {.tv_usec = cfg->timeout_ms * 1000};
            if (!host_select(8, &tls->rset, &tls->wset, NULL, &timeout)) return 0;
        } else {
            CHECK(tcp_ready && socket_checks > 0);
            CHECK(tls->socket_nonblocking); /* Bypassing select must not block I/O. */
        }
        tls->state = ESP_TLS_HANDSHAKE;
        return 0; /* handshake WANT_READ, independent of cfg.non_block */
    }
    CHECK(tls->state == ESP_TLS_HANDSHAKE);
    tls->state = ESP_TLS_DONE;
    return 1;
}
esp_err_t esp_tls_get_conn_state(esp_tls_t *tls, esp_tls_conn_state_t *state)
{
    *state = tls->state;
    return ESP_OK;
}
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd)
{
    *fd = tls->socket_fd;
    return ESP_OK;
}
void *esp_tls_get_ssl_context(esp_tls_t *tls) { return &tls->ssl; }
const mbedtls_ssl_config *mbedtls_ssl_context_get_config(const mbedtls_ssl_context *ssl)
{
    return ssl->config;
}
void mbedtls_ssl_conf_renegotiation(mbedtls_ssl_config *cfg, int enabled)
{
    CHECK(cfg == &active_tls->config && enabled == MBEDTLS_SSL_RENEGOTIATION_DISABLED);
    cfg->renegotiation = enabled;
}
int esp_tls_get_bytes_avail(esp_tls_t *tls) { CHECK(tls == active_tls); return 0; }
int esp_tls_conn_read(esp_tls_t *tls, unsigned char *data, size_t length)
{
    CHECK(tls == active_tls && length > 0);
    ++ssl_reads;
    if (pending_write) {
        /* A HelloRequest alert can flush the outstanding application record,
         * making the next ssl_write repeat plaintext. Never enter this path. */
        invalid_read_during_write = true;
        pending_write = NULL;
    }
    if (read_result > 0) {
        CHECK((size_t)read_result <= length);
        memset(data, 'R', (size_t)read_result);
    }
    return read_result;
}
int esp_tls_conn_write(esp_tls_t *tls, const unsigned char *data, size_t length)
{
    CHECK(tls == active_tls && length > 0);
    ++ssl_writes;
    if (pending_write) CHECK(data == pending_write && length == pending_length);
    int result = write_index < write_count ? write_results[write_index++] : (int)length;
    if (result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE) {
        pending_write = data;
        pending_length = length;
    } else pending_write = NULL;
    return result;
}
int esp_tls_conn_destroy(esp_tls_t *tls)
{
    CHECK(tls == active_tls && live_tls == 1);
    --live_tls;
    free(tls);
    active_tls = NULL;
    pending_write = NULL;
    return 0;
}

static void reset(void)
{
    CHECK(!live_jobs && !live_tls && !queued_callback && !dns_callback);
    queue_fail = dns_failure = tcp_ready = read_ready = false;
    dns_async = write_ready = true;
    socket_error = 0;
    read_result = ESP_TLS_ERR_SSL_WANT_READ;
    dns_calls = tls_calls = ssl_reads = ssl_writes = socket_checks = 0;
    elapsed_ms = max_tls_wait_ms = write_count = write_index = 0;
    pending_write = NULL;
    invalid_read_during_write = false;
    posthandshake_warnings = 0;
    tls_name[0] = tls_address[0] = 0;
}
static void connect_ready(mqtt_transport_nb_t *transport)
{
    *transport = (mqtt_transport_nb_t){0};
    CHECK(mqtt_transport_nb_connect(transport, "mqtt.example.test", 8883) == 0);
    run_dns_begin();
    run_dns_done();
    CHECK(mqtt_transport_nb_connect(transport, "mqtt.example.test", 8883) == 0);
    CHECK(active_tls->state == ESP_TLS_CONNECTING);
    CHECK(!FD_ISSET(7, &active_tls->wset)); /* First 1ms select timed out. */
    for (unsigned i = 0; i < 5; ++i) {
        elapsed_ms += 10;
        CHECK(mqtt_transport_nb_connect(transport, "mqtt.example.test", 8883) == 0);
        CHECK(tls_calls == 1); /* Independent zero-poll while still connecting. */
    }
    tcp_ready = true;
    CHECK(mqtt_transport_nb_connect(transport, "mqtt.example.test", 8883) == 0);
    CHECK(active_tls->state == ESP_TLS_HANDSHAKE);
    CHECK(mqtt_transport_nb_connect(transport, "mqtt.example.test", 8883) == 1);
    CHECK(transport->connected && transport->socket_fd == 7 && max_tls_wait_ms <= 1);
    CHECK(strcmp(tls_address, "192.0.2.19") == 0);
    CHECK(strcmp(tls_name, "mqtt.example.test") == 0);
    CHECK(active_tls->config.renegotiation == MBEDTLS_SSL_RENEGOTIATION_DISABLED);
    read_ready = write_ready = true;
}

static void dns_lifetime(void)
{
    reset();
    mqtt_transport_nb_t transport = {0};
    CHECK(mqtt_transport_nb_connect(&transport, "cancel-before-tcpip.test", 8883) == 0);
    mqtt_transport_nb_close(&transport);
    CHECK(live_jobs == 1);
    run_dns_begin();
    CHECK(strcmp(dns_name, "cancel-before-tcpip.test") == 0);
    run_dns_done();
    CHECK(live_jobs == 0 && live_tls == 0);

    reset();
    CHECK(mqtt_transport_nb_connect(&transport, "cancel-dns-pending.test", 8883) == 0);
    run_dns_begin();
    mqtt_transport_nb_close(&transport);
    CHECK(live_jobs == 1);
    dns_failure = true;
    run_dns_done();
    CHECK(live_jobs == 0);

    reset();
    queue_fail = true;
    CHECK(mqtt_transport_nb_connect(&transport, "queue-full.test", 8883) == -1);
    CHECK(live_jobs == 0);
    mqtt_transport_nb_close(&transport);
    reset();
    dns_async = false;
    dns_failure = true;
    CHECK(mqtt_transport_nb_connect(&transport, "invalid.test", 8883) == 0);
    run_dns_begin();
    CHECK(mqtt_transport_nb_connect(&transport, "invalid.test", 8883) == -1);
    mqtt_transport_nb_close(&transport);
    CHECK(live_jobs == 0);
    puts("PASS async DNS: queued/pending cancellation, late callback, queue failure, DNS failure");
}

static void connect_and_cancel(void)
{
    reset();
    mqtt_transport_nb_t transport;
    connect_ready(&transport);
    mqtt_transport_nb_close(&transport);
    mqtt_transport_nb_close(&transport);
    CHECK(live_jobs == 0 && live_tls == 0);

    reset();
    transport = (mqtt_transport_nb_t){0};
    CHECK(mqtt_transport_nb_connect(&transport, "tcp-error.test", 8883) == 0);
    run_dns_begin();
    run_dns_done();
    CHECK(mqtt_transport_nb_connect(&transport, "tcp-error.test", 8883) == 0);
    tcp_ready = true;
    socket_error = 111;
    CHECK(mqtt_transport_nb_connect(&transport, "tcp-error.test", 8883) == -1);
    CHECK(tls_calls == 1);
    mqtt_transport_nb_close(&transport);
    reset();
    dns_async = false;
    tcp_ready = true;
    transport = (mqtt_transport_nb_t){.cfg.common_name = "override.example.test"};
    CHECK(mqtt_transport_nb_connect(&transport, "mqtt.example.test", 8883) == 0);
    run_dns_begin();
    CHECK(mqtt_transport_nb_connect(&transport, "mqtt.example.test", 8883) == 0);
    CHECK(mqtt_transport_nb_connect(&transport, "mqtt.example.test", 8883) == 1);
    CHECK(strcmp(tls_name, "override.example.test") == 0);
    mqtt_transport_nb_close(&transport);
    puts("PASS TCP/TLS: readiness after 51ms, bounded select, hostname/SNI, local renegotiation disable, close");
}

static void want_interleaving(void)
{
    const int wants[] = {ESP_TLS_ERR_SSL_WANT_WRITE, ESP_TLS_ERR_SSL_WANT_READ};
    for (unsigned i = 0; i < sizeof(wants) / sizeof(wants[0]); ++i) {
        reset();
        mqtt_transport_nb_t transport;
        connect_ready(&transport);
        const char message[] = "immutable MQTT record";
        write_results[0] = wants[i];
        write_results[1] = (int)sizeof(message);
        write_count = 2;
        CHECK(mqtt_transport_nb_write(&transport, message, sizeof(message)) == 0);
        CHECK(pending_write == (const unsigned char *)message);
        char input[32];
        bool started = false;
        unsigned reads = ssl_reads;
        CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) == 0);
        CHECK(ssl_reads == reads && !started && !invalid_read_during_write);
        /* WANT_READ retries must poll read readiness, WANT_WRITE write. */
        write_ready = wants[i] != ESP_TLS_ERR_SSL_WANT_WRITE;
        read_ready = wants[i] != ESP_TLS_ERR_SSL_WANT_READ;
        CHECK(mqtt_transport_nb_write(&transport, message, sizeof(message)) == 0);
        CHECK(ssl_writes == 1);
        write_ready = read_ready = true;
        CHECK(mqtt_transport_nb_write(&transport, message, sizeof(message)) == (int)sizeof(message));
        CHECK(pending_write == NULL && ssl_writes == 2);
        read_result = 4;
        CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) == 4 && started);
        mqtt_transport_nb_close(&transport);
    }

    reset();
    mqtt_transport_nb_t transport;
    connect_ready(&transport);
    read_result = ESP_TLS_ERR_SSL_WANT_WRITE;
    char input[32];
    bool started = false;
    /* Alert output during application read cannot be confused with the next
     * application's write completion. The caller aborts this connection. */
    errno = 0;
    CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) == -2);
    CHECK(errno == EPROTO && posthandshake_warnings == 1);
    CHECK(started && ssl_writes == 0);
    CHECK(!mqtt_transport_nb_can_read(&transport) && !mqtt_transport_nb_can_write(&transport));
    CHECK(mqtt_transport_nb_write(&transport, "never send", 10) < 0);
    CHECK(ssl_writes == 0);
    mqtt_transport_nb_close(&transport);
    puts("PASS TLS WANT: same write args, no read while write pending, read-WANT_WRITE fails closed");
}

static void partial_progress_and_idle(void)
{
    reset();
    mqtt_transport_nb_t transport;
    connect_ready(&transport);
    char input[16];
    bool started = false;
    read_ready = write_ready = false;
    CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) == 0);
    CHECK(!started && ssl_reads == 0);
    CHECK(mqtt_transport_nb_write(&transport, "packet", 6) == 0 && ssl_writes == 0);
    read_ready = write_ready = true;
    CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) == 0);
    CHECK(started && transport.read_want == ESP_TLS_ERR_SSL_WANT_READ);
    write_results[0] = 2;
    write_results[1] = ESP_TLS_ERR_SSL_WANT_WRITE;
    write_results[2] = 4;
    write_count = 3;
    const char packet[] = "packet";
    CHECK(mqtt_transport_nb_write(&transport, packet, 6) == 2);
    CHECK(mqtt_transport_nb_write(&transport, packet + 2, 4) == 0);
    CHECK(mqtt_transport_nb_write(&transport, packet + 2, 4) == 4);
    CHECK(ssl_writes == 3 && mqtt_transport_nb_can_read(&transport));
    read_result = 0;
    CHECK(mqtt_transport_nb_read(&transport, input, sizeof(input), &started) < 0);
    mqtt_transport_nb_close(&transport);
    puts("PASS transport idle/partial: no readiness is no I/O, read-WANT_READ allows TX, partial progress, EOF");
}

int main(void)
{
    dns_lifetime();
    connect_and_cancel();
    want_interleaving();
    partial_progress_and_idle();
    CHECK(live_jobs == 0 && live_tls == 0);
    printf("mqtt_transport_nb PASS: %u checks; actual adapter, simulated IDF/lwIP/TLS boundary\n", checks);
    return 0;
}
