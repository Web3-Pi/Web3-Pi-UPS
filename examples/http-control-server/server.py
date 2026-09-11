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
    python3 server.py --secret "<HTTP-key from OLED>" [--device-id <id>] [--port 8080] \
                      [--max-commands 8] [--cmd-max-age 600]

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
import sys
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

# Device-side receiver limits — mirror the "Receiver limits" block at the top of
# ../../firmware-ESP32-LTE-M/main/http_backend.c (esp32:0.8.10+):
#   - the device applies at most 8 commands per response (CMDS_PER_RESP_MAX);
#     anything beyond is left un-acked and simply re-sent on the next poll,
MAX_COMMANDS_PER_RESPONSE = 8
#   - a 2xx response body above 2047 B (HTTP_RESP_MAX - 1) is dropped WHOLE by
#     the device (no commands applied) and reported back as "resp_dropped",
DEVICE_RESPONSE_MAX_BYTES = 2047
#   - a command id longer than 47 chars (ACK_ID_MAX - 1) is rejected as bad_id
#     and reported back cut to its first 47 chars (our "c-NNNN" ids are tiny).
DEVICE_ID_MAX_CHARS = 47
# Commands not delivered within this many seconds are dropped un-delivered: a
# shutdown that fires an hour late (device was offline) is worse than none.
# 0 = keep forever. Override with --cmd-max-age.
COMMAND_MAX_AGE_S = 600


