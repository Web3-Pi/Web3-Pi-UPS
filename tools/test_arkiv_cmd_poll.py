#!/usr/bin/env python3
"""
Host regression test for the Arkiv w3pups-cmd sweep and the ACK tracker
(GitHub issue #9). Firmware functions are sliced VERBATIM out of
firmware-ESP32-LTE-M/main/{arkiv_rpc.c, cmdauth_arkiv.c, arkiv_ack.c} by the
same string markers the external reviewer's reproduction used and compiled
with the real cJSON against stubs. rpc_post captures the last body; in
"paged" mode it models the LIVE node as probed 2026-09-11: entities listed
NEWEST FIRST, `seq > A [&& seq < B]` honoured, PAGE_N per page, a cursor on
every full page. Crypto verify always OK; forward_to_rp2040 records the
dispatch order; fw_ota consumes one chosen SEQ as a local op.

  T1  seq 1,2 pending -> 1 then 2, last_ctr 2; T2 foreign seq 2 does not shadow
      valid seq 1; T3 two WS keys -> two sweeps, no double dispatch; T4 the body
      carries `seq > last_ctr` and resultsPerPage "0x6"
  T5  12 pending newest-first -> both pages GATHERED (2nd window `seq < 7`),
      1..8 dispatched + `more`; next sweep 9..12 in one query, no duplicates
  T5c 7 pending (reviewer scenario, page 1 = 7..2) -> 1..7 in order, 1 not lost
  T5d 20 pending (> page budget) -> lowest 8 seen, ascending, `more` (WARN case)
  T6  duplicate seq once; T7 local op stops the sweep + `more`, next sweep
      continues; T8 8 ACK slots; T9/T10 enqueue/seal failure keeps the mapping;
      T11 success clears it; T12 unsorted page -> seq order
  T13 over-cap page (1st or 2nd) -> sweep aborted, nothing consumed
  T14 slot refreshed mid-emit -> the newer mapping survives (re-find by seq+id)

cJSON: managed_components (after `tools/idf build`) or $IDF_PATH; never copied
into the repo. Run from the Web3-Pi-UPS root: python3 tools/test_arkiv_cmd_poll.py
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware-ESP32-LTE-M")
MAIN = os.path.join(FW, "main")


def find_cjson():
    cands = [os.path.join(FW, "managed_components", "espressif__cjson", "cJSON"),
             os.path.join(os.environ.get("IDF_PATH", "/nonexistent"), "components", "json", "cJSON")]
    for d in cands:
        if os.path.isfile(os.path.join(d, "cJSON.c")):
            return d
    sys.exit("cJSON not found — run `tools/idf build` or set IDF_PATH (looked in: %s)" % ", ".join(cands))


def between(src, start, end):
    return src[src.index(start):src.index(end, src.index(start))]


def fixture(seqs, foreign=()):
    entries = [{
        "creator": "0x" + ("11" if i in foreign else "00") * 20,
        "value": "0x" + "00" * 7 + "%02x" % (seq & 255) + "0000",   # 10 B, p[7]=seq
        "stringAttributes": [{"key": "command_id", "value": "00000000-0000-0000-0000-%012d" % seq},
                             {"key": "sig", "value": "00" * 64}],
        "numericAttributes": [{"key": "seq", "value": seq}, {"key": "epoch", "value": 0}],
    } for i, seq in enumerate(seqs)]
    return json.dumps({"result": {"data": entries}}, separators=(",", ":"))


PREFIX = r'''
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "common/protocol.h"
#include "arkiv_cfg.h"
#include "arkiv_ack.h"
#include "arkiv_writer.h"
#include "arkiv_crypto/aead.h"
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ADDR_LEN 20
#define ARKIV_CLAIMED 2
typedef int SemaphoreHandle_t;
#define portMAX_DELAY 0
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(SemaphoreHandle_t s, int t) { (void)s; (void)t; }
static void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static bool s_ready = true, s_have_owner = true, enqueue_ok = true, seal_ok = true;
static bool refresh_during_enqueue;
static int s_claim_state = ARKIV_CLAIMED;
static uint8_t s_owner_addr[20], s_owner_pub[64];
static uint32_t s_key_epoch;
static uint64_t s_last_ctr, s_cur_block;
static bool persist_progress(uint64_t c, uint64_t b) { (void)c; (void)b; return true; }
typedef int arkiv_keccak256_ctx;
static void arkiv_keccak256_init(arkiv_keccak256_ctx *c) { *c = 0; }
static void arkiv_keccak256_update(arkiv_keccak256_ctx *c, const uint8_t *p, size_t n) { (void)c; (void)p; (void)n; }
static void arkiv_keccak256_finish(arkiv_keccak256_ctx *c, uint8_t *out) { (void)c; memset(out, 0, 32); }
static int arkiv_secp256k1_verify(const uint8_t *p, const uint8_t *d, const uint8_t *s) { (void)p; (void)d; (void)s; return 1; }
bool arkiv_writer_ready(void) { return true; }
uint64_t arkiv_writer_next_seq(void) { return 1; }
static const char *identity_iccid(void) { return "0000000000000000000"; }
int arkiv_writer_payload_seal(uint8_t t, uint64_t s, const char *e, const char *id, const uint8_t *p,
    size_t n, uint8_t *o, size_t cap, size_t *len, uint32_t *epoch)
{ (void)t; (void)s; (void)e; (void)id; (void)p; (void)n; (void)cap; *epoch = 0; *len = 1; o[0] = 0; return seal_ok ? 0 : -1; }
bool arkiv_writer_enqueue_create_entity(const char *c, const uint8_t *p, size_t n, uint32_t ttl,
    const arkiv_attr_t *a, size_t count)
{ (void)c; (void)p; (void)n; (void)ttl; (void)a; (void)count;
  if (refresh_during_enqueue) { arkiv_ack_track_pending(1, "newer-command-for-same-seq-xxxxx"); }
  return enqueue_ok; }
static bool cmdauth_arkiv_ready(void) { return true; }
static int cmdauth_arkiv_claim_state(void) { return ARKIV_CLAIMED; }
static unsigned ota_seq;   /* fw_ota stub consumes the frame whose SEQ == ota_seq */
static bool fw_ota_try_handle_downlink(const uint8_t *p, size_t n)
{ return n >= WUPS_HEADER_BYTES && ota_seq && p[7] == ota_seq; }
static bool wups_link_try_sys_reset(const uint8_t *p, size_t n) { (void)p; (void)n; return false; }
static unsigned dispatched, last_inner_seq, rpc_calls, order[64], order_n;
static void forward_to_rp2040(const char *id, const uint8_t *p, size_t n)
{ (void)id; if (n < WUPS_HEADER_BYTES) abort(); dispatched++; last_inner_seq = p[7]; if (order_n < 64) order[order_n++] = p[7]; }
static const char *response;      /* fixed fixture (pending_max == 0) */
static unsigned pending_max;      /* paged mode: entities seq 1..pending_max, newest first */
static unsigned fail_call;        /* rpc call index (1-based) that answers rpc_rc */
static esp_err_t rpc_rc = ESP_OK;
static char query[1024];
static int emit_entity(char *o, size_t cap, unsigned seq)
{
    return snprintf(o, cap, "{\"creator\":\"0x%040d\",\"value\":\"0x00000000000000%02x0000\","
        "\"stringAttributes\":[{\"key\":\"command_id\",\"value\":\"00000000-0000-0000-0000-%012u\"},"
        "{\"key\":\"sig\",\"value\":\"%0128d\"}],\"numericAttributes\":[{\"key\":\"seq\","
        "\"value\":%u},{\"key\":\"epoch\",\"value\":0}]}", 0, seq & 255, seq, 0, seq);
}
static void build_page(char *out, size_t cap, unsigned long lo, unsigned long hi)  /* (lo, hi) */
{
    unsigned long top = pending_max;
    if (hi && hi - 1 < top) top = hi - 1;
    size_t n = (size_t)snprintf(out, cap, "{\"result\":{\"data\":[");
    unsigned count = 0;
    for (unsigned long s = top; s > lo && count < ARKIV_CMD_PAGE_N; --s, ++count) {
        if (count) out[n++] = ',';
        n += (size_t)emit_entity(out + n, cap - n, (unsigned)s);
    }
    snprintf(out + n, cap - n, "]%s}}", count == ARKIV_CMD_PAGE_N ? ",\"cursor\":\"0xe55\"" : "");
}
static esp_err_t rpc_post(const char *body, char *out, size_t cap)
{
    rpc_calls++; snprintf(query, sizeof(query), "%s", body);
    if (pending_max) {
        const char *a = strstr(body, "seq > "), *b = strstr(body, "seq < ");
        build_page(out, cap, a ? strtoul(a + 6, NULL, 10) : 0, b ? strtoul(b + 6, NULL, 10) : 0);
    } else {
        if (strlen(response) >= cap) abort();
        snprintf(out, cap, "%s", response);
    }
    return (fail_call == 0 || fail_call == rpc_calls) ? rpc_rc : ESP_OK;
}
'''

TESTS = r'''
static int fails;
#define CHECK(name, cond) do { if (cond) printf("PASS %s\n", name); \
    else { fails++; printf("FAIL %s: %s\n", name, #cond); } } while (0)
static void reset(void)
{
    s_last_ctr = s_cur_block = 0; dispatched = rpc_calls = order_n = last_inner_seq = 0; ota_seq = 0;
    pending_max = fail_call = 0; rpc_rc = ESP_OK; refresh_during_enqueue = false; now_us = 0;
    memset(s_slots, 0, sizeof(s_slots)); enqueue_ok = seal_ok = true;
}
static bool order_is(unsigned from, unsigned to)   /* order == from..to, exactly */
{
    if (order_n != to - from + 1) return false;
    for (unsigned i = 0; i < order_n; ++i) if (order[i] != from + i) return false;
    return true;
}
int main(void)
{
    const uint8_t ok = 0;
    bool more;
    reset(); response = two; more = arkiv_rpc_poll_once();
    CHECK("T1 two pending -> 1 then 2, last_ctr 2, nothing more",
          dispatched == 2 && order_is(1, 2) && s_last_ctr == 2 && !more);
    arkiv_rpc_poll_once();
    CHECK("T1b re-sweep of the same page dispatches nothing", dispatched == 2 && s_last_ctr == 2);
    CHECK("T4 body filters seq > last_ctr, page 0x6",
          strstr(query, "&& seq > 2\",") && strstr(query, "resultsPerPage\":\"0x6\""));

    reset(); response = foreign; arkiv_rpc_poll_once(); arkiv_rpc_poll_once();
    CHECK("T2 foreign seq 2 rejected, valid seq 1 dispatched, counter 1",
          dispatched == 1 && last_inner_seq == 1 && s_last_ctr == 1);

    reset(); response = two;
    arkiv_rpc_fetch_and_verify_by_key("KEY_FOR_SEQ_1");
    arkiv_rpc_fetch_and_verify_by_key("KEY_FOR_SEQ_2");
    CHECK("T3 two WS keys -> two sweeps, no double dispatch, key not queried",
          rpc_calls == 2 && dispatched == 2 && order_is(1, 2) && !strstr(query, "KEY_FOR_SEQ"));

    reset(); pending_max = 12; more = arkiv_rpc_poll_once();
    CHECK("T5a 12 pending newest-first -> gather 2 pages, dispatch 1..8, more",
          dispatched == 8 && rpc_calls == 2 && order_is(1, 8) && s_last_ctr == 8 && more);
    CHECK("T5w 2nd page narrowed the window below the 1st (seq < 7)",
          strstr(query, "&& seq > 0 && seq < 7\","));
    more = arkiv_rpc_poll_once();
    CHECK("T5b sweep 2 dispatches 9..12 with one query, no duplicates, nothing more",
          dispatched == 12 && rpc_calls == 3 && order_is(1, 12) && s_last_ctr == 12 && !more);

    reset(); pending_max = 7; more = arkiv_rpc_poll_once();
    CHECK("T5c 7 pending (page 1 = 7..2) -> all 7 in order, seq 1 not lost",
          dispatched == 7 && rpc_calls == 2 && order_is(1, 7) && s_last_ctr == 7 && !more);

    reset(); pending_max = 20; more = arkiv_rpc_poll_once();
    CHECK("T5d 20 pending (> 3 pages) -> lowest 8 of the 18 seen, ascending, more",
          dispatched == 8 && rpc_calls == 3 && order_is(3, 10) && more);

    reset(); response = dup; arkiv_rpc_poll_once();
    CHECK("T6 duplicate seq dispatched once", dispatched == 1 && s_last_ctr == 5);

    reset(); response = three; ota_seq = 2; more = arkiv_rpc_poll_once();
    CHECK("T7a local op at seq 2 stops the sweep (seq 3 not dispatched) and reports more",
          dispatched == 1 && last_inner_seq == 1 && s_last_ctr == 2 && more);
    ota_seq = 0; more = arkiv_rpc_poll_once();
    CHECK("T7b next sweep dispatches seq 3", dispatched == 2 && last_inner_seq == 3 && s_last_ctr == 3 && !more);

    reset();
    for (int i = 1; i <= 5; i++) { now_us = i; arkiv_ack_track_pending((uint8_t)i, "test-id"); }
    bool all = true;
    for (int i = 1; i <= 5; i++) all = all && arkiv_ack_has_pending((uint8_t)i);
    CHECK("T8a five outstanding ACK mappings all kept", all);
    for (int i = 6; i <= 9; i++) { now_us = i; arkiv_ack_track_pending((uint8_t)i, "test-id"); }
    all = true;
    for (int i = 2; i <= 9; i++) all = all && arkiv_ack_has_pending((uint8_t)i);
    CHECK("T8b 9th mapping evicts only the oldest (8 slots)", all && !arkiv_ack_has_pending(1));

    reset(); arkiv_ack_track_pending(1, "test-id"); enqueue_ok = false;
    bool r1 = arkiv_ack_emit(1, &ok, 1), p1 = arkiv_ack_has_pending(1);
    enqueue_ok = true;
    bool r2 = arkiv_ack_emit(1, &ok, 1), p2 = arkiv_ack_has_pending(1);
    CHECK("T9 enqueue failure keeps the mapping; later emit succeeds", !r1 && p1 && r2 && !p2);

    reset(); arkiv_ack_track_pending(1, "test-id"); seal_ok = false;
    CHECK("T10 seal failure keeps the mapping", !arkiv_ack_emit(1, &ok, 1) && arkiv_ack_has_pending(1));

    reset(); arkiv_ack_track_pending(1, "test-id");
    CHECK("T11 successful emit clears the mapping", arkiv_ack_emit(1, &ok, 1) && !arkiv_ack_has_pending(1));

    reset(); response = unsorted; arkiv_rpc_poll_once();
    CHECK("T12 unsorted page dispatched in seq order", order_is(1, 3) && s_last_ctr == 3);

    reset(); response = two; rpc_rc = ESP_ERR_INVALID_SIZE; more = arkiv_rpc_poll_once();
    CHECK("T13 over-cap response: sweep skipped, nothing consumed, no re-query",
          dispatched == 0 && s_last_ctr == 0 && rpc_calls == 1 && !more);
    reset(); pending_max = 12; rpc_rc = ESP_ERR_INVALID_SIZE; fail_call = 2; more = arkiv_rpc_poll_once();
    CHECK("T13b over-cap 2nd page aborts the whole sweep (page 1 not dispatched)",
          dispatched == 0 && s_last_ctr == 0 && rpc_calls == 2 && !more);

    reset(); arkiv_ack_track_pending(1, "test-id"); refresh_during_enqueue = true;
    CHECK("T14 slot refreshed mid-emit keeps the newer mapping",
          arkiv_ack_emit(1, &ok, 1) && arkiv_ack_has_pending(1));

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    puts("ALL PASSED");
    return 0;
}
'''


def main():
    rpc = open(os.path.join(MAIN, "arkiv_rpc.c"), encoding="utf-8").read()
    auth = open(os.path.join(MAIN, "cmdauth_arkiv.c"), encoding="utf-8").read()
    ack = open(os.path.join(MAIN, "arkiv_ack.c"), encoding="utf-8").read()
    header = open(os.path.join(MAIN, "cmdauth_arkiv.h"), encoding="utf-8").read()
    functions = between(header, "typedef struct {", "} arkiv_cmd_t;") + "} arkiv_cmd_t;\n"
    functions += between(rpc, "static int hexnib(char c)\n{", "static void forward_to_rp2040(")
    functions += between(auth, "bool cmdauth_arkiv_check(", "esp_err_t cmdauth_arkiv_bind_owner(")
    functions += ack[ack.index("#define SLOTS"):]
    functions += between(rpc, "static void poll_once(", "static void poll_task(")

    fixtures = [("two", fixture([1, 2])), ("foreign", fixture([1, 2], foreign={1})),
                ("dup", fixture([5, 5])), ("three", fixture([1, 2, 3])),
                ("unsorted", fixture([3, 1, 2]))]
    fix_c = "\n".join("static const char *%s = %s;" % (k, json.dumps(v)) for k, v in fixtures)

    cjson = find_cjson()
    with tempfile.TemporaryDirectory(prefix="arkiv-cmd-poll-") as tmp:
        with open(os.path.join(tmp, "esp_err.h"), "w") as f:
            f.write("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
                    "#define ESP_ERR_INVALID_SIZE 0x104\n")
        src = os.path.join(tmp, "harness.c")
        with open(src, "w", encoding="utf-8") as f:
            f.write(PREFIX + functions + fix_c + "\n" + TESTS)
        exe = os.path.join(tmp, "harness")
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", tmp,
                        "-I", ROOT, "-I", MAIN, "-I", cjson,
                        "-I", os.path.join(FW, "components", "arkiv_crypto", "include"),
                        src, os.path.join(cjson, "cJSON.c"), "-o", exe], check=True)
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == "__main__":
    main()
