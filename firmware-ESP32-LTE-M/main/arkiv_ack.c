#include "arkiv_ack.h"
#include "arkiv_writer.h"
#include "arkiv_cfg.h"
#include "identity.h"
#include "arkiv_crypto/aead.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "arkiv_ack"

/* Tiny tracker — at most a few in-flight Paranoic commands at once. SEQ is
 * an 8-bit routing nonce; collisions inside the TTL would be a separate
 * upstream bug (allocateSeq backs it with Redis INCR & 0xff). */
#define SLOTS 4

typedef struct {
    bool     used;
    uint8_t  seq;
    int64_t  expires_us;
    char     command_id[40];  /* 36-char UUID + NUL, room for safety */
} ack_slot_t;

static ack_slot_t s_slots[SLOTS];
static SemaphoreHandle_t s_lock;

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void arkiv_ack_track_pending(uint8_t seq, const char *command_id)
{
    if (!command_id) return;
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    /* Free expired slots in passing so we don't run out under load. */
    int free_slot = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].expires_us < now) s_slots[i].used = false;
        if (!s_slots[i].used && free_slot < 0) free_slot = i;
        /* If the same SEQ is already pending, refresh in place — a re-poll
         * of the same Arkiv entity (counter check would block, but during
         * the first verify pass we'd reach here twice if the RESP didn't
         * land yet between polls). */
        if (s_slots[i].used && s_slots[i].seq == seq) {
            strncpy(s_slots[i].command_id, command_id, sizeof(s_slots[i].command_id) - 1);
            s_slots[i].command_id[sizeof(s_slots[i].command_id) - 1] = '\0';
            s_slots[i].expires_us = now + (int64_t)ARKIV_ACK_TRACK_TTL_MS * 1000;
            xSemaphoreGive(s_lock);
            return;
        }
    }
    if (free_slot < 0) {
        /* Tracker full — evict the oldest. The 4-slot cap is generous; if
         * we ever hit this the upstream cadence is wrong. */
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < SLOTS; ++i) {
            if (s_slots[i].expires_us < oldest) { oldest = s_slots[i].expires_us; free_slot = i; }
        }
        ESP_LOGW(TAG, "tracker full, evicting slot %d (seq=%u)",
                 free_slot, (unsigned)s_slots[free_slot].seq);
    }
    s_slots[free_slot].used       = true;
    s_slots[free_slot].seq        = seq;
    s_slots[free_slot].expires_us = now + (int64_t)ARKIV_ACK_TRACK_TTL_MS * 1000;
    strncpy(s_slots[free_slot].command_id, command_id,
            sizeof(s_slots[free_slot].command_id) - 1);
    s_slots[free_slot].command_id[sizeof(s_slots[free_slot].command_id) - 1] = '\0';
    xSemaphoreGive(s_lock);
}

bool arkiv_ack_has_pending(uint8_t seq)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    bool found = false;
    for (int i = 0; i < SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].seq == seq && s_slots[i].expires_us >= now) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool arkiv_ack_emit(uint8_t seq, const uint8_t *resp_payload, size_t resp_len)
{
    if (!arkiv_writer_ready()) {
        ESP_LOGW(TAG, "writer not ready — cannot emit w3pups-ack (seq=%u)", seq);
        return false;
    }

    /* Pull + clear the slot under lock; we don't want to hold it during the
     * (slow) HTTPS submit. */
    char cmd_id[40];
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SLOTS; ++i) {
        if (s_slots[i].used && s_slots[i].seq == seq && s_slots[i].expires_us >= now) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return false;
    }
    strncpy(cmd_id, s_slots[idx].command_id, sizeof(cmd_id) - 1);
    cmd_id[sizeof(cmd_id) - 1] = '\0';
    s_slots[idx].used = false;
    xSemaphoreGive(s_lock);

    /* Convention: first byte of the cmd RESP payload is the result code
     * (matches the backend's MQTT dispatcher in handleCmdResponse — 0 = OK,
     * non-zero = failure). Carry it as a numeric attribute. */
    int64_t code = 0;
    if (resp_payload && resp_len > 0) code = (int64_t)resp_payload[0];

    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') iccid = "0";

    /* Attributes the panel ingest reads (apps/api/src/lib/arkiv/ingest.ts
     * ingestAcks): type, device_id, command_id (string) + seq, code (numeric).
     * `seq` is the writer's shared monotonic — backend cursor moves once
     * for ack/telemetry/event together. */
    uint64_t ack_seq = arkiv_writer_next_seq();

    /* Seal the RESP body (ADR-0013 posture B). command_id is bound into the
     * AAD so a gateway cannot transplant this ack onto another command. `code`
     * stays a plaintext attribute (status routing). Fail-closed: drop on error. */
    if (resp_len > 256) {
        ESP_LOGW(TAG, "w3pups-ack RESP too large (%u) — dropping", (unsigned)resp_len);
        return false;
    }
    uint8_t  sealed[256 + ARKIV_AEAD_OVERHEAD];
    size_t   sealed_len = 0;
    uint32_t epoch = 0;
    int sr = arkiv_writer_payload_seal(ARKIV_AEAD_TYPE_ACK, ack_seq,
                                       "w3pups-ack", cmd_id,
                                       resp_payload, resp_len,
                                       sealed, sizeof(sealed), &sealed_len, &epoch);
    if (sr != 0) {
        ESP_LOGW(TAG, "w3pups-ack seal failed (rc=%d cmd=%s) — dropping (fail-closed)",
                 sr, cmd_id);
        return false;
    }

    arkiv_attr_t attrs[7] = {
        { .key = "type",       .value_str = "w3pups-ack",     .is_numeric = false },
        { .key = "device_id",  .value_str = iccid,            .is_numeric = false },
        { .key = "command_id", .value_str = cmd_id,           .is_numeric = false },
        { .key = "seq",        .value_num = (int64_t)ack_seq, .is_numeric = true },
        { .key = "code",       .value_num = code,             .is_numeric = true },
        { .key = "epoch",      .value_num = (int64_t)epoch,   .is_numeric = true },
        { .key = "scheme",     .value_num = 3,                .is_numeric = true },
    };

    /* Sealed RESP body. Use the ENQUEUE variant: this runs on wups_rx (4 KB
     * stack); the synchronous submit would stack-overflow during the TLS
     * handshake + RLP signing (caught during P4 bring-up). */
    bool ok = arkiv_writer_enqueue_create_entity(
        "application/octet-stream",
        sealed, sealed_len,
        15 * 60,  /* 15 min TTL — backend sweep is 30 s, ample headroom */
        attrs, sizeof(attrs) / sizeof(attrs[0]));
    if (!ok) {
        ESP_LOGW(TAG, "w3pups-ack enqueue failed (cmd=%s code=%lld)", cmd_id, (long long)code);
        return false;
    }
    ESP_LOGI(TAG, "w3pups-ack enqueued (cmd=%s code=%lld seq=%llu)",
             cmd_id, (long long)code, (unsigned long long)ack_seq);
    return true;
}
