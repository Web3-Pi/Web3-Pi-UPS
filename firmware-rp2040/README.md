# Web3‑Pi UPS RP2040 Firmware

A lightweight firmware for a small uninterruptible power supply (UPS) and system monitor built on the Raspberry Pi Pico (RP2040). It is written with the Arduino framework and managed with PlatformIO. The firmware drives a compact SSD1306 OLED over I2C, reads analog inputs for voltage monitoring, supports two buttons for navigation, and provides basic buzzer/LED feedback.

Key capabilities
- RP2040 (Raspberry Pi Pico) + Arduino via PlatformIO
- Compact status UI on SSD1306 OLED (I2C)
- Multiple information screens and a simple settings menu
- Smoothed ADC readings for stable voltage display
- Basic sleep/brightness control and audible feedback

Hardware (typical)
- Raspberry Pi Pico (RP2040)
- SSD1306 OLED display (I2C)
- Two buttons for navigation
- Buzzer and onboard LED
- Basic voltage sensing for input/output/battery lines

Project layout
- `platformio.ini` — PlatformIO environment and dependencies
- `src/` — application source code (main firmware entry in `main.cpp`)
- `include/` — headers (if needed)
- `lib/` — project-specific libraries (optional)
- `test/` — PlatformIO unit tests (optional)

Dependencies
- Managed via PlatformIO (e.g., Adafruit GFX and Adafruit SSD1306)

## Flashing

### Via SWD (Raspberry Pi Debug Probe) — preferred

Lets the Raspberry Pi 5 stay powered and connected the whole time
(the USB-C output port doubles as the RP2040 USB data port, so
flashing over USB-C requires unplugging the Pi 5).

Requirements:
- Raspberry Pi Debug Probe wired to **J401** on the UPS PCB (4-pin
  JST SH: probe `D` SC→J401.3 SWCLK, GND→J401.4, SD→J401.2 SWDIO).
- The board powered from any source (PD input, barrel jack, or battery)
  — the probe does not source 3.3 V.

Command (run from this directory):

```sh
~/.platformio/penv/bin/pio run -e pico_swd -t upload
```

This builds the `pico_swd` env and flashes via OpenOCD + CMSIS-DAP. A
successful run ends with `** Verified OK **` and `[SUCCESS]`.

### Via USB-C (picotool)

Bench bring-up only. Requires a USB-C cable from the dev machine to
the UPS output USB-C port and disconnects the Pi 5 for the duration
of the flash.

```sh
~/.platformio/penv/bin/pio run -e pico -t upload
```

### Serial monitor (USB-CDC, host-side)

```sh
~/.platformio/penv/bin/pio device monitor
```

Note: with the binary wire protocol v1, USB-CDC carries framed bytes
(start `0xAA 0x55`, end `0x55 0xAA`). The monitor will show them as
non-printable characters — use a hex viewer or the host-side service
to decode.