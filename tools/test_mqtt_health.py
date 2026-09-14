#!/usr/bin/env python3
"""Compile and run the production MQTT health state machine on the host.

Default: AddressSanitizer and UndefinedBehaviorSanitizer, warnings as errors.
MQTT_TEST_SANITIZERS=undefined selects UBSan; an empty value selects plain C.
Use --no-sanitize to explicitly override that environment setting with plain C.
Failures never fall back silently to another mode. Each subprocess has a 30 s
timeout so an unavailable host sanitizer runtime cannot hang CI indefinitely.
The executable is built in a temporary directory outside the source tree.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-sanitize", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = repo / "firmware-ESP32-LTE-M" / "main"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = "" if args.no_sanitize else os.environ.get(
        "MQTT_TEST_SANITIZERS", "address,undefined"
    ).strip()
    with tempfile.TemporaryDirectory(prefix="wups-mqtt-health-") as directory:
        executable = Path(directory) / "test_mqtt_health"
        command = compiler + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(source),
            str(repo / "tools" / "test_mqtt_health.c"),
            str(source / "mqtt_health.c"),
            "-o", str(executable),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"mqtt_health: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
