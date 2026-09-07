#!/usr/bin/env python3
"""
wups-mqtt-sniff — subscribe to one device's uplink topics on the broker and
decode every WUPS frame (header + known payloads) in human-readable form.

Zero dependencies (ssl + socket, MQTT 3.1.1 subset). Use a DASHBOARD /
operator MQTT account — NEVER the device's own ICCID credentials (the
device's client-id == ICCID; connecting with it kicks the live device off).

    MQTT_USER=... MQTT_PASS=... python3 tools/wups-mqtt-sniff.py 8988228066680569990
    python3 tools/wups-mqtt-sniff.py --selftest        # offline decoder tests
    python3 tools/wups-mqtt-sniff.py --selftest-net    # MQTT client round-trip (test.mosquitto.org)

What to look for on a unit whose UPS tiles are blank:
  * only NET/STATUS frames (src=ESP32) on `telemetry`  -> nothing relayed from RP2040
  * net.status v2 tail: sys_frames_rx frozen + sys_link_age_s growing  -> ESP32 not
    deframing anything from the RP2040 (RX dead / wups_rx task stuck)
  * sys_frames_rx creeping ~1/min + age sawtooth 0..60 s  -> only ping/pong gets
    through (RP2040 alive, its net.publish frames never arrive/decode)
  * sys_resync climbing fast  -> garbage on the inter-MCU UART
"""
import argparse
import os
import random
import select
import socket
import ssl
import struct
import sys
import time
from urllib.parse import urlparse

# --- WUPS constants (common/protocol.h) -------------------------------------
SYNC1, SYNC2, END1, END2 = 0xAA, 0x55, 0x55, 0xAA
HEADER = 10
ADDR = {0x00: "NULL", 0x01: "RPI", 0x02: "RP2040", 0x03: "CH32X", 0x04: "ESP32",
        0x05: "INTERNAL", 0xFF: "BCAST"}
CLASS = {0x01: "SYSTEM", 0x02: "POWER", 0x03: "NET", 0x04: "HOST", 0x05: "UI"}
OPS = {
    0x01: {0x01: "ping", 0x02: "hello", 0x03: "status_query", 0x04: "log",
           0x05: "reset", 0x10: "mode_changed"},
    0x02: {0x01: "status", 0x02: "enable", 0x03: "disable", 0x04: "cycle",
           0x05: "reset", 0x10: "event"},
    0x03: {0x01: "status", 0x02: "publish", 0x10: "downlink", 0x20: "time_sync",
           0x21: "config", 0x22: "fw_update", 0x23: "fw_xfer_begin",
           0x24: "fw_xfer_data", 0x25: "fw_xfer_end"},
    0x04: {0x01: "status", 0x02: "shutdown", 0x03: "reset", 0x04: "svc_restart",
           0x05: "svc_start", 0x06: "svc_stop", 0x10: "event"},
    0x05: {0x01: "button_event", 0x02: "set_screen", 0x03: "beep",
           0x04: "display_msg", 0x05: "trust_prompt", 0x06: "trust_result",
           0x07: "local_reset"},
}
NET_STATE = {0: "off", 1: "init", 2: "net_attach", 3: "ppp_up", 4: "mqtt_up", 5: "err"}
CHG = {0: "idle", 1: "charging", 2: "charged", 3: "fault"}


def fletcher8(data: bytes):
    a = b = 0
    for x in data:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def flags_str(f):
    s = [n for bit, n in ((1, "REQ"), (2, "RESP"), (4, "EVENT"), (0x80, "NEED_ACK")) if f & bit]
    return "|".join(s) or "0"


