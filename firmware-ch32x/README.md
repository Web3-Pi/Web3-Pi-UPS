# Web3 Pi UPS — CH32X035 Power Controller

USB-PD output and power-management firmware for **Web3 Pi UPS hardware
rev.3**. The current source reports **`ch32x:1.1.1`**.

## Architecture

The **CH32X035** controls the USB-C output as a PD source, the TPS55289
buck-boost converter and the MP2762A battery charger. The separate **HUSB238**
handles USB-C input negotiation. CH32X reads the HUSB238 source capabilities
and requests a suitable profile over I²C.

```text
USB-C charger -> HUSB238 input negotiation --+
DC barrel input ----------------------------+-> power path / battery charger
Sony NP-F battery --------------------------+-> TPS55289 -> USB-C output -> Pi
                                                   ^
                               CH32X035: output PD + power control
                                                   |
                                      USART2 / binary WUPS
                                                   |
                                             RP2040 router
```

Earlier board revisions shared SOURCE/SINK duties through a CC mux. The
current main loop no longer runs that role-switching manager: on rev.3, input
PD belongs to HUSB238 and CH32X's PD state machine serves the output port.

## Features

- Four output PD profiles: **5 V / 5 A, 9 V / 3 A, 12 V / 2.25 A,
  15 V / 1.8 A**, with Raspberry Pi 5 PSU identification.
- HUSB238 input profile selection in the **9–20 V** range: prefer a profile
  supplying at least 45 W in the order **15, 12, 18, 20, 9 V**. For weaker
  sources, select near the highest available power with the same voltage
  preference. See [User/husb238.h](User/husb238.h) for the complete policy.
- MP2762A charging and telemetry for a protected 2S Sony NP-F battery pack.
- Voltage/current setpoints, input contract, charger status and fault reporting.
- Binary WUPS status/events and output enable, disable, cycle and reset commands.

## Hardware Interfaces

| Component | Function | I²C address |
|---|---|---|
| HUSB238 | USB-C input PD negotiation | `0x08` |
| LM75B | Board temperature | `0x48` |
| MP2762A | Battery charger and ADC telemetry | `0x5C` |
| TPS55289 | Buck-boost output converter | `0x75` |

The I²C bus uses **PC18 SCL / PC19 SDA**. The RP2040 link is **USART2**, with
**PA2 TX / PA3 RX at 921600 baud**. See [User/main.c](User/main.c) for the
current GPIO map and the [main README](../README.md) for hardware files.

## Building

### Option 1 — MounRiver Studio 2

