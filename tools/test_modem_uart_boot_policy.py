#!/usr/bin/env python3
"""Exercise the actual boot UART selector/init with actual OTA rollback policy.

Only public ESP-IDF state/flash calls, GPIO and clock are simulated. The
selected baud is also checked dynamically at prepare/DTE boundaries by the
complete bring-up harness test_modem_apn.py. No device or network is used.
MQTT_TEST_SANITIZERS defaults to address,undefined; overrides are explicit.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import test_fw_ota_policy as ota
from test_modem_radio_adapter import function


def main():
    repo = Path(__file__).resolve().parents[1]
    firmware = repo / "firmware-ESP32-LTE-M/main"
    modem = (firmware / "modem.c").read_text()
    main_source = (firmware / "main.c").read_text()
    ota_source = (firmware / "fw_ota.c").read_text()
    header = (firmware / "fw_ota.h").read_text()
    deadline = re.search(r"(?m)^#define FW_OTA_VERIFY_WINDOW_S\s+(.+)$", header)
    if not deadline:
        raise SystemExit("Production OTA deadline missing")
    calls = [main_source.index(call) for call in (
        "fw_ota_boot_log();", "ESP_ERROR_CHECK(modem_init());",
        "modem_ensure_on();", "modem_at_pass_through_start();")]
    if calls != sorted(calls):
        raise SystemExit("OTA boot log / UART selection must precede modem startup")
    if not re.search(r'\.baud_rate\s*=\s*s_modem_boot_baud\s*,', modem):
        raise SystemExit("Diagnostic UART must share the latched boot baud")
    definitions = []
    for name in ("MODEM_BAUD", "MODEM_PWR_GPIO", "MODEM_DTR_GPIO"):
        match = re.search(r'^#define\s+' + name + r'\s+([^\n]+)', modem, re.M)
        if not match:
            raise SystemExit("Production constant missing: " + name)
        definitions.append("#define " + name + " " + match[1])
    for name in ("s_modem_boot_baud", "s_modem_boot_baud_latched"):
        match = re.search(r'^static (?:int|bool) ' + name + r'\b[^;]*;', modem, re.M)
        if not match:
            raise SystemExit("Production boot state missing: " + name)
        definitions.append(match[0])
    ota_functions = "\n".join(ota.production_function(ota_source, name) for name in (
        "fw_ota_try_modem_recovery", "fw_ota_finish_modem_recovery",
        "claim_in_progress", "release_in_progress", "confirm_reason_name",
        "confirm_running_image", "fw_ota_mark_uplink_healthy", "fw_ota_rollback_tick"))
    production = "\n".join(definitions) + "\n" + "\n".join(function(modem, signature) for signature in (
        "static void modem_select_boot_baud(", "esp_err_t modem_init("))
    tests = (repo / "tools/test_modem_uart_boot_policy.c").read_text()
    prelude, cases = tests.split("/* PRODUCTION_FUNCTIONS */", 1)
    generated = (ota.PRELUDE + "\n#define FW_OTA_VERIFY_WINDOW_S " + deadline[1] + "\n" +
                 ota_functions + "\n" + ota.CASES.replace("int main(void)", "int ota_policy_regression(void)") +
                 "\n" + prelude + "\n" + production + "\n" + cases)
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_uart_boot_policy sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-uart-boot-") as directory:
        temporary = Path(directory)
        source = temporary / "test.c"
        source.write_text(generated)
        for baud in (115200, 230400):
            binary = temporary / ("test_" + str(baud))
            command = shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-DCONFIG_WUPS_MODEM_UART_BAUD=" + str(baud), str(source), "-o", str(binary)]
            if sanitizers:
                command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
