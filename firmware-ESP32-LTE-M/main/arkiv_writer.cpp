/*
 * Track 2 / ADR-0011 P4 — device-side Arkiv entity writer.
 *
 * See arkiv_writer.h for the contract. This file wraps the C++ helpers in
 * components/arkiv_crypto/ (ops_tx_data + tx_signer + secp256k1) behind a
 * C-callable façade and handles the JSON-RPC plumbing (nonce, gas price,
 * raw-tx submit) via arkiv_rpc.c.
 */

#include "arkiv_writer.h"

extern "C" {
#include "arkiv_rpc.h"
#include "arkiv_cfg.h"
#include "cmdauth_arkiv.h"
#include "identity.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
}

#include "arkiv_crypto/ops_tx_data.h"
#include "arkiv_crypto/tx_signer.h"
#include "arkiv_crypto/secp256k1.h"
#include "arkiv_crypto/aead.h"

#include <string>
#include <vector>
#include <string.h>

#define TAG "arkiv_writer"

/* prov (read-only, provisioned) — same source as cmdauth_arkiv. */
#define PROV_PARTITION  "prov"
#define PROV_NAMESPACE  "w3pups"
#define KEY_DEV_PRIV    "ak_dev_priv"

/* Writable NVS (default partition) for the device-write monotonic seq.
 * Separate from cmdauth_arkiv's `w3arkiv` namespace — that one keeps the
 * inbound replay baseline (`last_ctr`); this one is the outbound entity
 * sequence the panel ingest uses as its cursor. */
#define WRITER_NAMESPACE "w3wr"
#define KEY_OUT_SEQ      "out_seq"

#define ADDR_LEN 20
#define PUB_LEN  64
#define PRIV_LEN ARKIV_SECP256K1_PRIVKEY_LEN

/* Gas defaults — Braga is a testnet with a sub-Gwei base fee (observed
 * ~250 wei on a chain explorer). A 1-Gwei floor here was 4 million times
 * the real price and made 0.001 GLM only cover a single tx; drop to 1 Kwei
 * which is still well above the real base. CreateOp with a few-hundred-
 * byte payload + a handful of attributes consumes ~80-150K gas; 300K
 * gives headroom. Worst case ≈ 300K × 1 Kwei = 3·10^8 wei = 3·10^-10 GLM
 * — millions of tx per 0.001 GLM funding. */
static constexpr uint64_t GAS_PRICE_FLOOR_WEI = 1'000ULL;
static constexpr uint64_t GAS_LIMIT_DEFAULT   = 300'000ULL;

static SemaphoreHandle_t s_lock;
static bool              s_ready;
static uint8_t           s_dev_priv[PRIV_LEN];
static uint8_t           s_dev_addr[ADDR_LEN];
/* Outbound entity seq — the AEAD nonce's uniqueness depends on this NEVER
 * repeating, even across crashes (a repeated (key,nonce) is a catastrophic GCM
 * break). Window-reservation: s_seq_reserved (persisted) is an exclusive upper
 * bound on every seq that may have been handed out; in-RAM increments are issued
 * below it, and NVS is only touched once per SEQ_RESERVE_WINDOW. */
static uint64_t          s_seq_next;        /* next value to hand out (RAM) */
static uint64_t          s_seq_reserved;    /* persisted high-water (exclusive) */
static SemaphoreHandle_t s_seq_lock;
static constexpr uint64_t SEQ_RESERVE_WINDOW = 1024;
static constexpr uint64_t SEQ_MAX_48         = (1ULL << 48) - 1;  /* nonce carries low 48 bits */

/* Payload-seal (ADR-0013) per-epoch key cache. The three K_dir are derived once
 * (ECDH + 3×HKDF) per binding generation and reused; re-derived when cmdauth's
 * binding_gen changes (bind / epoch bump / clear). RAM only — never persisted. */
static SemaphoreHandle_t s_seal_lock;
static uint32_t          s_seal_gen;
static bool              s_seal_valid;
static uint32_t          s_seal_epoch;
static uint8_t           s_k_tlm[32], s_k_ack[32], s_k_event[32];
static char              s_owner_lower[2 + ADDR_LEN * 2 + 1];  /* "0x"+40+nul */
static char              s_devpub_lower[PUB_LEN * 2 + 1];      /* 128+nul     */

