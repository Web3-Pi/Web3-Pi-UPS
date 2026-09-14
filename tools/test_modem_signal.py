#!/usr/bin/env python3
"""Host regression for CPSI measurements, wire compatibility and HTTP JSON.

Compiles the production parser and actual HTTP projection with minimal cJSON
stubs. Address/undefined sanitizers are on by default. No radio/network access.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    source = (firmware / "http_backend.c").read_text()
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match[0]), source, flags=re.S)
    start = source.index("static void add_net_telemetry(")
    brace = masked.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    sanitizers = os.environ.get("MODEM_SIGNAL_TEST_SANITIZERS", "address,undefined").strip()
    with tempfile.TemporaryDirectory(prefix="modem-signal-") as directory:
        temporary = Path(directory)
        (temporary / "net_telemetry.inc").write_text(source[start:end] + "\n")
        binary = temporary / "test_modem_signal"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_signal.c"),
            str(firmware / "modem_signal.c"), "-o", str(binary),
        ]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        print("modem_signal: sanitizers=" + (sanitizers or "none explicitly"), flush=True)
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