def decode_frame(buf: bytes):
    """Return (dict, err). dict has dst,src,cls,op,flags,seq,payload,ck_ok."""
    if len(buf) < HEADER + 4:
        return None, "too short (%d B)" % len(buf)
    if buf[0] != SYNC1 or buf[1] != SYNC2:
        return None, "bad sync %02X %02X" % (buf[0], buf[1])
    dst, src, cls, op, flags, seq = buf[2:8]
    plen = buf[8] | (buf[9] << 8)
    total = HEADER + plen + 4
    if len(buf) < total:
        return None, "truncated: header says %d B payload, have %d B total" % (plen, len(buf))
    payload = bytes(buf[HEADER:HEADER + plen])
    a, b = fletcher8(buf[2:HEADER + plen])
    ck_ok = (a == buf[HEADER + plen] and b == buf[HEADER + plen + 1])
    end_ok = (buf[HEADER + plen + 2] == END1 and buf[HEADER + plen + 3] == END2)
    return {"dst": dst, "src": src, "cls": cls, "op": op, "flags": flags, "seq": seq,
            "payload": payload, "ck_ok": ck_ok, "end_ok": end_ok,
            "trailing": len(buf) - total}, None


def build_frame(dst, src, cls, op, flags, seq, payload: bytes) -> bytes:
    hdr = bytes([SYNC1, SYNC2, dst, src, cls, op, flags, seq,
                 len(payload) & 0xFF, len(payload) >> 8])
    a, b = fletcher8(hdr[2:] + payload)
    return hdr + payload + bytes([a, b, END1, END2])


# --- payload decoders --------------------------------------------------------
class LinkTracker:
    """Derives per-minute rates from consecutive net.status v2 samples."""
    def __init__(self):
        self.prev = None  # (t, frames_rx, resync)

    def note(self, frames_rx, resync):
        now = time.time()
        out = ""
        if self.prev:
            dt = now - self.prev[0]
            if dt > 0:
                out = " | since last net.status (%.0fs): +%d frames (%.1f/min), +%d resync" % (
                    dt, frames_rx - self.prev[1], 60.0 * (frames_rx - self.prev[1]) / dt,
                    resync - self.prev[2])
        self.prev = (now, frames_rx, resync)
        return out


LINK = LinkTracker()


def decode_payload(cls, op, flags, p: bytes) -> str:
    if cls == 0x03 and op == 0x01 and len(p) >= 20:               # net.status
        ver, state, rssi, rsrp, rsrq = struct.unpack_from("<BBbbb", p, 0)
        errors, ip, btx, brx = struct.unpack_from("<HIII", p, 6)
        s = "net.status v%d state=%s rssi=%d rsrp=%d rsrq=%d bytes_tx=%d bytes_rx=%d" % (
            ver, NET_STATE.get(state, state), rssi, rsrp, rsrq, btx, brx)
        if len(p) >= 30:
            frx, rsy, age = struct.unpack_from("<IIH", p, 20)
            s += "\n      >>> SYS-LINK: frames_rx=%d resync=%d link_age=%ds%s" % (
                frx, rsy, age, LINK.note(frx, rsy))
            if age > 90:
                s += "   <<< ESP32 has NOT decoded a frame from RP2040 for %ds" % age
        else:
            s += "  (v1: no sys-link tail)"
        return s
    if cls == 0x02 and op == 0x01 and len(p) >= 1:                # power.status
        if p[0] == 2 and len(p) >= 40:
            (ver, fl, cs, _r, vin, pdin_v, pdin_a, vout, vset, vread, ilim, pdo_v, pdo_a,
             vbat, ichg, vsys, iin, tlm, tmp, faults, up) = struct.unpack_from(
                "<BBBBHHHHHHHHHHhHHhhHI", p, 0)
            return ("power.status v2 chg=%s vin=%dmV vbat=%dmV ichg=%dmA vout=%dmV vsys=%dmV "
                    "iin=%dmA T=%d.%ddC faults=0x%04X flags=0x%02X ch32x_uptime=%ds" % (
                        CHG.get(cs, cs), vin, vbat, ichg, vout, vsys, iin, tmp // 10,
                        abs(tmp) % 10, faults, fl, up))
        return "power.status v%d (%d B) %s" % (p[0], len(p), p.hex())
    if cls == 0x04 and op == 0x01 and len(p) >= 12:               # host.status
        ver, eth, cput, mem, disk, load, up = struct.unpack_from("<BBhBBHI", p, 0)
        return "host.status v%d cpu=%.1fC mem=%d%% disk=%d%% load=%.2f uptime=%ds eth=0x%02X" % (
            ver, cput / 10, mem, disk, load / 100, up, eth)
    if cls == 0x01 and op == 0x01 and (flags & 2) and len(p) >= 8:  # pong
        ver, _r, fw, up = struct.unpack_from("<BBHI", p, 0)
        tail = p[8:].decode("ascii", "replace")
        return "pong fw=0x%04X uptime=%dms %s" % (fw, up, tail)
    if flags & 2 and len(p) >= 1:                                   # generic RESP
        return "RESP result=%d%s" % (p[0], (" " + p[1:].hex()) if len(p) > 1 else "")
    return "payload(%d B) %s" % (len(p), p.hex())


