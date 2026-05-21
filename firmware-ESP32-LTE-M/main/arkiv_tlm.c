#include "arkiv_tlm.h"
#include "arkiv_writer.h"
#include "cmdauth_arkiv.h"
#include "identity.h"
#include "wups_proto.h"

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
        arkiv_attr_t attrs[3] = {
            { .key = "type",      .value_str = "w3pups-telemetry",     .is_numeric = false },
            { .key = "device_id", .value_str = iccid,                  .is_numeric = false },
            { .key = "seq",       .value_num = (int64_t)seq, .is_numeric = true  },
        };
        uint8_t txh[32];
        esp_err_t rc = arkiv_writer_create_entity(
            "application/octet-stream", payload, n,
            5 * 60,  /* 5 min TTL — backend sweeps every 30 s */
            attrs, sizeof(attrs) / sizeof(attrs[0]),
            txh);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "telemetry submit failed (seq=%llu, %u B)",
                     (unsigned long long)seq, (unsigned)n);
        } else {
            ESP_LOGI(TAG, "w3pups-telemetry submitted (seq=%llu, %u B)",
                     (unsigned long long)seq, (unsigned)n);
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
