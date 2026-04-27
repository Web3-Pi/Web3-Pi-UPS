# Web3 Pi UPS — USB-PD Dual-Role Controller (CH32X035)

USB Power Delivery firmware for the **CH32X035 RISC-V microcontroller**, acting as the power management controller for the **Web3 Pi UPS v2** — a dedicated DC UPS for Raspberry Pi 5.

The MCU dynamically switches between **SOURCE** and **SINK** roles: it powers the Raspberry Pi by default (SOURCE), and renegotiates power from an upstream USB-C charger when one is connected (SINK).

## Features

- **USB-PD 3.0 dual-role** (SOURCE + SINK) on a single CC pair via an analog mux
- **SOURCE**: 4 fixed PDOs advertised to the Raspberry Pi
  - 5V @ 5A · 9V @ 3A · 12V @ 2.25A · 15V @ 1.8A
- **SINK**: negotiates 12–15V / ≥26W from an upstream USB-C charger
- **Raspberry Pi 5 PSU identification** — responds to RPi5 VDM discovery as a compatible 27W supply
- **Battery-backed power** — Sony NP-F series (NP-F550 / F770 / F970)
- **Programmable buck-boost output** via TPS55289 (I²C, voltage + current limit)
- **Charger control + telemetry** via MP2762A (input/battery V/I, charge state, faults)
- **JSON status** sent over UART at 921600 baud to the UI MCU (RP2040)

## Architecture (v2)

In v1 the UPS used two CH32X035 MCUs (one for SINK input, one for SOURCE output). v2 collapses both roles into a single CH32X035 with a 74LVC1G3157 analog mux on the CC lines:

```
                ┌────────────────────────┐
USB-C charger ──┤ SINK port              │
                │              ┌─────────┴────────┐
                │              │  74LVC1G3157     │   PDC_CC_SEL (PB3)
                │              │  CC mux          │◄──── 0 = SOURCE
                │              └─────────┬────────┘      1 = SINK
                │ SOURCE port            │
Raspberry Pi 5 ─┤◄───────────────────────┤
                └────────────────────────┘
                       CH32X035 PD PHY
```

Role transitions are driven by `PDC_CC_DET` (PB11, active-low host detect). When a charger is plugged in, the controller switches to SINK for a 500 ms negotiation window, then returns to SOURCE.

## Hardware

| Component | Part | Function |
|-----------|------|----------|
| MCU | CH32X035F8U6 | RISC-V RV32IMACXW, 62 KB Flash, 20 KB RAM |
| DC-DC | TPS55289 | Buck-boost converter, 3–30 V output, I²C-controlled |
| Charger | MP2762A | 2S Li-ion / Li-Po charger with ADC telemetry |
| Temp sensor | LM75B | Board temperature |
| CC mux | 74LVC1G3157 | Analog mux for SOURCE/SINK CC switching |
| I²C mux | 74LVC1G157 | Switches SYS_I²C between PD controller and RP2040 |

### Power Input

- USB-C Power Delivery (12–15 V negotiated via SINK role)
- Barrel jack (12–20 V DC)

### I²C Bus (SYS_I²C, PC18/PC19)

```
0x48 — LM75B    (temperature sensor)
0x5C — MP2762A  (battery charger)
0x75 — TPS55289 (buck-boost converter)
```

The 74LVC1G157 mux (controlled by `SYS_I²C_SEL`) lets the UI MCU (RP2040) take the bus when needed.

### Pin Mapping (CH32X035F8U6)

| Pin  | Function    | Description                                   |
|------|-------------|-----------------------------------------------|
| PA0  | ADC0        | VBUS_OUT_ADC                                  |
| PA1  | ADC1        | DC_INP_ADC_SRC                                |
| PA2  | USART2_TX   | PDC_SRC_TX → RP2040                           |
| PA3  | USART2_RX   | PDC_SRC_RX ← RP2040                           |
| PA4  | GPIO        | CHG_nINT (charger interrupt)                  |
| PA5  | ADC5        | PD_SRC_VBAT (battery voltage)                 |
| PA6  | GPIO        | DC_INP_EN_SRC                                 |
| PA7  | GPIO        | VBUS_OUT_EN                                   |
| PB0  | GPIO        | PDS_EN (TPS55289 enable)                      |
| PB3  | GPIO        | PDC_CC_SEL (CC mux: 0 = SOURCE, 1 = SINK)     |
| PB11 | GPIO        | PDC_CC_DET (host detect, active-low)          |
| PB12 | GPIO        | PDC_SRC_STAT (status LED)                     |
| PC14 | CC1         | PDC_SRC_CC1                                   |
| PC15 | CC2         | PDC_SRC_CC2                                   |
| PC16 | USB_N       | PDC_SRC_USB_N                                 |
| PC17 | USB_P       | PDC_SRC_USB_P                                 |
| PC18 | I²C_SCL     | SYS_I²C_SCL                                   |
| PC19 | I²C_SDA     | SYS_I²C_SDA                                   |

