#!/usr/bin/env python3
"""Exercise production MQTT identity guard (ASan/UBSan by default).

MQTT_TEST_SANITIZERS=undefined selects UBSan; an empty value selects plain C.
Some macOS sandboxes cannot initialize ASan; that is not a passing ASan run.
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
    with tempfile.TemporaryDirectory(prefix="mqtt-packet-guard-") as build:
        binary = Path(build) / "test_mqtt_packet_guard"
        subprocess.run(
            compiler + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                "-fno-omit-frame-pointer",
                "-I", str(sources), str(sources / "mqtt_packet_guard.c"),
                str(repo / "tools" / "test_mqtt_packet_guard.c"),
                "-o", str(binary),
            ] + sanitizer_flags, check=True, timeout=30,
        )
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
