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