#!/usr/bin/env python3
"""Test the actual support-diagnostics module using a fake SDK and monotonic clock.

No device or network access. MQTT_TEST_SANITIZERS defaults to address,undefined;
use undefined for UBSan or an empty value for plain C. No silent fallback.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


STUBS = {
    "esp_modem_c_api_types.h": """#pragma once
typedef struct esp_modem_dce_wrap esp_modem_dce_t;
""",
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10c
const char *esp_err_to_name(esp_err_t);
""",
    "esp_modem_api.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_modem_c_api_types.h"
esp_err_t esp_modem_command(esp_modem_dce_t *, const char *,
                            esp_err_t (*)(uint8_t *, size_t), uint32_t);
""",
    "esp_timer.h": """#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_log.h": """#pragma once
void test_log(const char *, const char *, ...);
#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log(tag, __VA_ARGS__)
""",
    "modem_diag_clock.h": """#pragma once
void modem_diag_clock_log(const char *command);
""",
}


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    source = root / "firmware-ESP32-LTE-M" / "main"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="modem-support-diag-") as directory:
        temporary = Path(directory)
        for name, contents in STUBS.items():
            (temporary / name).write_text(contents)
        executable = temporary / "test_modem_support_diag"
        command = compiler + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(source),
            str(source / "modem_support_diag.c"),
            str(root / "tools" / "test_modem_support_diag.c"), "-o", str(executable),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"modem_support_diag: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        # A fresh process provides normal static startup state for each scenario.
        for case in ("full", "errors", "timeout", "invalid", "bounds", "deadline",
                     "readiness", "clock_budget"):
            subprocess.run([str(executable), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
