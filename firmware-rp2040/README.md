# Web3 Pi UPS — RP2040 Firmware

Firmware for the Web3 Pi UPS OLED panel and WUPS protocol router. The current
source reports **`rp2040:1.2.4`**. It uses the Arduino framework with the
pinned arduino-pico **5.6.1** core and PlatformIO configuration in
[platformio.ini](platformio.ini).

## What It Does

- Displays input, output, battery and system telemetry on a **64 × 32
  SSD1306 OLED**, with local voltage measurements and two-button navigation.
- Routes binary WUPS frames between the Raspberry Pi host, CH32X035 power
  controller and optional ESP32-S3 LTE-M card.
- Provides a local menu for brightness, sound, information and output power
  control. These settings work without the LTE card; **Network** opens the
  ESP32 menu when the card is present.
- Displays device trust prompts, remote notices, modem alerts and firmware
  update progress.
- Receives RP2040 updates from the ESP32 or a USB host, verifies their SHA-256
  digest, stages them in LittleFS and supports rollback after an unconfirmed
  update.

On the home screen, **hold LEFT** to open the local menu. LEFT moves through
items or changes a value; RIGHT selects. Cutting output power requires the
menu's confirmation screen.

### Current UI Behavior

Version 1.2.4 sets the factory brightness to **Lvl 1/6**. On the first boot
with older version-1 settings, brightness is reset to this level and the saved
sound setting is preserved. Subsequent brightness choices persist. Muting
sound silences all buzzer output, including battery and power alarms.

The **FW UPDATE** banner is silent and displays update progress without the
modem's “no uplink” error line. Ordinary remote notices have their own display
layer and expire after 60 seconds or a button press. They do not replace the
modem alarm state.

## Connections

Pin assignments below come from [src/main.cpp](src/main.cpp).

| Interface | RP2040 pins / transport |
|---|---|
| OLED I²C | GPIO8 SDA, GPIO9 SCL; address `0x3C` |
| CH32X035 | UART0: GPIO16 TX, GPIO17 RX; 921600 baud |
| ESP32-S3 M.2 card | UART1: GPIO20 TX, GPIO21 RX, GPIO22 CTS, GPIO23 RTS; 921600 baud |
| Raspberry Pi host | USB-CDC through the UPS output USB-C port |
| Debug probe UART | J350: GPIO0 TX, GPIO1 RX; PIO UART at 921600 baud |
| Buttons | GPIO13 LEFT, GPIO14 RIGHT; active low |
| Buzzer | GPIO15 |
| Local ADC | GPIO28 battery voltage, GPIO29 output voltage |

The RP2040 is the routing hub. The wire format and payload structures are
shared through [../common/protocol.h](../common/protocol.h); see also the
[protocol guide](../common/protocol_desc.md). USB-CDC carries binary frames,
so a text-only serial monitor is not a telemetry decoder. Use the
[host service](https://github.com/Web3-Pi/Web3-Pi-UPS-Service) or
[Workbench](https://github.com/Web3-Pi/Web3-Pi-UPS-Workbench).

## Build

Install PlatformIO and put `pio` on `PATH` (a standard local installation also
provides `~/.platformio/penv/bin/pio`). Run from this directory:

```sh
pio run -e pico
```

Dependencies include Adafruit GFX, Adafruit SSD1306 and QRCode; PlatformIO
installs them from the committed configuration. Keep the platform commit and
arduino-pico core pins together when upgrading the toolchain.

Build artifacts are in `.pio/build/pico/`: `firmware.uf2`, `firmware.bin` and
`firmware.elf`. The `pico_swd` environment writes to `.pio/build/pico_swd/`.
[CI](../.github/workflows/firmware-ci.yml) builds the `pico` environment.

## Flashing

### SWD — Raspberry Pi Debug Probe

SWD keeps the Raspberry Pi 5 connected to the UPS output USB-C port.

- Wire the Debug Probe's **D** connector to **J401** (4-pin JST SH):
  SC → J401.3 SWCLK, GND → J401.4, SD → J401.2 SWDIO.
- Power the UPS from PD input, barrel jack or battery. The probe does not
  supply the board's 3.3 V rail.

```sh
pio run -e pico_swd -t upload
```

This uses OpenOCD and CMSIS-DAP; a successful upload reports verification.

### USB-C — picotool

Connect the development machine to the UPS **output** USB-C port for bench
flashing. The Raspberry Pi must be disconnected from that port during this
operation.

```sh
pio run -e pico -t upload
```

### Relayed Firmware Updates

The `net.fw_xfer_*` receiver in [src/fw_update.cpp](src/fw_update.cpp) accepts
a firmware image over WUPS. The existing firmware stages the new image and a
rollback snapshot in LittleFS. The new firmware confirms itself after valid
WUPS traffic; missing confirmation or repeated failed boots can trigger
rollback. Failure before the update code can run requires physical recovery.

Keep `board_build.filesystem_size = 1m` stable across compatible updates so
the staged image, rollback snapshot and pending-verification state remain at
the same flash location. Inspect the flash-layout notes in `platformio.ini`
before changing it.

## Source Layout

| File | Purpose |
|---|---|
| [src/main.cpp](src/main.cpp) | Hardware initialization, screens, telemetry and frame dispatch |
| [src/wups_router.cpp](src/wups_router.cpp) | Per-port deframing and WUPS routing |
| [src/local_menu.cpp](src/local_menu.cpp) | Local menu and ESP32 network-menu handoff |
| [src/ui_settings.cpp](src/ui_settings.cpp) | Persisted brightness/sound settings and migration |
| [src/trust_ui.cpp](src/trust_ui.cpp) | Prompts rendered for the ESP32 |
| [src/fw_update.cpp](src/fw_update.cpp) | Update staging, verification and rollback |

See the [main project README](../README.md) for hardware and the other firmware
targets.
