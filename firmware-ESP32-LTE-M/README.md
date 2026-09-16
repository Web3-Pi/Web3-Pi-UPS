# Web3 Pi UPS — firmware-ESP32-LTE-M

Firmware for the **W3P MODEM V1 LTE-M expansion card** for Web3 Pi UPS.
Provides cellular telemetry, authenticated commands and firmware updates over
MQTT, HTTP or Arkiv, independently of the Raspberry Pi's local network.

The current firmware uses **LTE Cat-M1 only**, on **B3 (1800 MHz) and B20
(800 MHz)**. NB-IoT selection is disabled. Version 0.8.16 adds
[bounded MQTT/TLS service and guarded modem recovery](docs/MQTT-RECOVERY-15-17.md).
The [0.8.15 release added SINR telemetry and fixed-APN build variants](docs/RELEASE-0.8.15.md).
For the underlying radio and recovery behavior, see the
[0.8.14 integration and validation notes](docs/RELEASE-0.8.14.md).

## Hardware

The committed defaults target the W3P MODEM V1 M.2 card:

- **MCU:** ESP32-S3FH4R2, 4 MB flash; PSRAM is disabled.
- **Modem:** SIMCom SIM7080G, controlled over UART1 at 115200 baud.
- **Power:** hardware supplies the modem's 3.8 V rail; this card has no AXP2101
  PMU. The LilyGo PMU path is disabled.
- **UPS link:** UART2 carries the WUPS protocol to the RP2040.

| Modem signal | ESP32 GPIO |
|---|---|
| PWRKEY control | 1 |
| ESP32 TX → modem RX | 2 |
| ESP32 RX ← modem TX | 4 |
| DTR, held low | 5 |
| RI, currently unused by firmware | 6 |

The M.2 connector uses a custom pinout. The earlier
[LilyGo board notes](docs/info.md) and
[LilyGo schematic](docs/T-SIM7080G_Schematic.pdf) are prototype references;
their flash size, GPIO assignments and PMU setup differ from this target.

## Current state

The firmware establishes PPP using `esp_modem`, supervises the selected backend,
exchanges telemetry and commands with the RP2040, and supports ESP32 HTTPS OTA
and relayed RP2040 updates. MQTT producers use bounded application queues;
an independent monitor checks fresh publication proof. ESP32 OTA has bounded
download retries, checked HTTP Range resumption and rollback protection.

The modem's radio configuration is read back and verified before registration.
`CSCLK=0`, `CPSMS=0` and disabled LTE-M eDRX keep the UART and data path available.
The firmware reads these settings before changing them, avoids redundant
persistent writes, and verifies the active PSM/eDRX state after registration.

The default APN profile uses the existing fleet classification: the five
known legacy SIM ICCIDs use `iot.1nce.net`; other SIMs use `sensor.net`. A
fixed-APN build overrides this classification. In either case, the selected
APN is applied to the DCE and `AT+CGDCONT` before PPP and retained on retries.
The profiles below are separate build choices; the firmware does not switch
between APNs automatically after a connection failure.

Version 0.8.15 adds SINR to `net.status` v3, alongside RSRP, RSRQ and RSSI.
Missing or invalid SINR uses the signed `-128` sentinel; `0 dB` is a valid
reading. MQTT and Arkiv carry the extended frame, and HTTP includes `sinr_db`
when available. A compatible panel is needed to display the new measurement.

Full `AT+CEREG?`, `AT+COPS?` and `AT+CPSI?` replies are logged at startup and
during CMUX supervision. Additional diagnostics include `AT+CGDCONT?`,
`AT+CPSMS?`, `AT+CPSMRDP`, `AT+CEDRXS?`, `AT+CEDRX?`, `AT+CEDRXRDP`,
`AT+CSCLK?`, `AT+CPSMCFG?` and `AT+CPSMCFGEXT?`. Captures are bounded and report
completion/truncation status. UTC correlation is labelled synchronized only
after SNTP confirmation; earlier captures are marked unsynchronized.
Optional diagnostics yield to OTA and PPP loss. Plain DATA-mode fallback has
no concurrent AT channel, so periodic modem diagnostics are unavailable there.

See [MQTT resilience and its verification limits](docs/MQTT-RESILIENCE.md) for
queue ownership, recovery and image-confirmation behavior. Short bench tests
and host fault injection do not establish that field LTE outages are resolved.

## Requirements

- **ESP-IDF v6.0.2**, with its ESP32-S3 compiler and Python dependencies.
- macOS / Linux. Windows untested (Bash wrappers, POSIX signals).
- For flashing or monitoring, USB access to the ESP32-S3 programming/console
  interface on the W3P MODEM V1 card.

## Build / flash / monitor

From this directory, with ESP-IDF v6.0.2 activated:

```sh
idf.py set-target esp32s3       # first build in a fresh checkout
idf.py build
```

`tools/idf` is a local wrapper around the same commands. It currently refers to
the maintainer's EIM installation and v6.0.2 checkout; use an activated `idf.py`
directly on another machine. The wrapper coordinates with `tools/serial-monitor`
when an operation needs the serial port:

```sh
# Pane A: serial monitor in the foreground (log → logs/serial.log)
tools/serial-monitor --truncate

# Pane B: build + flash (the monitor yields the port automatically and reconnects after the flash)
tools/idf build
tools/idf flash
```

The generated `sdkconfig` is ignored by Git. For a clean build from the committed
defaults, preserve any intentional local overrides, remove the stale generated
configuration, then reconfigure. Verify **4 MB flash**, the custom two-slot
partition table, rollback support and the incremental MQTT packet-ID option.

The component lock and local MQTT/HTTPS OTA components are part of the build.
Do not remove the local overrides or change their source hashes to bypass a
configuration error; the patches and adapter must be reviewed together.

Production Arkiv images require the locally supplied, gitignored
`main/arkiv_ws_token.h`. Fresh public checkouts and CI compile with the committed
placeholder instead: WSS push cannot connect and command handling falls back
to HTTP polling. Keep the real header out of commits and source archives.

### APN Build Profiles

Use separate build directories so the CMake cache keeps each profile isolated.
From this directory with ESP-IDF activated:

```sh
# Default fleet classification (legacy ICCID list -> 1nce, other SIMs -> sensor)
idf.py -B build-auto -DPROJECT_VER=0.8.16 -DWUPS_FIXED_APN= build

# Always use iot.1nce.net, independent of ICCID
idf.py -B build-1nce -DPROJECT_VER=0.8.16-1nce -DWUPS_FIXED_APN=iot.1nce.net build

# Always use sensor.net, independent of ICCID
idf.py -B build-sensor -DPROJECT_VER=0.8.16-sensor -DWUPS_FIXED_APN=sensor.net build
```

Each directory contains `firmware-ESP32-LTE-M.bin`, the application image for
the existing two-slot OTA layout. The version suffix is also reported in the
runtime `esp32:` identity. A fixed-APN image must match the intended SIM's data
service. Neither profile includes per-device provisioning or changes APN
when registration or data transfer fails. See the
[0.8.15 release notes](docs/RELEASE-0.8.15.md) for validation scope.

### One-time migration to the two-OTA partition table (OTA-1)

The first flash of the OTA-1 layout (`ota_0`/`ota_1` + `otadata`) on a unit
that ran the old single-`factory` table **must erase the old `nvs` region
first**: the partition shrank 0x6000 → 0x4000 in place and `idf.py flash`
never touches it, so a truncated wear-leveled NVS can silently resurrect a
stale WS-9 anti-replay counter (`last_ctr`) or trip
`ESP_ERR_NVS_NO_FREE_PAGES`, which auto-erases all runtime state (including
`backend_mode`). With the serial monitor stopped (or yielded):

```sh
ESPPORT=/dev/cu.usbmodemXXXX  # replace with the connected device
esptool.py --port "$ESPPORT" erase_region 0x9000 0x6000
idf.py -p "$ESPPORT" flash
```

**Never** use `erase_flash` on a provisioned unit — it wipes the per-device
`prov` partition at 0x310000. After the `erase_region`, cmdauth reseeds
`cur_epoch` from `prov` and `last_ctr` restarts at 0 (the panel's counter is
monotonically higher), so no re-provisioning is needed. This jump cannot be
taken over the air; fielded factory-layout devices need this USB flash once.

Reset the currently installed firmware without reflashing (configuration changes
require a rebuild and flash before they take effect):

```sh
tools/reset
```

## Layout

```
firmware-ESP32-LTE-M/
├── CMakeLists.txt           # top-level ESP-IDF project
├── README.md                # this file
├── version.txt              # ESP-IDF application version
├── components/              # local components, including reviewed SDK patches
├── main/
│   ├── CMakeLists.txt
│   ├── main.c               # application initialization and supervision
│   ├── modem.c              # SIM7080G, registration, PPP and diagnostics
│   ├── mqtt.c               # SDK owner, producer queues and health monitor
│   └── fw_ota.c             # ESP32 OTA and RP2040 update relay
├── docs/
│   ├── RELEASE-0.8.15.md     # SINR telemetry and fixed-APN variants
│   ├── RELEASE-0.8.14.md     # integrated changes and validation evidence
│   ├── MQTT-RESILIENCE.md    # behavior, tests and remaining hardware gates
│   ├── info.md              # historical LilyGo prototype reference
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

- [0.8.15 release notes](docs/RELEASE-0.8.15.md) — SINR telemetry and fixed-APN variants
- [0.8.14 integration notes](docs/RELEASE-0.8.14.md) — modem/PPP/MQTT/OTA changes and validation
- [MQTT resilience](docs/MQTT-RESILIENCE.md) — ownership, proof, recovery and tests
- [MQTT stopped-task adapter](../docs/mqtt-sdk-adapter.md) — pinned SDK contract
- [Local MQTT patch](components/espressif__mqtt/WEB3PI_PATCH.md) and
  [local HTTPS OTA patch](components/esp_https_ota/PROVENANCE.md) — provenance and regressions
- [docs/info.md](docs/info.md) — historical LilyGo prototype pinout and power domains
- [docs/T-SIM7080G_Schematic.pdf](docs/T-SIM7080G_Schematic.pdf) — LilyGo prototype schematic
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
