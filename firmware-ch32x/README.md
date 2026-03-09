# Web3 Pi UPS - USB-PD Source Controller

USB Power Delivery source firmware for the **CH32X035 RISC-V microcontroller**, designed as the power management controller for the **Web3 Pi UPS** - a dedicated DC UPS for Raspberry Pi 5.

## Features

- **USB-PD 3.0 Source** with 4 fixed PDOs:
  - 5.1V @ 5A (25.5W)
  - 9V @ 3A (27W)
  - 12V @ 2.25A (27W)
  - 15V @ 1.8A (27W)
- **Raspberry Pi 5 compatibility** - responds to RPi5's vendor-defined messages for 27W PSU identification
- **Battery-backed power** - supports Sony NP-F series batteries (NP-F550/F770/F970)
- **Intelligent charging** - MP2762A charger IC with temperature monitoring
- **Fuel gauge integration** - MAX17261 for accurate battery state-of-charge
- **Real-time status reporting** - JSON telemetry via UART to UI controller

## Hardware

| Component | Part | Function |
|-----------|------|----------|
| MCU | CH32X035F8U6 | RISC-V RV32IMAC, 62KB Flash, 20KB RAM |
| DC-DC | TPS55289 | Buck-boost converter, 3-30V output |
| Charger | MP2762A | 2S Li-ion/Li-Po charger |
| Fuel Gauge | MAX17261 | Battery SoC monitoring |
| Temp Sensor | LM75B | Board temperature |

### Pin Mapping

| Pin | Function | Description |
|-----|----------|-------------|
| PA0 | ADC | VBUS output voltage sense |
| PA2/PA3 | USART2 | TX/RX to UI MCU (RP2040) |
| PA7 | GPIO | VBUS output enable |
| PB0 | GPIO | TPS55289 enable |
| PB12 | GPIO | Status LED |
| PC14/PC15 | CC1/CC2 | USB-PD CC lines |
| PC18/PC19 | I2C | System I2C bus |

## Building

### Prerequisites

- MounRiver Studio (MRS) toolchain or
- `riscv-none-embed-gcc` toolchain

### Output Files

- `USBPD_SRC.elf` - ELF executable
- `USBPD_SRC.hex` - Intel HEX for flashing
- `USBPD_SRC.map` - Memory map

## Project Structure

```
├── Core/           # RISC-V core definitions
├── Debug/          # Debug UART utilities
├── Ld/             # Linker script (62KB Flash, 20KB RAM)
├── Peripheral/     # CH32X035 HAL drivers
│   ├── inc/        # Peripheral headers
│   └── src/        # Peripheral implementations
├── Startup/        # Startup assembly
└── User/           # Application code
    ├── main.c          # Entry point, GPIO init
    ├── PD_Process.c    # USB-PD state machine
    ├── tps55289.c      # DC-DC control
    ├── max17261.c      # Fuel gauge driver
    ├── mp2762a.c       # Charger driver
    ├── lm75b.c         # Temperature sensor
    ├── ups_status.c    # JSON status reporting
    └── pdc_uart.c      # UART communication
```

## JSON Status Protocol

The controller sends periodic status updates to the UI MCU at 115200 baud:

```json
{"up":1234,"pd":18,"pdo":2,"cc":1,"t":392,"vs":90,"is":30,"vr":90,"ir":30,"soc":85,"bv":8200,"ba":-1500,"cs":0,"pg":0,"vi":0,"ii":0,"ci":0,"cf":0}
```

| Field | Description | Unit |
|-------|-------------|------|
| `up` | Uptime | seconds |
| `pd` | PD state machine state | enum |
| `pdo` | Active PDO index | 1-4 |
| `cc` | Sink connected | 0/1 |
| `t` | Board temperature | 0.1°C |
| `vs`/`is` | VBUS voltage/current setpoint | 0.1V / 0.1A |
| `vr`/`ir` | VBUS voltage/current readback | 0.1V / 0.1A |
| `soc` | Battery state of charge | % |
| `bv` | Battery voltage | mV |
| `ba` | Battery current (+charge/-discharge) | mA |
| `cs` | Charge state (0=off, 1=pre, 2=fast, 3=done) | enum |
| `pg` | Power good (input present) | 0/1 |
| `vi`/`ii` | Charger input voltage/current | mV / mA |
| `ci` | Charge current | mA |
| `cf` | Charger fault flags | bitmask |

## USB-PD Implementation

### State Machine

The PD source follows USB-PD 3.0 specification:

1. **Idle** - Waiting for sink connection (CC detection)
2. **Sink Connected** - CC pull-down detected, apply Rp
3. **Send SRC_CAP** - Advertise available PDOs
4. **Wait REQUEST** - Sink selects desired PDO
5. **Send ACCEPT** - Acknowledge selection
6. **Voltage Transition** - Adjust TPS55289 output
7. **Send PS_RDY** - Power supply ready

### Raspberry Pi 5 Support

The firmware responds to RPi5's vendor-defined discovery message, identifying itself as a compatible 27W power supply. This enables full 5A operation on Raspberry Pi 5.

## I2C Bus Architecture

```
SYS_I2C Bus (PC18/PC19)
    │
    ├── 0x36: MAX17261 (Fuel Gauge)
    ├── 0x48: LM75B (Temperature)
    ├── 0x5C: MP2762A (Charger)
    └── 0x75: TPS55289 (DC-DC)
```

The I2C mux (74LVC1G157) allows the UI MCU (RP2040) to access devices when `SYS_I2C_SEL` (PA5) is asserted.

## Low Power Operation

When no sink is connected, the controller enters STANDBY mode to conserve power. Wake-up occurs via:
- GPIO interrupt on CC line changes (default)
- USB-PD peripheral wake-up (alternative)

## Development Notes

- **Timing critical**: USB-PD requires GoodCRC response within 30μs
- **No CC pull-downs**: For source mode, remove any external pull-down resistors on CC lines
- **Debug UART**: 921600 baud on dedicated debug pins
- **Memory constrained**: Use static allocation, avoid dynamic memory
