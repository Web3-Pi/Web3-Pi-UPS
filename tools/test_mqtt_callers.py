#!/usr/bin/env python3
"""Exercise actual MQTT caller functions with host-only boundary stubs.

MQTT_TEST_SANITIZERS defaults to address,undefined; set undefined or empty
explicitly on hosts with unavailable ASan. No device or network access.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware-ESP32-LTE-M/main"


def function(source, signature):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: " " * len(m[0]), source, flags=re.S)
    start = source.index(signature)
    brace = masked.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[start:end] + "\n"


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/protocol.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NS_MODE "w3mode"
#define KEY_PREV_MODE "prev_mode"
#define ARKIV_CLAIMED 2
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef enum { MQTT_RECEIPT_UNKNOWN, MQTT_RECEIPT_PENDING,
               MQTT_RECEIPT_SDK_ACCEPTED, MQTT_RECEIPT_FAILED } mqtt_receipt_status_t;
static wups_backend_mode_t s_cur_mode = WUPS_BACKEND_MODE_MQTT;
static uint64_t s_confirm_receipt;
static bool s_confirm_sdk_accepted, s_confirm_done;
static int route, publish_calls, tracked_submits, critical_submits;
static char captured_topic[64];
static uint8_t captured_payload[256];
static size_t captured_len;
static int captured_qos, captured_retain;
static uint32_t captured_key;
static bool admit = true;
static const char *identity_iccid(void) { return "test"; }
static const char *mqtt_topic_event(void) { return "t/test/event"; }
static wups_backend_mode_t backend_mode_get(void) { return s_cur_mode; }
static int capture(int how, const char *topic, const void *payload, size_t len,
                   int qos, int retain, uint32_t key) {
    ++publish_calls; route = how;
    assert(strlen(topic) < sizeof(captured_topic) && len <= sizeof(captured_payload));
    strcpy(captured_topic, topic);
    if (len) memcpy(captured_payload, payload, len);
    captured_len = len; captured_qos = qos; captured_retain = retain; captured_key = key;
    return admit ? 0 : -2;
}
static int mqtt_publish_raw(const char *t, const void *p, size_t n, int q, int r) {
    return capture(1, t, p, n, q, r, 0);
}
static int mqtt_publish_snapshot(const char *t, const void *p, size_t n, int q, int r, uint32_t k) {
    return capture(2, t, p, n, q, r, k);
}
static int mqtt_publish_critical(const char *t, const void *p, size_t n, int q, int r) {
    ++critical_submits; return capture(3, t, p, n, q, r, 0);
}
static int mqtt_publish_tracked(const char *t, const void *p, size_t n, int q, int r, uint64_t *receipt) {
    ++tracked_submits; int rc = capture(4, t, p, n, q, r, 0);
    *receipt = rc == 0 ? (uint64_t)tracked_submits : 0; return rc;
}
static mqtt_receipt_status_t receipt_status;
static int receipt_takes, receipt_forgets;
static mqtt_receipt_status_t mqtt_receipt_take(uint64_t token) {
    assert(token != 0); ++receipt_takes; return receipt_status;
}
static void mqtt_receipt_forget(uint64_t token) { assert(token != 0); ++receipt_forgets; }
static bool marker_cached, marker_persisted, commit_ok;
static uint8_t previous_mode;
static int read_opens, write_opens, commits, erases, nonmqtt_emits;
static int nonmqtt_result;
static int nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    assert(strcmp(ns, NS_MODE) == 0); *h = mode;
    if (mode == NVS_READONLY) ++read_opens; else ++write_opens; return ESP_OK;
}
static int nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) {
    assert(h == NVS_READONLY && strcmp(key, KEY_PREV_MODE) == 0);
    *v = previous_mode; return marker_cached ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_erase_key(nvs_handle_t h, const char *key) {
    assert(h == NVS_READWRITE && strcmp(key, KEY_PREV_MODE) == 0);
    ++erases; bool existed = marker_cached; marker_cached = false;
    return existed ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_commit(nvs_handle_t h) {
    assert(h == NVS_READWRITE); ++commits;
    if (!commit_ok) return ESP_FAIL;
    marker_persisted = marker_cached; return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t emit_mode_changed(wups_backend_mode_t from, wups_backend_mode_t to, bool pending) {
    (void)from; (void)to; assert(!pending); ++nonmqtt_emits; return nonmqtt_result;
}
static int arkiv_state, arkiv_observations, http_observations, event_observations;
static bool ack_pending, ack_success;
static int cmdauth_arkiv_claim_state(void) { return arkiv_state; }
static void arkiv_tlm_observe_frame(const uint8_t *p, size_t n) { (void)p; (void)n; ++arkiv_observations; }
static void http_backend_observe_telemetry_frame(const uint8_t *p, size_t n) { (void)p; (void)n; ++http_observations; }
static void arkiv_event_observe_frame(const uint8_t *p, size_t n) { (void)p; (void)n; ++event_observations; }
static bool arkiv_ack_has_pending(uint8_t seq) { (void)seq; return ack_pending; }
static bool arkiv_ack_emit(uint8_t seq, const uint8_t *p, size_t n) {
    (void)seq; (void)p; (void)n; return ack_success;
}
'''

TESTS = r'''
static size_t make_frame(uint8_t *out, uint8_t src, uint8_t cls, uint8_t op,
                         uint8_t flags, uint8_t version, size_t payload_len) {
    memset(out, 0, 256); out[0] = WUPS_SYNC1; out[1] = WUPS_SYNC2;
    out[2] = WUPS_ADDR_BROADCAST; out[3] = src; out[4] = cls; out[5] = op;
    out[6] = flags; out[8] = (uint8_t)payload_len; out[9] = (uint8_t)(payload_len >> 8);
    if (payload_len) out[10] = version;
    wups_fletcher8(out + 2, 8 + payload_len, out + 10 + payload_len, out + 11 + payload_len);
    out[12 + payload_len] = WUPS_END1; out[13 + payload_len] = WUPS_END2;
    return payload_len + WUPS_FRAMING_BYTES;
}
static void checksum(uint8_t *frame, size_t len) {
    wups_fletcher8(frame + 2, len - 6, frame + len - 4, frame + len - 3);
}
static void net_publish(const char *topic, const uint8_t *payload, size_t len, int qos, int retain) {
    uint8_t buf[512]; size_t topic_len = strlen(topic);
    wups_net_publish_v1_hdr_t h = { .version = 1, .qos = (uint8_t)qos,
        .retain = (uint8_t)retain, .topic_len = (uint8_t)topic_len, .payload_len = (uint16_t)len };
    memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), topic, topic_len);
    if (len) memcpy(buf + sizeof(h) + topic_len, payload, len);
    handle_net_publish(buf, (uint16_t)(sizeof(h) + topic_len + len));
}
static void test_routing(void) {
    uint8_t frame[256];
    size_t n = make_frame(frame, WUPS_ADDR_CH32X, WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                         WUPS_FLAG_EVENT, 2, sizeof(wups_power_status_v2_t));
    net_publish("telemetry", frame, n, 0, 0);
    assert(route == 2 && captured_key == ((WUPS_ADDR_CH32X << 16) | (WUPS_CLASS_POWER << 8) | WUPS_OP_PWR_STATUS));
    assert(captured_len == n && memcmp(captured_payload, frame, n) == 0);
    uint32_t power_key = captured_key;
    n = make_frame(frame, WUPS_ADDR_RPI, WUPS_CLASS_HOST, WUPS_OP_HOST_STATUS,
                   WUPS_FLAG_EVENT, 1, sizeof(wups_host_status_v1_t));
    net_publish("telemetry", frame, n, 1, 1); assert(route == 2 && captured_key != power_key);
    assert(captured_qos == 1 && captured_retain == 1);
    n = make_frame(frame, WUPS_ADDR_ESP32, WUPS_CLASS_NET, WUPS_OP_NET_STATUS,
                   WUPS_FLAG_EVENT, 2, sizeof(wups_net_status_v2_t));
    net_publish("telemetry", frame, n, 0, 0); assert(route == 2);
    net_publish("t/test/telemetry", frame, n, 2, 1);
    assert(route == 1 && captured_qos == 2 && captured_retain == 1);
    frame[n - 4] ^= 1; net_publish("telemetry", frame, n, 0, 0); assert(route == 1);
    checksum(frame, n); frame[6] = WUPS_FLAG_RESP; checksum(frame, n);
    net_publish("telemetry", frame, n, 0, 0); assert(route == 1);
    frame[6] = WUPS_FLAG_EVENT; frame[10] = 3; checksum(frame, n);
    net_publish("telemetry", frame, n, 0, 0); assert(route == 1);
    net_publish("pubtest", NULL, 0, 1, 1); assert(route == 1 && captured_len == 0 && captured_retain == 1);
    const uint8_t binary[] = {0, 255, 0, 1};
    net_publish("telemetry", binary, sizeof(binary), 0, 0);
    assert(route == 1 && captured_len == sizeof(binary) && memcmp(binary, captured_payload, sizeof(binary)) == 0);
    net_publish("cmd/response", binary, sizeof(binary), 2, 1);
    assert(route == 3 && captured_qos == 2 && captured_retain == 1);
    n = make_frame(frame, WUPS_ADDR_CH32X, WUPS_CLASS_POWER, WUPS_OP_PWR_EVENT,
                   WUPS_FLAG_EVENT, 1, sizeof(wups_power_event_v1_t));
    frame[11] = WUPS_PWR_EVT_MAINS_LOST; checksum(frame, n);
    net_publish("event", frame, n, 1, 0); assert(route == 3);
    frame[11] = 254; checksum(frame, n); net_publish("event", frame, n, 1, 0); assert(route == 1);
    frame[4] = WUPS_CLASS_SYSTEM; frame[5] = WUPS_OP_SYS_LOG; checksum(frame, n);
    net_publish("event", frame, n, 1, 0); assert(route == 1);
    arkiv_state = ARKIV_CLAIMED; ack_pending = ack_success = true;
    int before = publish_calls;
    net_publish("cmd/response", frame, n, 1, 0); assert(publish_calls == before);
    ack_success = false;
    net_publish("cmd/response", frame, n, 1, 0); assert(publish_calls == before + 1 && route == 3);
    arkiv_state = 0; ack_pending = false;
    s_cur_mode = WUPS_BACKEND_MODE_HTTP;
    net_publish("telemetry", binary, sizeof(binary), 0, 0); assert(http_observations == 1);
    s_cur_mode = WUPS_BACKEND_MODE_MQTT;
    puts("PASS actual UART publication routing, flags/binary preservation, strict snapshots and Arkiv/HTTP diversion");
}
static void reset_confirmation(void) {
    s_confirm_receipt = 0; s_confirm_sdk_accepted = s_confirm_done = false;
    marker_cached = marker_persisted = commit_ok = admit = true;
    previous_mode = WUPS_BACKEND_MODE_ARKIV; s_cur_mode = WUPS_BACKEND_MODE_MQTT;
    receipt_status = MQTT_RECEIPT_PENDING;
    tracked_submits = critical_submits = receipt_takes = receipt_forgets = 0;
    read_opens = write_opens = commits = erases = nonmqtt_emits = 0;
    nonmqtt_result = ESP_FAIL;
}
static void test_confirmation(void) {
    reset_confirmation();
    backend_mode_emit_post_switch_confirm();
    assert(tracked_submits == 1 && marker_persisted && erases == 0 && s_confirm_receipt != 0);
    backend_mode_emit_post_switch_confirm();
    assert(tracked_submits == 1 && receipt_takes == 1 && erases == 0);
    receipt_status = MQTT_RECEIPT_SDK_ACCEPTED; commit_ok = false;
    backend_mode_emit_post_switch_confirm();
    assert(s_confirm_sdk_accepted && !s_confirm_done && marker_persisted && !marker_cached && commits == 1);
    int reads = read_opens; commit_ok = true;
    backend_mode_emit_post_switch_confirm();
    assert(tracked_submits == 1 && s_confirm_done && !marker_persisted && commits == 2 && read_opens == reads);
    backend_mode_emit_post_switch_confirm(); assert(read_opens == reads);

    reset_confirmation(); admit = false;
    backend_mode_emit_post_switch_confirm(); assert(!s_confirm_receipt && erases == 0);
    admit = true; backend_mode_emit_post_switch_confirm(); assert(tracked_submits == 2);
    receipt_status = MQTT_RECEIPT_FAILED; backend_mode_emit_post_switch_confirm();
    assert(!s_confirm_receipt && marker_persisted && erases == 0);
    backend_mode_emit_post_switch_confirm(); assert(tracked_submits == 3);
    receipt_status = MQTT_RECEIPT_UNKNOWN; backend_mode_emit_post_switch_confirm();
    assert(!s_confirm_receipt && marker_persisted);

    reset_confirmation();
    assert(emit_via_mqtt(WUPS_BACKEND_MODE_MQTT, WUPS_BACKEND_MODE_ARKIV, true, NULL) == ESP_OK);
    assert(critical_submits == 1 && tracked_submits == 0 && captured_payload[13] == WUPS_MODE_CHANGED_FLAG_PENDING);
    reset_confirmation(); marker_cached = marker_persisted = false;
    backend_mode_emit_post_switch_confirm(); reads = read_opens;
    backend_mode_emit_post_switch_confirm(); assert(s_confirm_done && read_opens == reads && tracked_submits == 0);
    reset_confirmation(); s_cur_mode = WUPS_BACKEND_MODE_HTTP;
    backend_mode_emit_post_switch_confirm(); assert(nonmqtt_emits == 1 && erases == 0);
    reset_confirmation(); s_cur_mode = WUPS_BACKEND_MODE_ARKIV; previous_mode = WUPS_BACKEND_MODE_MQTT;
    nonmqtt_result = ESP_OK; backend_mode_emit_post_switch_confirm();
    assert(nonmqtt_emits == 1 && tracked_submits == 0 && s_confirm_done);
    puts("PASS actual post-switch receipt handling, deferred NVS persistence, retry and non-MQTT compatibility");
}
int main(void) { test_routing(); test_confirmation(); puts("ALL PASSED"); }
'''

backend = (MAIN / "backend_mode.c").read_text()
link = (MAIN / "wups_link.c").read_text()
source = PREFIX
for signature in ["static bool is_known_mode(", "static void build_mode_changed_payload(",
                  "static uint16_t encode_mqtt_frame(", "static esp_err_t emit_via_mqtt(",
                  "static bool clear_post_switch_marker(", "void backend_mode_emit_post_switch_confirm("]:
    source += function(backend, signature)
for signature in ["static bool mqtt_wups_frame_valid(", "static uint32_t mqtt_wups_snapshot_key(",
                  "static bool mqtt_wups_critical_event(", "static void handle_net_publish("]:
    source += function(link, signature)
source += TESTS

with tempfile.TemporaryDirectory(prefix="mqtt-callers-") as temp:
    c_file, binary = Path(temp) / "callers.c", Path(temp) / "callers"
    c_file.write_text(source)
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-I", str(ROOT), str(c_file), "-o", str(binary)]
    if sanitizers:
        command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
    print("Sanitizers: " + (sanitizers or "disabled explicitly"), flush=True)
    subprocess.run(command, check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30)