def describe(buf: bytes, indent="    ") -> str:
    f, err = decode_frame(buf)
    if err:
        return indent + "NOT A WUPS FRAME: %s | %s" % (err, buf[:48].hex())
    cls_n = CLASS.get(f["cls"], "cls%02X" % f["cls"])
    op_n = OPS.get(f["cls"], {}).get(f["op"], "op%02X" % f["op"])
    head = "%s%s -> %s  %s.%s  flags=%s seq=%d len=%d  ck=%s end=%s%s" % (
        indent, ADDR.get(f["src"], "%02X" % f["src"]), ADDR.get(f["dst"], "%02X" % f["dst"]),
        cls_n, op_n, flags_str(f["flags"]), f["seq"], len(f["payload"]),
        "OK" if f["ck_ok"] else "BAD", "OK" if f["end_ok"] else "BAD",
        (" trailing=%dB" % f["trailing"]) if f["trailing"] else "")
    body = decode_payload(f["cls"], f["op"], f["flags"], f["payload"])
    return head + "\n" + indent + "  " + body


# --- minimal MQTT 3.1.1 client ---------------------------------------------
def _enc_len(n):
    out = bytearray()
    while True:
        d, n = n & 0x7F, n >> 7
        if n:
            out.append(d | 0x80)
        else:
            out.append(d)
            return bytes(out)


def _str(s: str) -> bytes:
    b = s.encode()
    return struct.pack(">H", len(b)) + b


