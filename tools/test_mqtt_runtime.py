#!/usr/bin/env python3
"""Compile actual mqtt.c and its portable modules against pthread host boundaries.

Holds the SDK boundary for 15 real seconds by default. A controlled monotonic
clock accelerates retry schedules. It does not emulate UART hardware, TLS,
physical OTA or SDK internals. MQTT_TEST_SDK_STALL_MS can explicitly shorten
the held boundary for development; the full regression run uses its default.
MQTT_TEST_SANITIZERS defaults to address,undefined; choose undefined or empty
explicitly if the host lacks a working ASan runtime. Every case is a fresh process.
MQTT_TEST_RUNTIME_CASES optionally selects comma-separated case names.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware-ESP32-LTE-M/main"
HOST = ROOT / "tools/mqtt_runtime_host"
sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
all_cases = ("isolation", "init", "registration", "start", "backoff", "revive", "pressure", "probe",
             "ppp_transport", "ppp_auth", "recovery_commit")
cases = tuple(part.strip() for part in os.environ.get("MQTT_TEST_RUNTIME_CASES", ",".join(all_cases)).split(","))
if not cases or any(case not in all_cases for case in cases):
    raise SystemExit("MQTT_TEST_RUNTIME_CASES must select from: " + ",".join(all_cases))
print("Sanitizers: " + (sanitizers or "disabled explicitly"), flush=True)
print("Runtime cases: " + ",".join(cases), flush=True)
print("mqtt.c SHA256: " + hashlib.sha256((MAIN / "mqtt.c").read_bytes()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix="mqtt-runtime-") as temp:
    binary = Path(temp) / "runtime"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
               "-pthread", "-DCONFIG_MQTT_MSG_ID_INCREMENTAL=1", "-I", str(HOST), "-I", str(MAIN),
               str(HOST / "runtime.c"), str(MAIN / "mqtt_dispatch_queue.c"),
               str(MAIN / "mqtt_health.c"), str(MAIN / "mqtt_packet_guard.c"), "-o", str(binary)]
    if sanitizers:
        command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True, timeout=30)
    for case in cases:
        subprocess.run([str(binary), case], check=True, timeout=35)
    print("ALL SELECTED RUNTIME CASES PASSED", flush=True)
