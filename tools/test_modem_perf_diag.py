#!/usr/bin/env python3
"""Exercise the real CPU runtime-delta helper with independent sample fixtures.

The runtime task is disabled: no scheduler, hardware, serial port or network is
used. These tests verify arithmetic and invalid-window handling, not the 30 s
sampling cadence, logging overhead, ISR attribution or live FreeRTOS counters.
MQTT_TEST_SANITIZERS defaults to address,undefined, with explicit overrides.
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
    print("modem_perf_diag sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    for name in ("perf_diag.c", "perf_diag.h"):
        print(name + " SHA256=" + hashlib.sha256((firmware / name).read_bytes()).hexdigest(), flush=True)
    print("Scope: production CPU delta helper; runtime task disabled; no device accessed.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-perf-diag-") as directory:
        temporary = Path(directory)
        (temporary / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_WUPS_PERF_DIAG 0\n")
        binary = temporary / "test_modem_perf_diag"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_perf_diag.c"),
            str(firmware / "perf_diag.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
