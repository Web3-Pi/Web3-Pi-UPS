#pragma once
#include "esp_err.h"
typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
