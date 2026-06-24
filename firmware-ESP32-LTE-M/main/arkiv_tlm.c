#include "arkiv_tlm.h"
#include "arkiv_writer.h"
#include "arkiv_rpc.h"
#include "cmdauth_arkiv.h"
#include "identity.h"
#include "wups_proto.h"
#include "arkiv_crypto/aead.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "arkiv_tlm"

/* Cache for the latest inner status snapshot per class. A "fresh" entry
 * was observed within ARKIV_TLM_FRESHNESS_MS — older data is silently
 * dropped from the next emit to avoid replaying a stale reading the
 * panel would interpret as live. */
#define ARKIV_TLM_FRESHNESS_MS (90 * 1000)

/* Re-announce the device pubkey (plaintext dev_pub attr) on the first telemetry
 * after boot and every Nth thereafter, so a backend that (re)starts late still
 * learns it within one window without a device reboot. 60 × 30 s ≈ 30 min. */
#define ARKIV_TLM_DEVPUB_PERIOD 60u

typedef struct {
    uint8_t  cls;
    uint8_t  op;
    bool     valid;
    int64_t  observed_us;
    uint16_t len;
    uint8_t  data[ARKIV_TLM_MAX_INNER];
} slot_t;

/* Three slots: POWER.STATUS, HOST.STATUS, NET.STATUS. */
static slot_t s_slots[3];
static SemaphoreHandle_t s_lock;

/* Device-wallet balance cache (wei), refreshed from the arkiv_tlm task (6 KB
 * stack, RPC-safe) every ARKIV_TLM_BALANCE_PERIOD successful telemetry submits.
 * The OLED "Balance" screen reads this WITHOUT any RPC: a blocking eth_getBalance
 * on the 4 KB wups_rx button-event task overflowed its stack and rebooted the
 * device. */
#define ARKIV_TLM_BALANCE_PERIOD 8u   /* ≈ every 8 × 30 s = 4 min */
static uint64_t s_bal_wei;
static bool     s_bal_valid;

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static slot_t *slot_for(uint8_t cls, uint8_t op)
{
    if (cls == WUPS_CLASS_POWER && op == WUPS_OP_PWR_STATUS) return &s_slots[0];
    if (cls == WUPS_CLASS_HOST  && op == WUPS_OP_HOST_STATUS) return &s_slots[1];
    if (cls == WUPS_CLASS_NET   && op == WUPS_OP_NET_STATUS) return &s_slots[2];
    return NULL;
}

void arkiv_tlm_observe_frame(const uint8_t *frame, uint16_t frame_len)
{
    /* WUPS frame layout (common/protocol.h):
     *   0-1 SYNC, 2 DST, 3 SRC, 4 CLS, 5 OP, 6 FLAGS, 7 SEQ,
     *   8-9 LEN(LE), then payload, then CK_A/CK_B. */
    if (!frame || frame_len < WUPS_HEADER_BYTES + 2 /* +CK */) return;
    uint8_t cls = frame[4];
    uint8_t op  = frame[5];
    uint16_t inner_len = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
    if (WUPS_HEADER_BYTES + inner_len + 2 > frame_len) return;  /* short */
    if (inner_len == 0 || inner_len > ARKIV_TLM_MAX_INNER) return;

    slot_t *s = slot_for(cls, op);
    if (!s) return;

    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s->data, frame + WUPS_HEADER_BYTES, inner_len);
    s->cls         = cls;
    s->op          = op;
    s->len         = inner_len;
    s->valid       = true;
    s->observed_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

/* Build the TLV blob from the currently-fresh slots. Returns total bytes
 * written into out[0..out_cap); 0 means nothing fresh (skip the tx). */
static size_t build_payload(uint8_t *out, size_t out_cap)
{
    if (out_cap < 2) return 0;
    out[0] = 1; /* version */
    out[1] = 0; /* flags */
    size_t pos = 2;

    int64_t now = esp_timer_get_time();
    int64_t cutoff = now - (int64_t)ARKIV_TLM_FRESHNESS_MS * 1000;
    bool any = false;

    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); ++i) {
        slot_t *s = &s_slots[i];
        if (!s->valid || s->observed_us < cutoff) continue;
        if (pos + 4 + s->len > out_cap) break;
        out[pos++] = s->cls;
        out[pos++] = s->op;
        out[pos++] = (uint8_t)(s->len & 0xFF);
        out[pos++] = (uint8_t)((s->len >> 8) & 0xFF);
        memcpy(out + pos, s->data, s->len);
        pos += s->len;
        any = true;
    }
    xSemaphoreGive(s_lock);
    return any ? pos : 0;
}

