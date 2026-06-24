#include "arkiv_event.h"
#include "arkiv_writer.h"
#include "cmdauth_arkiv.h"
#include "identity.h"
#include "wups_proto.h"
#include "arkiv_crypto/aead.h"

#include <string.h>
#include "esp_log.h"

#define TAG "arkiv_event"

/* Translate a power.event code (common/protocol.h WUPS_PWR_EVT_*) into a
 * stable, plaintext class string the panel surfaces verbatim. Plaintext
 * by design (§12.3 — class is the dead-man key, not the body). */
static const char *power_event_class(uint8_t evt)
{
    switch (evt) {
        case 1: return "power_loss";       /* WUPS_PWR_EVT_MAINS_LOST */
        case 2: return "mains_restored";   /* WUPS_PWR_EVT_MAINS_RESTORED */
        case 3: return "battery_low";      /* WUPS_PWR_EVT_CHARGE_LOW */
        case 4: return "charge_full";      /* WUPS_PWR_EVT_CHARGE_FULL */
        case 5: return "fault";            /* WUPS_PWR_EVT_FAULT */
        default: return NULL;
    }
}

static const char *host_event_class(uint8_t evt)
{
    /* host.event codes aren't formalized yet in common/protocol.h, so for
     * now we surface the raw numeric. Once host events get their own enum,
     * extend this with proper names — keep the strings panel-stable. */
    (void)evt;
    return NULL;
}

void arkiv_event_observe_frame(const uint8_t *frame, uint16_t frame_len)
{
    if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) return;
    if (!arkiv_writer_ready()) return;
    if (!frame || frame_len < WUPS_HEADER_BYTES + 2) return;

    uint8_t  cls = frame[4];
    uint8_t  op  = frame[5];
    uint16_t inner_len = (uint16_t)frame[8] | ((uint16_t)frame[9] << 8);
    if (WUPS_HEADER_BYTES + inner_len + 2 > frame_len || inner_len < 2) return;
    const uint8_t *inner = frame + WUPS_HEADER_BYTES;

    /* power.event v1 layout: { u8 version, u8 event } */
    const char *cls_str = NULL;
    if (cls == WUPS_CLASS_POWER && op == WUPS_OP_PWR_EVENT) {
        if (inner[0] != 1) return; /* unsupported version */
        cls_str = power_event_class(inner[1]);
    } else if (cls == WUPS_CLASS_HOST && op == WUPS_OP_HOST_EVENT) {
        if (inner[0] != 1) return;
        cls_str = host_event_class(inner[1]);
    }
    if (!cls_str) return;  /* not an event we recognise */

    const char *iccid = identity_iccid();
    if (!iccid || iccid[0] == '\0') iccid = "0";

    uint64_t seq = arkiv_writer_next_seq();

    /* Seal the inner event bytes (ADR-0013 posture B). `class` stays a
     * plaintext attribute — it is the dead-man / alert key the backend reads
     * WITHOUT decrypting (§12.3). Fail-closed: drop on any seal error. */
    if (inner_len > 256) {
        ESP_LOGW(TAG, "w3pups-event inner too large (%u) — dropping", (unsigned)inner_len);
        return;
    }
    uint8_t  sealed[256 + ARKIV_AEAD_OVERHEAD];
    size_t   sealed_len = 0;
    uint32_t epoch = 0;
    int sr = arkiv_writer_payload_seal(ARKIV_AEAD_TYPE_EVENT, seq,
                                       "w3pups-event", NULL,
                                       inner, inner_len, sealed, sizeof(sealed),
                                       &sealed_len, &epoch);
    if (sr != 0) {
        ESP_LOGW(TAG, "w3pups-event seal failed (rc=%d class=%s) — dropping (fail-closed)",
                 sr, cls_str);
        return;
    }

    arkiv_attr_t attrs[6] = {
        { .key = "type",      .value_str = "w3pups-event", .is_numeric = false },
        { .key = "device_id", .value_str = iccid,          .is_numeric = false },
        { .key = "class",     .value_str = cls_str,        .is_numeric = false },
        { .key = "seq",       .value_num = (int64_t)seq,   .is_numeric = true  },
        { .key = "epoch",     .value_num = (int64_t)epoch, .is_numeric = true  },
        { .key = "scheme",    .value_num = 3,              .is_numeric = true  },
    };

    /* Sealed inner bytes. Use the enqueue path: this is called from wups_rx
     * (4 KB stack), see the P4 stack-overflow incident in arkiv_ack.c. */
    bool ok = arkiv_writer_enqueue_create_entity(
        "application/octet-stream", sealed, sealed_len,
        60 * 60,  /* 1 h TTL — events are sticky; ample headroom */
        attrs, sizeof(attrs) / sizeof(attrs[0]));
    if (!ok) {
        ESP_LOGW(TAG, "w3pups-event enqueue failed (class=%s)", cls_str);
    } else {
        ESP_LOGI(TAG, "w3pups-event enqueued (class=%s seq=%llu)",
                 cls_str, (unsigned long long)seq);
    }
}
