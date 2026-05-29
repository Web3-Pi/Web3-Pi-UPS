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
        "power": {"charge_state": 1, "vbus_in_mv": 5012, "vbus_out_mv": 5050,
                  "ibus_out_ma": 1840, "vbat_mv": 7920, "ibat_ma": -200,
                  "temp_dc": 253, "faults": 0},
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
            has_cmds = bool(json.loads(resp_body or b"{}").get("commands"))
            if resp_sig and hmac.compare_digest(want, resp_sig.lower()):
                print("response signature: OK")
            elif has_cmds:
                print("response signature: MISSING/INVALID — firmware would ignore commands")
                sys.exit(2)
            else:
                print("response signature: (none; no commands to verify)")
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()}")
        sys.exit(1)


if __name__ == "__main__":
    main()