static void tlm_task(void *arg)
{
    (void)arg;
    /* Initial settling delay so the first emit happens after at least one
     * full power.status cycle from CH32X (CH32X emits every 1 s). */
    vTaskDelay(pdMS_TO_TICKS(10 * 1000));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ARKIV_TLM_PERIOD_MS));

        if (!arkiv_writer_ready()) continue;
        if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) continue;
        const char *iccid = identity_iccid();
        if (!iccid || iccid[0] == '\0') continue;

        /* TLV blob — header (2) + up to 3 items, each ~ (4 + ARKIV_TLM_MAX_INNER) */
        uint8_t payload[2 + 3 * (4 + ARKIV_TLM_MAX_INNER)];
        size_t n = build_payload(payload, sizeof(payload));
        if (n == 0) {
            /* Nothing fresh to send. Don't burn gas on a no-op tx. */
            continue;
        }

        uint64_t seq = arkiv_writer_next_seq();

        /* Seal the TLV body (ADR-0013 posture B): owner-only AES-256-GCM.
         * Fail-closed — on ANY seal error (incl. UNCLAIMED or seq==0) drop the
         * entity; never publish plaintext on a claimed device. */
        uint8_t  sealed[sizeof(payload) + ARKIV_AEAD_OVERHEAD];
        size_t   sealed_len = 0;
        uint32_t epoch = 0;
        int sr = arkiv_writer_payload_seal(ARKIV_AEAD_TYPE_TELEMETRY, seq,
                                           "w3pups-telemetry", NULL,
                                           payload, n, sealed, sizeof(sealed),
                                           &sealed_len, &epoch);
        if (sr != 0) {
            ESP_LOGW(TAG, "telemetry seal failed (rc=%d seq=%llu) — dropping (fail-closed)",
                     sr, (unsigned long long)seq);
            continue;
        }

        /* Announce the device pubkey (plaintext, ADR-0013) on the FIRST
         * telemetry after boot so the owner's browser can ECDH against it — the
         * Arkiv entity exposes no tx hash to ecrecover from, and dev_pub is
         * public. Once-per-boot keeps it off the §4.6 data budget; the backend
         * stamps + persists arkivDevicePubkey on first sight. */
        static uint32_t s_tlm_ok = 0;   /* successful telemetry submits since boot */
        char dev_pub_hex[129];
        const bool announce = (s_tlm_ok % ARKIV_TLM_DEVPUB_PERIOD) == 0u;
        if (announce) {
            static const char H[] = "0123456789abcdef";
            const uint8_t *dp = cmdauth_arkiv_device_pub();
            for (int i = 0; i < 64; i++) {
                dev_pub_hex[2 * i]     = H[dp[i] >> 4];
                dev_pub_hex[2 * i + 1] = H[dp[i] & 0x0F];
            }
            dev_pub_hex[128] = '\0';
        }

        arkiv_attr_t attrs[6] = {
            { .key = "type",      .value_str = "w3pups-telemetry", .is_numeric = false },
            { .key = "device_id", .value_str = iccid,              .is_numeric = false },
            { .key = "seq",       .value_num = (int64_t)seq,       .is_numeric = true  },
            { .key = "epoch",     .value_num = (int64_t)epoch,     .is_numeric = true  },
            { .key = "scheme",    .value_num = 3,                  .is_numeric = true  },
        };
        size_t nattrs = 5;
        if (announce) {
            attrs[5] = (arkiv_attr_t){ .key = "dev_pub", .value_str = dev_pub_hex,
                                       .is_numeric = false };
            nattrs = 6;
        }
        uint8_t txh[32];
        esp_err_t rc = arkiv_writer_create_entity(
            "application/octet-stream", sealed, sealed_len,
            5 * 60,  /* 5 min TTL — backend sweeps every 30 s */
            attrs, nattrs, txh);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "telemetry submit failed (seq=%llu, %u B)",
                     (unsigned long long)seq, (unsigned)sealed_len);
        } else {
            s_tlm_ok++;   /* drives the periodic dev_pub re-announce */
            ESP_LOGI(TAG, "w3pups-telemetry submitted enc (seq=%llu epoch=%u, %u B%s)",
                     (unsigned long long)seq, (unsigned)epoch, (unsigned)sealed_len,
                     announce ? ", +dev_pub" : "");
            /* Refresh the cached device-wallet balance for the OLED "Balance"
             * screen — done HERE (6 KB stack, RPC-safe), never on the 4 KB
             * wups_rx button task. First refresh on the first submit, then
             * every ARKIV_TLM_BALANCE_PERIOD submits. */
            if ((s_tlm_ok % ARKIV_TLM_BALANCE_PERIOD) == 1u) {
                const uint8_t *da = cmdauth_arkiv_device_addr();
                uint64_t bal = 0;
                if (da && arkiv_eth_get_balance(da, &bal) == ESP_OK) {
                    s_bal_wei   = bal;
                    s_bal_valid = true;
                }
            }
        }
    }
}

void arkiv_tlm_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* 6 KB stack: TLS + tx signing scratch space. */
    xTaskCreate(tlm_task, "arkiv_tlm", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Arkiv telemetry task started (period=%dms)",
             ARKIV_TLM_PERIOD_MS);
}

bool arkiv_tlm_cached_balance_wei(uint64_t *out_wei)
{
    if (!s_bal_valid) return false;
    if (out_wei) *out_wei = s_bal_wei;
    return true;
}