class Mqtt:
    def __init__(self, url, user, password, client_id, keepalive=60, insecure=False):
        u = urlparse(url)
        self.host, self.port = u.hostname, u.port or (8883 if u.scheme == "mqtts" else 1883)
        self.tls = u.scheme in ("mqtts", "ssl", "tls")
        self.user, self.password, self.cid, self.keepalive = user, password, client_id, keepalive
        self.insecure = insecure
        self.sock = None
        self.buf = b""
        self.last_tx = 0.0

    def connect(self):
        raw = socket.create_connection((self.host, self.port), timeout=15)
        if self.tls:
            ctx = ssl.create_default_context()
            if self.insecure:
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
            raw = ctx.wrap_socket(raw, server_hostname=self.host)
        self.sock = raw
        flags = 0x02  # clean session
        if self.user is not None:
            flags |= 0x80
            if self.password is not None:
                flags |= 0x40
        body = _str("MQTT") + bytes([4, flags]) + struct.pack(">H", self.keepalive) + _str(self.cid)
        if self.user is not None:
            body += _str(self.user)
            if self.password is not None:
                body += _str(self.password)
        self._send(0x10, body)
        t, payload = self._recv_packet(timeout=15)
        if t != 0x20 or len(payload) < 2:
            raise RuntimeError("no CONNACK (got type 0x%02X)" % t)
        rc = payload[1]
        if rc != 0:
            raise RuntimeError("CONNACK refused rc=%d (%s)" % (
                rc, {1: "bad protocol", 2: "id rejected", 3: "server unavailable",
                     4: "bad user/pass", 5: "not authorized"}.get(rc, "?")))

    def subscribe(self, topic, qos=0, pid=1):
        self._send(0x82, struct.pack(">H", pid) + _str(topic) + bytes([qos]))

    def publish(self, topic, payload: bytes, qos=0):
        self._send(0x30 | (qos << 1), _str(topic) + (struct.pack(">H", 7) if qos else b"") + payload)

    def _send(self, first_byte, body: bytes):
        self.sock.sendall(bytes([first_byte]) + _enc_len(len(body)) + body)
        self.last_tx = time.time()

    def _fill(self, timeout):
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return False
        chunk = self.sock.recv(4096)
        if not chunk:
            raise ConnectionError("broker closed the connection")
        self.buf += chunk
        return True

    def _recv_packet(self, timeout):
        """Return (type_byte, payload) or (None, None) on timeout."""
        deadline = time.time() + timeout
        while True:
            # try parse
            if len(self.buf) >= 2:
                mult, n, i = 1, 0, 1
                ok = False
                while i < len(self.buf) and i < 5:
                    d = self.buf[i]
                    n += (d & 0x7F) * mult
                    mult <<= 7
                    i += 1
                    if not (d & 0x80):
                        ok = True
                        break
                if ok and len(self.buf) >= i + n:
                    t = self.buf[0]
                    payload = self.buf[i:i + n]
                    self.buf = self.buf[i + n:]
                    return t, payload
            left = deadline - time.time()
            if left <= 0:
                return None, None
            self._fill(min(left, 1.0))

    def loop(self, on_message, run_seconds=None):
        start = time.time()
        while run_seconds is None or time.time() - start < run_seconds:
            if time.time() - self.last_tx > self.keepalive / 2:
                self._send(0xC0, b"")                       # PINGREQ
            t, payload = self._recv_packet(timeout=1.0)
            if t is None:
                continue
            typ = t & 0xF0
            if typ == 0x30:                                   # PUBLISH
                qos = (t >> 1) & 3
                tl = struct.unpack(">H", payload[:2])[0]
                topic = payload[2:2 + tl].decode("utf-8", "replace")
                pos = 2 + tl
                if qos:
                    pid = struct.unpack(">H", payload[pos:pos + 2])[0]
                    pos += 2
                    self._send(0x40, struct.pack(">H", pid))  # PUBACK
                on_message(topic, payload[pos:])
            elif typ == 0x90:
                rc = payload[2] if len(payload) > 2 else 0x80
                if rc & 0x80:
                    raise RuntimeError("SUBSCRIBE refused (rc=0x%02X) — ACL?" % rc)
                print("[mqtt] subscribed (granted qos=%d)" % rc, flush=True)
            elif typ == 0xD0:
                pass                                          # PINGRESP
            else:
                print("[mqtt] packet type 0x%02X (%d B)" % (t, len(payload)), flush=True)


# --- main ---------------------------------------------------------------------
STATS = {}
LAST_SEEN = {}


def on_message(topic, payload: bytes):
    ts = time.strftime("%H:%M:%S")
    sub = topic.split("/", 2)[-1] if topic.count("/") >= 2 else topic
    key = sub
    line = "%s  %-14s %4d B" % (ts, sub, len(payload))
    if payload[:1] == b"{" or sub in ("status", "identify"):
        print("%s  %s" % (line, payload.decode("utf-8", "replace")), flush=True)
    else:
        f, err = decode_frame(payload)
        if not err:
            key = "%s:%s.%s" % (sub, CLASS.get(f["cls"], f["cls"]),
                                OPS.get(f["cls"], {}).get(f["op"], f["op"]))
        print(line + "\n" + describe(payload), flush=True)
    STATS[key] = STATS.get(key, 0) + 1
    LAST_SEEN[key] = time.time()


def print_summary():
    print("\n=== summary (%s) ===" % time.strftime("%H:%M:%S"))
    now = time.time()
    for k in sorted(STATS):
        print("  %-40s %5d msgs   last %4.0fs ago" % (k, STATS[k], now - LAST_SEEN[k]))
    if not any(k.startswith("telemetry:POWER") for k in STATS):
        print("  !! no POWER.status seen on telemetry — nothing relayed from RP2040/CH32X")
    print(flush=True)


