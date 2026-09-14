#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef struct host_client *esp_mqtt_client_handle_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct {
    struct { struct { const char *uri; } address;
             struct { int (*crt_bundle_attach)(void *); } verification; } broker;
    struct { const char *client_id, *username;
             struct { const char *password; } authentication; } credentials;
    struct { struct { const char *topic, *msg; int msg_len, qos, retain; } last_will;
             int keepalive; } session;
    struct { int timeout_ms; bool disable_auto_reconnect; } network;
    struct { size_t limit; } outbox;
    struct { int stack_size; } task;
} esp_mqtt_client_config_t;
typedef struct {
    int error_type, connect_return_code, esp_tls_last_esp_err, esp_transport_sock_errno;
} esp_mqtt_error_codes_t;
typedef struct {
    const char *topic, *data;
    int topic_len, data_len, total_data_len, current_data_offset, msg_id;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t, *esp_mqtt_event_handle_t;
#define ESP_EVENT_ANY_ID -1
enum { MQTT_EVENT_CONNECTED = 1, MQTT_EVENT_DISCONNECTED, MQTT_EVENT_PUBLISHED,
       MQTT_EVENT_DATA, MQTT_EVENT_ERROR };
enum { MQTT_ERROR_TYPE_TCP_TRANSPORT = 1, MQTT_ERROR_TYPE_CONNECTION_REFUSED,
       MQTT_CONNECTION_REFUSE_BAD_USERNAME = 4, MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED = 5 };
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int32_t id,
                                         esp_event_handler_t handler, void *argument);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client);
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t client);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic,
                             const char *data, int len, int qos, int retain, bool store);