The project is set up for **[MounRiver Studio 2](http://mounriver.com) (MRS2) V2.4.0**. After cloning:

1. Open MRS2 → **File → Open Folder…** → select this `firmware-ch32x/` directory.
2. The Solution Explorer should populate from the committed `.project` / `.cproject` / `USB-PD.wvproj` files.
3. Build (Ctrl/Cmd + B). Artifacts go to `obj/`.
4. For flashing see **Flashing (macOS)** below — the project does not require a WCH-Link, the CH32X035 USB ISP bootloader is used directly. MRS2 generates a local `.mrs/launch.json` from `USB-PD.wvproj` on first run (it is git-ignored — contains your absolute path).

### Option 2 — Standalone GCC

Use the **WCH** `riscv-none-embed-gcc` toolchain (GCC 8.2 in the CI
MounRiver V1.92 bundle), which supports `rv32imacxw` and WCH interrupt
attributes. Run from this directory:

```bash
make            # build into build/
# Or select the WCH toolchain explicitly:
make CROSS=/path/to/toolchain/bin/riscv-none-embed-
make clean      # remove command-line build artifacts
```

### Output Files

| Build route | Artifacts |
|---|---|
| Committed [Makefile](Makefile), used by CI | `build/USB-PD.elf`, `build/USB-PD.hex`, `build/USB-PD.map` |
| MounRiver Studio 2 | `obj/USB-PD.elf`, `obj/USB-PD.hex`, `obj/USB-PD.map`, `obj/USB-PD.lst` |

The commands below use the MRS2 `obj/` image. Substitute `build/USB-PD.hex`
when flashing a command-line build directly with `wchisp`. The `flash.sh`
helper currently reads `obj/USB-PD.hex` only.

### Toolchain Details

- Compiler: `riscv-none-embed-gcc` (or `riscv32-wch-elf-gcc` in newer MRS_Toolchain bundles)
- Architecture: RV32IMACXW (RISC-V with multiply, atomic, compressed, WCH custom extensions)
- ABI: ilp32
- Optimization: `-Os` (size)
- Linker script: `Ld/Link.ld`

## Flashing (macOS)

Programming uses the **CH32X035 built-in USB ISP bootloader** — no WCH-Link required. On macOS the open-source [`wchisp`](https://github.com/ch32-rs/wchisp) CLI handles erase / program / verify / reset.

### Prerequisites

This project uses a **locally-patched fork of `wchisp`** that adds a `flash --unprotect` flag (`-U`). The flag does WRITE_CONFIG → ISP_KEY → ERASE → PROGRAM → VERIFY in a **single USB session**, mirroring what WCHISPTool's GUI does. With upstream `wchisp` 0.3.0 you have to run `wchisp config unprotect` separately, which sends `IspEnd(1)` and resets the MCU out of the bootloader, forcing a second BOOT+RESET — the patch eliminates that.

The patch is not bundled in this repository. A public checkout of upstream
`wchisp` does not by itself provide the `--unprotect` flag. If using the
maintainer's patched build, verify the capability first:

```sh
wchisp flash --help | grep -- --unprotect
```

With stock upstream, use the documented two-step first-flash sequence below.
The `flash.sh --first` helper requires the patched build and checks for its
flag. Ordinary re-flashing does not require that patch.

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

Factory chips ship with **read-out protection enabled** (`RDPR = 0xFF`, not `0xA5`). Without unprotecting first, `wchisp flash` writes the firmware fine but **`verify` fails with `mismatch`** because the bootloader returns `0xFF` on read.

With the **patched `wchisp`** (see Prerequisites), one BOOT+RESET is enough:

```bash
wchisp flash --unprotect obj/USB-PD.hex
```

This sends WRITE_CONFIG (unprotect) → ISP_KEY → ERASE → PROGRAM → VERIFY → RESET in one USB session — same flow as WCHISPTool GUI.

> **Stock upstream `wchisp` (no patch)** — you'd need two CLI calls and **two** BOOT+RESET sequences:
> ```bash
> wchisp config unprotect              # sends IspEnd(1) → MCU resets out of bootloader
> # BOOT + RESET again here
> wchisp flash obj/USB-PD.hex
> ```

### Subsequent flashes during development

Once unprotected, the chip stays unprotected across power cycles. BOOT + RESET, then:

```bash
wchisp flash obj/USB-PD.hex
```

### Production flashing

For a chip that still has **RDPR engaged**, the existing production flow
keeps that protection and bypasses readback verification:

```bash
wchisp flash --no-verify obj/USB-PD.hex
```

This skips the readback verification pass and leaves existing protection
unchanged. It does **not** enable protection on an already-unprotected chip;
check the device configuration as part of production programming.

### Helper script: `flash.sh`

A ready-made wrapper lives at [`firmware-ch32x/flash.sh`](flash.sh). Build the `.hex` in MRS2 first, then:

```bash
./flash.sh                    # Dev re-flash (chip already unprotected)
./flash.sh --first            # Fresh chip — single-session unprotect + flash (one BOOT+RESET).
                              # Requires patched wchisp; the script aborts if the --unprotect flag is missing.
./flash.sh --prod             # Production — flashes with --no-verify, RDPR stays engaged
./flash.sh --help             # Usage summary
```

The script waits for the MCU to enter boot mode before flashing, so the batch loop is: plug in next board → BOOT + RESET → script auto-detects and flashes → `✓ Flashed at HH:MM:SS`.

### Troubleshooting

| Symptom                                              | Cause / fix                                                                              |
|------------------------------------------------------|------------------------------------------------------------------------------------------|
| `No WCH ISP USB device found (4348:55e0 ...)`        | Not in boot mode. Re-do BOOT + RESET sequence.                                           |
| `Verify failed, mismatch` after `flash` on a fresh chip | RDPR engaged. Use `wchisp flash --unprotect` (patched build) or `wchisp config unprotect` + re-enter boot + `wchisp flash` (upstream), or `--no-verify` for production. |
| `flash.sh --first` aborts with "this wchisp build doesn't support 'flash --unprotect'" | `~/.cargo/bin/wchisp` is the upstream build. Use a build containing the local patch, or the upstream two-step sequence above. |
| Device disappears after `wchisp config unprotect`    | Expected — upstream `unprotect` resets the chip via `IspEnd(1)`. Re-enter boot mode, or use the patched `flash --unprotect` instead. |
| Charge-only USB cable                                  | Use a known-good USB-C / USB-A data cable.                                               |

## Binary Status and Commands

The current firmware uses **WUPS wire protocol v1**. It sends
`power.status` **v2** once per second to the RP2040 and emits power events
for mains changes, faults and charge thresholds. Status includes input/output
measurements, the HUSB238 input contract, battery/charger state, temperatures
and fault flags. The old JSON interface is no longer the command/status API.

The authoritative payload definitions are in
[../common/protocol.h](../common/protocol.h), included by
[User/wups_proto.h](User/wups_proto.h). The
[protocol guide](../common/protocol_desc.md) describes addressing, frame
checksums and routing. Diagnostic ASCII may share the UART with framed data;
receivers must synchronize to valid WUPS frames rather than parse lines.

Supported requests include `system.ping`, `system.status_query` and
`power.enable`, `power.disable`, `power.cycle`, `power.reset`. Power-control
commands can interrupt the Raspberry Pi's supply. Coordinate an OS shutdown
through the host service before deliberately cutting power.

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
  PD_Process.{c,h}   USB-PD output state machine
  husb238.{c,h}      input PD capabilities, profile selection and telemetry
  wups_proto.h      shared binary protocol include
  tps55289.{c,h}     buck-boost driver
  mp2762a.{c,h}      charger driver
  lm75b.{c,h}        temperature sensor driver
  i2c_lib.{c,h}      software-bitbang I²C
  ch32x035_it.{c,h}  interrupt handlers
  system_ch32x035.{c,h}  clock setup
Makefile        headless build into build/
USB-PD.wvproj   MounRiver Studio 2 project
.project        Eclipse / CDT project descriptor
.cproject       Eclipse / CDT build configuration
.template       MRS chip / toolchain metadata
```

## USB-PD Implementation Notes

- The output PD PHY is integrated in CH32X035. Timing-sensitive responses
  share the main loop with charger and converter polling; avoid adding
  blocking work to interrupt handlers or between PD service calls.
- The HUSB238 input contract is read back after attach. USB-C detach re-arms
  profile selection, independently of barrel-input power.
- The rev.3 board's `PDS_EN` pull-down means a CH32X reset can interrupt the
  output supply. Do not assume a controller reset or flash is transparent
  to a connected Raspberry Pi.

See [the battery-mode telemetry fix](../docs/ch32x-battery-mode-telemetry-fix.md)
for the distinction between charger readings and battery-only operation.
