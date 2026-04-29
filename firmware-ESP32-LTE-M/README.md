# Web3 Pi UPS — firmware-ESP32-LTE-M

Firmware for the **LTE-M expansion card** for Web3 Pi UPS.
Provides independent cellular connectivity (NB-IoT / Cat-M) so the UPS
can keep talking to the internet even when the local network goes down.

## Hardware

Prototyped on the [LilyGo T-SIM7080G-S3](https://lilygo.cc/en-us/products/t-sim7080-s3) dev board:

- **MCU**: ESP32-S3-WROOM-1 (Xtensa, 16 MB Flash, OPI PSRAM)
- **Modem**: SimCom SIM7080G (NB-IoT + Cat-M1, **no** 2G/3G/4G fallback)
- **PMU**: X-Powers AXP2101 (I²C, Li-Ion charging, solar input, controls every rail)
- **SIM**: 1nce (M2M)

After we're done prototyping on the dev board, our hardware engineer will
design a dedicated **M.2 2232** card that plugs into the Web3 Pi UPS expansion
slot (custom pinout — not NVMe). Communication with the RP2040 inside the
UPS goes over UART.

Pinout, power-domain notes, datasheet warnings, and peripheral datasheets:
[docs/info.md](docs/info.md).

## Current state

Skeleton:
- boots on ESP32-S3,
- minimal init (chip info + a heartbeat every 5 s),
- the full AI ↔ device dev loop works end-to-end (build + flash + monitor + log
  driven from Claude Code; see [docs/dev-loop.md](docs/dev-loop.md)).

Roadmap:
1. **PMU init** — AXP2101 over I²C: DC3 (modem main, 3.0 V), BLDO1 (level shifter, 3.3 V — **must not be turned off**), TS-pin disabled.
2. **Modem power-on** — pulse PWRKEY on GPIO41, bring up UART1 @ 115200 on pins 4/5.
3. **AT pass-through** — bridge USB-CDC ⇄ UART1 for hands-on AT exploration.
4. **PPP** — via the `esp_modem` component from ESP-IDF (supports SIM7080G as a SIM7000/SIM7070 variant); `esp_netif` provides a ready-to-use IP interface.
5. **MQTT** — via `esp-mqtt` over the PPP TCP/IP stack.
6. **Arkiv** — on the same stack.

## Requirements

- **ESP-IDF v6.0** installed via [EIM](https://github.com/espressif/idf-im-cli).
- macOS / Linux. Windows untested (Bash wrappers, POSIX signals).
- LilyGo T-SIM7080G-S3 connected over **USB-C** (the ESP32-S3 programming port — *not* the Micro-USB which goes straight to the modem).

## Build / flash / monitor

We use local copies of the tools from [ESP32-Ai-Dev-Loop](../../ESP32-Ai-Dev-Loop/) (separate repo, MIT-licensed). Full description in [docs/dev-loop.md](docs/dev-loop.md).

```sh
# Pane A: serial monitor in the foreground (log → logs/serial.log)
tools/serial-monitor --truncate

# Pane B: build + flash (the monitor yields the port automatically and reconnects after the flash)
tools/idf set-target esp32s3       # once, on the first build after cloning
tools/idf build
tools/idf flash
```

If you have a stale `sdkconfig` from before `sdkconfig.defaults` landed
(symptom: `flash=2 MB` in the boot banner instead of 16 MB), delete it
and run `tools/idf reconfigure` to regenerate it from the defaults.

Reset without reflashing (e.g. after a menuconfig change that affects hardware setup):

```sh
tools/reset
```

For an AI client, the Claude Code MCP integration is configured at the
monorepo level in `Web3-Pi-UPS-Mono-Repo/.mcp.json`. In a Claude Code
session you'll see native tool calls
`mcp__esp-idf-lte-m__build_project`, `flash_project`, `set_target`,
`clean_project`.

## Layout

```
firmware-ESP32-LTE-M/
├── CMakeLists.txt           # top-level ESP-IDF project
├── README.md                # this file
├── main/
│   ├── CMakeLists.txt
│   └── main.c               # entry point + heartbeat (skeleton)
├── docs/
│   ├── info.md              # board overview, pinout, power domains, datasheet notes
│   ├── dev-loop.md          # pointer to ESP32-Ai-Dev-Loop
│   ├── datasheets/          # PDFs: ESP32-S3-WROOM + SIM7080G (AT, MQTT, TCP/UDP, SSL, SPEC)
│   ├── T-SIM7080G_Schematic.pdf
│   └── LilyGo-T-SIM7080G/   # optional local clone of the upstream repo (gitignored)
└── tools/                   # local copies of the ESP32-Ai-Dev-Loop tools
    ├── idf                  # idf.py wrapper + serial port coordination with the monitor
    ├── idf-mcp-server       # launcher for `idf.py mcp-server`
    ├── serial_monitor.py    # core: serial monitor with auto-reconnect + log file
    ├── serial-monitor       # monitor launcher (uses the IDF venv Python)
    └── reset                # RTS pulse to reset the chip without reflashing
```

## Documentation

In this repo:

- [docs/info.md](docs/info.md) — board overview, pinout, power domains, datasheet warnings
- [docs/dev-loop.md](docs/dev-loop.md) — pointer to [ESP32-Ai-Dev-Loop](../../ESP32-Ai-Dev-Loop/), full description of the dev loop
- [docs/T-SIM7080G_Schematic.pdf](docs/T-SIM7080G_Schematic.pdf) — board schematic (handy for verifying pinout / power domains)
- [docs/datasheets/](docs/datasheets/) — the datasheets we actually use:
  - [ESP32-S3-WROOM-1/1U Datasheet](docs/datasheets/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
  - [SIM7080G AT Command Manual V1.05](docs/datasheets/SIM7070_SIM7080_SIM7090_AT_Command_Manual_V1.05.pdf) — the canonical AT reference
  - [SIM7080G MQTT(S) Application Note V1.03](docs/datasheets/SIM7070_SIM7080_SIM7090_MQTTS_Application_Note_V1.03.pdf)
  - [SIM7080G TCP/UDP(S) Application Note V1.03](docs/datasheets/SIM7070_SIM7080_SIM7090_TCPUDPS_Application_Note_V1.03.pdf)
  - [SIM7080G SSL Application Note V1.00](docs/datasheets/SIM7070_SIM7080_SIM7090_SSL_Application_Note_V1.00.pdf)
  - [SIM7080 Series Spec](docs/datasheets/SIM7080_Series_SPEC_20200427.pdf)

Upstream reference (not committed to this repo, but useful):

- [LilyGo-T-SIM7080G](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G) — official board repo: full set of Arduino examples (AT, MQTT, MQTTS, GPS, sleep modes, NB-IoT), 30+ datasheets (Chinese editions, FOTA, GNSS, low-power), pre-built firmware, libraries (TinyGSM, XPowersLib).
  Clone it locally into `docs/LilyGo-T-SIM7080G/` (gitignored) if you need offline access:
  ```sh
  git clone https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G.git docs/LilyGo-T-SIM7080G
  ```
