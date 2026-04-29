#!/usr/bin/env python3
"""
Send an AT command to the SIM7080G modem via the firmware's pass-through
bridge, and read the response from the running serial monitor's log.

Architecture:
  - tools/serial_monitor.py owns the serial port. It also creates a named
    FIFO (default <repo>/logs/serial-input.fifo) and runs a background
    thread that forwards bytes from the FIFO to the serial port.
  - This script writes the AT command (with CRLF) to that FIFO. The chip's
    USB-CDC stdin sees the bytes, the firmware's pass-through bridge
    forwards them to the modem on UART1, the modem responds, and the
    response shows up in the monitor's normal log capture.
  - We tail the log file for the new lines and print them with `<<<` tags.

This indirection avoids opening the serial port from this process — on
macOS, opening /dev/cu.usbmodem* pulses DTR even when pyserial is told not
to, which on ESP32-S3 USB-Serial-JTAG resets the chip.

Usage:
    python tools/send_at.py "AT"
    python tools/send_at.py "AT+CSQ" --timeout 5
    python tools/send_at.py 'AT+CGDCONT=1,"IP","iot.1nce.net"'
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import sys
import time
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOG = PROJECT_ROOT / "logs" / "serial.log"
DEFAULT_PID = PROJECT_ROOT / "logs" / "serial-monitor.pid"
DEFAULT_FIFO = PROJECT_ROOT / "logs" / "serial-input.fifo"


def stamp() -> str:
    now = dt.datetime.now()
    return now.strftime("%H:%M:%S.") + f"{now.microsecond // 1000:03d}"


def emit(streams, prefix: str, text: str) -> None:
    line = f"[{stamp()}] {prefix} {text.rstrip()}\n"
    for s in streams:
        s.write(line)
        s.flush()


def monitor_alive(pid_path: Path) -> bool:
    if not pid_path.exists():
        return False
    try:
        pid = int(pid_path.read_text().strip())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def read_response_from_log(log_path: Path, start_offset: int,
                            timeout: float) -> list[str]:
    """
    Tail the log file starting at byte offset `start_offset` for up to
    `timeout` seconds. Return new lines that look like modem replies
    (i.e. anything that isn't a monitor `---` annotation, an `>>>` echo,
    or an ESP_LOGI line from the firmware itself).
    """
    deadline = time.monotonic() + timeout
    new_lines: list[str] = []
    while time.monotonic() < deadline:
        try:
            with log_path.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(start_offset)
                while True:
                    line = f.readline()
                    if not line:
                        break
                    new_lines.append(line.rstrip("\n"))
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    return new_lines


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", help='AT command to send (e.g. "AT" or \'AT+COPS?\')')
    p.add_argument("--log", default=str(DEFAULT_LOG),
                   help=f"log file to tail for the response (default {DEFAULT_LOG})")
    p.add_argument("--pid-file", default=str(DEFAULT_PID),
                   help=f"monitor PID file (default {DEFAULT_PID})")
    p.add_argument("--input-fifo", default=str(DEFAULT_FIFO),
                   help=f"FIFO that the monitor reads from (default {DEFAULT_FIFO})")
    p.add_argument("--timeout", type=float, default=3.0,
                   help="seconds to wait for the modem response (default 3)")
    p.add_argument("--quiet-stdout", action="store_true",
                   help="don't echo to stdout, only write to the log file")
    args = p.parse_args()

    log_path = Path(args.log)
    fifo_path = Path(args.input_fifo)
    pid_path = Path(args.pid_file)

    if not monitor_alive(pid_path):
        sys.stderr.write(
            f"ERROR: serial monitor doesn't seem to be running "
            f"(no live PID at {pid_path}).\n"
            "Start it first with: tools/serial-monitor\n"
        )
        return 1

    if not fifo_path.exists():
        sys.stderr.write(
            f"ERROR: monitor FIFO not found at {fifo_path}.\n"
            "Make sure you're using a recent monitor that creates the FIFO.\n"
        )
        return 1

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_fp = log_path.open("a", buffering=1, encoding="utf-8", errors="replace")
    streams = [log_fp] if args.quiet_stdout else [sys.stdout, log_fp]

    cmd = args.command.rstrip()

    # Note where the log ends BEFORE we send, so we can find new lines after.
    try:
        start_offset = log_path.stat().st_size
    except FileNotFoundError:
        start_offset = 0

    emit(streams, ">>>", cmd)

    # Write the AT command to the FIFO. open() blocks until the monitor has
    # the read end open, which it should have since the monitor is alive.
    try:
        with fifo_path.open("wb") as f:
            f.write((cmd + "\r\n").encode("utf-8"))
            f.flush()
    except Exception as e:
        sys.stderr.write(f"ERROR: writing to FIFO {fifo_path} failed: {e}\n")
        log_fp.close()
        return 1

    # Tail the log for `timeout` seconds. The monitor's reads will land here
    # via its existing emit_line() pathway. We filter our own ">>>" echo and
    # the monitor's own annotation lines.
    new_lines = read_response_from_log(log_path, start_offset, args.timeout)
    for raw in new_lines:
        # Skip lines we wrote ourselves (the >>> echo) and any internal
        # monitor notes. Show real device output from the chip.
        if " >>> " in raw or " --- " in raw:
            continue
        # Format already includes [HH:MM:SS.mmm], emit as-is, but tag with <<<.
        # Strip the leading timestamp so we can re-tag uniformly.
        ts_end = raw.find("] ")
        if ts_end > 0:
            content = raw[ts_end + 2:]
        else:
            content = raw
        emit(streams, "<<<", content)

    log_fp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
