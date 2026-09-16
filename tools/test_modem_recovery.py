#!/usr/bin/env python3
"""Host regression of the production LTE registration recovery policy."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    sanitizers = os.environ.get("MODEM_RECOVERY_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="modem-recovery-") as directory:
        binary = Path(directory) / "test_modem_recovery"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(firmware), str(root / "tools/test_modem_recovery.c"),
            str(firmware / "modem_recovery.c"), str(firmware / "modem_radio_policy.c"),
            "-o", str(binary),
        ]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
