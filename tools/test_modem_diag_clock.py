#!/usr/bin/env python3
"""Compile the actual diagnostic clock helper against host clock/log/lock stubs.

No device, network, SNTP initialization or wall-clock mutation. Every scenario
runs in a fresh process, matching a fresh firmware boot. MQTT_TEST_SANITIZERS
defaults to address,undefined; set undefined or empty explicitly when needed.
No failed sanitizer run silently falls back. Each subprocess has a 30 s timeout.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    source = root / "firmware-ESP32-LTE-M/main/modem_diag_clock.c"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    cases = ("unsynced", "late_sync", "utc", "invalid", "age", "backward", "concurrent")
    with tempfile.TemporaryDirectory(prefix="modem-diag-clock-") as directory:
        temp = Path(directory)
        (temp / "freertos").mkdir()
        (temp / "freertos/FreeRTOS.h").write_text('''#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void diag_test_lock(portMUX_TYPE *lock);
void diag_test_unlock(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) diag_test_lock(lock)
#define portEXIT_CRITICAL(lock) diag_test_unlock(lock)
''')
        (temp / "esp_log.h").write_text('''#pragma once
void diag_test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) diag_test_log(__VA_ARGS__)
''')
        (temp / "esp_timer.h").write_text('''#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
''')
        # Include standard headers before renaming the calls: macOS system
        # prototypes have linker aliases that must not rename our stub symbol.
        (temp / "clock_under_test.c").write_text('''#include <sys/time.h>
#include <time.h>
int diag_test_gettimeofday(struct timeval *, void *);
struct tm *diag_test_gmtime_r(const time_t *, struct tm *);
size_t diag_test_strftime(char *, size_t, const char *, const struct tm *);
#define gettimeofday diag_test_gettimeofday
#define gmtime_r diag_test_gmtime_r
#define strftime diag_test_strftime
''' + '#include "' + str(source) + '"\n')
        binary = temp / "test_modem_diag_clock"
        command = compiler + [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
            "-Werror", "-pedantic", "-pthread", "-I", str(temp), "-I", str(source.parent),
            str(temp / "clock_under_test.c"), str(root / "tools/test_modem_diag_clock.c"),
            "-o", str(binary),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"modem_diag_clock: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        for case in cases:
            subprocess.run([str(binary), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
