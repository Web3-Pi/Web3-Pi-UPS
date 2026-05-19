#include "identity.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "identity"

#ifndef FW_VERSION_STR
#define FW_VERSION_STR "esp32:0.5.0+dev"
#endif

#ifndef HW_VERSION_STR
#define HW_VERSION_STR "v3.0"
#endif

/* Track 0 / WS-10 provisioning schema. Keep byte-for-byte in sync with the
 * panel's NVS blob generator (apps/api/scripts/ make-nvs / provisioning CSV)
 * and the partition label in partitions.csv. */
#define PROV_PARTITION   "prov"
#define PROV_NAMESPACE   "w3pups"
#define PROV_KEY_SECRET  "mqtt_secret"

#define MQTT_SECRET_BYTES     32
#define MQTT_PASSWORD_HEX_LEN (MQTT_SECRET_BYTES * 2)

static bool s_secret_ready;
static char s_iccid[24] = {0};
static char s_mqtt_password_hex[MQTT_PASSWORD_HEX_LEN + 1] = {0};
static char s_imei[24] = {0};

static bool iccid_shape_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 19 || n > 20) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/*
 * Load the per-device MQTT secret from the read-only `prov` NVS partition and
 * hex-encode it into s_mqtt_password_hex. The secret is the MQTT password
 * verbatim (32 random bytes → 64 hex chars), identical to what the backend
 * stores sealed and pushes to EMQX. NOTHING here is derived from the ICCID.
 *
 * A blank/missing `prov` means the unit was never provisioned. We do NOT
 * format or fall back — fail loudly so an unprovisioned unit cannot silently
 * authenticate (there is no fleet-wide secret any more).
 */
static esp_err_t load_mqtt_secret(void)
{
    esp_err_t err = nvs_flash_init_partition(PROV_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "prov partition init failed (%s) — device not provisioned "
                 "(Track 0 / WS-10: flash the per-device NVS blob)",
                 esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(PROV_PARTITION, PROV_NAMESPACE,
                                  NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition(%s/%s) failed: %s",
                 PROV_PARTITION, PROV_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    uint8_t raw[MQTT_SECRET_BYTES];
    size_t len = sizeof(raw);
    err = nvs_get_blob(h, PROV_KEY_SECRET, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(%s) failed: %s",
                 PROV_KEY_SECRET, esp_err_to_name(err));
        return err;
    }
    if (len != MQTT_SECRET_BYTES) {
        ESP_LOGE(TAG, "%s has %u bytes, expected %d",
                 PROV_KEY_SECRET, (unsigned)len, MQTT_SECRET_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    for (int i = 0; i < MQTT_SECRET_BYTES; ++i) {
        sprintf(s_mqtt_password_hex + i * 2, "%02x", raw[i]);
    }
    s_mqtt_password_hex[MQTT_PASSWORD_HEX_LEN] = '\0';
    return ESP_OK;
}

esp_err_t identity_init(void)
{
    esp_err_t err = load_mqtt_secret();
    if (err != ESP_OK) {
        s_mqtt_password_hex[0] = '\0';
        return err;
    }
    s_secret_ready = true;
    /* Log only that it loaded + a short prefix for cross-checking against the
     * panel's provisioning artifact — never the full secret. */
    ESP_LOGI(TAG, "per-device MQTT secret loaded (mqtt_password=%.8s...)",
             s_mqtt_password_hex);
    return ESP_OK;
}

esp_err_t identity_set_iccid(const char *iccid)
{
    if (!s_secret_ready) {
        ESP_LOGE(TAG, "identity_init() not called / not provisioned");
        return ESP_ERR_INVALID_STATE;
    }
    if (!iccid || !iccid_shape_ok(iccid)) {
        ESP_LOGE(TAG, "ICCID rejected (must be 19-20 digits): %s",
                 iccid ? iccid : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_iccid, iccid, sizeof(s_iccid) - 1);
    s_iccid[sizeof(s_iccid) - 1] = '\0';
    /* ICCID is the MQTT username + topic prefix (ADR-0002); it is NOT used to
     * derive the password any more (Track 0 / WS-10). Not sensitive. */
    ESP_LOGI(TAG, "ICCID=%s", s_iccid);
    return ESP_OK;
}

const char *identity_iccid(void)             { return s_iccid; }
const char *identity_mqtt_password_hex(void) { return s_mqtt_password_hex; }

void identity_set_imei(const char *imei)
{
    if (!imei) { s_imei[0] = '\0'; return; }
    strncpy(s_imei, imei, sizeof(s_imei) - 1);
    s_imei[sizeof(s_imei) - 1] = '\0';
    /* Strip trailing whitespace / CR / LF that some AT command paths leave on. */
    for (size_t i = strlen(s_imei); i > 0; --i) {
        char c = s_imei[i - 1];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') s_imei[i - 1] = '\0';
        else break;
    }
}

const char *identity_imei(void) { return s_imei; }

const char *identity_fw_version(void) { return FW_VERSION_STR; }
const char *identity_hw_version(void) { return HW_VERSION_STR; }
