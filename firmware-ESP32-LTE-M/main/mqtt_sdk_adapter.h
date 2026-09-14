#pragma once

#include "esp_err.h"
#include "mqtt_client.h"

/* Owner-task-only. Unlike esp-mqtt 1.0.0 start(), this requires evidence that
 * the previous SDK task finished ALL transport/outbox cleanup. It never stops
 * a live task, destroys a client or discards queued messages.
 *
 * before_restart runs ONLY after that evidence is established, under the SDK's
 * recursive API mutex, before the new task can deliver callbacks. The hook may
 * update short application state (e.g. retire a session/probe), but must not
 * call SDK APIs, wait or log. It also runs on a failed task-create attempt:
 * the old socket is still definitively closed in that case.
 *
 * ESP_ERR_INVALID_STATE means not safely stopped; no hook/start was performed.
 * Initial creation and failed initial xTaskCreate retries use SDK start(). */
esp_err_t mqtt_sdk_revive_stopped(esp_mqtt_client_handle_t client,
                                 void (*before_restart)(void *), void *arg);
