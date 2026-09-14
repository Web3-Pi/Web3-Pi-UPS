#!/usr/bin/env python3
"""Run actual modem.c GPIO init and pre-PPP awake verification on the host.

The extracted functions link the real portable awake policy. Only GPIO,
RTOS delay/logging and the radio_at adapter boundary are replaced. There is
no UART, radio, device or network access. GPIO latch ordering is simulated;
physical pin voltage and modem firmware command support need bench checks.

MQTT_TEST_SANITIZERS defaults to address,undefined. Set undefined or an empty
value explicitly where host ASan does not work; there is no silent fallback.
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
    firmware = root / "firmware-ESP32-LTE-M/main"
    source = (firmware / "modem.c").read_text()
    main_source = (firmware / "main.c").read_text()
    # Review the actual call sites, not similarly worded comments. This is
    # static integration evidence, separate from the runtime assertions.
    init = re.search(r'ESP_ERROR_CHECK\(modem_init\(\)\);', main_source)
    ensure = re.search(r'^\s*modem_ensure_on\(\);', main_source, re.M)
    task = re.search(r'^\s*modem_at_pass_through_start\(\);', main_source, re.M)
    if not (init and ensure and task and init.start() < ensure.start() < task.start()):
        raise SystemExit("GPIO initialization must precede raw AT probe and supervisor startup")
    supervisor = function(source, "static void ppp_supervisor_task(")
    exclusions = (r's_fail_stage\s*!=\s*MODEM_FAIL_RADIO\s*&&\s*'
                  r's_fail_stage\s*!=\s*MODEM_FAIL_AWAKE')
    if not re.search(r'if\s*\(\s*' + exclusions +
                     r'\s*&&\s*consecutive_fails\s*>=\s*pwrcycle_thresh\s*\)', supervisor):
        raise SystemExit("RADIO and AWAKE failures must both exclude threshold power cycling")
    if not re.search(r'backoff_cap\s*=\s*s_alert_active\s*&&\s*' + exclusions +
                     r'\s*\?\s*10000u\s*:\s*PPP_BACKOFF_MAX_MS\s*;', supervisor):
        raise SystemExit("RADIO and AWAKE failures must both preserve the normal retry backoff")
    definitions = []
    for name in ("MODEM_PWR_GPIO", "MODEM_DTR_GPIO"):
        match = re.search(r'^#define\s+' + name + r'\s+(\d+)\b', source, re.M)
        if match is None:
            raise SystemExit("Cannot extract actual board pin: " + name)
        definitions.append("#define " + name + " " + match[1])
    extracted = "\n".join(definitions) + "\n" + "\n".join(
        function(source, signature) for signature in (
            "esp_err_t modem_init(", "static bool modem_awake_check_before_ppp("))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_awake_integration sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("modem.c SHA256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("main.c static order: modem_init -> modem_ensure_on -> supervisor PASS", flush=True)
    print("supervisor static policy: RADIO/AWAKE exclude power-cycle threshold and short retry cap PASS", flush=True)
    print("Scope: strict dynamic readback; unsupported legacy replies block PPP, not proof that power saving is enabled.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-awake-integration-") as directory:
        temporary = Path(directory)
        (temporary / "modem_awake_integration.inc").write_text(extracted)
        binary = temporary / "test_modem_awake_integration"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_awake_integration.c"),
            str(firmware / "modem_awake_policy.c"),
            str(firmware / "modem_radio_policy.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
