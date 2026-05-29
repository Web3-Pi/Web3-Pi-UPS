#!/usr/bin/env python3
"""
Web3 Pi UPS — HTTP control-mode reference server (HTTP-2 / plan §4.18a).

A tiny, dependency-free reference for self-hosters who want to drive a UPS
from their own infrastructure (no EMQX, no Arkiv). It:

  - accepts the device's periodic signed telemetry POST,
  - verifies the HMAC-SHA256 signature (per-device secret),
  - prints the decoded telemetry,
  - lets you enqueue a command from the terminal (typed at the prompt),
  - returns queued commands in the POST response and clears them once the
    device acks them on a later POST.

This is intentionally minimal and single-device. It is NOT production infra:
nonce replay state is in-memory, there is no persistence, and one secret is
configured at startup. The protocol it speaks is frozen by the device side —
see HTTP-1-design-note.md and ../../firmware-ESP32-LTE-M/main/http_backend.c.

Endpoint:  POST /api/v1/devices/{device_id}/telemetry
Auth:      HMAC-SHA256( secret, ts || nonce || raw_body ) in X-W3PUPS-Sig
           (hex), with X-W3PUPS-Ts (unix seconds) + X-W3PUPS-Nonce (hex).
TLS:       terminate it in front (nginx/Caddy/Traefik) for a public deploy.
           This stub speaks plain HTTP — see the README for a TLS recipe.

Usage:
    python3 server.py --secret "<HTTP-key from OLED>" [--device-id <id>] [--port 8080]

The secret is the device's **HTTP key** — the short code shown on the device's
OLED (menu → "HTTP Key"). It is separate from the MQTT/Arkiv secret, generated
on the device and re-rollable from the same menu. Type it here exactly as
shown; spaces/dashes and case don't matter (it is normalised).
"""

import argparse
import hashlib
import hmac
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def normalize_key(code: str) -> bytes:
    """Match the firmware's HMAC key: the OLED code, uppercased, with any
    separators (spaces/dashes) stripped, as ASCII bytes."""
    return re.sub(r"[^0-9A-Za-z]", "", code).upper().encode("ascii")

# How far the device clock may drift from ours before we reject (seconds).
TS_SKEW_TOLERANCE_S = 300
# Remember this many recent nonces for replay rejection.
NONCE_CACHE_MAX = 512


class State:
    """Shared server state, guarded by a lock."""

    def __init__(self, secret: bytes, device_id: str):
        self.secret = secret
        self.device_id = device_id
        self.lock = threading.Lock()
        self.commands = []          # queued, not yet acked: [{id, cmd, args}]
        self.seen_nonces = []       # recent nonces (replay window)
        self._next_id = 1

    def enqueue(self, cmd: str, args: dict):
        with self.lock:
            cid = f"c-{self._next_id:04d}"
            self._next_id += 1
            self.commands.append({"id": cid, "cmd": cmd, "args": args})
        return cid

    def take_pending(self):
        with self.lock:
            return list(self.commands)

    def apply_acks(self, acked_ids):
        if not acked_ids:
            return
        with self.lock:
            before = len(self.commands)
            self.commands = [c for c in self.commands if c["id"] not in acked_ids]
            return before - len(self.commands)

    def check_nonce(self, nonce: str) -> bool:
        """True if nonce is fresh; records it. False if replayed."""
        with self.lock:
            if nonce in self.seen_nonces:
                return False
            self.seen_nonces.append(nonce)
            if len(self.seen_nonces) > NONCE_CACHE_MAX:
                self.seen_nonces.pop(0)
            return True


STATE: State = None  # set in main()


def verify_signature(ts: str, nonce: str, body: bytes, sig_hex: str) -> bool:
    mac = hmac.new(STATE.secret,
                   ts.encode() + nonce.encode() + body,
                   hashlib.sha256).hexdigest()
    # Constant-time compare to avoid leaking the signature byte-by-byte.
    return hmac.compare_digest(mac, (sig_hex or "").lower())


def print_telemetry(device_id: str, tlm: dict):
    ts = tlm.get("ts")
    when = time.strftime("%H:%M:%S", time.localtime(ts)) if ts else "??:??:??"
    print(f"\n[{when}] telemetry from {device_id} "
          f"(fw={tlm.get('fw_ver','?')}, uptime={tlm.get('uptime_s','?')}s)")
    p = tlm.get("power")
    if p:
        print(f"    power : vin={p.get('vbus_in_mv')}mV vout={p.get('vbus_out_mv')}mV "
              f"iout={p.get('ibus_out_ma')}mA vbat={p.get('vbat_mv')}mV "
              f"ibat={p.get('ibat_ma')}mA temp={p.get('temp_dc')}dC "
              f"cs={p.get('charge_state')} faults={p.get('faults')}")
    h = tlm.get("host")
    if h:
        print(f"    host  : eth={h.get('eth_state')} cpu={h.get('cpu_temp_dc')}dC "
              f"mem={h.get('mem_pct')}% disk={h.get('disk_pct')}% "
              f"load={h.get('load_x100')} uptime={h.get('uptime_s')}s")
    n = tlm.get("net")
    if n:
        print(f"    net   : state={n.get('state')} rssi={n.get('rssi_dbm')}dBm "
              f"tx={n.get('bytes_tx')} rx={n.get('bytes_rx')}")
    acks = tlm.get("acks") or []
    if acks:
        print(f"    acks  : {acks}")


