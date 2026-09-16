/* MQTT-only ESP-TLS adapter; no global transport or IDF modifications. */
#pragma once
#include "esp_tls.h"
#include <stddef.h>

typedef struct mqtt_dns_job mqtt_dns_job_t;
typedef struct {
    esp_tls_t *tls;
    esp_tls_cfg_t cfg;
    tls_keep_alive_cfg_t keep_alive;
    mqtt_dns_job_t *dns;
    char address[48];
    int socket_fd;
    int read_want, write_want;
    bool connected, failed;
} mqtt_transport_nb_t;

/* Each call makes at most one nonblocking TLS progress attempt.
 * 1 = connected, 0 = in progress, -1 = failure. cfg pointers retain SDK lifetime. */
int mqtt_transport_nb_connect(mqtt_transport_nb_t *t, const char *host, int port);
/* 0 = WANT_READ/WANT_WRITE/no readiness; positive = application bytes;
 * negative = terminal transport error. read may report progress on ciphertext
 * before a whole authenticated record becomes available. */
int mqtt_transport_nb_read(mqtt_transport_nb_t *t, char *data, int length, bool *started);
int mqtt_transport_nb_write(mqtt_transport_nb_t *t, const char *data, int length);
bool mqtt_transport_nb_can_read(const mqtt_transport_nb_t *t);
bool mqtt_transport_nb_can_write(const mqtt_transport_nb_t *t);
void mqtt_transport_nb_close(mqtt_transport_nb_t *t);
