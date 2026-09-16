/* Web3-Pi MQTT-only ESP-TLS transport. SPDX-License-Identifier: Apache-2.0 */
#include "mqtt_transport_nb.h"
#include "mqtt_service.h"
#include "esp_log.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/sockets.h"
#include "mbedtls/ssl.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ESP-TLS loops internally across output records. Each service write must
 * fit one record, so a positive partial return cannot hide another WANT. */
_Static_assert(MBEDTLS_SSL_OUT_CONTENT_LEN >= MQTT_SERVICE_WRITE_CHUNK,
               "bounded MQTT write chunk exceeds the TLS output record size");

/* DNS callbacks can outlive a cancelled client. A separate job owns its name
 * and result until both the client and lwIP callback have released it. */
struct mqtt_dns_job {
    atomic_uint references;
    atomic_int result;
    char name[256];
    char address[48];
};

static void dns_release(mqtt_dns_job_t *job)
{
    if (job && atomic_fetch_sub(&job->references, 1) == 1) free(job);
}

static void dns_done(const char *name, const ip_addr_t *address, void *arg)
{
    (void)name;
    mqtt_dns_job_t *job = arg;
    int result = -1;
    if (address && ipaddr_ntoa_r(address, job->address, sizeof(job->address))) result = 1;
    atomic_store_explicit(&job->result, result, memory_order_release);
    dns_release(job);
}

static void dns_begin(void *arg)
{
    mqtt_dns_job_t *job = arg;
    ip_addr_t address;
    err_t err = dns_gethostbyname(job->name, &address, dns_done, job);
    if (err != ERR_INPROGRESS) dns_done(job->name, err == ERR_OK ? &address : NULL, job);
}

int mqtt_transport_nb_connect(mqtt_transport_nb_t *t, const char *host, int port)
{
    if (!t->dns) {
        if (!host || strlen(host) >= sizeof(t->dns->name)) return -1;
        t->dns = calloc(1, sizeof(*t->dns));
        if (!t->dns) return -1;
        atomic_init(&t->dns->references, 2);
        atomic_init(&t->dns->result, 0);
        strcpy(t->dns->name, host);
        if (tcpip_try_callback(dns_begin, t->dns) != ERR_OK) {
            dns_release(t->dns);
            dns_release(t->dns);
            t->dns = NULL;
            return -1;
        }
        return 0;
    }
    int resolved = atomic_load_explicit(&t->dns->result, memory_order_acquire);
    if (resolved <= 0) return resolved;
    if (!t->tls) {
        strcpy(t->address, t->dns->address);
        t->cfg.non_block = true;
        t->cfg.tls_version = ESP_TLS_VER_TLS_1_2;
        /* Connecting to the resolved address must still authenticate and send
         * SNI for the original server, never for the numeric address. */
        if (!t->cfg.common_name) t->cfg.common_name = host;
        t->tls = esp_tls_init();
        t->socket_fd = -1;
        if (!t->tls) return -1;
    }
    /* IDF v6.0.2's CONNECTING branch waits using cfg.timeout_ms and reuses
     * select's mutated fd_sets on the next call. Poll independently once that
     * state is reached, and skip its select for this one already-ready step.
     * This does not change socket flags: only the INIT branch does that. */
    esp_tls_conn_state_t state;
    bool skip_connect_poll = false;
    if (esp_tls_get_conn_state(t->tls, &state) != ESP_OK) return -1;
    if (state == ESP_TLS_CONNECTING) {
        int fd;
        if (esp_tls_get_conn_sockfd(t->tls, &fd) != ESP_OK || fd < 0) return -1;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval timeout = {0};
        int poll = select(fd + 1, NULL, &set, NULL, &timeout);
        if (poll <= 0) return poll;
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) return -1;
        skip_connect_poll = true;
    }
    t->cfg.non_block = !skip_connect_poll;
    t->cfg.timeout_ms = 1; /* positive: IDF interprets zero as an infinite select */
    int result = esp_tls_conn_new_async(t->address, strlen(t->address), port, &t->cfg, t->tls);
    t->cfg.non_block = true;
    if (result == 1) {
        if (esp_tls_get_conn_sockfd(t->tls, &t->socket_fd) != ESP_OK) return -1;
#if defined(MBEDTLS_SSL_RENEGOTIATION)
        /* ESP-TLS owns a dedicated mutable config for this connection. Use
         * mbedTLS's public context/config accessors; never modify shared/global
         * config. No post-handshake renegotiation may emit records while a
         * suspended application write is waiting to be retried. */
        mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(t->tls);
        mbedtls_ssl_config *config = (mbedtls_ssl_config *)mbedtls_ssl_context_get_config(ssl);
        mbedtls_ssl_conf_renegotiation(config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
        t->connected = true;
    }
    return result;
}

static int ready(mqtt_transport_nb_t *t, bool write)
{
    if (!t->connected || t->socket_fd < 0) return -1;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(t->socket_fd, &set);
    struct timeval timeout = {0};
    return select(t->socket_fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &timeout);
}

int mqtt_transport_nb_read(mqtt_transport_nb_t *t, char *data, int length, bool *started)
{
    if (t->failed) return -1;
    if (!mqtt_transport_nb_can_read(t)) return 0;
    int available = esp_tls_get_bytes_avail(t->tls);
    int poll = available > 0 ? 1 : ready(t, t->read_want == ESP_TLS_ERR_SSL_WANT_WRITE);
    if (poll <= 0) return poll;
    *started = true;
    int result = esp_tls_conn_read(t->tls, (unsigned char *)data, length);
    t->read_want = result;
    if (result == ESP_TLS_ERR_SSL_WANT_WRITE) {
        /* TLS 1.2 application input normally needs WANT_READ only. A refused
         * renegotiation / post-handshake alert can suspend output here. Do not
         * risk flushing that record as a later application SSL_write: close
         * this connection, retain the QoS outbox, and reconnect explicitly. */
        t->failed = true;
        ESP_LOGW("mqtt_nb", "TLS read needs blocked post-handshake output; recovering without interleaving");
        errno = EPROTO;
        return -2;
    }
    if (result == ESP_TLS_ERR_SSL_WANT_READ) return 0;
    return result == 0 ? -1 : result;
}

int mqtt_transport_nb_write(mqtt_transport_nb_t *t, const char *data, int length)
{
    if (t->failed) return -1;
    if (!mqtt_transport_nb_can_write(t)) return 0;
    int poll = ready(t, t->write_want != ESP_TLS_ERR_SSL_WANT_READ);
    if (poll <= 0) return poll;
    int result = esp_tls_conn_write(t->tls, (const unsigned char *)data, length);
    t->write_want = result;
    if (result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
    return result == 0 ? -1 : result;
}

bool mqtt_transport_nb_can_read(const mqtt_transport_nb_t *t)
{
    return !t->failed && t->write_want != ESP_TLS_ERR_SSL_WANT_READ &&
           t->write_want != ESP_TLS_ERR_SSL_WANT_WRITE;
}

bool mqtt_transport_nb_can_write(const mqtt_transport_nb_t *t)
{
    return !t->failed && t->read_want != ESP_TLS_ERR_SSL_WANT_WRITE;
}

void mqtt_transport_nb_close(mqtt_transport_nb_t *t)
{
    if (t->tls) esp_tls_conn_destroy(t->tls);
    dns_release(t->dns);
    memset(t, 0, sizeof(*t));
    t->socket_fd = -1;
}
