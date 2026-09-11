#!/usr/bin/env python3
"""
Host regression test for the OLED "Balance" screen path (GitHub issue #8):

  * parse_result_wei()  — sliced verbatim out of firmware-ESP32-LTE-M/main/
    arkiv_rpc.c (plus its hexnib helper) and compiled against the real cJSON,
    so the eth_getBalance parser is tested byte-for-byte: 0x-hex quantity →
    uint64, fail-closed with ESP_ERR_INVALID_SIZE at ≥ 2^64 wei.
  * bal_format() — the marker-delimited BAL_FORMAT_BEGIN/END slice of
    firmware-ESP32-LTE-M/main/oled_menu.c, including the BAL_* thresholds and
    the two fixed gate strings: (state, last_good, wei) → the 64x32 OLED text
    (≤ 4 rows, every row ≤ 10 chars), 6-decimal W3P, "no gas" / "low gas",
    "stale" after a failed read that follows a good one.

The extracted C is compiled with the host `cc` into a small stdin-driven
harness; this script feeds it the vectors and checks every answer. Stubs
replace esp_err.h (same numeric codes as ESP-IDF). cJSON comes from the
project's managed_components (present after `tools/idf build`) or from
$IDF_PATH/components/json/cJSON — it is never copied into the repo, and the
binary lands outside the tree.

Run from the Web3-Pi-UPS repo root: python3 tools/test_arkiv_balance.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "firmware-ESP32-LTE-M", "main")
RPC_C = os.path.join(MAIN, "arkiv_rpc.c")
OLED_C = os.path.join(MAIN, "oled_menu.c")

ESP_OK, ESP_FAIL, ESP_ERR_INVALID_SIZE = 0, -1, 0x104
RC_NAME = {ESP_OK: "OK", ESP_FAIL: "FAIL", ESP_ERR_INVALID_SIZE: "INVALID_SIZE"}
NONE, OK, HIGH, FAIL = 0, 1, 2, 3          # arkiv_tlm_bal_state_t
OLED_COLS, OLED_ROWS = 10, 4
GLM = 10 ** 18

fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL " + msg)


def find_cjson():
    cands = [os.path.join(ROOT, "firmware-ESP32-LTE-M", "managed_components",
                          "espressif__cjson", "cJSON")]
    if os.environ.get("IDF_PATH"):
        cands.append(os.path.join(os.environ["IDF_PATH"], "components", "json", "cJSON"))
    for d in cands:
        if os.path.isfile(os.path.join(d, "cJSON.c")):
            return d
    sys.exit("cJSON not found — run `tools/idf build` in firmware-ESP32-LTE-M "
             "or set IDF_PATH (looked in: %s)" % ", ".join(cands))


def slice_function(src, signature):
    """Body of a C function whose definition line starts with `signature`,
    up to the first line that is exactly `}`."""
    m = re.search(r"^" + re.escape(signature) + r"[^\n;]*\n\{\n.*?\n\}\n", src, re.S | re.M)
    if not m:
        sys.exit("could not slice `%s`" % signature)
    return m.group(0)


def slice_markers(src, begin, end):
    i, j = src.find(begin), src.find(end)
    if i < 0 or j < 0 or j < i:
        sys.exit("markers %s / %s not found" % (begin, end))
    return src[src.index("\n", i) + 1:j]


HARNESS_HEAD = r"""
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "cJSON.h"
#include "arkiv_tlm.h"
typedef int esp_err_t;
#define ESP_OK               0
#define ESP_FAIL            -1
#define ESP_ERR_INVALID_SIZE 0x104
"""

HARNESS_MAIN = r"""
static void put_escaped(const char *s)
{
    for (; *s; ++s) putchar(*s == '\n' ? '|' : *s);
    putchar('\n');
}
int main(void)
{
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        if (line[0] == 'P') {                       /* P <json> */
            uint64_t v = 0xDEADBEEFULL;
            esp_err_t rc = parse_result_wei(line + 2, &v);
            printf("rc=%d v=%" PRIu64 "\n", rc, v);
        } else if (line[0] == 'F') {                /* F <state> <last_good> <wei> [cap] */
            arkiv_tlm_balance_t b = {0};
            unsigned st = 0, lg = 0, cap = 64;
            unsigned long long wei = 0;
            sscanf(line + 2, "%u %u %llu %u", &st, &lg, &wei, &cap);
            b.state = (arkiv_tlm_bal_state_t)st; b.last_good = (arkiv_tlm_bal_state_t)lg; b.wei = wei;
            char out[64]; memset(out, 'X', sizeof out);
            bal_format(out, cap, &b);
            if (strlen(out) >= cap) { puts("OVERFLOW"); continue; }
            put_escaped(out);
        } else if (line[0] == 'T') {                /* fixed gate strings */
            put_escaped(BAL_TXT_NO_KEY);
            put_escaped(BAL_TXT_MODE_OFF);
        }
    }
    return 0;
}
"""


def build(tmp):
    cjson = find_cjson()
    rpc = open(RPC_C, encoding="utf-8").read()
    oled = open(OLED_C, encoding="utf-8").read()
    src = (HARNESS_HEAD
           + slice_function(rpc, "static int hexnib(char c)")
           + slice_function(rpc, "static esp_err_t parse_result_wei(")
           + slice_markers(oled, "/* BAL_FORMAT_BEGIN", "/* BAL_FORMAT_END")
           + HARNESS_MAIN)
    c_path = os.path.join(tmp, "harness.c")
    with open(c_path, "w", encoding="utf-8") as f:
        f.write(src)
    exe = os.path.join(tmp, "harness")
    cjson_o = os.path.join(tmp, "cJSON.o")
    subprocess.run(["cc", "-std=c11", "-w", "-c", os.path.join(cjson, "cJSON.c"), "-o", cjson_o],
                   check=True)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", cjson, "-I", MAIN,
                    c_path, cjson_o, "-o", exe], check=True)
    return exe


def rpc(result_json_fragment):
    return '{"jsonrpc":"2.0","id":1,%s}' % result_json_fragment


PARSE_VECTORS = [   # (label, response body, expected rc, expected value or None)
    ("zero",              rpc('"result":"0x0"'),                      ESP_OK, 0),
    ("empty 0x",          rpc('"result":"0x"'),                       ESP_OK, 0),
    ("18 GLM",            rpc('"result":"0xf9ccd8a1c5080000"'),       ESP_OK, 18 * GLM),
    ("uppercase 0X",      rpc('"result":"0XFF"'),                     ESP_OK, 255),
    ("leading zeros",     rpc('"result":"0x00000000000000000000001"'), ESP_OK, 1),
    ("uint64 max",        rpc('"result":"0xffffffffffffffff"'),       ESP_OK, 2 ** 64 - 1),
    ("2^64",              rpc('"result":"0x10000000000000000"'),      ESP_ERR_INVALID_SIZE, None),
    ("19 GLM",            rpc('"result":"%s"' % hex(19 * GLM)),       ESP_ERR_INVALID_SIZE, None),
    ("non-hex digit",     rpc('"result":"0x12g4"'),                   ESP_FAIL, None),
    ("result not string", rpc('"result":1234'),                       ESP_FAIL, None),
    ("missing result",    rpc('"error":{"code":-32000}'),             ESP_FAIL, None),
    ("malformed JSON",    '{"jsonrpc":"2.0","result":',               ESP_FAIL, None),
    ("empty body",        "",                                         ESP_FAIL, None),
]

FORMAT_VECTORS = [  # (label, state, last_good, wei, expected rows)
    ("none / checking",  NONE, NONE, 0,             ["BALANCE", "checking.."]),
    ("fail, no value",   FAIL, NONE, 0,             ["BALANCE", "offline"]),
    ("fail, last good",  FAIL, OK, 5 * 10 ** 12,    ["BALANCE", "0.000005", "W3P", "low gas"]),
    ("fail, stale",      FAIL, OK, 10 ** 15,        ["BALANCE", "0.001000", "W3P", "stale"]),
    ("fail after high",  FAIL, HIGH, 0,             ["BALANCE", ">18.446744", "W3P", "stale"]),
    ("high",             HIGH, HIGH, 0,             ["BALANCE", ">18.446744", "W3P"]),
    ("zero → no gas",    OK, OK, 0,                 ["BALANCE", "0.000000", "W3P", "no gas"]),
    ("3e8-1 → no gas",   OK, OK, 3 * 10 ** 8 - 1,   ["BALANCE", "0.000000", "W3P", "no gas"]),
    ("3e8 → low gas",    OK, OK, 3 * 10 ** 8,       ["BALANCE", "0.000000", "W3P", "low gas"]),
    ("5e12 → low gas",   OK, OK, 5 * 10 ** 12,      ["BALANCE", "0.000005", "W3P", "low gas"]),
    ("1e13-1 → low gas", OK, OK, 10 ** 13 - 1,      ["BALANCE", "0.000009", "W3P", "low gas"]),
    ("1e13 → no hint",   OK, OK, 10 ** 13,          ["BALANCE", "0.000010", "W3P"]),
    ("0.001 GLM",        OK, OK, 10 ** 15,          ["BALANCE", "0.001000", "W3P"]),
    ("truncates, no round", OK, OK, 123456789012345678, ["BALANCE", "0.123456", "W3P"]),
    ("1 GLM",            OK, OK, GLM,               ["BALANCE", "1.000000", "W3P"]),
    ("18 GLM",           OK, OK, 18 * GLM,          ["BALANCE", "18.000000", "W3P"]),
    ("uint64 max",       OK, OK, 2 ** 64 - 1,       ["BALANCE", "18.446744", "W3P"]),
]


def main():
    tmp = tempfile.mkdtemp(prefix="test_arkiv_balance.")
    exe = build(tmp)
    check(hex(18 * GLM) == "0xf9ccd8a1c5080000", "18 GLM hex vector self-check")
    check(19 * GLM > 2 ** 64 - 1, "19 GLM exceeds uint64 (vector premise)")

    lines = ["P " + body for _, body, _, _ in PARSE_VECTORS]
    lines += ["F %d %d %d" % (st, lg, wei) for _, st, lg, wei, _ in FORMAT_VECTORS]
    lines += ["F %d %d %d 8" % (OK, OK, 2 ** 64 - 1)]  # tiny cap: must stay NUL-terminated
    lines += ["T"]
    out = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True,
                         text=True, check=True).stdout.splitlines()
    expected_lines = len(PARSE_VECTORS) + len(FORMAT_VECTORS) + 1 + 2
    check(len(out) == expected_lines, "harness answered %d lines, want %d" % (len(out), expected_lines))
    if len(out) != expected_lines:
        return finish()

    i = 0
    for label, _, want_rc, want_v in PARSE_VECTORS:
        m = re.match(r"rc=(-?\d+) v=(\d+)$", out[i]); i += 1
        if not m:
            check(False, "parse %s: unexpected harness line %r" % (label, out[i - 1]))
            continue
        rc, v = int(m.group(1)), int(m.group(2))
        check(rc == want_rc, "parse %s: rc %s want %s" % (label, RC_NAME.get(rc, rc), RC_NAME[want_rc]))
        if want_rc == ESP_OK:
            check(v == want_v, "parse %s: value %d want %d" % (label, v, want_v))
        else:
            check(v == 0xDEADBEEF, "parse %s: *out touched on error" % label)
        print("ok  parse %-18s -> %s" % (label, RC_NAME.get(rc, rc)))

    for label, _, _, _, want_rows in FORMAT_VECTORS:
        rows = out[i].split("|"); i += 1
        check(rows == want_rows, "format %s: got %r want %r" % (label, rows, want_rows))
        check(len(rows) <= OLED_ROWS, "format %s: %d rows > %d" % (label, len(rows), OLED_ROWS))
        for r in rows:
            check(len(r) <= OLED_COLS, "format %s: row %r is %d chars > %d" % (label, r, len(r), OLED_COLS))
        print("ok  format %-20s -> %s" % (label, " / ".join(rows)))

    check(out[i] != "OVERFLOW" and len(out[i]) == 7, "tiny cap: truncated to cap-1, NUL-terminated (%r)" % out[i]); i += 1
    for fixed in out[i:i + 2]:
        rows = fixed.split("|")
        check(len(rows) <= OLED_ROWS and all(len(r) <= OLED_COLS for r in rows),
              "fixed gate text %r exceeds %dx%d" % (fixed, OLED_COLS, OLED_ROWS))
        print("ok  fixed  %s" % " / ".join(rows))
    return finish()


def finish():
    if fails:
        print("\n%d FAILED" % len(fails))
        return 1
    print("\nALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
