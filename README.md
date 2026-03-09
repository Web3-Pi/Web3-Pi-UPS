# Web3 Pi UPS

Uninterruptible Power Supply for Web3 Pi — firmware, system service, electronics, and enclosure.

<!-- TODO: Add render images
<p align="center">
  <img src="docs/images/render-front.png" alt="Web3 Pi UPS - front view" width="600">
</p>
-->

## Repository Structure

```
firmware-rp2040/       RP2040 MCU firmware
firmware-ch32x/        CH32X MCU firmware
service/               Linux system service
hardware/
├── electronics/       Schematics (PDF), Gerber files, BOM
└── enclosure/         3D models (STEP, STL)
docs/
└── images/            Renders and photos
```

## License

This project uses dual licensing:

| Component | License | File |
|-----------|---------|------|
| `firmware-rp2040/`, `firmware-ch32x/`, `service/` | [GPL-3.0](LICENSE-SOFTWARE) | `LICENSE-SOFTWARE` |
| `hardware/` | [CERN-OHL-S v2](LICENSE-HARDWARE) | `LICENSE-HARDWARE` |