class Handler(BaseHTTPRequestHandler):
    # Quieter default logging — we print our own lines.
    def log_message(self, fmt, *args):
        pass

    def _send_json(self, code: int, obj: dict, sign_nonce: str = None):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        # Sign the response so the device can authenticate commands regardless
        # of transport (TLS optional). Bind to the request nonce to stop replay
        # of an old signed response. The device verifies this before executing
        # any command (an unsigned/invalid response → commands ignored).
        if sign_nonce is not None:
            sig = hmac.new(STATE.secret, sign_nonce.encode() + body,
                           hashlib.sha256).hexdigest()
            self.send_header("X-W3PUPS-Sig", sig)
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        expected = f"/api/v1/devices/{STATE.device_id}/telemetry"
        if self.path != expected:
            self._send_json(404, {"error": "not_found"})
            return

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""

        ts = self.headers.get("X-W3PUPS-Ts", "")
        nonce = self.headers.get("X-W3PUPS-Nonce", "")
        sig = self.headers.get("X-W3PUPS-Sig", "")

        if not verify_signature(ts, nonce, body, sig):
            print("  ! rejected: bad signature")
            self._send_json(401, {"error": "bad_signature"})
            return

        try:
            skew = abs(int(time.time()) - int(ts))
        except ValueError:
            self._send_json(401, {"error": "bad_timestamp"})
            return
        if skew > TS_SKEW_TOLERANCE_S:
            print(f"  ! rejected: timestamp skew {skew}s > {TS_SKEW_TOLERANCE_S}s")
            self._send_json(401, {"error": "stale_timestamp"})
            return

        if not STATE.check_nonce(nonce):
            print("  ! rejected: replayed nonce")
            self._send_json(401, {"error": "replay"})
            return

        try:
            tlm = json.loads(body)
        except json.JSONDecodeError:
            self._send_json(400, {"error": "bad_json"})
            return

        # Acks first (clear delivered commands), then print, then hand back
        # whatever is still queued.
        cleared = STATE.apply_acks(tlm.get("acks") or [])
        print_telemetry(STATE.device_id, tlm)
        if cleared:
            print(f"    ({cleared} command(s) acked and cleared)")

        pending = STATE.take_pending()
        if pending:
            print(f"    -> returning {len(pending)} queued command(s): "
                  f"{[c['id'] for c in pending]}")
        # Sign the response (bound to this request's nonce) so the device will
        # accept the commands. Required whenever commands are present.
        self._send_json(200, {"commands": pending}, sign_nonce=nonce)


HELP = """\
commands you can enqueue (typed here, delivered on the device's next poll):
    beep [freq_hz] [dur_ms]      ui.beep        (default 1500 Hz / 150 ms)
    msg <text...>                ui.display_msg (shown on the OLED)
    shutdown [delay_s]           host.shutdown  (RPi, default 5 s)
    reset [delay_s]              host.reset     (RPi, default 5 s)
    powercycle [off_ms]          power.cycle    (CH32X, default 1500 ms)
    list                         show queued (un-acked) commands
    help                         this text
"""


def repl():
    print(HELP, end="")
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        parts = line.split()
        c = parts[0].lower()
        try:
            if c == "help":
                print(HELP, end="")
            elif c == "list":
                for cmd in STATE.take_pending():
                    print(f"    {cmd['id']}: {cmd['cmd']} {cmd['args']}")
            elif c == "beep":
                args = {}
                if len(parts) > 1:
                    args["freq_hz"] = int(parts[1])
                if len(parts) > 2:
                    args["dur_ms"] = int(parts[2])
                print("    queued", STATE.enqueue("ui.beep", args))
            elif c == "msg":
                text = line[len("msg"):].strip()
                print("    queued", STATE.enqueue("ui.display_msg", {"text": text}))
            elif c == "shutdown":
                args = {"delay_s": int(parts[1])} if len(parts) > 1 else {}
                print("    queued", STATE.enqueue("host.shutdown", args))
            elif c == "reset":
                args = {"delay_s": int(parts[1])} if len(parts) > 1 else {}
                print("    queued", STATE.enqueue("host.reset", args))
            elif c == "powercycle":
                args = {"off_ms": int(parts[1])} if len(parts) > 1 else {}
                print("    queued", STATE.enqueue("power.cycle", args))
            else:
                print(f"    unknown command '{c}' — try 'help'")
        except ValueError:
            print("    bad argument (expected a number)")


def main():
    global STATE
    ap = argparse.ArgumentParser(description="Web3 Pi UPS HTTP control-mode reference server")
    ap.add_argument("--secret", required=True,
                    help="device HTTP key (the code shown on the OLED; "
                         "spaces/dashes/case ignored)")
    ap.add_argument("--device-id", default=None,
                    help="device_id in the URL path (default: derived from --secret prompt)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    secret = normalize_key(args.secret)
    if len(secret) < 8:
        ap.error("--secret looks too short — paste the full code shown on the OLED")

    device_id = args.device_id
    if not device_id:
        device_id = input("device_id (ICCID or override): ").strip()
    if not device_id:
        ap.error("a device_id is required")

    STATE = State(secret, device_id)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"listening on http://{args.host}:{args.port}")
    print(f"endpoint    POST /api/v1/devices/{device_id}/telemetry")
    print(f"device_id   {device_id}")
    print("type 'help' for the command menu; Ctrl-C / Ctrl-D to quit\n")

    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    try:
        repl()
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    main()
