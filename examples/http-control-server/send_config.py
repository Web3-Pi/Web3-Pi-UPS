#!/usr/bin/env python3
"""
Set the UPS's HTTP control-mode endpoint from the Raspberry Pi host
(HTTP-2 / plan §4.18a, config option "A").

The production board has no USB-C — you don't re-flash the ESP32 to point it
at your server. Instead the RPi sends a `net.config` WUPS frame down the
existing serial link; the RP2040 routes it to the ESP32 (no RP2040 change),
which persists the value in NVS. It takes effect when the device is in HTTP
backend mode.

This writes the binary frame to the RP2040 USB-CDC port (the same port the
host service uses, enumerated as "Web3_Pi_UPS"). Requires pyserial:

    sudo apt install -y python3-serial
    # or, in a venv:  pip install pyserial

The serial port is **auto-detected** by its USB descriptor (the RP2040
enumerates as "Web3_Pi_UPS"), so you normally don't pass --port at all. If you
have several matching devices, pass --port to disambiguate. To see candidates:
`python3 send_config.py --list` (or, on the Pi, `ls -l /dev/serial/by-id/`).

Examples:
    # point the device at your server (the device appends /api/v1/devices/{id}/telemetry)
    python3 send_config.py --url https://ups.example.com

    # plain HTTP on a VPS (no domain/TLS needed)
    python3 send_config.py --url http://203.0.113.10:8080

    # optional: override the device_id used in the URL path (default = ICCID)
    python3 send_config.py --device-id my-ups-01

    # clear the override (revert to the compile-time default / ICCID)
    python3 send_config.py --url ""

    # explicit port (skip auto-detect):
    python3 send_config.py --port /dev/serial/by-id/usb-Web3_Pi_Web3_Pi_UPS-if00 --url ...
"""

import argparse
import sys
import time

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required: sudo apt install -y python3-serial "
             "(or pip install pyserial in a venv)")

# --- wire protocol v1 constants (see common/protocol.h) -------------------
SYNC1, SYNC2 = 0xAA, 0x55
END1, END2 = 0x55, 0xAA
ADDR_RPI, ADDR_ESP32 = 0x01, 0x04
CLASS_NET = 0x03
OP_NET_CONFIG = 0x21
FLAG_REQ, FLAG_RESP = 0x01, 0x02

CFG_ITEM_HTTP_URL = 0x01
CFG_ITEM_DEVICE_ID = 0x02


def fletcher8(data: bytes) -> tuple[int, int]:
    a = b = 0
    for x in data:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def build_frame(dst, src, cls, op, flags, seq, payload: bytes) -> bytes:
    hdr = bytes([dst, src, cls, op, flags, seq,
                 len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    a, b = fletcher8(hdr + payload)
    return bytes([SYNC1, SYNC2]) + hdr + payload + bytes([a, b, END1, END2])


def build_net_config(item: int, value: str) -> bytes:
    val = value.encode()
    if len(val) > 200:
        sys.exit("value too long (max 200 bytes)")
    payload = bytes([1, item, len(val), 0]) + val
    return build_frame(ADDR_ESP32, ADDR_RPI, CLASS_NET, OP_NET_CONFIG,
                       FLAG_REQ, 0x42, payload)


def read_result(ser, timeout_s=3.0):
    """Best-effort: scan inbound bytes for a net.config RESP and report it."""
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf.extend(chunk)
            # Look for our RESP signature: ... CLASS_NET, OP_NET_CONFIG, FLAG_RESP
            for i in range(len(buf) - 13):
                if (buf[i] == SYNC1 and buf[i + 1] == SYNC2 and
                        buf[i + 4] == CLASS_NET and buf[i + 5] == OP_NET_CONFIG and
                        buf[i + 6] == FLAG_RESP):
                    # payload starts at i+10: version, item, result, reserved
                    if i + 12 < len(buf):
                        result = buf[i + 12]
                        return result
        else:
            time.sleep(0.05)
    return None


def find_ports():
    """Return [(device, description)] for ports that look like the UPS's
    RP2040 USB-CDC (USB descriptor "Web3_Pi" / "Web3_Pi_UPS")."""
    hits = []
    for p in list_ports.comports():
        hay = " ".join(str(x) for x in (p.product, p.manufacturer,
                                        p.description, p.hwid)).lower()
        if "web3" in hay:
            hits.append((p.device, p.description))
    return hits


def resolve_port(explicit):
    if explicit:
        return explicit
    hits = find_ports()
    if len(hits) == 1:
        print(f"auto-detected port: {hits[0][0]} ({hits[0][1]})")
        return hits[0][0]
    if not hits:
        sys.exit("no Web3_Pi_UPS serial port found. Plug in the UPS, or pass "
                 "--port explicitly (try: ls -l /dev/serial/by-id/).")
    print("multiple candidate ports — pass one with --port:")
    for dev, desc in hits:
        print(f"  {dev}  ({desc})")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description="Set UPS HTTP control-mode endpoint via the RPi serial link")
    ap.add_argument("--port", help="RP2040 USB-CDC device (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--list", action="store_true", help="list candidate serial ports and exit")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--url", help='base URL, e.g. "http://1.2.3.4:8080" (empty string clears)')
    g.add_argument("--device-id", help="device_id override (empty string clears → use ICCID)")
    args = ap.parse_args()

    if args.list:
        hits = find_ports()
        if not hits:
            print("no Web3_Pi_UPS ports found (try: ls -l /dev/serial/by-id/)")
        for dev, desc in hits:
            print(f"{dev}  ({desc})")
        return
    if args.url is None and args.device_id is None:
        ap.error("one of --url or --device-id is required")

    port = resolve_port(args.port)

    if args.url is not None:
        frame = build_net_config(CFG_ITEM_HTTP_URL, args.url)
        what = f'HTTP_URL = "{args.url}"'
    else:
        frame = build_net_config(CFG_ITEM_DEVICE_ID, args.device_id)
        what = f'DEVICE_ID = "{args.device_id}"'

    # The w3p-ups host service normally OWNS this port. Two writers on one
    # CDC interleave bytes and corrupt framing, so open exclusively and, if
    # that fails, tell the user to stop the service for a moment.
    try:
        ser = serial.Serial(port, args.baud, timeout=0.2, exclusive=True)
    except serial.SerialException as e:
        sys.exit(f"could not open {port} exclusively: {e}\n"
                 "The w3p-ups service likely holds it. Stop it briefly, e.g.:\n"
                 "    sudo systemctl stop w3p-ups\n"
                 "    python3 send_config.py ...\n"
                 "    sudo systemctl start w3p-ups")
    with ser:
        ser.write(frame)
        ser.flush()
        print(f"sent net.config {what} ({len(frame)} bytes) to {port}")
        result = read_result(ser)
        if result is None:
            print("no RESP seen (the device may still have applied it; the RESP "
                  "is routed to the RPi and can be missed amid telemetry)")
        elif result == 0:
            print("device acked: OK")
        else:
            print(f"device acked: ERROR (result={result})")


if __name__ == "__main__":
    main()