/* Chain-health tracking — lets us tell "submit OK but nothing is being
 * included" apart from "submit OK and the chain is moving". Updated on
 * every submit cycle (one extra eth_blockNumber RPC per cycle).
 *
 * Stall is reported as ESP_LOGW after 1 cycle without head progress and
 * escalated to ESP_LOGE after 2, with the pending-tx backlog included so
 * an operator can see at a glance "Braga RPC is happy, but my last N tx
 * are sitting in mempool because no new blocks are being produced". */
static uint64_t s_health_last_head            = 0;
static int64_t  s_health_last_head_change_us  = 0;
static uint64_t s_health_baseline_nonce       = 0;
static uint32_t s_health_consec_stalled       = 0;

/* --- Async submit pipeline: queue + dedicated worker task --------------- */

struct WriteJob {
    std::string                content_type;
    std::vector<uint8_t>       payload;
    uint32_t                   expires_in_seconds = 0;
    std::vector<arkiv::Attribute> attrs;
};

static QueueHandle_t s_queue;   /* holds WriteJob* */
static constexpr UBaseType_t WRITER_QUEUE_LEN = 8;
static void writer_task(void *arg);

extern "C" bool arkiv_writer_ready(void) { return s_ready; }

extern "C" const uint8_t *arkiv_writer_device_addr(void)
{
    return s_ready ? s_dev_addr : nullptr;
}

/* Persist the reservation high-water. Returns true only on a fully-committed
 * write — the caller fails closed on false (never hands out an unreserved seq). */
static bool persist_seq_reserved(uint64_t reserved)
{
    nvs_handle_t wh;
    if (nvs_open(WRITER_NAMESPACE, NVS_READWRITE, &wh) != ESP_OK) return false;
    esp_err_t e1 = nvs_set_u64(wh, KEY_OUT_SEQ, reserved);
    esp_err_t e2 = nvs_commit(wh);
    nvs_close(wh);
    return e1 == ESP_OK && e2 == ESP_OK;
}

