#!/usr/bin/env python3
"""Exercise the actual modem.c bring-up function with host SDK boundaries.

The complete production function runs, including SIM response parsing, APN
selection, registration timeout and CMUX/DATA entry. The SDK double models
the DCE's copied PDP APN separately from the modem's AT-programmed context.
This catches the old first-boot mismatch; no UART, device or network is used.
ASan/UBSan are enabled by default, with the same explicit override as other
MQTT/modem host tests. This is not a hardware registration test.
"""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_modem_radio_adapter import function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "firmware-ESP32-LTE-M/main/modem.c").read_text()
    apn_state = re.search(r'^static const char \*s_apn = .*?;\n'
                          r'static bool s_apn_seeded = .*?;', source, re.M)
    if apn_state is None:
        raise SystemExit("Cannot extract production APN state")
    profile_default = re.search(r'^#ifndef WUPS_FIXED_APN\n#define WUPS_FIXED_APN ""\n#endif',
                                source, re.M)
    if profile_default is None:
        raise SystemExit("Cannot extract production APN profile default")
    definitions = []
    for name in ("MODEM_TX_GPIO", "MODEM_RX_GPIO", "MODEM_UART", "MODEM_BAUD",
                 "MODEM_REG_WAIT_MS", "CMUX_ENTRY_FAILS_MAX", "CMUX_FALLBACK_RETRY_S"):
        match = re.search(r'^#define\s+' + name + r'\s+([^\n]+)', source, re.M)
        if match is None:
            raise SystemExit("Cannot extract production constant " + name)
        definitions.append("#define " + name + " " + match[1].split("/*", 1)[0].strip())
    bringup = function(source, "static esp_err_t ppp_bringup_dce(")
    extracted = "\n".join(definitions) + "\n" + profile_default[0] + "\n" + apn_state[0] + "\n" + bringup
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_apn sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("modem.c SHA256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Scope: complete production bring-up; SDK/AT/radio/clock boundaries are simulated.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-apn-") as directory:
        temporary = Path(directory)
        include = temporary / "modem_apn_bringup.inc"
        include.write_text(extracted)
        for profile, apn in (("auto", ""), ("1nce", "iot.1nce.net"), ("sensor", "sensor.net")):
            binary = temporary / ("test_modem_apn_" + profile)
            command = shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-DCONFIG_WUPS_MODEM_UART_BAUD=230400",
                "-I", str(temporary), str(root / "tools/test_modem_apn.c"), "-o", str(binary)]
            if apn:
                command += ['-DWUPS_FIXED_APN="' + apn + '"']
            if sanitizers:
                command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
            print("Build/test APN profile=" + profile, flush=True)
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
