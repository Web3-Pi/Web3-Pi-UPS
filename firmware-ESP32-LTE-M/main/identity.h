#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Device identity helpers for ADR-0002 / ADR-0005:
 *   - ICCID = SIM serial = device identity (used as MQTT username + topic prefix).
 *   - mqtt_password = hex(HMAC-SHA256(MASTER_SECRET, ICCID))
 *
 * MASTER_SECRET is compiled in via secrets.h (`MASTER_SECRET_HEX`).
 *
 * Two-phase init:
 *   1. identity_init() once at boot — decodes MASTER_SECRET_HEX into raw bytes.
 *   2. identity_set_iccid() after AT+CCID has succeeded — derives the MQTT
 *      password and unblocks identity_get_*() accessors.
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