def selftest():
    ns = struct.pack("<BBbbbBHIII", 2, 4, -65, -95, -15, 0, 0, 0, 3819197, 2600011) + \
        struct.pack("<IIH", 1234, 7, 42)
    assert len(ns) == 30
    fr = build_frame(0xFF, 0x04, 0x03, 0x01, 0x04, 9, ns)
    f, err = decode_frame(fr)
    assert err is None and f["ck_ok"] and f["end_ok"], err
    print(describe(fr))
    ps = struct.pack("<BBBBHHHHHHHHHHhHHhhHI", 2, 0x0D, 1, 0, 14580, 15000, 3000, 5046, 5100,
                     5046, 5000, 5100, 3000, 7897, 525, 7900, 1200, 461, 470, 0, 86400)
    assert len(ps) == 40
    fr2 = build_frame(0xFF, 0x03, 0x02, 0x01, 0x04, 77, ps)
    print(describe(fr2))
    bad = bytearray(fr2)
    bad[20] ^= 0xFF
    f, _ = decode_frame(bytes(bad))
    assert not f["ck_ok"], "checksum must fail on corruption"
    print(describe(build_frame(0x01, 0x02, 0x05, 0x03, 0x02, 5, b"\x00")))  # beep RESP
    print("selftest OK")


def _roundtrip(url, user, password, label):
    topic = "w3p/sniff-selftest/%08x" % random.getrandbits(32)
    m = Mqtt(url, user, password, "wups-sniff-%06x" % random.getrandbits(24))
    m.connect()
    m.subscribe(topic + "/#")
    got = []
    fr = build_frame(0xFF, 0x04, 0x03, 0x01, 0x04, 1,
                     struct.pack("<BBbbbBHIII", 1, 4, -70, 0, 0, 0, 0, 0, 1, 2))
    t0 = time.time()
    sent = False

    def cb(t, p):
        got.append((t, p))
        on_message(t, p)
    while time.time() - t0 < 20 and not got:
        if not sent and time.time() - t0 > 1.5:
            m.publish(topic + "/telemetry", fr, qos=1)
            sent = True
        m.loop(cb, run_seconds=1)
    assert got and got[0][1] == fr, "%s round-trip failed" % label
    print("selftest-net %s OK (%d B round-trip)" % (label, len(fr)))


def selftest_net():
    _roundtrip("mqtt://test.mosquitto.org:1883", None, None, "anonymous")
    _roundtrip("mqtt://test.mosquitto.org:1884", "rw", "readwrite", "authenticated")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iccid", nargs="?", help="device ICCID (subscribes to t/<iccid>/#)")
    ap.add_argument("--url", default=os.environ.get("MQTT_URL", "mqtts://broker.w3p.ovh:8883"))
    ap.add_argument("--user", default=os.environ.get("MQTT_USER"))
    ap.add_argument("--password", default=os.environ.get("MQTT_PASS"))
    ap.add_argument("--topic", help="override subscription filter")
    ap.add_argument("--insecure", action="store_true", help="skip TLS certificate verification")
    ap.add_argument("--seconds", type=float, help="stop after N seconds (default: run until Ctrl-C)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--selftest-net", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.selftest_net:
        return selftest_net()
    if not a.iccid and not a.topic:
        ap.error("iccid (or --topic) required")
    if not a.user:
        ap.error("MQTT_USER/--user required — use an operator account, never the device ICCID")
    topic = a.topic or "t/%s/#" % a.iccid
    cid = "wups-sniff-%06x" % random.getrandbits(24)
    print("[mqtt] connecting %s as %s (client-id %s)" % (a.url, a.user, cid), flush=True)
    m = Mqtt(a.url, a.user, a.password, cid, insecure=a.insecure)
    m.connect()
    m.subscribe(topic)
    print("[mqtt] subscribed to %s — waiting (net.status every ~60 s, power.status every 30 s if relay alive)" % topic, flush=True)
    try:
        m.loop(on_message, run_seconds=a.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        print_summary()


if __name__ == "__main__":
    sys.exit(main())
