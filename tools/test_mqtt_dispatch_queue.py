#!/usr/bin/env python3
"""Compile/run the production queue; MQTT_TEST_SANITIZERS defaults to
address,undefined. Set it to undefined for UBSan only or empty for plain C.
There is no automatic sanitizer fallback; unsupported runtimes fail loudly.
"""
import os
from pathlib import Path
import subprocess
import tempfile

sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
root = Path(__file__).resolve().parents[1]
main = root / "firmware-ESP32-LTE-M/main"
with tempfile.TemporaryDirectory(prefix="mqtt-dispatch-queue-") as temp:
    binary = Path(temp) / "test_mqtt_dispatch_queue"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
               "-Wall", "-Wextra", "-Werror", "-I", str(main),
               str(main / "mqtt_dispatch_queue.c"),
               str(root / "tools/test_mqtt_dispatch_queue.c"), "-o", str(binary)]
    if sanitizers:
        command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
    print("Sanitizers: " + (sanitizers or "disabled explicitly"), flush=True)
    subprocess.run(command, check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=30)
