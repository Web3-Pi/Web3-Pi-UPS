#!/usr/bin/env python3
"""Extract actual modem.c AT collector/adapter/CPSI parser for host regression.

The SDK boundary supplies cumulative reply snapshots, matching esp_modem DTE
with CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED=y. This does not emulate
the UART, DTE locking or allocation. No device, port or network is accessed.
MQTT_TEST_SANITIZERS defaults to address,undefined; select undefined or empty
explicitly on a host without working ASan. No silent fallback.
"""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, signature):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match[0]), source, flags=re.S)
    start = source.index(signature)
    brace = masked.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    source = (firmware / "modem.c").read_text()
    collector = re.search(r'#if !CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED\b.*?\n\} s_radio_reply;', source, re.S)
    if collector is None:
        raise SystemExit("Cannot find actual guarded radio collector; update extraction explicitly")
    extracted = collector[0] + "\n" + "\n".join(function(source, signature) for signature in (
        "static esp_err_t radio_reply_cb(", "static bool radio_at(", "static void poll_cpsi("))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_radio_adapter sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("modem.c SHA256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-radio-adapter-") as directory:
        temporary = Path(directory)
        (temporary / "modem_radio_adapter.inc").write_text(extracted)
        binary = temporary / "test_modem_radio_adapter"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-DCONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED=1",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_radio_adapter.c"),
            str(firmware / "modem_radio_policy.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
