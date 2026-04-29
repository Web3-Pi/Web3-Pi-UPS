#!/usr/bin/env python3
"""
Serial monitor with file logging and auto-reconnect.

Reads from a USB-CDC serial port, prints to stdout AND tees to a log file
with per-line timestamps. Automatically reconnects when the port goes away
(e.g. when esptool grabs it for `idf.py flash`), so you can keep this
running in the background for the entire session.

Default port:  auto-detected from /dev/cu.usbmodem*
Default baud:  115200 (ESP-IDF default)
Default log:   <project>/logs/serial.log

Usage:
    python tools/serial_monitor.py
    python tools/serial_monitor.py --port /dev/cu.usbmodem1101 --log logs/foo.log
    python tools/serial_monitor.py --truncate     # start with empty log
"""

from __future__ import annotations

import argparse
import datetime as dt
import glob
import os
import signal
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.stderr.write(
        "ERROR: pyserial not installed. Run:\n"
        "    . ~/esp/v6.0/esp-idf/export.sh\n"
        "    pip install pyserial\n"
    )
    sys.exit(2)


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOG = PROJECT_ROOT / "logs" / "serial.log"
DEFAULT_PID = PROJECT_ROOT / "logs" / "serial-monitor.pid"

# SIGUSR1 puts the monitor into a yield state so a concurrent `idf.py flash`
# (esptool) can grab the port without "multiple access" errors. The wrapper
# sends SIGUSR2 when the flash actually finishes — this is the normal exit
# path. YIELD_SECONDS is just a safety net for the case where the wrapper
# crashes / gets killed before sending SIGUSR2.
YIELD_SECONDS = 60.0


def autodetect_port() -> str | None:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        sys.stderr.write(
            f"Multiple USB serial ports found, please specify --port:\n  "
            + "\n  ".join(candidates)
            + "\n"
        )
    return None


def stamp() -> str:
    now = dt.datetime.now()
    return now.strftime("%H:%M:%S.") + f"{now.microsecond // 1000:03d}"


def emit_line(stream, log_fp, line: str) -> None:
    """Write a single line to stdout + log file with a timestamp."""
    formatted = f"[{stamp()}] {line.rstrip()}\n"
    stream.write(formatted)
    stream.flush()
    log_fp.write(formatted)
    log_fp.flush()


def emit_note(stream, log_fp, note: str) -> None:
    """Emit a monitor-side annotation line (connect/disconnect events)."""
    formatted = f"[{stamp()}] --- {note} ---\n"
    stream.write(formatted)
    stream.flush()
    log_fp.write(formatted)
    log_fp.flush()


def pulse_reset(ser: "serial.Serial") -> None:
    """
    Reset the ESP32 by pulsing the EN pin via RTS.

    The board has the standard auto-reset wiring: RTS drives EN through an
    inverting transistor, so RTS=True → EN low (chip in reset), RTS=False →
    EN high (chip runs). DTR drives IO0 the same way; we keep DTR=False so
    the chip enters normal mode (not download mode) on release.
    """
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)


def run(port: str, baud: int, log_path: Path, pid_path: Path,
        reset_on_connect: bool) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_fp = log_path.open("a", buffering=1, encoding="utf-8", errors="replace")
    out = sys.stdout

    pid_path.parent.mkdir(parents=True, exist_ok=True)
    pid_path.write_text(str(os.getpid()))

    emit_note(out, log_fp, f"monitor start pid={os.getpid()} port={port} baud={baud} log={log_path}")

    state = {"stop": False, "yield_until": 0.0}

    def handle_sigterm(_signum, _frame):
        state["stop"] = True

    def handle_sigusr1(_signum, _frame):
        # Yield the port. yield_until acts as a safety-net deadline; the
        # normal exit path is SIGUSR2 from the flash wrapper when esptool
        # actually finishes.
        state["yield_until"] = time.monotonic() + YIELD_SECONDS

    def handle_sigusr2(_signum, _frame):
        # Flash wrapper signals "I'm done with the port, you can come back
        # immediately" — clear the yield window.
        state["yield_until"] = 0.0

    signal.signal(signal.SIGINT, handle_sigterm)
    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGUSR1, handle_sigusr1)
    signal.signal(signal.SIGUSR2, handle_sigusr2)

    pending = bytearray()

    try:
        while not state["stop"]:
            now = time.monotonic()
            if now < state["yield_until"]:
                remaining = state["yield_until"] - now
                emit_note(out, log_fp, f"yielding port for ~{remaining:.0f}s (SIGUSR1)")
                time.sleep(min(remaining, 1.0))
                continue

            ser = None
            try:
                # Disable pyserial's default DTR/RTS toggling on open so that
                # connect-time line state is fully under our control.
                ser = serial.Serial(port, baud, timeout=0.2,
                                    dsrdtr=False, rtscts=False)
            except (serial.SerialException, OSError) as e:
                emit_note(out, log_fp, f"port unavailable ({e}); retry in 1s")
                time.sleep(1.0)
                continue

            if reset_on_connect:
                try:
                    pulse_reset(ser)
                    emit_note(out, log_fp, f"connected to {port} (reset pulse sent)")
                except Exception as e:
                    emit_note(out, log_fp, f"connected to {port} (reset failed: {e})")
            else:
                emit_note(out, log_fp, f"connected to {port}")

            try:
                while not state["stop"]:
                    if time.monotonic() < state["yield_until"]:
                        emit_note(out, log_fp, "SIGUSR1 received; closing port to yield")
                        break
                    try:
                        chunk = ser.read(4096)
                    except (serial.SerialException, OSError) as e:
                        emit_note(out, log_fp, f"read error ({e}); reconnecting")
                        break

                    if not chunk:
                        continue

                    pending.extend(chunk)
                    while True:
                        nl = pending.find(b"\n")
                        if nl < 0:
                            break
                        line_bytes = bytes(pending[:nl])
                        del pending[: nl + 1]
                        line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")
                        emit_line(out, log_fp, line)
            finally:
                try:
                    ser.close()
                except Exception:
                    pass
                emit_note(out, log_fp, "disconnected")

        if pending:
            line = bytes(pending).decode("utf-8", errors="replace")
            emit_line(out, log_fp, line)
        emit_note(out, log_fp, "monitor stop")
    finally:
        try:
            if pid_path.exists() and pid_path.read_text().strip() == str(os.getpid()):
                pid_path.unlink()
        except Exception:
            pass
        log_fp.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP32 serial monitor with file logging.")
    parser.add_argument("--port", default=None,
                        help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--log", default=str(DEFAULT_LOG),
                        help=f"Log file path (default: {DEFAULT_LOG})")
    parser.add_argument("--truncate", action="store_true",
                        help="Truncate the log file before starting")
    parser.add_argument("--pid-file", default=str(DEFAULT_PID),
                        help=f"PID file path (default: {DEFAULT_PID})")
    parser.add_argument("--no-reset", action="store_true",
                        help="Don't pulse RTS/EN to reset the chip on each connect")
    args = parser.parse_args()

    port = args.port or autodetect_port()
    if not port:
        sys.stderr.write("ERROR: no serial port found and --port not given.\n")
        return 1

    log_path = Path(args.log)
    if args.truncate and log_path.exists():
        log_path.unlink()

    return run(port, args.baud, log_path, Path(args.pid_file),
               reset_on_connect=not args.no_reset)


if __name__ == "__main__":
    sys.exit(main())
