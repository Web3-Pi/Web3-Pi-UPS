#ifndef MQTT_NB_HOST_H
#define MQTT_NB_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>

#define ESP_OK 0
#define ESP_TLS_VER_TLS_1_2 12
#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)
#define MBEDTLS_SSL_RENEGOTIATION 1
#define MBEDTLS_SSL_RENEGOTIATION_ENABLED 1
#define MBEDTLS_SSL_RENEGOTIATION_DISABLED 0
#ifndef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#endif
typedef int esp_err_t;
typedef enum { ESP_TLS_INIT, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_DONE, ESP_TLS_FAIL } esp_tls_conn_state_t;
typedef struct { int renegotiation; } mbedtls_ssl_config;
typedef struct { mbedtls_ssl_config *config; } mbedtls_ssl_context;
typedef struct { bool keep_alive_enable; int keep_alive_idle, keep_alive_interval, keep_alive_count; } tls_keep_alive_cfg_t;
typedef struct {
    bool non_block;
    int tls_version;
    const char *common_name;
    int timeout_ms;
} esp_tls_cfg_t;
typedef struct {
    esp_tls_conn_state_t state;
    int socket_fd;
    fd_set rset, wset;
    bool socket_nonblocking;
    mbedtls_ssl_config config;
    mbedtls_ssl_context ssl;
} esp_tls_t;
typedef int err_t;
#define ERR_OK 0
#define ERR_INPROGRESS (-5)
#define ERR_ARG (-16)
typedef struct { char text[48]; } ip_addr_t;
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
err_t tcpip_try_callback(void (*callback)(void *), void *arg);
err_t dns_gethostbyname(const char *, ip_addr_t *, dns_found_callback, void *);
char *ipaddr_ntoa_r(const ip_addr_t *, char *, int);

esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
esp_err_t esp_tls_get_conn_state(esp_tls_t *, esp_tls_conn_state_t *);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *, int *);
void *esp_tls_get_ssl_context(esp_tls_t *);
const mbedtls_ssl_config *mbedtls_ssl_context_get_config(const mbedtls_ssl_context *);
void mbedtls_ssl_conf_renegotiation(mbedtls_ssl_config *, int);
int esp_tls_get_bytes_avail(esp_tls_t *);
int esp_tls_conn_read(esp_tls_t *, unsigned char *, size_t);
int esp_tls_conn_write(esp_tls_t *, const unsigned char *, size_t);
int esp_tls_conn_destroy(esp_tls_t *);
int host_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int host_getsockopt(int, int, int, void *, socklen_t *);
void *host_calloc(size_t, size_t);
void host_free(void *);
void host_log(const char *, const char *, ...);

#endif
