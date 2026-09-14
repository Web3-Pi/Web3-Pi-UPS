#!/usr/bin/env python3
"""Compile the production radio-policy module and run its host tests.

MODEM_RADIO_TEST_SANITIZERS defaults to address,undefined. Select undefined for
UBSan or an empty value for plain C explicitly; there is no silent fallback.
No modem, serial port, SDK, network or radio is accessed.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    main_source = root / "firmware-ESP32-LTE-M" / "main"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MODEM_RADIO_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="modem-radio-policy-") as directory:
        executable = Path(directory) / "test_modem_radio_policy"
        command = compiler + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(main_source), str(root / "tools" / "test_modem_radio_policy.c"),
            str(main_source / "modem_radio_policy.c"), "-o", str(executable),
        ]
        if sanitizers:
            command += [f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer"]
        print(f"modem_radio_policy: sanitizers={sanitizers or 'none'}", flush=True)
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
