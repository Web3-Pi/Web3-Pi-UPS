/*
 * HTTP-2 (§4.18a) — HTTP control-mode runtime config store. See http_cfg.h.
 */

#include "http_cfg.h"
#include "identity.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "secrets.h"

#define TAG "http_cfg"

#define NS_HTTP     "w3http"
#define KEY_URL     "url"
#define KEY_DEVID   "devid"
#define KEY_SECRET  "secret"

/* Crockford base32 — excludes I, L, O, U to avoid hand-transcription mistakes. */
static const char SECRET_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Compile-time fallback. secrets.h.example documents this; absent → "". */
#ifndef HTTP_ENDPOINT_BASE
#define HTTP_ENDPOINT_BASE ""
#endif

esp_err_t http_cfg_get_url(char *out, size_t cap)
{
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = cap;
        err = nvs_get_str(h, KEY_URL, out, &len);
        nvs_close(h);
        if (err == ESP_OK && out[0] != '\0') return ESP_OK;
        if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_SIZE;
        /* fall through to compile default on NOT_FOUND / empty */
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", NS_HTTP, esp_err_to_name(err));
    }

    const char *def = HTTP_ENDPOINT_BASE;
    if (def[0] == '\0') return ESP_ERR_NOT_FOUND;
    if (strlen(def) + 1 > cap) return ESP_ERR_INVALID_SIZE;
    strcpy(out, def);
    return ESP_OK;
}

esp_err_t http_cfg_set_url(const char *url)
{
    if (!url) return ESP_ERR_INVALID_ARG;
    if (strlen(url) > HTTP_CFG_URL_MAX) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (url[0] == '\0') {
        err = nvs_erase_key(h, KEY_URL);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(h, KEY_URL, url);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP endpoint URL %s", url[0] ? "updated" : "cleared");
    }
    return err;
}

esp_err_t http_cfg_get_device_id(char *out, size_t cap)
{
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = cap;
        err = nvs_get_str(h, KEY_DEVID, out, &len);
        nvs_close(h);
        if (err == ESP_OK && out[0] != '\0') return ESP_OK;
        if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_SIZE;
    }

    /* Default device_id = SIM ICCID. */
    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') return ESP_ERR_NOT_FOUND;
    if (strlen(iccid) + 1 > cap) return ESP_ERR_INVALID_SIZE;
    strcpy(out, iccid);
    return ESP_OK;
}

esp_err_t http_cfg_set_device_id(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    if (strlen(id) > HTTP_CFG_DEVID_MAX) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (id[0] == '\0') {
        err = nvs_erase_key(h, KEY_DEVID);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_str(h, KEY_DEVID, id);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void gen_secret(char out[HTTP_CFG_SECRET_CHARS + 1])
{
    for (int i = 0; i < HTTP_CFG_SECRET_CHARS; ++i) {
        /* esp_random() is the hardware RNG; & 31 indexes the 32-char alphabet. */
        out[i] = SECRET_ALPHABET[esp_random() & 31u];
    }
    out[HTTP_CFG_SECRET_CHARS] = '\0';
}

static esp_err_t store_secret(const char *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_SECRET, s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t http_cfg_get_secret(char *out, size_t cap)
{
    if (!out || cap < HTTP_CFG_SECRET_CHARS + 1) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_HTTP, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = cap;
        err = nvs_get_str(h, KEY_SECRET, out, &len);
        nvs_close(h);
        if (err == ESP_OK && out[0] != '\0') return ESP_OK;
    }

    /* Absent → generate one now and persist it (first-use generation). */
    char fresh[HTTP_CFG_SECRET_CHARS + 1];
    gen_secret(fresh);
    err = store_secret(fresh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist generated HTTP secret: %s",
                 esp_err_to_name(err));
        return err;
    }
    memcpy(out, fresh, HTTP_CFG_SECRET_CHARS + 1);
    ESP_LOGI(TAG, "generated new HTTP-mode secret (shown on OLED, never logged)");
    return ESP_OK;
}

esp_err_t http_cfg_regenerate_secret(void)
{
    char fresh[HTTP_CFG_SECRET_CHARS + 1];
    gen_secret(fresh);
    esp_err_t err = store_secret(fresh);
    if (err == ESP_OK) ESP_LOGI(TAG, "HTTP-mode secret regenerated");
    return err;
}
