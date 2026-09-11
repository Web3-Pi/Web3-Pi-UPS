/*
 * test_power_status_decode.c — host regression test for
 * firmware-ESP32-LTE-M/main/power_status_decode.h (GitHub issue #7: HTTP
 * telemetry decoded power.status v2 bytes through the v1 struct).
 *
 * Run from the Web3-Pi-UPS repo root (binary lands outside the tree):
 *   cc -std=c11 -Wall -Wextra -Werror -I firmware-ESP32-LTE-M/main \
 *      -o "${TMPDIR:-/tmp}/test_power_status_decode" \
 *      tools/test_power_status_decode.c && "${TMPDIR:-/tmp}/test_power_status_decode"
 *
 * Vectors are shared with the other decoders of the same bytes so the
 * implementations stay byte-locked:
 *   T1  Rust host  Web3-Pi-UPS-Service payloads.rs  power_status v1 known bytes
 *   T2  Rust host  payloads.rs  power_status_v2_known_bytes
 *   T3  tools/test_wups_deframer.py  power_status() (struct.pack vector)
 *   T4  Panel      telemetryDecode.test.ts  powerV2() builder shape
 *   T5  reviewer's on-device capture behind issue #7 (+ the old v1 misdecode)
 *   T6  every field distinct (catches any offset swap)
 *   T7  aggregate temperature rule
 *   T8  length / version dispatch edge cases
 *   T9  signedness and pass-through semantics
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "power_status_decode.h"

static int fails = 0;
static int checks = 0;

#define CHECK(cond, msg) do { checks++; \
    if (!(cond)) { printf("FAIL %s\n", msg); fails++; } } while (0)

#define CHECK_EQ(got, want, msg) do { checks++; \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { printf("FAIL %s: got %lld want %lld\n", msg, _g, _w); fails++; } \
    } while (0)

/* --- little helpers ----------------------------------------------------- */

static void le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, (uint16_t)v); le16(p + 2, (uint16_t)(v >> 16)); }

/* struct.pack("<BBBBHHHHHHHHHHhHHhhHI", ...) — same field order as the deframer test. */
static void v2_pack(uint8_t out[40], uint8_t flags, uint8_t cs, uint8_t reserved,
                    uint16_t vbus_in, uint16_t pd_in_mv, uint16_t pd_in_ma,
                    uint16_t vbus_out, uint16_t vout_set, uint16_t vout_read,
                    uint16_t iout_limit, uint16_t pd_out_mv, uint16_t pd_out_ma,
                    uint16_t vbat, int16_t ichg, uint16_t vsys, uint16_t iin,
                    int16_t temp_lm, int16_t temp_mp, uint16_t faults, uint32_t uptime)
{
    out[0] = 2; out[1] = flags; out[2] = cs; out[3] = reserved;
    le16(out + 4, vbus_in);   le16(out + 6, pd_in_mv);   le16(out + 8, pd_in_ma);
    le16(out + 10, vbus_out); le16(out + 12, vout_set);  le16(out + 14, vout_read);
    le16(out + 16, iout_limit); le16(out + 18, pd_out_mv); le16(out + 20, pd_out_ma);
    le16(out + 22, vbat);     le16(out + 24, (uint16_t)ichg);
    le16(out + 26, vsys);     le16(out + 28, iin);
    le16(out + 30, (uint16_t)temp_lm); le16(out + 32, (uint16_t)temp_mp);
    le16(out + 34, faults);   le32(out + 36, uptime);
}

/* Decode expecting failure: return 0 and *out fully zeroed (pre-filled with 0xA5). */
static void expect_reject(const uint8_t *raw, size_t len, const char *msg)
{
    wups_power_status_view_t out, zero;
    memset(&out, 0xA5, sizeof(out));
    memset(&zero, 0, sizeof(zero));
    CHECK_EQ(wups_power_status_decode(raw, len, &out), 0, msg);
    CHECK(memcmp(&out, &zero, sizeof(out)) == 0, msg);
}

/* --- shared vectors ----------------------------------------------------- */

