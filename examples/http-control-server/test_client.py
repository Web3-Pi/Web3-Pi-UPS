#!/usr/bin/env python3
"""
Simulate a device telemetry POST against the reference server (HTTP-2).

Useful to smoke-test the server without hardware: it builds the exact signed
request the firmware sends (ts || nonce || body, HMAC-SHA256), so a 200 here
means the firmware will authenticate too. Also handy to confirm a public TLS
deployment before pointing a real device at it.

    python3 test_client.py --secret "<HTTP-key>" --device-id TESTDEV \\
        --url http://127.0.0.1:8080 [--acks c-0001 c-0002]
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import sys
import time
import urllib.request

# Device receiver limits — mirror the "Receiver limits" block in
# ../../firmware-ESP32-LTE-M/main/http_backend.c. A response above the byte
# cap is dropped whole by the device, commands past the count cap are left
# un-acked, and an over-long id is rejected as bad_id.
DEVICE_RESPONSE_MAX_BYTES = 2047
DEVICE_MAX_COMMANDS_PER_RESPONSE = 8
DEVICE_ID_MAX_CHARS = 47


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secret", required=True, help="device HTTP key (OLED code)")
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--url", required=True, help="base URL, no trailing slash")
    ap.add_argument("--acks", nargs="*", default=[], help="command ids to ack")
    args = ap.parse_args()

    # Match the firmware key: OLED code, uppercased, separators stripped.
    secret = re.sub(r"[^0-9A-Za-z]", "", args.secret).upper().encode("ascii")
    body = json.dumps({
        "ts": int(time.time()),
        "fw_ver": "esp32:test",
        "uptime_s": 42,
        # power.status v2 shape as emitted since esp32:0.8.9 (21 keys). A unit
        # whose CH32X still speaks v1 sends the legacy 8-key object (no
        # "version") instead. temp_mp_dc is null while the charger is unpowered.
        "power": {"version": 2, "flags": 0x1F, "charge_state": 3,
                  "vbus_in_mv": 14709, "pd_in_mv": 15000, "pd_in_ma": 1750,
                  "vbus_out_mv": 5051, "vout_set_mv": 5000, "vout_read_mv": 5046,
                  "iout_limit_ma": 5010, "pd_out_mv": 5000, "pd_out_ma": 3000,
                  "vbat_mv": 7897, "ibat_ma": 0, "vsys_mv": 7900, "iin_ma": 1200,
                  "temp_lm_dc": 350, "temp_mp_dc": 440, "temp_dc": 440,
                  "faults": 0, "uptime_s": 86272},
        "acks": args.acks,
    }).encode()

    ts = str(int(time.time()))
    nonce = os.urandom(8).hex()
    sig = hmac.new(secret, ts.encode() + nonce.encode() + body,
                   hashlib.sha256).hexdigest()

    url = f"{args.url}/api/v1/devices/{args.device_id}/telemetry"
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-W3PUPS-Device", args.device_id)
    req.add_header("X-W3PUPS-Ts", ts)
    req.add_header("X-W3PUPS-Nonce", nonce)
    req.add_header("X-W3PUPS-Sig", sig)

    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            resp_body = r.read()
            resp_sig = r.headers.get("X-W3PUPS-Sig", "")
            print(f"HTTP {r.status}")
            print(resp_body.decode())
            # Verify the response signature exactly as the firmware does:
            # HMAC(secret, request_nonce || response_body). Commands are only
            # trustworthy if this matches.
            want = hmac.new(secret, nonce.encode() + resp_body,
                            hashlib.sha256).hexdigest()
            cmds = json.loads(resp_body or b"{}").get("commands") or []
            if resp_sig and hmac.compare_digest(want, resp_sig.lower()):
                print("response signature: OK")
            elif cmds:
                print("response signature: MISSING/INVALID — firmware would ignore commands")
                sys.exit(2)
            else:
                print("response signature: (none; no commands to verify)")
            # Would the real device accept this response? Check its limits.
            if len(resp_body) > DEVICE_RESPONSE_MAX_BYTES:
                print(f"WARNING: response body is {len(resp_body)} B > "
                      f"{DEVICE_RESPONSE_MAX_BYTES} B — the device drops it whole "
                      f"(no commands applied) and reports resp_dropped", file=sys.stderr)
            if len(cmds) > DEVICE_MAX_COMMANDS_PER_RESPONSE:
                print(f"WARNING: {len(cmds)} commands > {DEVICE_MAX_COMMANDS_PER_RESPONSE} "
                      f"— the device applies only the first "
                      f"{DEVICE_MAX_COMMANDS_PER_RESPONSE}; the rest stay un-acked",
                      file=sys.stderr)
            long_ids = [c["id"] for c in cmds if isinstance(c, dict)
                        and isinstance(c.get("id"), str) and len(c["id"]) > DEVICE_ID_MAX_CHARS]
            if long_ids:
                print(f"WARNING: {len(long_ids)} command id(s) longer than "
                      f"{DEVICE_ID_MAX_CHARS} chars — the device rejects them as bad_id: "
                      f"{long_ids}", file=sys.stderr)
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()}")
        sys.exit(1)


if __name__ == "__main__":
    main()
