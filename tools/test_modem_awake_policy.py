#!/usr/bin/env python3
"""Compile the production awake policy and run deterministic host regressions.

MQTT_TEST_SANITIZERS defaults to address,undefined; explicitly select undefined
for UBSan or an empty value for plain C. There is no automatic fallback.
These tests do not use the SDK, modem, serial port, radio, or network.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    sources = root / "firmware-ESP32-LTE-M" / "main"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="modem-awake-policy-") as directory:
        executable = Path(directory) / "test_modem_awake_policy"
        command = compiler + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(sources), str(root / "tools" / "test_modem_awake_policy.c"),
            str(sources / "modem_awake_policy.c"),
            str(sources / "modem_radio_policy.c"), "-o", str(executable),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"modem_awake_policy: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
