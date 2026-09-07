#!/usr/bin/env python3
"""
Regression test for the WUPS byte-stream deframer (host-side mirror).

The three MCU firmwares (ESP32 wups_link.c, RP2040 wups_router.cpp, CH32X
User/main.c) implement the same hand-written state machine; there is no
host build for them, so this file carries a byte-exact Python port of BOTH
the pre-2026-09 deframer (`Deframer(fixed=False)`) and the fixed one
(`fixed=True`), and pins down the behaviours that matter:

  1. healthy stream: every frame is delivered by both variants
  2. ONE dropped byte with the OLD deframer: it never re-locks and delivers
     only the inner frames embedded in net.publish wrappers (the 2026-09-07
     field blackout) — documents the bug, guards against re-introducing it
  3. the FIXED deframer re-locks on the very next frame
  4. a sender-side guard byte (WUPS_GUARD_BYTE) lets even the OLD deframer
     re-lock (this is how an updated RP2040 protects a field CH32X)
  5. fuzz: random drop / insert / flip corruptions on a long mixed stream —
     the fixed deframer never delivers a frame whose bytes were corrupted
     and loses at most the frames touched by each corruption

If you change any of the three rx_byte() functions, mirror the change here.
Run: python3 tools/test_wups_deframer.py
"""
import random
import struct
import sys

SYNC1, SYNC2, END1, END2, GUARD = 0xAA, 0x55, 0x55, 0xAA, 0x00
MAX_PAYLOAD = 240


def fletcher8(data):
    a = b = 0
    for x in data:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def build_frame(dst, src, cls, op, flags, seq, payload, guard=False):
    hdr = bytes([SYNC1, SYNC2, dst, src, cls, op, flags, seq, len(payload) & 0xFF, len(payload) >> 8])
    a, b = fletcher8(hdr[2:] + payload)
    return hdr + payload + bytes([a, b, END1, END2]) + (bytes([GUARD]) if guard else b"")


class Deframer:
    """Byte-exact mirror of rx_byte() in wups_link.c / wups_router.cpp / CH32X main.c."""

    def __init__(self, fixed):
        self.fixed = fixed
        self.state = "SYNC1"
        self.resync = 0
        self.delivered = []

    def _reset_with(self, b):
        # fixed: rx_reset_with(b); old: rx_reset()
        self.state = "SYNC2" if (self.fixed and b == SYNC1) else "SYNC1"

    def feed(self, b):
        st = self.state
        if st == "SYNC1":
            if b == SYNC1:
                self.state = "SYNC2"
        elif st == "SYNC2":
            if b == SYNC2:
                self.a = self.b_ = 0
                self.hdr = []
                self.pl = bytearray()
                self.state = "HDR"
            else:
                self.resync += 1
                self._reset_with(b)
        elif st == "HDR":
            self.hdr.append(b)
            self.a = (self.a + b) & 0xFF
            self.b_ = (self.b_ + self.a) & 0xFF
            if len(self.hdr) == 8:
                self.len = self.hdr[6] | (self.hdr[7] << 8)
                if self.len > MAX_PAYLOAD:
                    self._reset_with(b)
                else:
                    self.state = "PL" if self.len else "CKA"
        elif st == "PL":
            self.pl.append(b)
            self.a = (self.a + b) & 0xFF
            self.b_ = (self.b_ + self.a) & 0xFF
            if len(self.pl) >= self.len:
                self.state = "CKA"
        elif st == "CKA":
            self.cka = b
            self.state = "CKB"
        elif st == "CKB":
            if self.cka == self.a and b == self.b_:
                self.state = "E1"
            else:
                self._reset_with(b)
        elif st == "E1":
            if b == END1:
                self.state = "E2"
            else:
                self._reset_with(b)
        elif st == "E2":
            if b == END2:
                self.delivered.append((tuple(self.hdr[:6]), bytes(self.pl)))
                self.state = "SYNC1"
            else:
                self._reset_with(b)

    def feed_all(self, data):
        for b in data:
            self.feed(b)
        return self


# --- realistic traffic on the RP2040 -> ESP32 link --------------------------
def power_status(seq):
    ps = struct.pack("<BBBBHHHHHHHHHHhHHhhHI", 2, 0x0D, 1, 0, 14580, 15000, 3000, 5046, 5100,
                     5046, 5000, 5100, 3000, 7897, 525, 7900, 1200, 461, 470, 0, 86400 + seq)
    return build_frame(0xFF, 0x03, 0x02, 0x01, 0x04, seq & 0xFF, ps)   # inner frame, src=CH32X


def net_publish(seq, topic, inner, qos, guard):
    hdr = struct.pack("<BBBBH", 1, qos, 0, len(topic), len(inner))
    return build_frame(0x04, 0x02, 0x03, 0x02, 0x01, seq & 0xFF, hdr + topic.encode() + inner, guard)


def syslog(seq, text, guard):
    return build_frame(0xFF, 0x03, 0x01, 0x04, 0x04, seq & 0xFF, bytes([1, 2, len(text), 0]) + text.encode(), guard)


