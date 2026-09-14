#include "mqtt_sdk_adapter.h"

/* This is the only application file allowed to depend on MQTT private layout.
 * main/CMakeLists.txt pins both this header and mqtt_client.c by SHA-256. */
#include "mqtt_client_priv.h"

#ifdef MQTT_DISABLE_API_LOCKS
#error "The MQTT lifecycle adapter requires SDK recursive API locking"
#endif

/* mqtt_client.c (esp-mqtt 1.0.0): set only after close + outbox cleanup +
 * state=DISCONNECTED; the old task then only calls vTaskDelete(NULL). */
#define WUPS_MQTT_STOPPED_BIT (1u << 0)

esp_err_t mqtt_sdk_revive_stopped(esp_mqtt_client_handle_t client,
                                 void (*before_restart)(void *), void *arg)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    MQTT_API_LOCK(client);
    if (client->state != MQTT_STATE_DISCONNECTED ||
        !(xEventGroupGetBits(client->status_bits) & WUPS_MQTT_STOPPED_BIT) ||
        client->run) {
        MQTT_API_UNLOCK(client);
        return ESP_ERR_INVALID_STATE;
    }

    /* start() creates the task before that task clears STOPPED_BIT. Reserve
     * the stopped-to-started transition here to reject a second start even if
     * the newly created task has not been scheduled yet. */
    xEventGroupClearBits(client->status_bits, WUPS_MQTT_STOPPED_BIT);
    if (before_restart) before_restart(arg);
    esp_err_t err = esp_mqtt_client_start(client); /* same recursive mutex */
    if (err != ESP_OK) {
        /* Under this mutex, with the verified state above and a hook that
         * makes no SDK calls, pinned start() can fail only at xTaskCreate.
         * No new task exists, so a later allocation retry is safe. */
        xEventGroupSetBits(client->status_bits, WUPS_MQTT_STOPPED_BIT);
    }
    MQTT_API_UNLOCK(client);
    return err;
}
