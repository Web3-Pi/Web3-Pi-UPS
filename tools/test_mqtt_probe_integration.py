#!/usr/bin/env python3
"""Compile production guard + health integration with sanitizers by default.

MQTT_TEST_SANITIZERS=undefined selects UBSan; an empty value selects plain C.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main():
    repo = Path(__file__).resolve().parents[1]
    sources = repo / "firmware-ESP32-LTE-M" / "main"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    sanitizer_flags = [f"-fsanitize={sanitizers}"] if sanitizers else []
    with tempfile.TemporaryDirectory(prefix="mqtt-probe-integration-") as build:
        binary = Path(build) / "test_mqtt_probe_integration"
        subprocess.run(
            compiler + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                "-fno-omit-frame-pointer", "-I", str(sources),
                str(sources / "mqtt_packet_guard.c"),
                str(sources / "mqtt_health.c"),
                str(repo / "tools" / "test_mqtt_probe_integration.c"),
                "-o", str(binary),
            ] + sanitizer_flags, check=True, timeout=30,
        )
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
