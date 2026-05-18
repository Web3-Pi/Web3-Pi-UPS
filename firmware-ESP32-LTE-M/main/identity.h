#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Device identity helpers (ADR-0002; Track 0 / WS-10 superseded ADR-0005):
 *   - ICCID = SIM serial = device identity (MQTT username + topic prefix).
 *   - mqtt_password = the per-device secret read verbatim from the read-only
 *     `prov` NVS partition (random 32 B → 64 hex chars), provisioned at
 *     production. NOT derived from the (label-visible) ICCID and NOT a
 *     fleet-wide secret any more.
 *
 * Two-phase init:
 *   1. identity_init() once at boot — loads + hex-encodes the per-device
 *      secret from `prov`. Fails loudly if the unit was never provisioned.
 *   2. identity_set_iccid() after AT+CCID has succeeded — validates/stores
 *      the ICCID (username/topic only; no longer affects the password).
 */

esp_err_t identity_init(void);

/* Returns ESP_OK if iccid passes the 19-20 digit shape the backend expects
 * AND the MQTT password could be derived. After this, the accessors below
 * return real values. */
esp_err_t identity_set_iccid(const char *iccid);

/* Stable C-string accessors. Empty string if identity_set_iccid() hasn't been
 * called yet (or the ICCID was rejected). */
const char *identity_iccid(void);
const char *identity_mqtt_password_hex(void);

/* Stash the modem's IMEI here (read via esp_modem_get_imei) so the MQTT
 * identify publisher can include it. Optional — empty string if unset. */
void identity_set_imei(const char *imei);
const char *identity_imei(void);

/* Compile-time firmware identity, exposed for the identify publish. */
const char *identity_fw_version(void);
const char *identity_hw_version(void);
