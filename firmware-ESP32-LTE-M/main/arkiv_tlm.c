#include "arkiv_tlm.h"
#include "arkiv_writer.h"
#include "arkiv_rpc.h"
#include "cmdauth_arkiv.h"
#include "identity.h"
#include "modem.h"
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

/* Device-wallet balance for the OLED "Balance" screen (issue #8). Refreshed
 * from THIS task only (6 KB stack, RPC-safe) — a blocking eth_getBalance on
 * the 4 KB wups_rx button task overflowed its stack and rebooted the device.
 * Independent of telemetry: the old refresh lived in the submit-OK branch, so
 * an UNCLAIMED or unfunded unit (every submit fails, 4 RPCs per attempt) never
 * read its balance — exactly when the owner needs the screen. Due rules
 * (bal_due): device key present AND PPP up, then (a) the on-demand flag from
 * the OLED (one read per ARKIV_TLM_BAL_DEBOUNCE_S — deferred, never dropped),
 * (b) every 30 s tick until the first successful read, (c) every
 * ARKIV_TLM_BALANCE_PERIOD ticks counted from the last SUCCESS, so a failed
 * periodic read is retried on the next tick instead of leaving a stale value
 * up for a whole period. A dead gateway therefore costs one RPC per 30 s —
 * the same as phase (b), and a quarter of what a claimed unit's telemetry
 * already spends on such an outage.
 *
 * Uplink-watchdog interplay: rpc_post stamps s_last_rpc_ok_s on every 2xx
 * round-trip, so phase (b) alone keeps the Arkiv uplink "fresh" while
 * telemetry itself fails; the steady (c) cadence (240 s) cannot on its own
 * (ARKIV_UPLINK_FRESH_SECS = 120 s, modem.c). Acceptable — a completed HTTPS
 * round-trip IS a healthy link, and the watchdog never trips while UNCLAIMED. */
#define ARKIV_TLM_BALANCE_PERIOD 8u   /* 8 × 30 s = 4 min after the last success */
#define ARKIV_TLM_BAL_DEBOUNCE_S 3u   /* on-demand reads: at most one per 3 s */
static struct {                        /* guarded by s_lock (shared with the slots) */
    arkiv_tlm_bal_state_t state;       /* outcome of the last attempt */
    arkiv_tlm_bal_state_t last_good;   /* NONE / OK (wei valid) / HIGH — survives FAIL */
    uint64_t wei;
    uint32_t updated_s;                /* esp_timer seconds of the last good read */
} s_bal;
static volatile bool s_bal_req;        /* on-demand refresh; set from any task */
static uint32_t s_tick, s_bal_tick;    /* tlm task only: loop ticks / tick of last success */
static uint32_t s_bal_try_s;           /* tlm task only: seconds of the last attempt */
static bool     s_bal_seen;            /* tlm task only: a good read has landed */

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* `tick` = true at the 30 s iteration boundary (rules b + c), false from the
 * 1 s wait slices (rule a only). A pending request survives a closed gate. */
static bool bal_due(bool tick)
{
    if (!cmdauth_arkiv_ready() || !modem_ppp_is_up()) return false;
    if (s_bal_req && now_s() - s_bal_try_s >= ARKIV_TLM_BAL_DEBOUNCE_S) return true;
    if (!tick) return false;
    if (!s_bal_seen) return true;
    return (s_tick - s_bal_tick) >= ARKIV_TLM_BALANCE_PERIOD;
}

/* tlm task ONLY (HTTPS + TLS scratch on this stack); s_lock is never held
 * across the RPC. */
static void bal_refresh(void)
{
    s_bal_req   = false;   /* cleared BEFORE the RPC: a request landing mid-read re-arms */
    s_bal_try_s = now_s();
    uint64_t wei = 0;
    esp_err_t rc = arkiv_eth_get_balance(cmdauth_arkiv_device_addr(), &wei);
    const bool good = (rc == ESP_OK || rc == ESP_ERR_INVALID_SIZE);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (rc == ESP_OK) {
        s_bal.state = s_bal.last_good = ARKIV_TLM_BAL_OK;
        s_bal.wei   = wei;
    } else if (rc == ESP_ERR_INVALID_SIZE) {
        s_bal.state = s_bal.last_good = ARKIV_TLM_BAL_HIGH;   /* >= 2^64 wei: value unknown */
    } else {
        s_bal.state = ARKIV_TLM_BAL_FAIL;   /* last_good + wei stay: the OLED shows them as stale */
    }
    if (good) s_bal.updated_s = now_s();
    xSemaphoreGive(s_lock);
    if (good) {
        s_bal_seen = true;
        s_bal_tick = s_tick;   /* rule (c) counts from here */
    }
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "wallet balance %llu wei", (unsigned long long)wei);
    } else if (rc == ESP_ERR_INVALID_SIZE) {
        ESP_LOGI(TAG, "wallet balance >= 2^64 wei (HIGH)");
    } else {
        ESP_LOGW(TAG, "wallet balance read failed: %s", esp_err_to_name(rc));
    }
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
    if (!s || !s_lock) return;   /* no lock = not started: nothing consumes the slots */

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
        /* 30 s telemetry cadence, waited in 1 s slices so an OLED balance
         * request (rule a) is served within ~1 s. Cadence itself unchanged. */
        for (int i = 0; i < ARKIV_TLM_PERIOD_MS / 1000; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (bal_due(false)) bal_refresh();
        }
        s_tick++;
        /* Balance BEFORE the telemetry gates below: UNCLAIMED / unfunded units
         * never pass them, yet that is when the owner checks the screen. */
        if (bal_due(true)) bal_refresh();

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

        arkiv_attr_t attrs[7] = {
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
            /* fw version rides the same once-per-boot announcement: the
             * panel otherwise keeps showing whatever MQTT last reported —
             * Workbench/USB flashes never pass through the backend
             * (field report 2026-08-16: panel said 0.8.0 on a 0.8.2 unit). */
            attrs[6] = (arkiv_attr_t){ .key = "fw",
                                       .value_str = identity_fw_version(),
                                       .is_numeric = false };
            nattrs = 7;
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
        }
    }
}

void arkiv_tlm_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    /* Created once HERE (app_main, before the emit task exists); every other
     * entry point returns early while s_lock is NULL, so no lazy-init race. */
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) { ESP_LOGE(TAG, "mutex alloc failed — telemetry disabled"); return; }
    /* 6 KB stack: TLS + tx signing scratch space. */
    xTaskCreate(tlm_task, "arkiv_tlm", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Arkiv telemetry task started (period=%dms)",
             ARKIV_TLM_PERIOD_MS);
}

bool arkiv_tlm_balance(arkiv_tlm_balance_t *out)
{
    if (!out) return false;
    *out = (arkiv_tlm_balance_t){ 0 };
    if (!s_lock) return false;   /* not started (not Arkiv mode): NONE */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state     = s_bal.state;
    out->last_good = s_bal.last_good;
    out->wei       = s_bal.wei;
    out->age_s     = s_bal.updated_s ? now_s() - s_bal.updated_s : 0;
    xSemaphoreGive(s_lock);
    return out->last_good != ARKIV_TLM_BAL_NONE;
}

void arkiv_tlm_request_balance_refresh(void) { s_bal_req = true; }