class State:
    """Shared server state, guarded by a lock."""

    def __init__(self, secret: bytes, device_id: str,
                 max_commands: int = MAX_COMMANDS_PER_RESPONSE,
                 cmd_max_age: float = COMMAND_MAX_AGE_S):
        self.secret = secret
        self.device_id = device_id
        self.max_commands = max_commands    # per response; 0 = unlimited (see take_pending)
        self.cmd_max_age = cmd_max_age      # seconds; 0 = never expire
        self.lock = threading.Lock()
        self.commands = []          # queued, not yet acked:
                                    #   [{id, cmd, args, queued_at, sent, last_sent_at}]
        self.seen_nonces = []       # recent nonces (replay window)
        self.prev_net = {}          # device_id -> (bytes_tx, bytes_rx) of the last net.status sample
        self._next_id = 1

    def enqueue(self, cmd: str, args: dict):
        with self.lock:
            cid = f"c-{self._next_id:04d}"
            self._next_id += 1
            self.commands.append({"id": cid, "cmd": cmd, "args": args,
                                  "queued_at": time.time(),
                                  "sent": 0, "last_sent_at": None})
        return cid

    def snapshot(self):
        """Queued commands incl. server-side bookkeeping (for 'list')."""
        with self.lock:
            return [dict(c) for c in self.commands]

    @staticmethod
    def wire(c: dict) -> dict:
        """The on-wire shape of a queued command (no server-side fields)."""
        return {"id": c["id"], "cmd": c["cmd"], "args": c["args"]}

    def take_pending(self, limit: int, max_bytes: int):
        """Commands to hand back in this response, oldest first.

        1. Expire commands queued longer than cmd_max_age ago (0 = never),
           delivered-but-unacked ones included (their ack may still be one
           poll behind; the device never re-executes an id it has run).
        2. Keep the OLDEST `limit`: the device applies at most that many per
           response and leaves the rest un-acked, so sending more only burns
           LTE bytes. limit == 0 = unlimited AND unbounded in size — only for
           provoking the device's overflow path on purpose.
        3. Trim from the end until the serialised {"commands": [...]} fits
           max_bytes (the device drops a larger body whole).
        Every command handed out is stamped (sent count, last_sent_at)."""
        now = time.time()
        with self.lock:
            if self.cmd_max_age > 0:
                keep = []
                for c in self.commands:
                    age = now - c["queued_at"]
                    if age > self.cmd_max_age:
                        how = (f"delivered {c['sent']}x, never acked" if c["sent"]
                               else "never delivered")
                        print(f"    ! dropping {c['id']} ({c['cmd']}): queued {age:.0f}s ago "
                              f"> --cmd-max-age {self.cmd_max_age:.0f}s, {how}")
                    else:
                        keep.append(c)
                self.commands = keep
            chosen = list(self.commands) if limit <= 0 else self.commands[:limit]
            if limit > 0:
                # Must serialise exactly like Handler._send_json (json.dumps default).
                while chosen and len(json.dumps(
                        {"commands": [self.wire(c) for c in chosen]}).encode()) > max_bytes:
                    chosen.pop()
                held = len(self.commands) - len(chosen)
                if held:
                    print(f"    ! {held} command(s) held back for a later poll "
                          f"(sending <= {limit} commands / {max_bytes} B per response; "
                          f"the device applies <= {MAX_COMMANDS_PER_RESPONSE})")
            for c in chosen:
                c["sent"] += 1
                c["last_sent_at"] = now
            return [self.wire(c) for c in chosen]

    def apply_acks(self, acked_ids, rejected=None):
        """Clear acked commands. Also drop the ones the device REJECTED — it
        will never ack those, so leaving them queued would re-send them on
        every poll forever. Returns the number of acked commands cleared."""
        acked_ids = acked_ids or []
        rejected = rejected or []
        with self.lock:
            before = len(self.commands)
            self.commands = [c for c in self.commands if c["id"] not in acked_ids]
            cleared = before - len(self.commands)
            for r in rejected:
                if not isinstance(r, dict) or not isinstance(r.get("id"), str):
                    continue
                rid, reason = r["id"], r.get("reason")
                # A bad_id report carries only the first 47 chars of an over-long
                # id, so match on prefix in that one case (and only that one:
                # a 47-char id rejected for another reason is exact).
                hits = [c for c in self.commands
                        if c["id"] == rid or
                        (reason == "bad_id" and len(rid) == DEVICE_ID_MAX_CHARS
                         and c["id"].startswith(rid))]
                for c in hits:
                    self.commands.remove(c)
                    print(f"    ! device rejected {c['id']} ({reason}) — dropped from queue")
                if not hits:
                    print(f"    ! device rejected {rid!r} ({reason}) — not in queue")
        return cleared

    def check_nonce(self, nonce: str) -> bool:
        """True if nonce is fresh; records it. False if replayed."""
        with self.lock:
            if nonce in self.seen_nonces:
                return False
            self.seen_nonces.append(nonce)
            if len(self.seen_nonces) > NONCE_CACHE_MAX:
                self.seen_nonces.pop(0)
            return True

    def net_delta(self, device_id: str, tx: int, rx: int):
        """Remember this device's cumulative (tx, rx) modem counters and return
        the delta since the previously remembered sample. None when there is no
        previous sample, when the sample is unchanged (the device re-sent a
        cached net.status snapshot), or when a counter went backwards (device
        rebooted, counters reset)."""
        with self.lock:
            prev = self.prev_net.get(device_id)
            self.prev_net[device_id] = (tx, rx)
        if prev is None or prev == (tx, rx):
            return None
        if tx < prev[0] or rx < prev[1]:
            return None
        return tx - prev[0], rx - prev[1]


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
    if p and p.get("version") == 2:
        # power.status v2 (since esp32:0.8.9): 21 keys, dispatched on "version".
        # temp_mp_dc is JSON null while the MP2762A charger is unpowered.
        temp_mp = p.get("temp_mp_dc")
        temp_mp_s = "n/a" if temp_mp is None else f"{temp_mp}dC"
        flags = p.get("flags")
        flags_s = f"0x{flags:02x}" if isinstance(flags, int) else str(flags)
        faults = p.get("faults")
        faults_s = f"0x{faults:04x}" if isinstance(faults, int) else str(faults)
        print(f"    power : v{p.get('version')} cs={p.get('charge_state')} flags={flags_s} "
              f"vin={p.get('vbus_in_mv')}mV pd_in={p.get('pd_in_mv')}mV/{p.get('pd_in_ma')}mA")
        print(f"            vout={p.get('vbus_out_mv')}mV set={p.get('vout_set_mv')}mV "
              f"read={p.get('vout_read_mv')}mV iout_limit={p.get('iout_limit_ma')}mA "
              f"pd_out={p.get('pd_out_mv')}mV/{p.get('pd_out_ma')}mA")
        print(f"            vbat={p.get('vbat_mv')}mV ibat={p.get('ibat_ma')}mA "
              f"vsys={p.get('vsys_mv')}mV iin={p.get('iin_ma')}mA")
        print(f"            temp_lm={p.get('temp_lm_dc')}dC temp_mp={temp_mp_s} "
              f"temp={p.get('temp_dc')}dC faults={faults_s} uptime={p.get('uptime_s')}s")
    elif p and p.get("version") is not None:
        # A power.status version this reference server does not know yet —
        # dump it raw rather than printing the legacy keys as None.
        print(f"    power : v{p.get('version')} (unknown shape) {p}")
    elif p:
        # Legacy v1 shape (no "version" key): units whose CH32X still emits
        # power.status v1, or firmware older than esp32:0.8.9.
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
        # On-wire cost readout: delta of the modem's cumulative counters
        # between two DISTINCT net.status samples. The device refreshes
        # net.status every ~60 s but POSTs every ~30 s, re-sending the cached
        # snapshot in between — an unchanged sample is skipped rather than
        # printed as a misleading "0 B". Also skipped when there is no previous
        # sample or a counter went backwards (device rebooted, counters reset).
        tx, rx = n.get("bytes_tx"), n.get("bytes_rx")
        if isinstance(tx, int) and isinstance(rx, int):
            delta = STATE.net_delta(device_id, tx, rx)
            if delta is not None:
                print(f"            Δtx={delta[0]} Δrx={delta[1]} B "
                      f"since previous net.status sample")
    acks = tlm.get("acks") or []
    if acks:
        print(f"    acks  : {acks}")
    rejected = tlm.get("rejected") or []
    if rejected:
        # {id, reason} with reason unsupported_cmd | bad_args | bad_id — never acked.
        print("    reject: " + ", ".join(
            f"{r.get('id')!r} ({r.get('reason')})" for r in rejected if isinstance(r, dict)))
    rd = tlm.get("resp_dropped")
    if isinstance(rd, dict):
        print(f"    resp_dropped: our previous response was {rd.get('bytes')} B > "
              f"device cap {rd.get('max')} B — dropped whole, nothing applied")


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

        # Acks first (clear delivered commands, drop rejected ones), then
        # print, then hand back whatever is still queued.
        cleared = STATE.apply_acks(tlm.get("acks") or [], tlm.get("rejected") or [])
        print_telemetry(STATE.device_id, tlm)
        if cleared:
            print(f"    ({cleared} command(s) acked and cleared)")
        rd = tlm.get("resp_dropped")
        if isinstance(rd, dict):
            print(f"  !!! the device DROPPED our previous response: {rd.get('bytes')} B "
                  f"exceeds its {rd.get('max')} B cap — none of those commands were "
                  f"applied. Keep --max-commands <= {MAX_COMMANDS_PER_RESPONSE} and "
                  f"msg texts short.")

        pending = STATE.take_pending(STATE.max_commands, DEVICE_RESPONSE_MAX_BYTES)
        if pending:
            print(f"    -> returning {len(pending)} queued command(s): "
                  f"{[c['id'] for c in pending]}")
        # Sign the response (bound to this request's nonce) so the device will
        # accept the commands. Required whenever commands are present.
        self._send_json(200, {"commands": pending}, sign_nonce=nonce)