extern "C" uint64_t arkiv_writer_next_seq(void)
{
    if (!s_seq_lock) return 0;   /* pre-init — 0 = "unavailable", caller drops */

    xSemaphoreTake(s_seq_lock, portMAX_DELAY);

    /* Refuse to approach the 48-bit nonce field wrap (astronomically far at any
     * real cadence). Fail-closed rather than risk a wrapped, reused nonce. */
    if (s_seq_next >= SEQ_MAX_48 - SEQ_RESERVE_WINDOW) {
        ESP_LOGE(TAG, "out_seq near 2^48 — refusing (would risk nonce reuse)");
        xSemaphoreGive(s_seq_lock);
        return 0;
    }

    /* Extend the durable reservation when the window is exhausted. The new
     * high-water is committed BEFORE the value is issued, so every handed-out
     * seq is < the persisted reservation and a reboot resumes strictly above
     * it — no (key,nonce) reuse is possible. */
    if (s_seq_next >= s_seq_reserved) {
        uint64_t nr = s_seq_next + SEQ_RESERVE_WINDOW;
        if (!persist_seq_reserved(nr)) {
            ESP_LOGE(TAG, "out_seq reservation persist failed — dropping (fail-closed)");
            xSemaphoreGive(s_seq_lock);
            return 0;   /* fail-closed: never seal with an undurable nonce */
        }
        s_seq_reserved = nr;
    }

    uint64_t v = s_seq_next;   /* v < s_seq_reserved (persisted) → durable */
    s_seq_next++;
    xSemaphoreGive(s_seq_lock);
    return v;                  /* >= 1 in normal operation; 0 only signals failure */
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

static void writer_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Re-derive the three per-stream K_dir if the owner binding changed. Caller
 * holds s_seal_lock. False if UNCLAIMED (no enc_pub) or any crypto step fails. */
static bool seal_refresh_keys_locked(void)
{
    const uint8_t *enc_pub    = cmdauth_arkiv_enc_pub();
    const uint8_t *owner_addr = cmdauth_arkiv_owner_addr();
    const uint8_t *dev_pub    = cmdauth_arkiv_device_pub();
    if (!enc_pub || !owner_addr || !dev_pub) return false;   /* unclaimed */

    uint32_t gen = cmdauth_arkiv_binding_gen();
    if (s_seal_valid && s_seal_gen == gen) return true;       /* cache hit */

    uint32_t epoch = cmdauth_arkiv_key_epoch();
    uint8_t  shared_x[32];
    /* ECDH peer is the owner ENCRYPTION key (ADR-0013), not owner_pub. */
    if (arkiv_secp256k1_ecdh(s_dev_priv, enc_pub, shared_x) != 0) return false;

    /* HKDF salt strings (== credentials.ts): "0x"+lower(owner_addr) | lower(dev_pub). */
    s_owner_lower[0] = '0'; s_owner_lower[1] = 'x';
    hex_encode(owner_addr, ADDR_LEN, s_owner_lower + 2);
    hex_encode(dev_pub, PUB_LEN, s_devpub_lower);

    bool ok =
        arkiv_aead_derive_key(shared_x, s_owner_lower, s_devpub_lower,
                              ARKIV_AEAD_TYPE_TELEMETRY, epoch, s_k_tlm) == 0 &&
        arkiv_aead_derive_key(shared_x, s_owner_lower, s_devpub_lower,
                              ARKIV_AEAD_TYPE_ACK, epoch, s_k_ack) == 0 &&
        arkiv_aead_derive_key(shared_x, s_owner_lower, s_devpub_lower,
                              ARKIV_AEAD_TYPE_EVENT, epoch, s_k_event) == 0;
    writer_secure_zero(shared_x, sizeof(shared_x));
    if (!ok) { s_seal_valid = false; return false; }

    s_seal_epoch = epoch;
    s_seal_gen   = gen;
    s_seal_valid = true;
    ESP_LOGI(TAG, "payload-seal keys derived (epoch=%u, gen=%u)",
             (unsigned)epoch, (unsigned)gen);
    return true;
}

extern "C" int arkiv_writer_payload_seal(uint8_t type_tag, uint64_t seq,
                                         const char *entity_type,
                                         const char *command_id_or_null,
                                         const uint8_t *pt, size_t pt_len,
                                         uint8_t *out, size_t out_cap,
                                         size_t *out_len, uint32_t *out_epoch)
{
    if (!s_ready || !s_seal_lock) return -1;
    if (seq == 0) return -2;                       /* next_seq() failure sentinel */
    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') return -3;

    xSemaphoreTake(s_seal_lock, portMAX_DELAY);
    int rc = -4;                                   /* default: unclaimed / derive fail */
    if (seal_refresh_keys_locked()) {
        const uint8_t *kdir = nullptr;
        switch (type_tag) {
            case ARKIV_AEAD_TYPE_TELEMETRY: kdir = s_k_tlm;   break;
            case ARKIV_AEAD_TYPE_ACK:       kdir = s_k_ack;   break;
            case ARKIV_AEAD_TYPE_EVENT:     kdir = s_k_event; break;
            default: break;
        }
        if (!kdir) {
            rc = -5;
        } else {
            uint32_t epoch = s_seal_epoch;
            int sr = arkiv_aead_seal(type_tag, epoch, seq, kdir, iccid,
                                     entity_type, command_id_or_null,
                                     pt, pt_len, out, out_cap, out_len);
            if (sr != 0) {
                rc = -6;
            } else {
                if (out_epoch) *out_epoch = epoch;
                rc = 0;
            }
        }
    }
    xSemaphoreGive(s_seal_lock);
    return rc;
}

extern "C" bool arkiv_writer_seal_prewarm(void)
{
    if (!s_ready || !s_seal_lock) return false;
    /* Derive the K_dir now (on the caller's stack — meant to be the 8 KB claim
     * task) so the first ack/event from the 4 KB wups_rx task only runs GCM. */
    xSemaphoreTake(s_seal_lock, portMAX_DELAY);
    bool ok = seal_refresh_keys_locked();
    xSemaphoreGive(s_seal_lock);
    return ok;
}

extern "C" esp_err_t arkiv_writer_init(void)
{
    if (s_ready) return ESP_OK;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_seq_lock) {
        s_seq_lock = xSemaphoreCreateMutex();
        if (!s_seq_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_seal_lock) {
        s_seal_lock = xSemaphoreCreateMutex();
        if (!s_seal_lock) return ESP_ERR_NO_MEM;
    }

    /* Recover the persisted out-seq reservation. Resume STRICTLY ABOVE the
     * stored value (persisted + 1): migration-proof across both the old
     * persist-every-bump semantics (stored = last used) and the new
     * window-reservation semantics (stored = first-unused high-water). At most
     * one seq is skipped per reboot — harmless for a 48-bit monotonic cursor.
     * The first next_seq() then forces a fresh durable reservation. */
    uint64_t persisted = 0;
    nvs_handle_t wh;
    if (nvs_open(WRITER_NAMESPACE, NVS_READONLY, &wh) == ESP_OK) {
        if (nvs_get_u64(wh, KEY_OUT_SEQ, &persisted) != ESP_OK) persisted = 0;
        nvs_close(wh);
    }
    s_seq_next     = persisted + 1;
    s_seq_reserved = s_seq_next;   /* == s_seq_next forces reservation on first issue */

    /* prov is brought up by identity_init(); this is idempotent. */
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

    size_t plen = sizeof(s_dev_priv);
    err = nvs_get_blob(ph, KEY_DEV_PRIV, s_dev_priv, &plen);
    nvs_close(ph);
    if (err != ESP_OK || plen != PRIV_LEN) {
        ESP_LOGW(TAG, "%s absent (%s) — Paranoic writer unavailable",
                 KEY_DEV_PRIV, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    if (arkiv_secp256k1_derive_address(s_dev_priv, s_dev_addr) != 0) {
        memset(s_dev_priv, 0, sizeof(s_dev_priv));
        ESP_LOGE(TAG, "device key derivation failed");
        return ESP_FAIL;
    }

    char hex[ADDR_LEN * 2 + 1];
    hex_encode(s_dev_addr, ADDR_LEN, hex);
    /* This line is the operator's cue: fund the address with Braga GLM
     * before the first telemetry/ACK/event tx, otherwise eth_sendRawTransaction
     * succeeds (the node still accepts it) but the tx reverts on inclusion. */
    ESP_LOGI(TAG, "Paranoic device wallet: 0x%s (fund this on Braga for P4 writes)",
             hex);

    /* Anchor the health timer at boot so a fresh device doesn't report a
     * multi-year "stall" on its first submit. */
    s_health_last_head_change_us = esp_timer_get_time();

    s_ready = true;

    /* Spin up the async writer pipeline if it isn't already running.
     * 10 KB stack: TLS handshake (~4 KB) + cJSON over the response
     * (1-2 KB) + RLP signing scratch (~1 KB) + vector heap headers. */
    if (!s_queue) {
        s_queue = xQueueCreate(WRITER_QUEUE_LEN, sizeof(WriteJob *));
        if (s_queue) {
            xTaskCreate(writer_task, "arkiv_writer", 10240, NULL, 4, NULL);
            ESP_LOGI(TAG, "writer task started (queue=%d)", (int)WRITER_QUEUE_LEN);
        } else {
            ESP_LOGW(TAG, "writer queue alloc failed — async path unavailable");
        }
    }
    return ESP_OK;
}

/* Decode the storage contract address into 20 raw bytes once. */
static const std::vector<uint8_t> &storage_addr_bytes()
{
    static const std::vector<uint8_t> bytes = []() {
        std::vector<uint8_t> v(ADDR_LEN, 0);
        /* Skip "0x" then last 40 hex chars. */
        const char *h = ARKIV_STORAGE_CONTRACT_HEX + 2;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        for (size_t i = 0; i < ADDR_LEN; ++i) {
            v[i] = (uint8_t)((nib(h[2 * i]) << 4) | nib(h[2 * i + 1]));
        }
        return v;
    }();
    return bytes;
}

/* Internal: do_submit using a pre-built CreateOp. Same locking + chain
 * plumbing as the public sync API. Returns ESP_OK / ESP_FAIL. */
static esp_err_t submit_create(arkiv::CreateOp &create, uint8_t out_tx_hash[32])
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t rc = ESP_FAIL;
    uint64_t nonce = 0;
    if (arkiv_eth_get_tx_count(s_dev_addr, &nonce) != ESP_OK) {
        ESP_LOGW(TAG, "eth_getTransactionCount failed");
        goto out;
    }

    /* Chain-health probe: one extra RPC, but it's how we detect
     * "submit acks but chain isn't producing blocks" — exactly the
     * case where eth_sendRawTransaction keeps returning a hash but the
     * tx never gets mined. Failure is non-fatal: we still attempt the
     * submit, just without a health update for this cycle. */
    {
        uint64_t head = 0;
        if (arkiv_eth_block_number(&head) == ESP_OK) {
            int64_t now_us = esp_timer_get_time();
            if (head != s_health_last_head) {
                if (s_health_last_head == 0) {
                    ESP_LOGI(TAG,
                             "arkiv_health: initial head=%llu pending_nonce=%llu",
                             (unsigned long long)head,
                             (unsigned long long)nonce);
                } else {
                    int64_t since_us = now_us - s_health_last_head_change_us;
                    ESP_LOGI(TAG,
                             "arkiv_health: head=%llu (+%llu after %lld s) "
                             "pending_nonce=%llu",
                             (unsigned long long)head,
                             (unsigned long long)(head - s_health_last_head),
                             (long long)(since_us / 1000000),
                             (unsigned long long)nonce);
                }
                s_health_last_head           = head;
                s_health_last_head_change_us = now_us;
                s_health_baseline_nonce      = nonce;
                s_health_consec_stalled      = 0;
            } else {
                s_health_consec_stalled++;
                int64_t stall_s =
                    (now_us - s_health_last_head_change_us) / 1000000;
                uint64_t unincluded = (nonce >= s_health_baseline_nonce)
                    ? (nonce - s_health_baseline_nonce)
                    : 0;
                if (s_health_consec_stalled >= 2) {
                    ESP_LOGE(TAG,
                             "arkiv_health: CHAIN STALLED — head=%llu "
                             "unchanged for %lld s, ~%llu tx from this "
                             "device unincluded (pending_nonce=%llu); "
                             "Braga RPC still acks but no blocks are being "
                             "produced",
                             (unsigned long long)head,
                             (long long)stall_s,
                             (unsigned long long)unincluded,
                             (unsigned long long)nonce);
                } else {
                    ESP_LOGW(TAG,
                             "arkiv_health: head=%llu stagnant for %lld s "
                             "(1 cycle), ~%llu tx unincluded",
                             (unsigned long long)head,
                             (long long)stall_s,
                             (unsigned long long)unincluded);
                }
            }
        } else {
            ESP_LOGW(TAG, "eth_blockNumber failed — health check skipped");
        }
    }

    uint64_t gas_price;
    if (arkiv_eth_gas_price(&gas_price) != ESP_OK) {
        ESP_LOGW(TAG, "eth_gasPrice failed");
        goto out;
    }
    if (gas_price < GAS_PRICE_FLOOR_WEI) gas_price = GAS_PRICE_FLOOR_WEI;

    {
        arkiv::OpsTxData ops;
        ops.creates.push_back(std::move(create));
        std::vector<uint8_t> tx_data = arkiv::build_ops_tx_data(ops);

        arkiv::tx::LegacyTx tx;
        tx.nonce         = nonce;
        tx.gas_price_wei = gas_price;
        tx.gas_limit     = GAS_LIMIT_DEFAULT;
        tx.to            = storage_addr_bytes();
        tx.value_wei     = 0;
        tx.data          = std::move(tx_data);
        tx.chain_id      = ARKIV_CHAIN_ID;
        std::vector<uint8_t> raw = arkiv::tx::sign_legacy_tx(tx, s_dev_priv);
        if (raw.empty()) {
            ESP_LOGE(TAG, "tx signing failed");
            goto out;
        }
        if (out_tx_hash) arkiv::tx::tx_hash(raw, out_tx_hash);

        char node_hash[64 + 1] = {0};
        esp_err_t sr = arkiv_eth_send_raw_tx(raw.data(), raw.size(),
                                             node_hash, sizeof(node_hash));
        if (sr != ESP_OK) {
            ESP_LOGW(TAG, "eth_sendRawTransaction failed");
            goto out;
        }
        ESP_LOGI(TAG, "submitted nonce=%llu hash=%s",
                 (unsigned long long)nonce, node_hash);
        rc = ESP_OK;
    }

out:
    xSemaphoreGive(s_lock);
    return rc;
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        WriteJob *job = nullptr;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job) continue;
        if (!s_ready) {
            ESP_LOGW(TAG, "writer not ready — dropping queued %s",
                     job->content_type.c_str());
            delete job;
            continue;
        }
        arkiv::CreateOp create;
        create.contentType      = job->content_type;
        create.expiresInSeconds = job->expires_in_seconds;
        create.payload          = std::move(job->payload);
        create.attributes       = std::move(job->attrs);
        uint8_t hash[32];
        esp_err_t rc = submit_create(create, hash);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "queued submit failed (type=%s)",
                     job->content_type.c_str());
        }
        delete job;
    }
}

