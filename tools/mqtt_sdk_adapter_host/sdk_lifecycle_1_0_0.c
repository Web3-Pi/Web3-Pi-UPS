/* SPDX-License-Identifier: Apache-2.0
 * Verbatim excerpts from espressif/esp-mqtt 1.0.0 mqtt_client.c.
 * Full source SHA-256: 4b24720b34c2bd44b0857a5251f5392663225c618595229540b35f1529663a9a
 * License: LICENSE-esp-mqtt. Test-only; never compiled into firmware.
 * The task cleanup tail is wrapped in a function to inject scheduler orderings.
 */
#include "mqtt_client_priv.h"

#define STOPPED_BIT (1u << 0)

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    if (!client) {
        ESP_LOGE(TAG, "Client was not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    MQTT_API_LOCK(client);
    if (client->state != MQTT_STATE_INIT && client->state != MQTT_STATE_DISCONNECTED) {
        ESP_LOGE(TAG, "Client has started");
        MQTT_API_UNLOCK(client);
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
#if MQTT_CORE_SELECTION_ENABLED
    ESP_LOGD(TAG, "Core selection enabled on %u", MQTT_TASK_CORE);
    if (xTaskCreatePinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle, MQTT_TASK_CORE) != pdTRUE) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#else
    ESP_LOGD(TAG, "Core selection disabled");
    if (xTaskCreate(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle) != pdTRUE) {
        ESP_LOGE(TAG, "Error create mqtt task");
        err = ESP_FAIL;
    }
#endif
    MQTT_API_UNLOCK(client);
    return err;
}

void sdk_fatal_cleanup(esp_mqtt_client_handle_t client)
{
    esp_transport_close(client->transport);
    outbox_delete_all_items(client->outbox);
    client->state = MQTT_STATE_DISCONNECTED;
    xEventGroupSetBits(client->status_bits, STOPPED_BIT);

    vTaskDelete(NULL);
}
