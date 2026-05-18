#include "cmdauth.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "uECC.h"

#define TAG "cmdauth"

/* prov (read-only, provisioned) — shared namespace with Track 0. */
#define PROV_PARTITION  "prov"
#define PROV_NAMESPACE  "w3pups"
#define KEY_OP_PUB      "bk_op_pub"     /* 65 B uncompressed SEC1 (04||X||Y) */
#define KEY_ROOT_PUB    "bk_root_pub"   /* 65 B — Phase 2 only, stored now    */
#define KEY_EPOCH       "bk_epoch"      /* u32                                */

/* writable runtime freshness state (default `nvs` partition). */
#define STATE_NAMESPACE "w3wsec"
#define KEY_LAST_CTR    "last_ctr"      /* u64 — highest accepted counter     */
#define KEY_CUR_EPOCH   "cur_epoch"     /* u32 — accepted operational epoch   */

#define ENV_MAGIC      "WAE1"
#define ENV_HEAD_LEN   26               /* magic..frame_len (see ADR-0009)    */
#define ENV_SIG_LEN    64
#define PUBKEY_XY_LEN  64               /* uECC wants X||Y, no 0x04 prefix    */

static bool     s_ready;
static uint8_t  s_op_pub[PUBKEY_XY_LEN];
static uint32_t s_cur_epoch;
static uint64_t s_last_ctr;

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

esp_err_t cmdauth_init(void)
{
    /* prov is already brought up by identity_init(); idempotent here. */
    esp_err_t err = nvs_flash_init_partition(PROV_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov init failed: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t ph;
    err = nvs_open_from_partition(PROV_PARTITION, PROV_NAMESPACE,
                                  NVS_READONLY, &ph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t op_pub[65];
    size_t len = sizeof(op_pub);
    err = nvs_get_blob(ph, KEY_OP_PUB, op_pub, &len);
    if (err == ESP_OK && (len != 65 || op_pub[0] != 0x04)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s read failed: %s (device not WS-9 provisioned)",
                 KEY_OP_PUB, esp_err_to_name(err));
        nvs_close(ph);
        return err;
    }
    memcpy(s_op_pub, op_pub + 1, PUBKEY_XY_LEN); /* drop 0x04 prefix */

    uint32_t prov_epoch = 0;
    err = nvs_get_u32(ph, KEY_EPOCH, &prov_epoch);
    nvs_close(ph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s read failed: %s", KEY_EPOCH, esp_err_to_name(err));
        return err;
    }

    /* Runtime freshness state in writable nvs. First boot: seed cur_epoch
     * from the pinned epoch and last_ctr from 0. */
    nvs_handle_t sh;
    err = nvs_open(STATE_NAMESPACE, NVS_READWRITE, &sh);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state open failed: %s", esp_err_to_name(err));
        return err;
    }
    if (nvs_get_u32(sh, KEY_CUR_EPOCH, &s_cur_epoch) != ESP_OK) {
        s_cur_epoch = prov_epoch;
        nvs_set_u32(sh, KEY_CUR_EPOCH, s_cur_epoch);
    }
    if (nvs_get_u64(sh, KEY_LAST_CTR, &s_last_ctr) != ESP_OK) {
        s_last_ctr = 0;
        nvs_set_u64(sh, KEY_LAST_CTR, s_last_ctr);
    }
    nvs_commit(sh);
    nvs_close(sh);

    s_ready = true;
    ESP_LOGI(TAG, "WS-9 ready (epoch=%u, last_ctr=%llu)",
             (unsigned)s_cur_epoch, (unsigned long long)s_last_ctr);
    return ESP_OK;
}

static bool persist_last_ctr(uint64_t ctr)
{
    nvs_handle_t sh;
    if (nvs_open(STATE_NAMESPACE, NVS_READWRITE, &sh) != ESP_OK) return false;
    esp_err_t e = nvs_set_u64(sh, KEY_LAST_CTR, ctr);
    if (e == ESP_OK) e = nvs_commit(sh);
    nvs_close(sh);
    return e == ESP_OK;
}

bool cmdauth_check_and_strip(const uint8_t *buf, size_t len,
                             const uint8_t **frame, size_t *frame_len)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "not initialised — rejecting command");
        return false;
    }
    if (len < ENV_HEAD_LEN + ENV_SIG_LEN) {
        ESP_LOGW(TAG, "envelope too short (%u)", (unsigned)len);
        return false;
    }
    if (memcmp(buf, ENV_MAGIC, 4) != 0) {
        ESP_LOGW(TAG, "bad magic — not a WAE1 envelope");
        return false;
    }

    uint32_t epoch   = rd_u32le(buf + 4);
    uint64_t counter = rd_u64le(buf + 8);
    uint64_t expiry  = rd_u64le(buf + 16);
    uint16_t flen    = (uint16_t)((uint16_t)buf[24] | ((uint16_t)buf[25] << 8));

    if ((size_t)ENV_HEAD_LEN + flen + ENV_SIG_LEN != len) {
        ESP_LOGW(TAG, "length mismatch (frame_len=%u, total=%u)",
                 (unsigned)flen, (unsigned)len);
        return false;
    }
    /* Phase 1: only the pinned/known epoch is accepted. epoch > cur needs a
     * root-signed rollover statement (Phase 2). epoch < cur = rollback. */
    if (epoch != s_cur_epoch) {
        ESP_LOGW(TAG, "epoch %u != cur %u — rejecting",
                 (unsigned)epoch, (unsigned)s_cur_epoch);
        return false;
    }

    const uint8_t *sig = buf + ENV_HEAD_LEN + flen;
    uint8_t digest[32];
    size_t dlen = 0;
    /* ESP-IDF v6 (mbedTLS 4): PSA Crypto is the supported hash path, same as
     * identity.c. psa_crypto_init() is idempotent. */
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, buf, ENV_HEAD_LEN + flen,
                         digest, sizeof(digest), &dlen) != PSA_SUCCESS ||
        dlen != sizeof(digest)) {
        ESP_LOGE(TAG, "sha256 (PSA) failed");
        return false;
    }
    if (uECC_verify(s_op_pub, digest, sizeof(digest), sig,
                    uECC_secp256k1()) != 1) {
        ESP_LOGW(TAG, "signature INVALID — dropping command");
        return false;
    }

    /* Freshness: monotonic counter is the baseline (works with no clock). */
    if (counter <= s_last_ctr) {
        ESP_LOGW(TAG, "replay: counter %llu <= last %llu",
                 (unsigned long long)counter,
                 (unsigned long long)s_last_ctr);
        return false;
    }
    /* expiry is an extra layer, not a precondition (ADR-0009 / §13.3): only
     * enforce when we plausibly have real wall-clock time from NTP. */
    if (expiry != 0) {
        time_t now = time(NULL);
        if (now > 1700000000 && (uint64_t)now > expiry) {
            ESP_LOGW(TAG, "expired (now=%lld > expiry=%llu)",
                     (long long)now, (unsigned long long)expiry);
            return false;
        }
    }

    s_last_ctr = counter;
    if (!persist_last_ctr(counter)) {
        ESP_LOGW(TAG, "failed to persist last_ctr (continuing)");
    }

    *frame     = buf + ENV_HEAD_LEN;
    *frame_len = flen;
    ESP_LOGI(TAG, "command verified (ctr=%llu, len=%u)",
             (unsigned long long)counter, (unsigned)flen);
    return true;
}