## Building

### Option 1 — MounRiver Studio 2

The project is set up for **[MounRiver Studio 2](http://mounriver.com) (MRS2) V2.4.0**. After cloning:

1. Open MRS2 → **File → Open Folder…** → select this `firmware-ch32x/` directory.
2. The Solution Explorer should populate from the committed `.project` / `.cproject` / `USB-PD.wvproj` files.
3. Build (Ctrl/Cmd + B). Artifacts go to `obj/`.
4. For flashing see **Flashing (macOS)** below — the project does not require a WCH-Link, the CH32X035 USB ISP bootloader is used directly. MRS2 generates a local `.mrs/launch.json` from `USB-PD.wvproj` on first run (it is git-ignored — contains your absolute path).

### Option 2 — Standalone GCC

With `riscv-none-embed-gcc` available in `PATH`:

```bash
cd obj
make            # build
make clean      # wipe artifacts
```

### Output Files

- `obj/USB-PD.elf` — ELF executable
- `obj/USB-PD.hex` — Intel HEX for flashing
- `obj/USB-PD.map` — memory map
- `obj/USB-PD.lst` — disassembly listing

### Toolchain Details

- Compiler: `riscv-none-embed-gcc` (or `riscv32-wch-elf-gcc` in newer MRS_Toolchain bundles)
- Architecture: RV32IMACXW (RISC-V with multiply, atomic, compressed, WCH custom extensions)
- ABI: ilp32
- Optimization: `-Os` (size)
- Linker script: `Ld/Link.ld`

## Flashing (macOS)

Programming uses the **CH32X035 built-in USB ISP bootloader** — no WCH-Link required. On macOS the open-source [`wchisp`](https://github.com/ch32-rs/wchisp) CLI handles erase / program / verify / reset.

### Prerequisites

```bash
# install wchisp (Rust toolchain required, or grab a release binary from the repo)
cargo install wchisp
wchisp --version    # tested with 0.3.0
```

### Entering boot mode

The CH32X035 enters USB ISP mode when **BOOT pin is pulled high during reset**. With the board powered off USB:

1. Hold the BOOT button (or jumper BOOT to VCC).
2. Press and release RESET.
3. Release BOOT.
4. The MCU enumerates as a WCH ISP device (USB VID `4348` / `1a86`, PID `55e0`).

Verify it is detected:

```bash
wchisp probe
# Device #0: CH32X035F8U6[0x5e23]
```

### First flash on a fresh CH32X035

Factory chips ship with **read-out protection enabled** (`RDPR ≠ 0xA5`), which causes `verify` to fail because the bootloader returns `0xFF` on read. Unprotect once per chip:

```bash
wchisp config unprotect
# NOTE: unprotect resets the device — re-enter boot mode before the next command.
wchisp flash obj/USB-PD.hex
```

`wchisp flash` performs erase → program → verify → reset in a single step.

### Subsequent flashes during development

Once unprotected, the chip stays unprotected across power cycles. Just enter boot mode and:

```bash
wchisp flash obj/USB-PD.hex
```

### Production flashing (50-unit run)

For shipped units, **keep RDPR engaged** to prevent end-user firmware readback. Skip `unprotect` entirely and bypass the readback check:

```bash
wchisp flash --no-verify obj/USB-PD.hex
```

The write itself still completes; only the verify pass (which would fail against a protected chip) is skipped.

### Helper script: `flash.sh`

A ready-made wrapper lives at [`firmware-ch32x/flash.sh`](flash.sh). Build the `.hex` in MRS2 first, then:

```bash
./flash.sh                    # Dev re-flash (chip already unprotected)
./flash.sh --first            # Fresh chip — runs `wchisp config unprotect` then flashes
./flash.sh --prod             # Production — flashes with --no-verify, RDPR stays engaged
./flash.sh --help             # Usage summary
```

The script waits for the MCU to enter boot mode before flashing, so for a 50-unit run the loop is: plug in next board → BOOT + RESET → script auto-detects and flashes → `✓ Flashed at HH:MM:SS`.

### Troubleshooting

| Symptom                                              | Cause / fix                                                                              |
|------------------------------------------------------|------------------------------------------------------------------------------------------|
| `No WCH ISP USB device found (4348:55e0 ...)`        | Not in boot mode. Re-do BOOT + RESET sequence.                                           |
| `Verify failed, mismatch` after `flash`              | RDPR engaged. Run `wchisp config unprotect` (then re-enter boot, re-flash), or use `--no-verify`. |
| Device disappears after `wchisp config unprotect`    | Expected — `unprotect` resets the chip. Re-enter boot mode and continue.                 |
| Data-only USB cable                                  | Use a known-good USB-C / USB-A data cable.                                               |

## JSON Status Protocol

The PD controller sends periodic status messages on USART2 (PA2/PA3 @ 921600 baud) to the RP2040 UI MCU:

```json
{"up":1234,"pd":18,"pdo":2,"cc":1,"role":1,"snk_ok":0,"snk_v":0,"snk_i":0,"t":392,"vs":51,"is":30,"vr":51,"ir":30,"bp":1,"cs":2,"pg":1,"vi":11850,"ii":681,"vb":8200,"ci":900,"cf":0}
```

| Field    | Description                              | Unit         |
|----------|------------------------------------------|--------------|
| `up`     | Uptime since boot                        | seconds      |
| `pd`     | PD state machine state                   | enum         |
| `pdo`    | Active / requested PDO index             | 1–4          |
| `cc`     | Sink connected                           | 0 / 1        |
| `role`   | Current role (0 = SINK, 1 = SOURCE)      | enum         |
| `snk_ok` | Last SINK negotiation succeeded          | 0 / 1        |
| `snk_v`  | Negotiated SINK voltage                  | 0.1 V        |
| `snk_i`  | Negotiated SINK current                  | 0.1 A        |
| `t`      | Board temperature (LM75B)                | 0.1 °C       |
| `vs`/`is`| VBUS voltage / current setpoint          | 0.1 V / 0.1 A|
| `vr`/`ir`| VBUS voltage / current readback          | 0.1 V / 0.1 A|
| `bp`     | Battery present (MP2762A UVLO)           | 0 / 1        |
| `cs`     | Charge state                             | enum         |
| `pg`     | Power good (input present)               | 0 / 1        |
| `vi`/`ii`| Charger input voltage / current          | mV / mA      |
| `vb`     | Battery voltage                          | mV           |
| `ci`     | Charge current                           | mA           |
| `cf`     | Charger fault flags                      | bitmask      |

**`pd` (PD state)** — selected values: `0` Idle · `1` Disconnected · `11` Sink connected · `12` Sending SRC_CAP · `13` Waiting REQUEST · `14` REQUEST received · `15` Sending ACCEPT · `17` Adjusting voltage · `18` Sending PS_RDY. Full list in `User/PD_Process.h`.

**`pdo` (PDO index)** — `1` 5V/5A · `2` 9V/3A · `3` 12V/2.25A · `4` 15V/1.8A.

**`cs` (charge state)** — `0` not charging · `1` trickle/pre-charge · `2` fast charge · `3` charge done.

**`cf` (fault flags, MP2762A REG14H)** — bit 7 watchdog · bit 6 OTG · bits 5:4 charge fault (input OVP / thermal / timer) · bit 3 battery OVP · bits 2:0 NTC fault.

## Project Structure

```
Core/           RISC-V core definitions
Debug/          UART debug printf utility (debug.c/h)
Ld/             Linker script (62 KB Flash, 20 KB RAM)
Peripheral/     CH32X035 HAL drivers
  inc/          peripheral headers
  src/          peripheral implementations
Startup/        startup assembly
User/           application code
  main.c             entry point, GPIO/I²C/UART init, main loop
  PD_Process.{c,h}   USB-PD state machine (dual-role)
  tps55289.{c,h}     buck-boost driver
  mp2762a.{c,h}      charger driver
  lm75b.{c,h}        temperature sensor driver
  i2c_lib.{c,h}      software-bitbang I²C
  ch32x035_it.{c,h}  interrupt handlers
  system_ch32x035.{c,h}  clock setup
USB-PD.wvproj   MounRiver Studio 2 project
.project        Eclipse / CDT project descriptor
.cproject       Eclipse / CDT build configuration
.template       MRS chip / toolchain metadata
```

## USB-PD Implementation Notes

- The PD PHY is integrated in the CH32X035 (BMC encoder/decoder + CC analog frontend).
- **Timing critical**: GoodCRC must be sent within 30 µs — handled in the `USBPD_IRQHandler`. Do not add blocking work in interrupts or the main loop.
- Detection in `PD_Detect()` is **role-aware**: SOURCE uses Rp pull-ups (CC_PU_330) and detects sink Rd; SINK uses Rd pull-downs and detects source Rp.
- For SOURCE mode the external pull-down on CC must be removed (otherwise sink detection misfires).
- Low-power STANDBY is **disabled** while dual-role is active so host detection on PB11 keeps running.

## SINK Negotiation Parameters (`PD_Process.h`)

| Constant               | Value     |
|------------------------|-----------|
| `SINK_MIN_VOLTAGE_MV`  | 12000     |
| `SINK_MAX_VOLTAGE_MV`  | 15000     |
| `SINK_MIN_POWER_MW`    | 26000     |
| `SINK_WINDOW_MS`       | 500       |

If the upstream charger does not offer a PDO meeting these limits within 500 ms, the controller returns to SOURCE mode and continues powering the Pi from the battery / barrel input.