HELP = """\
commands you can enqueue (typed here, delivered on the device's next poll):
    beep [freq_hz] [dur_ms]      ui.beep        (default 1500 Hz / 150 ms)
    msg <text...>                ui.display_msg (OLED; max 64 chars, 40 visible)
    shutdown [delay_s]           host.shutdown  (RPi, default 5 s)
    reset [delay_s]              host.reset     (RPi, default 5 s)
    powercycle [off_ms]          power.cycle    (CH32X, default 1500 ms)
    list                         show queued (un-acked) commands and their age
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
                now = time.time()
                for cmd in STATE.snapshot():
                    sent = f", sent {cmd['sent']}x, awaiting ack" if cmd["sent"] else ""
                    print(f"    {cmd['id']}: {cmd['cmd']} {cmd['args']}  "
                          f"(queued {now - cmd['queued_at']:.0f}s ago{sent})")
            elif c == "beep":
                args = {}
                if len(parts) > 1:
                    args["freq_hz"] = int(parts[1])
                if len(parts) > 2:
                    args["dur_ms"] = int(parts[2])
                print("    queued", STATE.enqueue("ui.beep", args))
            elif c == "msg":
                text = line[len("msg"):].strip()
                if len(text) > 64:
                    # The device caps ui.display_msg text at 64 chars.
                    print(f"    (text cut to 64 chars — dropped: {text[64:]!r})")
                    text = text[:64]
                if len(text) > 40:
                    print("    (note: the OLED shows the first 40 chars)")
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
    # Telemetry lines contain a non-ASCII glyph (Δ). A console that cannot
    # encode it must not abort do_POST mid-request (the device would get no
    # 2xx and never receive its commands) — degrade the glyph instead.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description="Web3 Pi UPS HTTP control-mode reference server")
    ap.add_argument("--secret", required=True,
                    help="device HTTP key (the code shown on the OLED; "
                         "spaces/dashes/case ignored)")
    ap.add_argument("--device-id", default=None,
                    help="device_id in the URL path (default: derived from --secret prompt)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--max-commands", type=int, default=MAX_COMMANDS_PER_RESPONSE,
                    help=f"commands per response (default {MAX_COMMANDS_PER_RESPONSE} = "
                         "the device's cap; 0 = unlimited and unbounded in size, "
                         "only to provoke the device's overflow path)")
    ap.add_argument("--cmd-max-age", type=int, default=COMMAND_MAX_AGE_S,
                    help="drop commands not delivered within N seconds "
                         f"(default {COMMAND_MAX_AGE_S}; 0 = never expire)")
    args = ap.parse_args()
    if args.max_commands < 0 or args.cmd_max_age < 0:
        ap.error("--max-commands and --cmd-max-age must be >= 0")
    if args.max_commands > MAX_COMMANDS_PER_RESPONSE:
        print(f"warning: --max-commands {args.max_commands} exceeds the device's cap of "
              f"{MAX_COMMANDS_PER_RESPONSE}: it applies the first {MAX_COMMANDS_PER_RESPONSE} "
              "and leaves the rest un-acked, so every poll re-sends them (wasted LTE bytes)",
              file=sys.stderr)

    secret = normalize_key(args.secret)
    if len(secret) < 8:
        ap.error("--secret looks too short — paste the full code shown on the OLED")

    device_id = args.device_id
    if not device_id:
        device_id = input("device_id (ICCID or override): ").strip()
    if not device_id:
        ap.error("a device_id is required")

    STATE = State(secret, device_id, args.max_commands, args.cmd_max_age)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"listening on http://{args.host}:{args.port}")
    print(f"endpoint    POST /api/v1/devices/{device_id}/telemetry")
    print(f"device_id   {device_id}")
    lim = f"<= {args.max_commands} commands / {DEVICE_RESPONSE_MAX_BYTES} B" \
        if args.max_commands else "UNLIMITED (device overflow test mode)"
    age = f"{args.cmd_max_age} s" if args.cmd_max_age else "never"
    print(f"limits      {lim} per response; undelivered commands expire after {age}")
    print("type 'help' for the command menu; Ctrl-C / Ctrl-D to quit\n")

    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    try:
        repl()
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    main()
