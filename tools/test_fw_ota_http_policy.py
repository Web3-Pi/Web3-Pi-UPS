#!/usr/bin/env python3
"""Compile the real portable OTA HTTP policy and exercise hostile metadata.

No SDK, HTTP connection or device is used. Strings have the same NUL-terminated
contract as HTTP header callbacks. ASan tests allocate truncated values at their
exact size so an overread at a missing separator is observable.
MQTT_TEST_SANITIZERS defaults to address,undefined; no silent fallback.
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
    print("fw_ota_http_policy sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    for name in ("fw_ota_http_policy.c", "fw_ota_http_policy.h"):
        print(name + " SHA256=" + hashlib.sha256((firmware / name).read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="fw-ota-http-policy-") as directory:
        binary = Path(directory) / "test_fw_ota_http_policy"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(firmware), str(root / "tools/test_fw_ota_http_policy.c"),
            str(firmware / "fw_ota_http_policy.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