/* T1: Rust power_status v1 known bytes. */
static const uint8_t T1_V1[20] = {
    0x01, 0x01, 0x34, 0x12, 0x78, 0x56, 0xFF, 0xFF, 0xCD, 0x0B, 0x00, 0x01, 0xCE, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* T2: Rust power_status_v2_known_bytes. */
static const uint8_t T2_V2[40] = {
    0x02, 0x1F, 0x02, 0x00,
    0x20, 0x4E, 0x98, 0x3A, 0xD6, 0x06,
    0xEC, 0x13, 0xEC, 0x13, 0xBA, 0x13, 0x88, 0x13, 0x88, 0x13, 0x88, 0x13,
    0xD0, 0x20, 0x06, 0xFF,
    0xFC, 0x21, 0xF4, 0x01, 0xFD, 0x00, 0x00, 0x80, 0x00, 0x07,
    0x40, 0xE2, 0x01, 0x00,
};

/* T5: the reviewer's capture (issue #7). */
static const uint8_t T5_CAPTURE[40] = {
    0x02, 0x1F, 0x03, 0x00, 0x75, 0x39, 0x98, 0x3A, 0xD6, 0x06, 0xBB, 0x13, 0x88, 0x13,
    0xB6, 0x13, 0x92, 0x13, 0x88, 0x13, 0xB8, 0x0B, 0xD9, 0x1E, 0x00, 0x00, 0xDC, 0x1E,
    0xB0, 0x04, 0x5E, 0x01, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x51, 0x01, 0x00,
};

/* T6: all fields distinct. */
static const uint8_t T6_DISTINCT[40] = {
    0x02, 0x15, 0x03, 0xEE, 0x75, 0x39, 0x98, 0x3A, 0xD6, 0x06, 0xBB, 0x13, 0x88, 0x13,
    0xB6, 0x13, 0x92, 0x13, 0x9C, 0x13, 0xB8, 0x0B, 0xD9, 0x1E, 0x06, 0xFF, 0xDC, 0x1E,
    0xB0, 0x04, 0x5E, 0x01, 0xB8, 0x01, 0x12, 0x05, 0x04, 0x03, 0x02, 0x01,
};

/* --- tests -------------------------------------------------------------- */

static void test_t1_v1_rust(void)
{
    int before = fails;
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(T1_V1, sizeof(T1_V1), &ps), 1, "T1 ver");
    CHECK_EQ(ps.version, 1, "T1 view.version");
    CHECK_EQ(ps.v1.charge_state, 1, "T1 charge_state");
    CHECK_EQ(ps.v1.vbus_in_mV, 4660, "T1 vbus_in");
    CHECK_EQ(ps.v1.vbus_out_mV, 22136, "T1 vbus_out");
    CHECK_EQ(ps.v1.ibus_out_mA, -1, "T1 ibus_out");
    CHECK_EQ(ps.v1.vbat_mV, 3021, "T1 vbat");
    CHECK_EQ(ps.v1.ibat_mA, 256, "T1 ibat");
    CHECK_EQ(ps.v1.temp_dC, -50, "T1 temp_dC");
    CHECK_EQ(ps.v1.faults, 0, "T1 faults");
    CHECK_EQ(ps.temp_agg_dC, -50, "T1 temp_agg");
    CHECK(ps.temp_mp_valid == false, "T1 temp_mp_valid false");
    printf("T1 v1 (Rust vector)                 %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t2_v2_rust(void)
{
    int before = fails;
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(T2_V2, sizeof(T2_V2), &ps), 2, "T2 ver");
    CHECK_EQ(ps.version, 2, "T2 view.version");
    CHECK_EQ(ps.v2.flags, 0x1F, "T2 flags");
    CHECK_EQ(ps.v2.charge_state, 2, "T2 charge_state");
    CHECK_EQ(ps.v2.vbus_in_mV, 20000, "T2 vbus_in");
    CHECK_EQ(ps.v2.pd_in_mV, 15000, "T2 pd_in_mV");
    CHECK_EQ(ps.v2.pd_in_mA, 1750, "T2 pd_in_mA");
    CHECK_EQ(ps.v2.vbus_out_mV, 5100, "T2 vbus_out");
    CHECK_EQ(ps.v2.vout_set_mV, 5100, "T2 vout_set");
    CHECK_EQ(ps.v2.vout_read_mV, 5050, "T2 vout_read");
    CHECK_EQ(ps.v2.iout_limit_mA, 5000, "T2 iout_limit");
    CHECK_EQ(ps.v2.pd_out_mV, 5000, "T2 pd_out_mV");
    CHECK_EQ(ps.v2.pd_out_mA, 5000, "T2 pd_out_mA");
    CHECK_EQ(ps.v2.vbat_mV, 8400, "T2 vbat");
    CHECK_EQ(ps.v2.ichg_mA, -250, "T2 ichg");
    CHECK_EQ(ps.v2.vsys_mV, 8700, "T2 vsys");
    CHECK_EQ(ps.v2.iin_mA, 500, "T2 iin");
    CHECK_EQ(ps.v2.temp_lm_dC, 253, "T2 temp_lm");
    CHECK_EQ(ps.v2.temp_mp_dC, WUPS_PWR2_TEMP_NA, "T2 temp_mp == N/A");
    CHECK(ps.temp_mp_valid == false, "T2 temp_mp_valid false");
    CHECK_EQ(ps.temp_agg_dC, 253, "T2 temp_agg (falls back to LM)");
    CHECK_EQ(ps.v2.faults, 0x0700, "T2 faults");
    CHECK_EQ(ps.v2.uptime_s, 123456, "T2 uptime");
    printf("T2 v2 (Rust vector)                 %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t3_deframer_vector(void)
{
    int before = fails;
    uint8_t raw[40];
    v2_pack(raw, 0x0D, 1, 0, 14580, 15000, 3000, 5046, 5100, 5046, 5000, 5100, 3000,
            7897, 525, 7900, 1200, 461, 470, 0, 86400);
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(raw, sizeof(raw), &ps), 2, "T3 ver");
    CHECK_EQ(ps.v2.flags, 0x0D, "T3 flags");
    CHECK_EQ(ps.v2.charge_state, 1, "T3 charge_state");
    CHECK_EQ(ps.v2.vbus_in_mV, 14580, "T3 vbus_in");
    CHECK_EQ(ps.v2.pd_in_mA, 3000, "T3 pd_in_mA");
    CHECK_EQ(ps.v2.vout_set_mV, 5100, "T3 vout_set");
    CHECK_EQ(ps.v2.pd_out_mV, 5100, "T3 pd_out_mV");
    CHECK_EQ(ps.v2.vbat_mV, 7897, "T3 vbat");
    CHECK_EQ(ps.v2.ichg_mA, 525, "T3 ichg");
    CHECK_EQ(ps.v2.temp_lm_dC, 461, "T3 temp_lm");
    CHECK_EQ(ps.v2.temp_mp_dC, 470, "T3 temp_mp");
    CHECK(ps.temp_mp_valid, "T3 temp_mp_valid");
    CHECK_EQ(ps.temp_agg_dC, 470, "T3 temp_agg = max");
    CHECK_EQ(ps.v2.uptime_s, 86400, "T3 uptime");
    printf("T3 v2 (deframer-test vector)        %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t4_panel_builder(void)
{
    int before = fails;
    /* Panel powerV2(): only these fields are set, everything else stays 0 —
     * including temp_mp_dC, which is a VALID reading (0.0 C), not the sentinel. */
    uint8_t raw[40] = {0};
    raw[0] = 2; raw[1] = 0x0C; raw[2] = 3;
    le16(raw + 4, 19500); le16(raw + 6, 20000); le16(raw + 8, 5000);
    le16(raw + 10, 5050); le16(raw + 22, 7400); le16(raw + 24, 0);
    le16(raw + 30, 312);  le16(raw + 34, 0);
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(raw, sizeof(raw), &ps), 2, "T4 ver");
    CHECK_EQ(ps.v2.flags, 0x0C, "T4 flags");
    CHECK_EQ(ps.v2.charge_state, 3, "T4 charge_state");
    CHECK_EQ(ps.v2.vbus_in_mV, 19500, "T4 vbus_in");
    CHECK_EQ(ps.v2.vbat_mV, 7400, "T4 vbat");
    CHECK_EQ(ps.v2.ichg_mA, 0, "T4 ichg");
    CHECK_EQ(ps.v2.temp_lm_dC, 312, "T4 temp_lm");
    CHECK_EQ(ps.v2.temp_mp_dC, 0, "T4 temp_mp = 0");
    CHECK(ps.temp_mp_valid == true, "T4 temp_mp 0 is VALID (not the sentinel)");
    CHECK_EQ(ps.temp_agg_dC, 312, "T4 temp_agg = max(0, 312)");
    printf("T4 v2 (Panel builder shape)         %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t5_reviewer_capture(void)
{
    int before = fails;
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(T5_CAPTURE, sizeof(T5_CAPTURE), &ps), 2, "T5 ver");
    CHECK_EQ(ps.v2.flags, 0x1F, "T5 flags");
    CHECK_EQ(ps.v2.charge_state, 3, "T5 charge_state (charge done)");
    CHECK_EQ(ps.v2.vbus_in_mV, 14709, "T5 vbus_in");
    CHECK_EQ(ps.v2.pd_in_mV, 15000, "T5 pd_in_mV");
    CHECK_EQ(ps.v2.pd_in_mA, 1750, "T5 pd_in_mA");
    CHECK_EQ(ps.v2.vbus_out_mV, 5051, "T5 vbus_out");
    CHECK_EQ(ps.v2.vout_set_mV, 5000, "T5 vout_set");
    CHECK_EQ(ps.v2.vout_read_mV, 5046, "T5 vout_read");
    CHECK_EQ(ps.v2.iout_limit_mA, 5010, "T5 iout_limit");
    CHECK_EQ(ps.v2.pd_out_mV, 5000, "T5 pd_out_mV");
    CHECK_EQ(ps.v2.pd_out_mA, 3000, "T5 pd_out_mA");
    CHECK_EQ(ps.v2.vbat_mV, 7897, "T5 vbat");
    CHECK_EQ(ps.v2.ichg_mA, 0, "T5 ichg");
    CHECK_EQ(ps.v2.vsys_mV, 7900, "T5 vsys");
    CHECK_EQ(ps.v2.iin_mA, 1200, "T5 iin");
    CHECK_EQ(ps.v2.temp_lm_dC, 350, "T5 temp_lm");
    CHECK_EQ(ps.v2.temp_mp_dC, 440, "T5 temp_mp");
    CHECK(ps.temp_mp_valid, "T5 temp_mp_valid");
    CHECK_EQ(ps.temp_agg_dC, 440, "T5 temp_agg");
    CHECK_EQ(ps.v2.faults, 0, "T5 faults");
    CHECK_EQ(ps.v2.uptime_s, 86272, "T5 uptime");

    /* Documentation of the bug: what the pre-0.8.9 v1 misdecode reported from
     * the very same bytes (this is what the reviewer saw in the HTTP body). */
    wups_power_status_v1_t bad;
    memcpy(&bad, T5_CAPTURE, sizeof(bad));
    CHECK_EQ(bad.temp_dC, 5000, "T5 OLD v1 misdecode: temp_dC == vout_set (5000)");
    CHECK_EQ(bad.faults, 5000, "T5 OLD v1 misdecode: faults == pd_out_mV (5000)");
    CHECK_EQ(bad.charge_state, 0x1F, "T5 OLD v1 misdecode: charge_state == flags");
    CHECK_EQ(bad.vbat_mV, 1750, "T5 OLD v1 misdecode: vbat == pd_in_mA");
    printf("T5 v2 (reviewer capture + old bug)  %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t6_all_distinct(void)
{
    int before = fails;
    wups_power_status_view_t ps;
    CHECK_EQ(wups_power_status_decode(T6_DISTINCT, sizeof(T6_DISTINCT), &ps), 2, "T6 ver");
    CHECK_EQ(ps.v2.flags, 0x15, "T6 flags");
    CHECK_EQ(ps.v2.charge_state, 3, "T6 charge_state");
    CHECK_EQ(ps.v2.reserved, 0xEE, "T6 reserved copied verbatim (ignored by callers)");
    CHECK_EQ(ps.v2.vbus_in_mV, 14709, "T6 vbus_in");
    CHECK_EQ(ps.v2.pd_in_mV, 15000, "T6 pd_in_mV");
    CHECK_EQ(ps.v2.pd_in_mA, 1750, "T6 pd_in_mA");
    CHECK_EQ(ps.v2.vbus_out_mV, 5051, "T6 vbus_out");
    CHECK_EQ(ps.v2.vout_set_mV, 5000, "T6 vout_set");
    CHECK_EQ(ps.v2.vout_read_mV, 5046, "T6 vout_read");
    CHECK_EQ(ps.v2.iout_limit_mA, 5010, "T6 iout_limit");
    CHECK_EQ(ps.v2.pd_out_mV, 5020, "T6 pd_out_mV");
    CHECK_EQ(ps.v2.pd_out_mA, 3000, "T6 pd_out_mA");
    CHECK_EQ(ps.v2.vbat_mV, 7897, "T6 vbat");
    CHECK_EQ(ps.v2.ichg_mA, -250, "T6 ichg");
    CHECK_EQ(ps.v2.vsys_mV, 7900, "T6 vsys");
    CHECK_EQ(ps.v2.iin_mA, 1200, "T6 iin");
    CHECK_EQ(ps.v2.temp_lm_dC, 350, "T6 temp_lm");
    CHECK_EQ(ps.v2.temp_mp_dC, 440, "T6 temp_mp");
    CHECK_EQ(ps.v2.faults, 0x0512, "T6 faults");
    CHECK_EQ(ps.v2.uptime_s, 0x01020304u, "T6 uptime");
    printf("T6 v2 (all fields distinct)         %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t7_aggregate_rule(void)
{
    int before = fails;
    static const struct { int16_t lm, mp, want; } cases[] = {
        {  253, WUPS_PWR2_TEMP_NA,  253 },
        {  -50, WUPS_PWR2_TEMP_NA,  -50 },
        { -100, -32767,            -100 },   /* -32767 is a real reading, not N/A */
        {  500,  300,               500 },
        { -100,  -50,               -50 },
        {    0,    0,                 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "T7 agg(lm=%d, mp=%d)", cases[i].lm, cases[i].mp);
        CHECK_EQ(wups_power_temp_agg_dC(cases[i].lm, cases[i].mp), cases[i].want, msg);
    }
    printf("T7 aggregate temperature rule       %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t8_length_version(void)
{
    int before = fails;
    uint8_t buf[104];

    expect_reject(T5_CAPTURE, 0, "T8 len 0");
    expect_reject(T5_CAPTURE, 1, "T8 len 1 (version byte only)");
    expect_reject(T1_V1, 19, "T8 v1 with 19 B");
    expect_reject(T5_CAPTURE, 39, "T8 v2 with 39 B");
    expect_reject(T5_CAPTURE, 20, "T8 v2 with 20 B is NOT decoded as v1");

    memcpy(buf, T5_CAPTURE, 40);
    buf[0] = 0;    expect_reject(buf, 40, "T8 version 0 / 40 B");
    buf[0] = 3;    expect_reject(buf, 40, "T8 version 3 / 40 B");
    buf[0] = 0xFF; expect_reject(buf, 40, "T8 version 0xFF / 40 B");
    memset(buf, 0, 40);
    expect_reject(buf, 40, "T8 40 zero bytes");
    expect_reject(NULL, 40, "T8 NULL raw");

    CHECK_EQ(wups_power_status_decode(T5_CAPTURE, 40, NULL), 0, "T8 NULL out → 0");

    /* v1 with a 40 B payload: trailer ignored, decoded as v1. */
    wups_power_status_view_t ref, got;
    memcpy(buf, T1_V1, 20);
    memset(buf + 20, 0xC3, 20);
    CHECK_EQ(wups_power_status_decode(T1_V1, 20, &ref), 1, "T8 v1/20 ref");
    CHECK_EQ(wups_power_status_decode(buf, 40, &got), 1, "T8 v1 with 40 B → 1");
    CHECK(memcmp(&ref, &got, sizeof(ref)) == 0, "T8 v1/40 == v1/20");

    /* v2 with 41 B and with 64 B of trailing garbage: identical to the 40 B decode. */
    memcpy(buf, T6_DISTINCT, 40);
    for (size_t i = 40; i < sizeof(buf); ++i) buf[i] = (uint8_t)(0x5A ^ i);
    CHECK_EQ(wups_power_status_decode(T6_DISTINCT, 40, &ref), 2, "T8 v2/40 ref");
    CHECK_EQ(wups_power_status_decode(buf, 41, &got), 2, "T8 v2 with 41 B → 2");
    CHECK(memcmp(&ref, &got, sizeof(ref)) == 0, "T8 v2/41 == v2/40");
    CHECK_EQ(wups_power_status_decode(buf, sizeof(buf), &got), 2, "T8 v2 + 64 B garbage → 2");
    CHECK(memcmp(&ref, &got, sizeof(ref)) == 0, "T8 v2/104 == v2/40");
    printf("T8 length / version dispatch        %s\n", fails == before ? "ok" : "FAIL");
}

static void test_t9_semantics(void)
{
    int before = fails;
    uint8_t raw[40];
    wups_power_status_view_t ps;

    /* ichg signedness from raw bytes at offset 24. */
    memcpy(raw, T5_CAPTURE, 40);
    raw[24] = 0x06; raw[25] = 0xFF;
    CHECK_EQ(wups_power_status_decode(raw, 40, &ps), 2, "T9 ichg neg ver");
    CHECK_EQ(ps.v2.ichg_mA, -250, "T9 ichg 06 FF → -250");
    raw[24] = 0x0D; raw[25] = 0x02;
    CHECK_EQ(wups_power_status_decode(raw, 40, &ps), 2, "T9 ichg pos ver");
    CHECK_EQ(ps.v2.ichg_mA, 525, "T9 ichg 0D 02 → 525");

    /* faults pass through untouched (low byte = MP2762A REG14H, bits 8-10 = TPS). */
    static const uint16_t faults[] = { 0x00FF, 0xFFFF };
    for (size_t i = 0; i < 2; ++i) {
        memcpy(raw, T5_CAPTURE, 40);
        le16(raw + 34, faults[i]);
        CHECK_EQ(wups_power_status_decode(raw, 40, &ps), 2, "T9 faults ver");
        CHECK_EQ(ps.v2.faults, faults[i], "T9 faults pass-through");
    }

    /* flags pass through, including bits above the 5 defined ones. */
    static const uint8_t flags[] = { 0x00, 0x1F, 0xE0 };
    for (size_t i = 0; i < 3; ++i) {
        memcpy(raw, T5_CAPTURE, 40);
        raw[1] = flags[i];
        CHECK_EQ(wups_power_status_decode(raw, 40, &ps), 2, "T9 flags ver");
        CHECK_EQ(ps.v2.flags, flags[i], "T9 flags pass-through");
    }

    /* charge_state 0..3 (MP2762A: not charging / pre-charge / fast / done). */
    for (uint8_t cs = 0; cs <= 3; ++cs) {
        memcpy(raw, T5_CAPTURE, 40);
        raw[2] = cs;
        CHECK_EQ(wups_power_status_decode(raw, 40, &ps), 2, "T9 cs ver");
        CHECK_EQ(ps.v2.charge_state, cs, "T9 charge_state pass-through");
    }
    printf("T9 signedness / pass-through        %s\n", fails == before ? "ok" : "FAIL");
}

int main(void)
{
    test_t1_v1_rust();
    test_t2_v2_rust();
    test_t3_deframer_vector();
    test_t4_panel_builder();
    test_t5_reviewer_capture();
    test_t6_all_distinct();
    test_t7_aggregate_rule();
    test_t8_length_version();
    test_t9_semantics();

    if (fails == 0) {
        printf("\nALL PASSED (%d checks)\n", checks);
        return 0;
    }
    printf("\nFAILED: %d of %d checks\n", fails, checks);
    return 1;
}
