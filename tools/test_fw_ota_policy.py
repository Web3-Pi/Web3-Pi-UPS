#!/usr/bin/env python3
"""Test actual fw_ota.c confirmation/rollback functions with host SDK stubs.

Only the named production function bodies are compiled; no policy is mirrored.
MQTT_TEST_SANITIZERS defaults to address,undefined; use undefined or empty
explicitly where the host ASan runtime is unavailable. No silent fallback.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def production_function(source: str, name: str) -> str:
    pattern = rf"(?m)^(?:static )?(?:bool|void|const char \*)\s*{name}\([^;]*?\)\n\{{"
    match = re.search(pattern, source)
    if not match:
        raise RuntimeError(f"production definition missing: {name}")
    # These selected bodies contain no brace-bearing string literals. Fail
    # compilation visibly if a future edit changes that extraction contract.
    depth = 1
    cursor = match.end()
    while cursor < len(source) and depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    if depth:
        raise RuntimeError(f"unterminated production definition: {name}")
    return source[match.start():cursor]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

typedef int esp_err_t;
enum { ESP_OK = 0, WUPS_BACKEND_MODE_MQTT = 1,
       WUPS_BACKEND_MODE_HTTP = 2, WUPS_BACKEND_MODE_ARKIV = 3 };
typedef enum {
    OTA_CONFIRM_AUTOMATIC_UPLINK,
    OTA_CONFIRM_AUTHORIZED_NEXT_UPDATE,
    OTA_CONFIRM_PHYSICAL_TRANSFER,
} ota_confirm_reason_t;
static bool s_pending_verify, s_marked_valid, s_validation_busy, s_in_progress;
static bool s_modem_recovery_busy;
static int s_claim_mux, lock_depth;
static int mode, valid_calls, rollback_calls, ota_hooks;
static int64_t clock_us;
static bool proof, last_hook, reenter_rollback, reenter_claim;
static bool invalidate_proof_on_claim;
static esp_err_t valid_result;
static bool claim_in_progress(void);
void fw_ota_rollback_tick(void);

#define portENTER_CRITICAL(mux) do { \
    (void)(mux); assert(lock_depth++ == 0); \
    if (invalidate_proof_on_claim) { \
        proof = false; invalidate_proof_on_claim = false; \
    } \
} while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); assert(--lock_depth == 0); } while (0)
#define pdMS_TO_TICKS(ms) (ms)
static void log_ignored(const char *fmt, ...) { (void)fmt; }
#define ESP_LOGW(tag, ...) log_ignored(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_ignored(__VA_ARGS__)
static int backend_mode_get(void) { return mode; }
static bool mqtt_publication_proof_fresh(void) { assert(lock_depth == 1); return proof; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
static void mqtt_ota_state_changed(bool active) {
    assert(active == s_in_progress); ota_hooks++; last_hook = active;
}
static void vTaskDelay(int ticks) { (void)ticks; assert(!lock_depth); }
static esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {
    assert(!lock_depth && s_validation_busy); valid_calls++;
    if (reenter_rollback) fw_ota_rollback_tick();
    return valid_result;
}
static void esp_ota_mark_app_invalid_rollback_and_reboot(void) {
    assert(!lock_depth && s_validation_busy); rollback_calls++;
    if (reenter_claim) assert(!claim_in_progress());
    /* Returning simulates no bootable rollback slot. */
}
'''

CASES = r'''
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
static void reset(int64_t seconds) {
    assert(!lock_depth);
    s_pending_verify = true; s_marked_valid = false;
    s_validation_busy = s_in_progress = s_modem_recovery_busy = false;
    mode = WUPS_BACKEND_MODE_MQTT; clock_us = seconds * INT64_C(1000000);
    proof = true; valid_result = ESP_OK;
    valid_calls = rollback_calls = ota_hooks = 0;
    reenter_rollback = reenter_claim = false; last_hook = false;
    invalidate_proof_on_claim = false;
}
static bool recovery_commit(void *context) {
    assert(lock_depth == 1);
    unsigned *calls = context;
    ++*calls;
    return true;
}
static bool recovery_rejected(void *context) { (void)context; return false; }
int main(void) {
    unsigned commits = 0;
    reset(10);
    CHECK(!fw_ota_try_modem_recovery(recovery_rejected, NULL));
    CHECK(!s_modem_recovery_busy);
    CHECK(claim_in_progress());
    CHECK(!fw_ota_try_modem_recovery(recovery_commit, &commits) && commits == 0);
    release_in_progress();
    CHECK(fw_ota_try_modem_recovery(recovery_commit, &commits) && commits == 1);
    CHECK(!claim_in_progress() && !s_in_progress);
    CHECK(!fw_ota_try_modem_recovery(recovery_commit, &commits) && commits == 1);
    fw_ota_finish_modem_recovery();
    CHECK(claim_in_progress()); release_in_progress();
    s_validation_busy = true;
    CHECK(!fw_ota_try_modem_recovery(recovery_commit, &commits) && commits == 1);
    reset(599); fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 1 && !s_pending_verify && s_marked_valid);
    fw_ota_mark_uplink_healthy(); CHECK(valid_calls == 1);
    for (int sec = 600; sec <= 601; sec++) {
        reset(sec); fw_ota_mark_uplink_healthy();
        CHECK(valid_calls == 0 && s_pending_verify && !s_marked_valid);
        fw_ota_rollback_tick(); CHECK(rollback_calls == 1);
    }
    reset(599); proof = false; fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 0 && s_pending_verify);
    /* A completed short transfer immediately before claim acquisition has
     * invalidated a formerly fresh snapshot. It cannot authorize this mark. */
    reset(599); invalidate_proof_on_claim = true; fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 0 && s_pending_verify && !proof);
    reset(599); CHECK(claim_in_progress());
    CHECK(ota_hooks == 1 && last_hook);
    fw_ota_mark_uplink_healthy(); CHECK(valid_calls == 0);
    clock_us = INT64_C(601000000); fw_ota_rollback_tick();
    CHECK(rollback_calls == 0); release_in_progress();
    CHECK(ota_hooks == 2 && !last_hook && !s_in_progress);
    fw_ota_mark_uplink_healthy(); CHECK(valid_calls == 0);
    fw_ota_rollback_tick(); CHECK(rollback_calls == 1);

    reset(10); CHECK(claim_in_progress()); release_in_progress();
    CHECK(ota_hooks == 2 && !last_hook); /* Sub-second failed transfer hooks. */
    CHECK(!s_validation_busy);
    reset(599); valid_result = -1; fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 1 && s_pending_verify && !s_marked_valid);
    valid_result = ESP_OK; fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 2 && !s_pending_verify);
    reset(599); valid_result = -1; fw_ota_mark_uplink_healthy();
    clock_us = INT64_C(600000000); valid_result = ESP_OK;
    fw_ota_mark_uplink_healthy(); CHECK(valid_calls == 1 && s_pending_verify);
    fw_ota_rollback_tick(); CHECK(rollback_calls == 1);

    reset(601); proof = false;
    CHECK(confirm_running_image(OTA_CONFIRM_AUTHORIZED_NEXT_UPDATE));
    CHECK(valid_calls == 1 && !s_pending_verify);
    reset(601); proof = false;
    CHECK(confirm_running_image(OTA_CONFIRM_PHYSICAL_TRANSFER));
    CHECK(valid_calls == 1 && !s_pending_verify);
    reset(601); valid_result = -1;
    CHECK(!confirm_running_image(OTA_CONFIRM_PHYSICAL_TRANSFER));
    CHECK(s_pending_verify && !s_marked_valid);
    reset(601); s_validation_busy = true;
    CHECK(!confirm_running_image(OTA_CONFIRM_AUTHORIZED_NEXT_UPDATE));
    CHECK(!claim_in_progress() && valid_calls == 0);

    /* Existing caller-selected HTTP/Arkiv proof remains eligible. */
    for (int selected = WUPS_BACKEND_MODE_HTTP; selected <= WUPS_BACKEND_MODE_ARKIV; selected++) {
        reset(599); mode = selected; proof = false;
        fw_ota_mark_uplink_healthy(); CHECK(valid_calls == 1);
    }
    /* Interleaved callers cannot both operate on otadata. */
    reset(601); reenter_rollback = true;
    CHECK(confirm_running_image(OTA_CONFIRM_PHYSICAL_TRANSFER));
    CHECK(valid_calls == 1 && rollback_calls == 0);
    reset(601); reenter_claim = true; fw_ota_rollback_tick();
    CHECK(rollback_calls == 1 && ota_hooks == 0);
    fw_ota_rollback_tick(); CHECK(rollback_calls == 1); /* No failure reboot loop. */
    reset(599); fw_ota_rollback_tick(); CHECK(rollback_calls == 0);
    printf("fw_ota_policy: %u checks, 0 failures\n", checks);
    return 0;
}
'''


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    firmware = repo / "firmware-ESP32-LTE-M" / "main"
    source = (firmware / "fw_ota.c").read_text()
    header = (firmware / "fw_ota.h").read_text()
    deadline = re.search(r"(?m)^#define FW_OTA_VERIFY_WINDOW_S\s+(.+)$", header)
    if not deadline:
        raise RuntimeError("production rollback deadline missing")
    functions = "\n\n".join(production_function(source, name) for name in (
        "fw_ota_try_modem_recovery", "fw_ota_finish_modem_recovery",
        "claim_in_progress", "release_in_progress", "confirm_reason_name",
        "confirm_running_image", "fw_ota_mark_uplink_healthy", "fw_ota_rollback_tick",
    ))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="wups-fw-ota-policy-") as directory:
        root = Path(directory)
        test_source = root / "test.c"
        test_source.write_text(PRELUDE + "\n#define FW_OTA_VERIFY_WINDOW_S " +
                               deadline.group(1) + "\n" + functions + "\n" + CASES)
        executable = root / "test"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            str(test_source), "-o", str(executable),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"fw_ota_policy: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
