# Web3 Pi UPS

The newest member of the [Web3 Pi](https://www.web3pi.io) ecosystem, built to ensure perfect reliability of your Ethereum node around the clock. Essential for Solo Staking, it shields your setup from any power disruption — no matter how brief. When the power goes out, your node stays up — and when the battery runs low, it shuts down safely.

**Open-source DC Uninterruptible Power Supply for Raspberry Pi 5** — USB-PD power delivery up to 27W, swappable Sony NP-F batteries, OLED status display, and a 3D-printed enclosure.

<p align="center">
  <img src="docs/images/renders/rpi_ups2.png" alt="Web3 Pi UPS with Raspberry Pi" width="550">
</p>

## Key Features

- **USB-PD Source** — 5V/9V/12V/15V output, Raspberry Pi 5 compatible (27W, 5A)
- **Sony NP-F battery** — hot-swappable NP-F550/F770/F970
- **Intelligent charging** — MP2762A 2S Li-ion charger with temperature monitoring (LM75B)
- **Adjustable DC-DC** — TPS55289 buck-boost converter, 3-30V range
- **OLED UI panel** — SSD1306 display with two buttons for status and settings
- **Dual MCU architecture** — CH32X035 (power/PD) + RP2040 (UI/monitoring)
- **3D-printed enclosure** — parametric STEP model, STL files

<p align="center">
  <img src="docs/images/renders/bez_bat.png" alt="Web3 Pi UPS - battery bay" width="360">
  <img src="docs/images/renders/razem-3.png" alt="Web3 Pi UPS - multiple units" width="360">
</p>

## Repository Structure

```
firmware-ch32x/                     CH32X035 firmware — USB-PD source, power management
firmware-rp2040/                    RP2040 firmware — OLED UI, status display, buttons
service/                            Linux system service (planned)
hardware/
├── electronics/
│   ├── main-board/                 Main power board — schematic, STEP, Gerber, BOM, PnP
│   └── ui-panel/                   OLED + buttons panel — schematic, STEP, Gerber, BOM
└── enclosure/                      3D-printed enclosure — STEP assembly, STL parts
docs/images/                        Renders and photos
```

## Electronics

### Main Board

4-layer PCB with USB-PD controller, charger, DC-DC converter, and NP-F battery connector.

<p align="center">
  <img src="docs/images/Main-Board-Top.png" alt="Main Board - top" width="320">
  <img src="docs/images/Main-Board-Bottom.png" alt="Main Board - bottom" width="320">
  <img src="docs/images/Main-Board-Angle.png" alt="Main Board - angle" width="320">
</p>

| Component | Part | Function |
|-----------|------|----------|
| MCU | CH32X035F8U6 | RISC-V, USB-PD 3.0 PHY |
| DC-DC | TPS55289 | Buck-boost, 3-30V output |
| Charger | MP2762A | 2S Li-ion charger |
| Temp Sensor | LM75B | Board temperature |

### UI Panel

2-layer PCB with SSD1306 OLED display, two tactile buttons, and RP2040 controller.

<p align="center">
  <img src="docs/images/UI-Panel-Top.png" alt="UI Panel - top" width="320">
  <img src="docs/images/UI-Panel-Bottom.png" alt="UI Panel - bottom" width="320">
</p>

## Enclosure

3D-printed enclosure designed for FDM printing. Multi-part assembly with snap-fit battery latch.

**Parts:** Enclosure Main, Enclosure Bottom, Side Cover, Buttons, Lock, Press Lock Button, OLED Dimmed Window

Full STEP assembly: [`hardware/enclosure/Web3_Pi_UPS_3DPrinted.step`](hardware/enclosure/Web3_Pi_UPS_3DPrinted.step)

## Building

### CH32X035 Firmware

Requires MounRiver Studio toolchain or `riscv-none-embed-gcc`.

### RP2040 Firmware

Requires [PlatformIO](https://platformio.org/). Build and upload:

```bash
cd firmware-rp2040
pio run -t upload
```

## Links

- [Web3 Pi UPS — product page](https://www.web3pi.io/products/ups)
- [Web3 Pi — project homepage](https://www.web3pi.io)

## License

This project uses dual licensing:

| Component | License | File |
|-----------|---------|------|
| `firmware-rp2040/`, `firmware-ch32x/`, `service/` | [GPL-3.0](LICENSE-SOFTWARE) | `LICENSE-SOFTWARE` |
| `hardware/` | [CERN-OHL-S v2](LICENSE-HARDWARE) | `LICENSE-HARDWARE` |

---

*Disclaimer: Raspberry Pi is a trademark of Raspberry Pi Ltd. The use of this trademark here is solely for descriptive purposes.*