def traffic(cycles, guard, seed=1):
    """`cycles` relay periods: 6 syslog broadcasts + 1 net.publish(telemetry) each.
    Returns (stream, frames) where frames = list of the top-level frame bytes."""
    rnd = random.Random(seed)
    frames = []
    for k in range(cycles):
        for j in range(6):
            frames.append(syslog(k * 7 + j, "MP2762A r00=%02x r0F=05 TJ=%d" % (rnd.randrange(256), rnd.randrange(30, 60)), guard))
        frames.append(net_publish(k * 7 + 6, "telemetry", power_status(k), 0, guard))
    return b"".join(frames), frames


def is_wrapper(f):
    return f[0][2] == 0x03 and f[0][3] == 0x02          # NET.publish


def is_inner_power(f):
    return f[0][2] == 0x02 and f[0][3] == 0x01          # POWER.status (only exists INSIDE wrappers here)


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)
    print("ok  :", msg)


def main():
    # 1. healthy stream
    stream, frames = traffic(10, guard=False)
    for fixed in (False, True):
        d = Deframer(fixed).feed_all(stream)
        check(len(d.delivered) == len(frames) and d.resync == 0,
              "healthy stream, %s deframer: %d/%d frames, resync=%d" % ("fixed" if fixed else "old", len(d.delivered), len(frames), d.resync))
    # guard bytes must be invisible to both variants
    stream_g, frames_g = traffic(10, guard=True)
    for fixed in (False, True):
        d = Deframer(fixed).feed_all(stream_g)
        check(len(d.delivered) == len(frames_g) and d.resync == 0,
              "healthy stream WITH guard bytes, %s deframer: %d/%d, resync=%d" % ("fixed" if fixed else "old", len(d.delivered), len(frames_g), d.resync))

    # 2./3. one dropped byte inside a syslog body at cycle 3, then 20 more cycles
    pre, _ = traffic(3, guard=False, seed=1)
    bad = bytearray(syslog(99, "PD: contract 15V/1.8A", False))
    del bad[15]
    post, post_frames = traffic(20, guard=False, seed=2)
    stream = pre + bytes(bad) + post
    old = Deframer(False).feed_all(stream)
    old_after = [f for f in old.delivered[3 * 7:]]
    check(not any(is_wrapper(f) for f in old_after) and any(is_inner_power(f) for f in old_after),
          "OLD deframer after ONE dropped byte: zero wrappers, only inner POWER.status frames (%d) — the 2026-09-07 trap"
          % sum(1 for f in old_after if is_inner_power(f)))
    new = Deframer(True).feed_all(stream)
    new_after = new.delivered[3 * 7:]
    check(len(new_after) == len(post_frames) and sum(1 for f in new_after if is_wrapper(f)) == 20,
          "FIXED deframer after ONE dropped byte: %d/%d subsequent frames incl. all 20 wrappers, resync=%d"
          % (len(new_after), len(post_frames), new.resync))

    # 4. guard byte on the wire rescues the OLD deframer (field CH32X case: no inner frames at all)
    def cmd(seq, guard):
        return build_frame(0x03, 0x02, 0x02, 0x02, 0x01, seq, b"\x01\x00", guard)   # power.enable REQ to CH32X
    for guard in (False, True):
        stream = b"".join(cmd(k, guard) for k in range(3))
        bad = bytearray(cmd(50, guard))
        del bad[9]
        stream += bytes(bad) + b"".join(cmd(k, guard) for k in range(4, 24))
        d = Deframer(False).feed_all(stream)
        if guard:
            check(len(d.delivered) == 23, "OLD deframer + sender GUARD byte: %d/23 frames after a dropped byte (re-locks)" % len(d.delivered))
        else:
            check(len(d.delivered) == 3, "OLD deframer, no guard: %d/23 frames — stuck forever (documents the bug)" % len(d.delivered))

    # 5. fuzz the FIXED deframer with random corruptions
    rnd = random.Random(2026)
    stream, frames = traffic(300, guard=True, seed=3)
    # Legit outcomes: any top-level frame (guard stripped) OR an inner power.status
    # frame — when a corruption hits a wrapper's header the deframer correctly
    # re-locks on the complete inner frame it carries (the ESP32 ignores those).
    valid = {f[:-1] if f[-1] == GUARD and len(f) > 14 else f for f in frames}
    valid |= {power_status(k) for k in range(300)}
    data = bytearray(stream)
    n_corrupt = 400
    for _ in range(n_corrupt):
        pos = rnd.randrange(len(data))
        kind = rnd.choice(("drop", "insert", "flip"))
        if kind == "drop":
            del data[pos]
        elif kind == "insert":
            data.insert(pos, rnd.randrange(256))
        else:
            data[pos] ^= rnd.randrange(1, 256)
    d = Deframer(True).feed_all(bytes(data))
    rebuilt = [build_frame(h[0], h[1], h[2], h[3], h[4], h[5], p) for h, p in d.delivered]
    forged = sum(1 for f in rebuilt if f not in valid)
    lost = len(frames) - len(d.delivered)
    check(lost <= 2 * n_corrupt,
          "fuzz: %d corruptions on %d frames -> %d delivered, %d lost (<= 2 per corruption), %d forged" % (n_corrupt, len(frames), len(d.delivered), lost, forged))
    # A Fletcher-8 collision is possible in theory (16-bit check); with 400 corruptions
    # expect none — flag it loudly if the RNG ever finds one so the sample is re-checked.
    check(forged == 0, "fuzz: every delivered frame is a genuine top-level or inner frame (no corrupted bytes passed)")
    print("ALL PASSED")


if __name__ == "__main__":
    main()
