/*
 * power_status_decode.h — version-dispatching decoder for the WUPS
 * power.status payload (header-only, ESP32 side; also compiles on the host).
 *
 * Purpose
 *   The CH32X emits power.status either as wups_power_status_v1_t (20 B,
 *   version byte = 1) or wups_power_status_v2_t (40 B, version byte = 2).
 *   v2 is NOT a prefix-compatible superset of v1: the layout differs from
 *   offset 1 onwards, so feeding a v2 payload through the v1 struct silently
 *   yields garbage (GitHub issue #7: the HTTP backend reported vout_set_mV as
 *   "temp_dc", pd_out_mV as "faults", etc.). Every consumer MUST dispatch on
 *   the version byte at offset 0 and pick the matching struct.
 *
 * Contract of wups_power_status_decode()
 *   - raw[0] == 2 && len >= 40  -> v2 decoded into out->v2, returns 2
 *   - raw[0] == 1 && len >= 20  -> v1 decoded into out->v1, returns 1
 *   - anything else             -> returns 0, *out is fully zeroed
 *   - bytes beyond the struct size are ignored (forward-compatible trailer)
 *   - a SHORT v2 (len < 40) is never decoded as v1, even though len >= 20
 *
 * Aggregate temperature (out->temp_agg_dC)
 *   temp = (temp_mp == -32768) ? temp_lm : max(temp_mp, temp_lm)
 *   -32768 is the v2 "MP2762A unpowered" sentinel (temp_mp_valid == false).
 *   Same rule as the historical v1 emitter on the CH32X, the RP2040 OLED,
 *   the Rust host (Web3-Pi-UPS-Service PowerStatusV2::to_v1) and the
 *   Workbench, so every surface shows the same single "board temperature".
 *   For v1 payloads temp_agg_dC == v1.temp_dC.
 *
 * Host regression test (run from the Web3-Pi-UPS repo root):
 *   cc -std=c11 -Wall -Wextra -Werror -I firmware-ESP32-LTE-M/main \
 *      -o "${TMPDIR:-/tmp}/test_power_status_decode" \
 *      tools/test_power_status_decode.c && "${TMPDIR:-/tmp}/test_power_status_decode"
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "wups_proto.h"

/* v2 temp_mp_dC sentinel: MP2762A unpowered, junction temperature unavailable. */
#define WUPS_PWR2_TEMP_NA ((int16_t)-32768)

/* Lock the wire layout this decoder relies on (common/protocol.h). */
_Static_assert(sizeof(wups_power_status_v1_t) == 20, "power.status v1 must be 20 B");
_Static_assert(sizeof(wups_power_status_v2_t) == 40, "power.status v2 must be 40 B");
_Static_assert(offsetof(wups_power_status_v1_t, temp_dC) == 12, "v1 temp_dC @12");
_Static_assert(offsetof(wups_power_status_v1_t, faults)  == 18, "v1 faults @18");
_Static_assert(offsetof(wups_power_status_v2_t, vbat_mV)    == 22, "v2 vbat_mV @22");
_Static_assert(offsetof(wups_power_status_v2_t, temp_lm_dC) == 30, "v2 temp_lm_dC @30");
_Static_assert(offsetof(wups_power_status_v2_t, temp_mp_dC) == 32, "v2 temp_mp_dC @32");
_Static_assert(offsetof(wups_power_status_v2_t, faults)     == 34, "v2 faults @34");
_Static_assert(offsetof(wups_power_status_v2_t, uptime_s)   == 36, "v2 uptime_s @36");

/* Decoded view. Exactly one of v1 / v2 is populated, selected by `version`
 * (1 or 2; 0 = nothing decoded). temp_agg_dC and temp_mp_valid are derived
 * for both versions so callers need no per-version temperature logic. */
typedef struct {
    uint8_t                version;       /* 0 = invalid, 1 or 2 */
    wups_power_status_v1_t v1;            /* valid when version == 1 */
    wups_power_status_v2_t v2;            /* valid when version == 2 */
    bool                   temp_mp_valid; /* v2 only: temp_mp_dC != WUPS_PWR2_TEMP_NA */
    int16_t                temp_agg_dC;   /* single "board temperature", see rule above */
} wups_power_status_view_t;

/* Aggregate rule shared with the other consumers (see file header). */
static inline int16_t wups_power_temp_agg_dC(int16_t lm, int16_t mp)
{
    if (mp == WUPS_PWR2_TEMP_NA) return lm;
    return (mp > lm) ? mp : lm;
}

/* Dispatch on raw[0] and copy the matching struct. Returns the decoded
 * version (1 or 2) or 0 when nothing usable was decoded (out zeroed). */
static inline uint8_t wups_power_status_decode(const uint8_t *raw, size_t len,
                                               wups_power_status_view_t *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!raw || len == 0) return 0;

    if (raw[0] == 2 && len >= sizeof(wups_power_status_v2_t)) {
        memcpy(&out->v2, raw, sizeof(out->v2));
        out->temp_mp_valid = (out->v2.temp_mp_dC != WUPS_PWR2_TEMP_NA);
        out->temp_agg_dC   = wups_power_temp_agg_dC(out->v2.temp_lm_dC,
                                                    out->v2.temp_mp_dC);
        out->version = 2;
        return 2;
    }
    if (raw[0] == 1 && len >= sizeof(wups_power_status_v1_t)) {
        memcpy(&out->v1, raw, sizeof(out->v1));
        out->temp_mp_valid = false;
        out->temp_agg_dC   = out->v1.temp_dC;
        out->version = 1;
        return 1;
    }
    return 0;
}
