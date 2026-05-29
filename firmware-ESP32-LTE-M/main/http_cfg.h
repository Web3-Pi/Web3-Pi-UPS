#pragma once

/*
 * HTTP-2 (§4.18a) — runtime configuration for the HTTP control-mode backend.
 *
 * The base URL of the user-hosted endpoint is NOT compiled in: a fielded unit
 * is pointed at the operator's server at runtime, written from the RPi host
 * over the WUPS link (net.config, see wups_link.c) and persisted in the
 * writable `nvs` partition. This avoids re-flashing the ESP32 — the production
 * board exposes only a JST programming header, not USB-C.
 *
 * A compile-time default (HTTP_ENDPOINT_BASE in secrets.h) is used only as a
 * fallback when NVS has no value yet, which keeps bench/dev bring-up simple.
 *
 * Storage: namespace `w3http` in the default `nvs` partition:
 *   - `url`    str   base URL, e.g. "https://ups.example.com" (no trailing /)
 *   - `devid`  str   optional device_id override; empty/absent → use ICCID
 *   - `secret` str   the HTTP-mode shared secret (HMAC key) — a short, random,
 *                    human-transcribable code shown on the OLED. This is a
 *                    SEPARATE secret from the MQTT/Arkiv per-device secret: it
 *                    is auto-generated on first use and can be re-rolled from
 *                    the OLED menu, so a self-hoster never has to extract the
 *                    factory secret — they just read this code off the screen.
 *
 * All survive a backend-mode switch (the switch only erases on factory reset,
 * which clears the whole nvs partition — by design).
 */

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest accepted URL / device_id value (matches net.config value cap). */
#define HTTP_CFG_URL_MAX   200
#define HTTP_CFG_DEVID_MAX  64

/* HTTP-mode shared secret: 16 chars of Crockford base32 ≈ 80 bits of entropy.
 * Short enough to read off the 64×32 OLED and hand-transcribe (shown as two
 * groups of 8), strong enough to resist offline brute-force of a captured
 * signed request. The HMAC key is the ASCII bytes of this string. */
#define HTTP_CFG_SECRET_CHARS 16

/* Copy the configured base URL into out[cap]. Order of precedence:
 *   1. NVS `w3http/url` if present and non-empty.
 *   2. Compile-time HTTP_ENDPOINT_BASE (secrets.h) if defined non-empty.
 * Returns ESP_OK on a value, ESP_ERR_NOT_FOUND if neither is set,
 * ESP_ERR_INVALID_SIZE if the value doesn't fit. */
esp_err_t http_cfg_get_url(char *out, size_t cap);

/* Persist a base URL in NVS. Pass an empty string to clear the override
 * (reverting to the compile-time default). */
esp_err_t http_cfg_set_url(const char *url);

/* Copy the device_id into out[cap]: the NVS override if set, otherwise the
 * ICCID (identity_iccid()). ESP_ERR_NOT_FOUND if neither is available yet. */
esp_err_t http_cfg_get_device_id(char *out, size_t cap);

/* Persist a device_id override in NVS (empty string clears it → use ICCID). */
esp_err_t http_cfg_set_device_id(const char *id);

/* Copy the HTTP-mode shared secret into out[cap] (NUL-terminated,
 * HTTP_CFG_SECRET_CHARS chars). Generates + persists a fresh random one on
 * first use, so the device always has one. `cap` must be ≥
 * HTTP_CFG_SECRET_CHARS + 1. */
esp_err_t http_cfg_get_secret(char *out, size_t cap);

/* Generate a new random HTTP-mode secret, replacing any existing one. The old
 * code stops working immediately; the operator must update their server. */
esp_err_t http_cfg_regenerate_secret(void);

#ifdef __cplusplus
}
#endif
