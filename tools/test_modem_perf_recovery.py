#!/usr/bin/env python3
"""Exercise real recovery request/lifecycle logic with virtual time and owner.

Production headers and complete module compile; only timer, delay, uplink,
OTA status and logs are simulated. No UART/network/device/reset is performed.
"""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_perf_recovery sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("perf_recovery.c SHA256=" + hashlib.sha256((firmware / "perf_recovery.c").read_bytes()).hexdigest(), flush=True)
    print("Scope: full production recovery module; virtual owner/time/uplink; no device accessed.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-perf-recovery-") as directory:
        temporary = Path(directory)
        (temporary / "freertos").mkdir()
        (temporary / "freertos/FreeRTOS.h").write_text(
            "#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n#define pdMS_TO_TICKS(ms) (ms)\n")
        (temporary / "freertos/task.h").write_text(
            '#pragma once\n#include "freertos/FreeRTOS.h"\nvoid vTaskDelay(TickType_t);\n'
            'unsigned uxTaskGetStackHighWaterMark(void *);\n')
        (temporary / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        (temporary / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        (temporary / "esp_log.h").write_text(
            "#pragma once\nvoid mock_log(const char *, const char *, ...);\n"
            "#define ESP_LOGI(...) mock_log(__VA_ARGS__)\n#define ESP_LOGE(...) mock_log(__VA_ARGS__)\n")
        for enabled in (0, 1):
            (temporary / "sdkconfig.h").write_text(
                f"#pragma once\n#define CONFIG_WUPS_PERF_RECOVERY_BENCH {enabled}\n")
            binary = temporary / ("test_modem_perf_recovery_" + str(enabled))
            command = shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-I", str(temporary), "-I", str(firmware),
                str(root / "tools/test_modem_perf_recovery.c"), str(firmware / "perf_recovery.c"),
                "-o", str(binary)]
            if sanitizers:
                command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=30)
            cases = ("healthy", "no_owner", "no_down", "no_proof", "ota_baseline", "ota_pending",
                     "second_failed", "final_unstable", "baseline_flap") if enabled else ("disabled",)
            for case in cases:
                subprocess.run([str(binary), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
