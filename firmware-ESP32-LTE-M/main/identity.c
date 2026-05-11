#include "identity.h"
#include "secrets.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "psa/crypto.h"

#define TAG "identity"

#ifndef MASTER_SECRET_HEX
#error "MASTER_SECRET_HEX must be defined in secrets.h (64 hex chars)."
#endif

#ifndef FW_VERSION_STR
#define FW_VERSION_STR "esp32:0.5.0+dev"
#endif

#ifndef HW_VERSION_STR
#define HW_VERSION_STR "v3.0"
#endif

#define MASTER_SECRET_BYTES 32
#define MQTT_PASSWORD_HEX_LEN 64

static uint8_t s_master_secret[MASTER_SECRET_BYTES];
static bool    s_master_ready;

static char s_iccid[24] = {0};
static char s_mqtt_password_hex[MQTT_PASSWORD_HEX_LEN + 1] = {0};
static char s_imei[24] = {0};

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t hex_decode(const char *hex, size_t hex_len,
                            uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return ESP_ERR_INVALID_ARG;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

esp_err_t identity_init(void)
{
    const char *hex = MASTER_SECRET_HEX;
    size_t hex_len = strlen(hex);
    if (hex_len != MASTER_SECRET_BYTES * 2) {
        ESP_LOGE(TAG, "MASTER_SECRET_HEX must be %d chars, got %u",
                 MASTER_SECRET_BYTES * 2, (unsigned)hex_len);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = hex_decode(hex, hex_len, s_master_secret, MASTER_SECRET_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MASTER_SECRET_HEX is not valid hex");
        return err;
    }
    s_master_ready = true;
    /* Don't log the secret itself — only that it was loaded. */
    ESP_LOGI(TAG, "master secret loaded (%d bytes)", MASTER_SECRET_BYTES);
    return ESP_OK;
}

static bool iccid_shape_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 19 || n > 20) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static esp_err_t derive_password_hex(const char *iccid, char out_hex[MQTT_PASSWORD_HEX_LEN + 1])
{
    /* mbedTLS 4 removed the legacy single-shot mbedtls_md_hmac() from the
     * public API; PSA Crypto is the supported path. ESP-IDF auto-inits PSA
     * in startup, but psa_crypto_init() is idempotent so call it for safety. */
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)st);
        return ESP_FAIL;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, MASTER_SECRET_BYTES * 8);

    psa_key_id_t key_id = 0;
    st = psa_import_key(&attr, s_master_secret, MASTER_SECRET_BYTES, &key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %ld", (long)st);
        return ESP_FAIL;
    }

    uint8_t digest[32];
    size_t mac_len = 0;
    st = psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                         (const uint8_t *)iccid, strlen(iccid),
                         digest, sizeof(digest), &mac_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS || mac_len != sizeof(digest)) {
        ESP_LOGE(TAG, "psa_mac_compute failed: %ld (got %u bytes)",
                 (long)st, (unsigned)mac_len);
        return ESP_FAIL;
    }

    for (int i = 0; i < 32; ++i) {
        sprintf(out_hex + i * 2, "%02x", digest[i]);
    }
    out_hex[MQTT_PASSWORD_HEX_LEN] = '\0';
    return ESP_OK;
}

esp_err_t identity_set_iccid(const char *iccid)
{
    if (!s_master_ready) {
        ESP_LOGE(TAG, "identity_init() not called yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (!iccid || !iccid_shape_ok(iccid)) {
        ESP_LOGE(TAG, "ICCID rejected (must be 19-20 digits): %s",
                 iccid ? iccid : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_iccid, iccid, sizeof(s_iccid) - 1);
    s_iccid[sizeof(s_iccid) - 1] = '\0';

    esp_err_t err = derive_password_hex(s_iccid, s_mqtt_password_hex);
    if (err != ESP_OK) {
        s_iccid[0] = '\0';
        s_mqtt_password_hex[0] = '\0';
        return err;
    }
    /* Log iccid (not sensitive) + first 8 hex of password for cross-checking
     * against the panel's createHmac() output, without leaking the full
     * secret. */
    ESP_LOGI(TAG, "ICCID=%s mqtt_password=%.8s... (HMAC-SHA256 of ICCID)",
             s_iccid, s_mqtt_password_hex);
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
