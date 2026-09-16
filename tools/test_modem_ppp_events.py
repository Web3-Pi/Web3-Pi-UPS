#!/usr/bin/env python3
"""Host regression for the actual modem.c PPP callbacks/lifecycle helpers.

The sealed 0.8.13 callbacks are retained verbatim as a failing regression
fixture. The fixed side is extracted from the current production source.
SDK event payloads, event-group scheduling and time are simulated; no device,
UART or network is accessed. The 120-second timer scenario uses virtual time.
MQTT_TEST_SANITIZERS defaults to address,undefined; there is no silent fallback.
"""
import argparse
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
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    source = (firmware / "modem.c").read_text()
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_ppp_events sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    print("modem.c SHA256=" + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-ppp-events-") as directory:
        temporary = Path(directory)
        common = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-pthread", "-I", str(temporary), "-I", str(root / "tools"),
            str(root / "tools/test_modem_ppp_events.c")]
        if sanitizers:
            common += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        baseline = temporary / "baseline"
        subprocess.run(common + ["-DPPP_LEGACY_BASELINE=1", "-o", str(baseline)],
                       check=True, timeout=30)
        result = subprocess.run([str(baseline)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="", flush=True)
        if result.returncode != 1 or "BASELINE_REPRODUCED" not in result.stdout:
            raise SystemExit("Expected old callback regression failure; got " +
                             str(result.returncode) + "\n" + result.stderr)
        if args.baseline_only:
            return
        bringup = function(source, "static esp_err_t ppp_bringup_dce(")
        teardown = function(source, "static void ppp_teardown_dce(")
        supervisor = function(source, "static void ppp_supervisor_task(")
        supervision = function(source, "static teardown_action_t supervise_uplink(")
        # Static wiring checks complement the actual runtime helper tests.
        # They explicitly do not claim to emulate the full modem/UART stack.
        if not (bringup.count("ppp_events_start_dial();") == 1 and
                bringup.index("modem_support_snapshot(false);") <
                bringup.index("ppp_events_start_dial();") <
                bringup.index("esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX)") <
                bringup.index("esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA)")):
            raise SystemExit("Arm the PPP observation window before both dial paths, after diagnostics")
        for call in ("esp_modem_at(", "esp_modem_set_mode(", "esp_modem_destroy(",
                     "modem_power_cycle("):
            if teardown.index("ppp_events_begin_stop();") > teardown.index(call):
                raise SystemExit("Expected stop must precede teardown action: " + call)
        if not (supervisor.index("ppp_events_begin_attempt();") <
                supervisor.index("ppp_bringup_dce();") <
                supervisor.index("ppp_events_wait_for_link(") <
                supervisor.index("if (phase == PPP_UP)")):
            raise SystemExit("Supervisor must consume the ordered lifecycle state")
        if "s_ppp_up" in source or re.search(r'bits\s*&\s*\(?\s*EVT_(?:GOT|LOST|PPP)',
                                              supervisor + supervision):
            raise SystemExit("PPP decisions must not use the old unordered event bits/flag")
        if "xEventGroupClearBits" in supervisor + supervision:
            raise SystemExit("No delayed post-wait clear may discard newer notifications")
        if supervision.count("if (ppp_events_take_loss())") != 4:
            raise SystemExit("Recheck current PPP at wake and after diagnostics/probes/recovery snapshot")
        for call in ('modem_network_snapshot("PPP up");', "wait_for_time_sync(15000);",
                     "run_http_get_test();"):
            tail = supervisor[supervisor.index(call) + len(call):]
            if not re.match(r'\s*\}?\s*if \(ppp_events_take_loss\(\)\) goto stop_ppp;', tail):
                raise SystemExit("PPP loss must stop startup after blocking operation: " + call)
        print("static PPP wiring PASS: CMUX/DATA arm, stop-before-SDK, ordered supervisor, post-diagnostic guards", flush=True)
        definitions = re.search(r'/\* PPP lifecycle state:.*?/\* END PPP lifecycle state \*/',
                                source, re.S)
        if definitions is None:
            raise SystemExit("Cannot find actual PPP lifecycle state declarations")
        signatures = (
            "static ppp_event_state_t ppp_events_snapshot(",
            "bool modem_ppp_is_up(",
            "uint32_t modem_ppp_generation(",
            "static void ppp_events_begin_attempt(",
            "static void ppp_events_start_dial(",
            "static void ppp_events_begin_stop(",
            "static bool ppp_events_take_loss(",
            "static bool ppp_events_observe(",
            "static ppp_phase_t ppp_events_wait_for_link(",
            "static void on_ip_event(",
            "static void on_netif_ppp_status(",
        )
        extracted = definitions[0] + "\n" + "\n".join(
            function(source, signature) for signature in signatures)
        (temporary / "modem_ppp_events.inc").write_text(extracted)
        fixed = temporary / "fixed"
        subprocess.run(common + ["-o", str(fixed)], check=True, timeout=30)
        subprocess.run([str(fixed)], check=True, timeout=30)


if __name__ == "__main__":
    main()