extern "C" bool arkiv_writer_enqueue_create_entity(const char *content_type,
                                                   const uint8_t *payload,
                                                   size_t payload_len,
                                                   uint32_t expires_in_seconds,
                                                   const arkiv_attr_t *attrs,
                                                   size_t attr_count)
{
    if (!s_queue || !content_type) return false;
    auto *job = new (std::nothrow) WriteJob();
    if (!job) return false;
    job->content_type        = content_type;
    job->expires_in_seconds  = expires_in_seconds;
    if (payload && payload_len) {
        job->payload.assign(payload, payload + payload_len);
    }
    for (size_t i = 0; i < attr_count; ++i) {
        const arkiv_attr_t &a = attrs[i];
        arkiv::Attribute attr;
        attr.key = a.key ? a.key : "";
        if (a.is_numeric) {
            attr.isNumeric    = true;
            attr.numericValue = a.value_num;
        } else {
            attr.isNumeric    = false;
            attr.stringValue  = a.value_str ? a.value_str : "";
        }
        job->attrs.push_back(std::move(attr));
    }
    /* xQueueSend returns immediately when full — that's the right policy on
     * a 4-KB-stack caller: dropping one ack/event is better than blocking. */
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "writer queue full — dropping %s", content_type);
        delete job;
        return false;
    }
    return true;
}

extern "C" esp_err_t arkiv_writer_create_entity(const char *content_type,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                uint32_t expires_in_seconds,
                                                const arkiv_attr_t *attrs,
                                                size_t attr_count,
                                                uint8_t out_tx_hash[32])
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!content_type) return ESP_ERR_INVALID_ARG;

    /* Mirror of the enqueue path's job marshalling, kept here so callers
     * that can afford to block (telemetry task, with its 6 KB stack) get
     * a synchronous result + tx hash without going through the queue. */
    arkiv::CreateOp create;
    create.contentType      = content_type;
    create.expiresInSeconds = expires_in_seconds;
    if (payload && payload_len) {
        create.payload.assign(payload, payload + payload_len);
    }
    for (size_t i = 0; i < attr_count; ++i) {
        const arkiv_attr_t &a = attrs[i];
        arkiv::Attribute attr;
        attr.key = a.key ? a.key : "";
        if (a.is_numeric) {
            attr.isNumeric    = true;
            attr.numericValue = a.value_num;
        } else {
            attr.isNumeric    = false;
            attr.stringValue  = a.value_str ? a.value_str : "";
        }
        create.attributes.push_back(std::move(attr));
    }
    return submit_create(create, out_tx_hash);
}
