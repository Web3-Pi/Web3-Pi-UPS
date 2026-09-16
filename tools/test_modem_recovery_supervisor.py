#!/usr/bin/env python3
"""#17 baseline/fix with actual supervisor, health and atomic commit guards.

Synthetic AT/DNS/event scheduling; no hardware/network. Baseline asserts the
unwanted resets to prove the trigger, fixed asserts recovery holds and bounds.
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
    fw = root / "firmware-ESP32-LTE-M/main"
    source = (fw / "modem.c").read_text()
    definitions = []
    for name in (
        "PPP_SUPERVISE_TICK_MS", "UPLINK_DEAD_SECS", "HTTP_UPLINK_FRESH_SECS",
        "ARKIV_UPLINK_FRESH_SECS", "ALERT_CLEAR_RESENDS", "ALERT_CLEAR_RESEND_S",
        "NET_STATUS_EMIT_PERIOD_S", "NET_STATUS_RSSI_DELTA_DB", "NET_STATE_PPP_UP",
        "NET_STATE_MQTT_UP", "UPLINK_HOLD_ALERT_S", "INET_PROBE_TIMEOUT_MS", "INET_PROBE_CACHE_S",
    ):
        definitions.append(re.search(rf"^#define\s+{name}\s+[^\n]+", source, re.M)[0])
    definitions.append(re.search(r"typedef enum \{\n\s+TEARDOWN_NORMAL.*?\} teardown_action_t;", source, re.S)[0])
    definitions.append(re.search(r"/\* PPP lifecycle state:.*?/\* END PPP lifecycle state \*/", source, re.S)[0])
    signatures = [
        "static void modem_registration_snapshot(", "static void modem_network_snapshot(",
        "static void uplink_health(", "static bool modem_recovery_commit_ppp(",
        "static bool modem_recovery_commit_backend(", "static teardown_action_t supervise_uplink(",
    ]
    declarations = re.search(r"typedef struct \{\n    ppp_event_state_t ppp;.*?\} modem_recovery_commit_t;", source, re.S)[0]
    extracted = declarations + "\n" + "\n".join(function(source, sig) for sig in signatures)
    mqtt = (fw / "mqtt.c").read_text()
    ota = (fw / "fw_ota.c").read_text()
    guards = function(mqtt, "bool mqtt_recovery_try_commit(") + function(ota, "bool fw_ota_try_modem_recovery(")
    baseline = root / "tools/test_modem_recovery_baseline.inc"
    if hashlib.sha256(baseline.read_bytes()).hexdigest() != "f8ccf7481aec51752dbf64be7812f1e523de2b9ed69e211f5dc3ec695b17733f":
        raise SystemExit("Sealed baseline was changed")
    sanitizers = os.environ.get("MODEM_RECOVERY_TEST_SANITIZERS", "address,undefined").strip()
    print("modem recovery supervisor: sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("modem.c SHA256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-recovery-supervisor-") as directory:
        temporary = Path(directory)
        (temporary / "definitions.inc").write_text("\n".join(definitions) + "\n")
        (temporary / "supervisor.inc").write_text(extracted)
        (temporary / "guards.inc").write_text(guards)
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(fw), "-I", str(root / "tools"),
            str(root / "tools/test_modem_recovery_supervisor.c"), str(fw / "mqtt_health.c"),
            str(fw / "modem_recovery.c"), str(fw / "modem_radio_policy.c"),
        ]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-sanitize-recover=all"]
        for mode in ("BASELINE", "FIXED"):
            binary = temporary / mode.lower()
            subprocess.run(command + ["-D" + mode, "-o", str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
