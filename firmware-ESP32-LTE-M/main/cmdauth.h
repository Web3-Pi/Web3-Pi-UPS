#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/*
 * WS-9 / ADR-0009 — backend-signed command auth-envelope (`WAE1`).
 *
 * The ESP32 is the only verifier (Decision C): it checks the backend
 * signature + freshness on every downlink command, strips the envelope, and
 * forwards ONLY the inner WUPS frame to the RP2040 (which is unchanged).
 *
 * Keys come from the read-only `prov` NVS partition (same one Track 0 uses
 * for the MQTT secret); freshness state lives in writable `nvs`.
 */

/* Load backend operational pubkey + epoch from `prov`, and the runtime
 * freshness state from `nvs` (initialised from the pinned epoch on first
 * boot). Returns ESP_OK only if the device is provisioned for WS-9. */
esp_err_t cmdauth_init(void);

/*
 * Verify a WAE1 envelope and, on success, hand back a pointer to the inner
 * WUPS frame *inside* `buf` (no copy) plus its length.
 *
 * Returns true  → valid: *frame / *frame_len point at the inner frame and
 *                 the freshness counter has been persisted.
 * Returns false → reject (bad magic/epoch/signature, replay, or expired) —
 *                 caller MUST drop the message.
 */
bool cmdauth_check_and_strip(const uint8_t *buf, size_t len,
                             const uint8_t **frame, size_t *frame_len);
