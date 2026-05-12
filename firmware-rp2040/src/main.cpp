#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SerialPIO.h>
#include <string.h>
#include "wups_proto.h"
#include "wups_router.h"

// --- I2C for OLED ---
constexpr uint8_t I2C0_SDA_PIN = 8;
constexpr uint8_t I2C0_SCL_PIN = 9;
TwoWire WireCustom(i2c0, I2C0_SDA_PIN, I2C0_SCL_PIN);

// --- Debug serial via PIO software UART (J350 -> Raspberry Pi Debug Probe UART) ---
// Both hardware UARTs are claimed (UART0 = CH32X on GPIO16/17, UART1 reserved
// for the M.2 ESP32). J350 uses GPIO0 (TX) / GPIO1 (RX) — same data the host
// service sees on USB-CDC, mirrored here so we can monitor with the Probe
// while the Pi 5 holds the USB-CDC port.
constexpr uint8_t DBG_TX_PIN = 0;
constexpr uint8_t DBG_RX_PIN = 1;
constexpr uint32_t DBG_BAUD = 921600;  // match CH32X UART baud — no need to remember different speeds
SerialPIO dbgSerial(DBG_TX_PIN, DBG_RX_PIN, 32);

// Software ring buffer in front of dbgSerial. Earlephilhower's SerialPIO has
// only an 8-byte hardware TX FIFO and its write() calls pio_sm_put_blocking,
// so writing a ~250-byte burst (e.g. a debug log line) straight to it would
// stall loop() for ~22 ms (or forever if the PIO TX SM stops draining).
// DbgRing absorbs bursts in 1 KB of RAM, drops bytes silently when full, and
// is drained in loop() only as fast as the PIO HW FIFO has room — fully
// non-blocking.
class DbgRing : public Print {
public:
  static constexpr size_t SIZE = 1024;
  size_t write(uint8_t c) override {
    size_t next = (_head + 1) % SIZE;
    if (next == _tail) return 0;  // full, drop
    _buf[_head] = c;
    _head = next;
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
      if (write(buf[i]) == 0) break;
      written++;
    }
    return written;
  }
  // Pump drains the ring. SerialPIO::write blocks max ~11 us per byte at
  // 921600, so worst-case pump time = SIZE * 11 us = 11 ms (full ring) —
  // well within the 50 ms loop budget. Bound by ring capacity to be safe.
  void pump(Stream& dst) {
    size_t budget = SIZE;
    while (_head != _tail && budget--) {
      dst.write(_buf[_tail]);
      _tail = (_tail + 1) % SIZE;
    }
  }
private:
  uint8_t _buf[SIZE];
  size_t _head = 0;
  size_t _tail = 0;
};
DbgRing dbgRing;

// Drops writes when the USB-CDC host has no port open. Earlephilhower's
// SerialUSB::write blocks ~50 ms per byte waiting for buffer space when
// DTR is deasserted — without this guard a ~290-byte debug burst freezes
// the loop for ~14 s, and nothing reaches the probe UART either.
class UsbCdcDropIfDetached : public Print {
public:
  size_t write(uint8_t c) override {
    if (Serial) Serial.write(c);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    if (Serial) Serial.write(buf, n);
    return n;
  }
};
UsbCdcDropIfDetached usbCdcOut;

// Print fan-out to USB-CDC (best-effort, dropped when no host) and dbgRing.
class TeePrint : public Print {
public:
  TeePrint(Print& a, Print& b) : _a(a), _b(b) {}
  size_t write(uint8_t c) override {
    _a.write(c);
    _b.write(c);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    _a.write(buf, n);
    _b.write(buf, n);
    return n;
  }
private:
  Print& _a;
  Print& _b;
};
TeePrint dbgOut(usbCdcOut, dbgRing);

// --- UART0 bidirectional (RP2040 <-> CH32X) ---
constexpr uint8_t GPIO16_PIN = 16;   // GPIO16 - UART0 TX (commands to CH32X)
constexpr uint8_t GPIO18_PIN = 18;   // GPIO18 - not used, set as input pullup
constexpr uint8_t GPIO19_PIN = 19;   // GPIO19 - not used, set as input pullup
constexpr uint8_t UART0_RX_PIN = 17; // GPIO17 - UART0 RX (status + responses from CH32X)

// --- UART1 (Serial2) — RP2040 <-> M.2 ESP32, hardware UART with full flow control ---
// All four pins are wired through the M.2 connector. CTS/RTS are wired up
// from day one so we never lose bytes when either side momentarily can't
// drain its FIFO; the deframer is robust against drops, but flow control
// keeps the link "boring" and saves debugging time later.
constexpr uint8_t  UART1_TX_PIN  = 20;  // GPIO20 - hardware UART1 TX
constexpr uint8_t  UART1_RX_PIN  = 21;  // GPIO21 - hardware UART1 RX
constexpr uint8_t  UART1_CTS_PIN = 22;  // GPIO22 - hardware UART1 CTS (input from ESP32 RTS)
constexpr uint8_t  UART1_RTS_PIN = 23;  // GPIO23 - hardware UART1 RTS (output to ESP32 CTS)
constexpr uint32_t UART1_BAUD    = 921600;

// --- GPIO26/27 - unused, set as output LOW ---
constexpr uint8_t GPIO26_PIN = 26;
constexpr uint8_t GPIO27_PIN = 27;

// --- ADC pins ---
constexpr uint8_t ADC_BATT_VOLT_PIN = A2; // ADC2 / GPIO28 - Battery voltage via divider
constexpr uint8_t ADC_VBUS_OUT_PIN = A3;  // ADC3 / GPIO29 - VBUS output voltage via divider

// --- Battery voltage divider resistors (kOhm) ---
// Divider connected to 2S battery pack (0-8.5V range)
// VADC = Vbat * R2 / (R1 + R2)
// R430=100k, R431=47k, C430=100nF filter, R432=0R to ADC
constexpr float BATT_R1_KOHM = 100.0f;  // R430 - Top resistor (to VBAT+)
constexpr float BATT_R2_KOHM = 47.0f;   // R431 - Bottom resistor (to GND)
constexpr float BATT_DIVIDER_RATIO = BATT_R2_KOHM / (BATT_R1_KOHM + BATT_R2_KOHM);

// --- VBUS output voltage divider resistors (kOhm) ---
// Divider connected to USB-C VBUS output (0-20V range)
// R527=27.4k, R526=5.1k
constexpr float VBUS_R1_KOHM = 27.4f;   // R527 - Top resistor (to VBUS_OUT)
constexpr float VBUS_R2_KOHM = 5.1f;    // R526 - Bottom resistor (to GND)
constexpr float VBUS_DIVIDER_RATIO = VBUS_R2_KOHM / (VBUS_R1_KOHM + VBUS_R2_KOHM);

// --- ADC calibration ---
constexpr float VREF = 3.3f;
constexpr int ADC_BITS = 12;
constexpr int ADC_MAX = (1 << ADC_BITS) - 1; // 4095

// ADC correction factor - set to 1.0 (no correction needed)
// RP2040 ADC reads within ~2% of actual voltage with this divider
constexpr float ADC_CORRECTION = 1.0f;

// --- OLED SSD1306 ---
constexpr int SCREEN_WIDTH  = 64;
constexpr int SCREEN_HEIGHT = 32;
constexpr int OLED_RESET    = -1;
constexpr uint8_t OLED_ADDR = 0x3C;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &WireCustom, OLED_RESET);

// --- Power hold ---
constexpr uint8_t RP_HOLD_VDD_PIN = 6;
constexpr uint8_t RP_UI_GPIO_PIN = 12;

// --- Buzzer ---
constexpr uint8_t BUZZER_PIN = 15;
constexpr unsigned long BAD_PSU_REMINDER_INTERVAL_MS = 10000; // Reminder beep every 10s
constexpr unsigned long LOW_BATTERY_INTERVAL_MS = 30000;      // <20% beep every 30s
constexpr unsigned long CRITICAL_BATTERY_INTERVAL_MS = 5000;  // <10% beep every 5s

// --- Buttons ---
constexpr uint8_t BTN_LEFT_PIN = 13;   // GPIO13 - left button (active LOW)
constexpr uint8_t BTN_RIGHT_PIN = 14;  // GPIO14 - right button (active LOW)
constexpr unsigned long DEBOUNCE_MS = 50;

// --- Screen navigation ---
constexpr uint8_t SCREEN_COUNT = 7;
constexpr uint8_t SCREEN_PD_DIAG = 4;       // PD/TPS diagnostic readout
constexpr uint8_t SCREEN_POWER_PATH = 5;    // VIN / IIN / VSYS power-path readout
constexpr uint8_t SCREEN_POWER_CTRL = 6;    // last screen — buttons act as power on/off
constexpr unsigned long AUTO_RETURN_MS = 15000;  // Auto-return to home after 15s

// --- Bad charger alert state ---
bool badChargerAlertPlayed = false;
unsigned long lastReminderTime = 0;

// --- Battery power loss detection ---
bool previousPowerGood = false;     // Initialized after startup stabilization
bool powerLossAlertPlayed = false;  // Prevent repeated alerts

// --- Low battery warning state ---
unsigned long lastLowBatteryBeep = 0;

unsigned long lastFrameTime = 0;  // millis() of the last power.status frame received

// --- Screen navigation state ---
uint8_t currentScreen = 0;
unsigned long lastInteractionTime = 0;
bool lastBtnLeftState = HIGH;
bool lastBtnRightState = HIGH;

// --- Cached CH32X power.status (binary v1) ---
// CH32X emits a power.status frame every 1 s. wups_on_local_frame() copies
// the payload here and projects fields into the `ui` snapshot below.
wups_power_status_v1_t Last_Power_Status = {};
unsigned long Last_Power_Status_Ms = 0;

// --- UPS view: snapshot for OLED rendering and alarm logic ----------------
// Populated by wups_on_local_frame() from CH32X power.status (binary v1)
// plus RP2040-local ADC readings (bv, soc). Read-only from the loop's PoV.
// PD detail (pd, pdo) and snk_* / role are not in v1 power.status — they
// stay at last-known until the protocol is extended.
static struct {
    // From CH32X power.status (binary v1):
    int t;          // tenths of °C
    int cs;         // charge state 0..3 (DSC/PRE/CHG/FUL)
    int pg;         // 1 if input good (derived: vbus_in_mV > 5 V)
    int vi;         // input voltage (mV)
    int ci;         // charge current (mA, signed)
    int cf;         // charger faults bitmap
    int bp;         // battery present 0/1 (derived: vbat_mV > 100)
    int vb, vbc;    // battery voltage (mV) — primary and from charger IC
    int vs, is;     // PD contract V/A (0.1 V / 0.1 A units)
    int vr, ir;     // VBUS_OUT V/A (0.1 V / 0.1 A units)
    // Held at last known from previous protocol; not yet in v1 power.status:
    int pd, pdo, role, snk_ok, snk_v, snk_i;
    // Computed locally on RP2040:
    int bv;         // battery voltage (mV, ADC + EMA)
    int soc;        // 0..100 from LUT + adaptive EMA
} ui;

// --- VBUS output voltage from ADC ---
int vbus_out_mV = 0;  // USB-C VBUS output voltage (mV) from ADC3

// --- Filtered battery voltage (EMA with alpha=0.1 for ~30s smoothing) ---
float filtered_batt_mV = -1.0f;   // From ADC, -1 = not initialized
constexpr float EMA_ALPHA = 0.1f;

// --- Filtered SOC (adaptive EMA: snap on large changes, smooth small oscillations) ---
float filtered_soc = -1.0f;
constexpr float SOC_EMA_ALPHA = 0.05f;
constexpr int SOC_SNAP_THRESHOLD = 3;  // Snap immediately if SOC changes by more than 3%
bool newFrameReceived = false;

// --- Battery presence debounce ---
bool noBatteryDebounced = false;
int noBatteryCounter = 0;
constexpr int NO_BATTERY_DEBOUNCE = 10;  // ~500ms at 50ms loop rate

// --- Display charge state (smoothed to prevent CHG/FUL flicker at end-of-charge) ---
int displayCs = 0;
int csChangeCounter = 0;
constexpr int CS_CHANGE_DEBOUNCE = 10;  // ~500ms stability before changing displayed state

// --- Startup stabilization ---
bool startupComplete = false;
constexpr unsigned long STARTUP_STABILIZE_MS = 3000;
unsigned long startupEndTime = 0;

// Convert battery voltage (2S pack, mV) to SOC percentage
// Based on Panasonic CGR18650CH 2250mAh discharge curve
// Uses linear interpolation between known points
// Note: 8.0V (4.0V/cell) and above is treated as 100% full
static int voltageToSoc(int bv_mV) {
  // Lookup table: {cell_voltage_mV, soc_percent}
  // Panasonic CGR18650CH 2250mAh
  // Starts at 4.0V = 100% (8.0V pack voltage)
  static const int16_t lut[][2] = {
    {4000, 100},
    {3900,  88},
    {3800,  75},
    {3700,  60},
    {3600,  40},
    {3500,  22},
    {3400,  10},
    {3300,   4},
    {3200,   0}
  };
  static const int LUT_SIZE = sizeof(lut) / sizeof(lut[0]);

  // Convert 2S pack voltage to per-cell voltage
  int cellV = bv_mV / 2;

  // Clamp to table bounds
  if (cellV >= lut[0][0]) return lut[0][1];           // >= 4.0V = 100%
  if (cellV <= lut[LUT_SIZE - 1][0]) return lut[LUT_SIZE - 1][1];  // <= 3.2V = 0%

  // Find segment and interpolate
  for (int i = 0; i < LUT_SIZE - 1; i++) {
    if (cellV <= lut[i][0] && cellV > lut[i + 1][0]) {
      // Linear interpolation
      int v1 = lut[i][0], v2 = lut[i + 1][0];
      int s1 = lut[i][1], s2 = lut[i + 1][1];
      return s1 + (int32_t)(cellV - v1) * (s2 - s1) / (v2 - v1);
    }
  }
  return 0;
}

// Draw battery icon with state and animation
// x,y = top-left corner, soc = 0-100%, cs = charge state, noBattery = true if no battery detected, animPhase = animation frame
static void drawBatteryIcon(int x, int y, int soc, int cs, bool noBattery, uint8_t animPhase) {
  const int W = 10, H = 22;
  const int TIP_W = 4, TIP_H = 2;

  // Battery tip (top center)
  oled.fillRect(x + (W - TIP_W) / 2, y, TIP_W, TIP_H, SSD1306_WHITE);

  // Battery body outline
  oled.drawRect(x, y + TIP_H, W, H - TIP_H, SSD1306_WHITE);

  // Inner fill area (2px margin inside body)
  int innerX = x + 2;
  int innerY = y + TIP_H + 2;
  int innerW = W - 4;
  int innerH = H - TIP_H - 4;

  // No battery: draw X through battery
  if (noBattery) {
    oled.drawLine(innerX, innerY, innerX + innerW - 1, innerY + innerH - 1, SSD1306_WHITE);
    oled.drawLine(innerX + innerW - 1, innerY, innerX, innerY + innerH - 1, SSD1306_WHITE);
    return;
  }

  // Calculate fill height based on SOC
  int fillH = (innerH * soc) / 100;
  int fillY = innerY + (innerH - fillH);

  if (cs == 1 || cs == 2) {
    // Charging animation: scrolling bars upward
    for (int i = 0; i < innerH; i++) {
      int py = innerY + i;
      // Create moving stripe pattern (subtract animPhase for upward movement)
      if (((innerH - 1 - i + animPhase / 2) % 4) < 2) {
        oled.drawFastHLine(innerX, py, innerW, SSD1306_WHITE);
      }
    }
  } else if (soc > 0) {
    // Static fill from bottom
    oled.fillRect(innerX, fillY, innerW, fillH, SSD1306_WHITE);
  }

  // Low battery indicator (< 10%): flash
  if (soc < 10 && cs == 0 && (animPhase / 8) % 2 == 0) {
    oled.fillRect(innerX, innerY, innerW, innerH, SSD1306_BLACK);
  }
}

// Play error sound sequence (alarm pattern) - blocking
static void playErrorSound() {
  // Three rapid high-pitched beeps
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, 2400, 100);
    delay(150);
  }
  
}

// Play gentle reminder beep - blocking
static void playReminderBeep() {
  tone(BUZZER_PIN, 2400, 80);
  delay(50);
  tone(BUZZER_PIN, 2400, 80);
  delay(50);
}

// Play startup melody - pleasant ascending tones
static void playStartupMelody() {
  // C5, E5, G5, C6 - major chord arpeggio
  const int notes[] = {523, 659, 784, 1047};
  const int durations[] = {100, 100, 100, 200};

  for (int i = 0; i < 4; i++) {
    tone(BUZZER_PIN, notes[i], durations[i]);
    delay(durations[i] + 30);
  }
  noTone(BUZZER_PIN);
}

// Play power loss alarm - alarming descending tones
static void playPowerLossAlarm() {
  // Descending urgent pattern
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 1500, 150);
    delay(180);
    tone(BUZZER_PIN, 1000, 150);
    delay(180);
    tone(BUZZER_PIN, 700, 200);
    delay(300);
  }
  noTone(BUZZER_PIN);
}

// Play low battery warning beep - single short beep
static void playLowBatteryBeep() {
  tone(BUZZER_PIN, 1800, 100);
  delay(100);
  noTone(BUZZER_PIN);
}

// Play critical battery warning beep - double short beep
static void playCriticalBatteryBeep() {
  tone(BUZZER_PIN, 2000, 80);
  delay(120);
  tone(BUZZER_PIN, 2000, 80);
  delay(80);
  noTone(BUZZER_PIN);
}

// Draw screen indicator dots (4 dots, current screen filled)
static void drawScreenIndicator(uint8_t screen) {
  const int baseX = 64 - (SCREEN_COUNT * 2);
  const int y = 30;

  for (uint8_t i = 0; i < SCREEN_COUNT; i++) {
    int x = baseX + (i * 2);
    if (i == screen) {
      // Current screen: filled 2px tall bar
      oled.drawFastVLine(x, y - 1, 2, SSD1306_WHITE);
    } else {
      // Other screens: single pixel dot
      oled.drawPixel(x, y, SSD1306_WHITE);
    }
  }
}

// Screen 0: Home dashboard
static void drawScreenHome(int soc, bool noBattery, uint8_t animPhase) {
  // Battery icon (left side, 10x22 pixels)
  drawBatteryIcon(0, 0, soc, displayCs, noBattery, animPhase);

  // SOC percentage (large text, right of icon)
  oled.setTextSize(2);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(15, 0);
  if (noBattery) {
    oled.print(F("--"));
  } else if (soc < 100) {
    oled.print(soc);
  } else {
    oled.print(F("100"));
  }
  oled.print(F("%"));

  // Middle line: charge state + battery voltage
  oled.setTextSize(1);
  oled.setCursor(15, 16);
  if (noBattery) {
    oled.print(F("NoB"));
  } else {
    switch (displayCs) {
      case 0: oled.print(F("DSC")); break;
      case 1: oled.print(F("PRE")); break;
      case 2: oled.print(F("CHG")); break;
      case 3: oled.print(F("FUL")); break;
      default: oled.print(F("---")); break;
    }
  }
  // Battery voltage (right of charge state)
  oled.setCursor(39, 16);
  oled.print(ui.bv / 1000);
  oled.print(F("."));
  oled.print((ui.bv % 1000) / 100);
  oled.print(F("V"));

  // Bottom line: input voltage + output voltage
  oled.setCursor(0, 25);
  // Input voltage (vi)
  if (ui.vi > 0) {
    oled.print(ui.vi / 1000);
    oled.print(F("."));
    oled.print((ui.vi % 1000) / 100);
    oled.print(F("V"));
  } else {
    oled.print(F("-.-V"));
  }

  // Output voltage (measured from ADC3)
  oled.setCursor(39, 25);
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  oled.print((vbus_out_mV % 1000) / 100);
  oled.print(F("V"));
}

// Screen 1: Power (TPS55289 SET vs READ)
static void drawScreenPower() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 0: Header
  oled.setCursor(0, 0);
  oled.print(F("PWR SET:"));

  // Line 1: Voltage SET, Current SET
  oled.setCursor(0, 8);
  oled.print(ui.vs / 10);
  oled.print(F("."));
  oled.print(ui.vs % 10);
  oled.print(F("V "));
  oled.print(ui.is / 10);
  oled.print(F("."));
  oled.print(ui.is % 10);
  oled.print(F("A"));

  // Line 2: Header
  oled.setCursor(0, 16);
  oled.print(F("PWR RD:"));

  // Line 3: Voltage READ, Current READ
  oled.setCursor(0, 24);
  oled.print(ui.vr / 10);
  oled.print(F("."));
  oled.print(ui.vr % 10);
  oled.print(F("V "));
  oled.print(ui.ir / 10);
  oled.print(F("."));
  oled.print(ui.ir % 10);
  oled.print(F("A"));

  drawScreenIndicator(1);
}

// Screen 2: Battery details
static void drawScreenBattery() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 0: Temperature
  oled.setCursor(0, 0);
  oled.print(F("T:"));
  oled.print(ui.t / 10);
  oled.print(F("."));
  oled.print(abs(ui.t % 10));
  oled.print(F("C"));

  // Line 1: Battery voltage (from ADC)
  oled.setCursor(0, 8);
  oled.print(F("Bat:"));
  oled.print(ui.bv / 1000);
  oled.print(F("."));
  int frac = (ui.bv % 1000) / 10;
  if (frac < 10) oled.print(F("0"));
  oled.print(frac);
  oled.print(F("V"));

  // Line 2: Charger fault flags (hex)
  oled.setCursor(0, 16);
  oled.print(F("CF:0x"));
  if (ui.cf < 16) oled.print(F("0"));
  oled.print(ui.cf, HEX);

  // Line 3: Charge current
  oled.setCursor(0, 24);
  oled.print(F("CI:"));
  oled.print(ui.ci);
  oled.print(F("mA"));

  drawScreenIndicator(2);
}

// Screen 3: USB-PD info (output side to RPi)
static void drawScreenPDInfo() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 0: Header
  oled.setCursor(0, 0);
  oled.print(F("PD OUT>Pi"));

  // Line 1: PD State
  oled.setCursor(0, 8);
  oled.print(F("State:"));
  oled.print(ui.pd);

  // Line 2: PDO Index
  oled.setCursor(0, 16);
  oled.print(F("PDO:"));
  oled.print(ui.pdo);

  // Line 3: Output voltage (measured from ADC3)
  oled.setCursor(0, 24);
  oled.print(F("Vo:"));
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  oled.print((vbus_out_mV % 1000) / 100);
  oled.print(F("V"));

  drawScreenIndicator(3);
}

// Screen 4: PD/TPS diagnostic readout. ui.vr/ir come from CH32X reading the
// TPS55289 REF/IOUT_LIMIT registers back, so they reflect what's currently
// programmed on the converter — which CH32X overwrites in STA_TX_ACCEPT to
// match the negotiated PDO:
//   pre-PD:        5.1V / 3.0A (VBUS_set_5V default)
//   PDO #1 5V5A:   5.1V / 5.0A
//   PDO #2 9V3A:   9.0V / 3.0A
//   PDO #3 12V:   12.0V / 2.25A
//   PDO #4 15V:   15.0V / 1.8A
// (ui.vs/is map to pd_contract_mV/mA — that's the SINK side of CH32X, i.e.
// the upstream charger contract, not the USB-C output. Don't use here.)
// Vo is RP2040's own ADC reading of VBUS_OUT, independent of the CH32X path.
static void drawScreenPDDiag() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.print(F("PD>Pi"));
  /* TPS55289 STATUS bits packed into the high byte of `faults` by CH32X:
   *   bit 8  = SCP (short-circuit protection)
   *   bit 9  = OCP (overcurrent protection)
   *   bit 10 = OVP (overvoltage protection)
   * Show single-letter suffix when a trip is latched so it's obvious on
   * the OLED what protection killed the rail. */
  if (ui.cf & ((1 << 8) | (1 << 9) | (1 << 10))) {
    oled.print(F(" !"));
    if (ui.cf & (1 << 8))  oled.print(F("S"));
    if (ui.cf & (1 << 9))  oled.print(F("O"));
    if (ui.cf & (1 << 10)) oled.print(F("V"));
  }

  oled.setCursor(0, 8);
  oled.print(F("S:"));
  oled.print(ui.vr / 10);
  oled.print(F("."));
  oled.print(ui.vr % 10);
  oled.print(F("V"));

  oled.setCursor(0, 16);
  oled.print(F("L:"));
  oled.print(ui.ir / 10);
  oled.print(F("."));
  oled.print(ui.ir % 10);
  oled.print(F("A"));

  oled.setCursor(0, 24);
  oled.print(F("Vo:"));
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  int frac = (vbus_out_mV % 1000) / 10;
  if (frac < 10) oled.print(F("0"));
  oled.print(frac);
  oled.print(F("V"));

  drawScreenIndicator(SCREEN_PD_DIAG);
}

// Screen 5: Power-path diagnostic. Diagnoses TPS55289 UVLO trips that
// don't show up in STATUS register — typical signature when running with
// no battery to cushion VSYS during a USB-C load step.
//   Vi = VIN to MP2762A (DC IN, e.g. 12 V from barrel jack)
//   Ii = IIN to MP2762A (current the chip is pulling from VIN)
//   Vs = VSYS rail (the rail feeding TPS55289 VIN — if this dips below
//        ~3 V under load, TPS resets and VBUS_OUT collapses)
// CH32X diagnostic-aliases pd_contract_mV/mA to VSYS/IIN whenever there
// is no upstream PD SINK contract (i.e. nearly always on the bench),
// so ui.vs/is carry VSYS/IIN here.
static void drawScreenPowerPath() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.print(F("Vi:"));
  oled.print(ui.vi / 1000);
  oled.print(F("."));
  int vi_frac = (ui.vi % 1000) / 100;
  oled.print(vi_frac);
  oled.print(F("V"));

  oled.setCursor(0, 8);
  oled.print(F("Ii:"));
  oled.print(ui.is / 10);
  oled.print(F("."));
  oled.print(ui.is % 10);
  oled.print(F("A"));

  oled.setCursor(0, 16);
  oled.print(F("Vs:"));
  /* ui.vs is in 0.1 V units (CH32X passes VSYS in mV through
   * pd_contract_mV, then RP2040 divides by 100 → 0.1 V). */
  oled.print(ui.vs / 10);
  oled.print(F("."));
  oled.print(ui.vs % 10);
  oled.print(F("V"));

  drawScreenIndicator(SCREEN_POWER_PATH);
}

// Screen 6: Power Control — LEFT disables VBUS_OUT, RIGHT re-enables it.
static void drawScreenPowerCtrl() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 0: header
  oled.setCursor(0, 0);
  oled.print(F("Power Ctrl"));

  // Line 1: ON / OFF status — derived from RP2040's own VBUS_OUT ADC so
  // we don't depend on the cached CH32X TPS55289 set values for the truth.
  bool outputOn = (vbus_out_mV > 1000);
  oled.setCursor(0, 8);
  oled.print(F("OUT: "));
  oled.print(outputOn ? F("ON") : F("OFF"));

  // Line 2: button hint — LEFT turns off, RIGHT turns on
  oled.setCursor(0, 16);
  oled.print(F("<OFF  ON>"));

  // Line 3: measured output voltage + screen indicator
  oled.setCursor(0, 24);
  oled.print(F("Vo:"));
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  oled.print((vbus_out_mV % 1000) / 100);
  oled.print(F("V"));

  drawScreenIndicator(SCREEN_POWER_CTRL);
}

// --- Web3 Pi UPS binary wire protocol v1 — RP2040 hub-side handling ---
//
// The router (wups_router.cpp) deframes bytes from USB-CDC, UART0 and UART1
// and calls back here when a frame is destined for RP2040 itself, broadcast,
// or the INTERNAL multicast. Forwarding to other ports is already done
// inside the router by the time we get here — this function is for *local*
// consumption only.
//
// Application behaviour for v1:
//   - CH32X power.status (cls=POWER op=PWR_STATUS, src=CH32X)
//       → cache in Last_Power_Status, project into the `ui` snapshot,
//         re-emit unicast to RPi as a push aggregate (preserving SRC=CH32X).
//   - system.ping (REQ) addressed to us
//       → reply with system.ping (RESP) carrying uptime + fw_version.
//   - system.hello broadcast
//       → ignored for now (could populate a presence map later).
//   - everything else
//       → silently dropped.
//
// Defined here (in main.cpp) because the body needs access to RP2040
// firmware globals (ui, lastFrameTime, etc.). Declared in wups_router.h.
static void wupsPublishTelemetryStatus(uint8_t seq, const wups_power_status_v1_t& s);

void wups_on_local_frame(uint8_t inbound_port, const WupsFrame& f) {
  (void)inbound_port;

  // CH32X power.status — cache and project into the `ui` snapshot so the
  // OLED / alarm code keeps working unchanged. PD detail and snk_* are not
  // in v1 power.status; they stay at last known.
  if (f.cls == WUPS_CLASS_POWER && f.op == WUPS_OP_PWR_STATUS &&
      f.src == WUPS_ADDR_CH32X &&
      f.len >= sizeof(wups_power_status_v1_t)) {
    wups_power_status_v1_t s;
    memcpy(&s, f.payload, sizeof(s));
    if (s.version != 1) return;

    Last_Power_Status    = s;
    Last_Power_Status_Ms = millis();

    ui.t   = s.temp_dC;
    ui.cs  = s.charge_state;
    ui.pg  = (s.vbus_in_mV > 5000) ? 1 : 0;     // derived: input "good"
    ui.vi  = s.vbus_in_mV;
    ui.ci  = s.ibat_mA;
    ui.cf  = s.faults;
    ui.bp  = (s.vbat_mV > 100) ? 1 : 0;          // derived: battery present
    ui.vb  = s.vbat_mV;
    ui.vbc = s.vbat_mV;                          // single source in v1
    // Set vs read separation lost in v1: report measured values for both.
    ui.vs  = (int)(s.pd_contract_mV / 100);
    ui.is  = (int)(s.pd_contract_mA / 100);
    ui.vr  = (int)(s.vbus_out_mV / 100);
    ui.ir  = (int)(s.ibus_out_mA / 100);
    // PD detail (pd, pdo, role, snk_*) is not in v1 power.status — hold previous values.

    newFrameReceived = true;
    lastFrameTime    = millis();

    // Mirror parsed power.status to the debug stream (USB-CDC + Probe UART
    // via J350) so the firmware author can read CH32X telemetry remotely
    // without unplugging the bench setup. One line per frame, fixed
    // column order so it's easy to grep / diff between runs.
    dbgOut.print(F("[t="));
    dbgOut.print(millis() / 1000);
    dbgOut.print(F("s] cs="));
    dbgOut.print(s.charge_state);
    dbgOut.print(F(" pg="));
    dbgOut.print((s.vbus_in_mV > 5000) ? 1 : 0);
    dbgOut.print(F(" Vin="));
    dbgOut.print(s.vbus_in_mV);
    dbgOut.print(F("mV Vsys="));
    dbgOut.print(s.pd_contract_mV);   // diag-aliased (see CH32X main.c)
    dbgOut.print(F("mV Iin="));
    dbgOut.print(s.pd_contract_mA);   // diag-aliased
    dbgOut.print(F("mA Vbat="));
    dbgOut.print(s.vbat_mV);
    dbgOut.print(F("mV Ichg="));
    dbgOut.print(s.ibat_mA);
    dbgOut.print(F("mA Vout="));
    dbgOut.print(s.vbus_out_mV);
    dbgOut.print(F("mV Iout="));
    dbgOut.print(s.ibus_out_mA);
    dbgOut.print(F("mA T="));
    dbgOut.print(s.temp_dC);
    dbgOut.print(F("dC f=0x"));
    dbgOut.println(s.faults, HEX);

    // Push-mode aggregate to RPi: forward the same frame on USB-CDC with
    // SRC=CH32X preserved. RPi service decodes and treats it as authoritative.
    wups_send_with_src(WUPS_PORT_RPI, WUPS_ADDR_RPI, WUPS_ADDR_CH32X,
                       WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                       WUPS_FLAG_EVENT, f.seq, &s, sizeof(s));

    // Drive the cellular uplink via net.publish to the ESP32. ESP32 is a
    // dumb pipe to MQTT — it forwards `topic`+`payload` to the broker and
    // auto-prefixes relative subtopics with `t/{iccid}/`. We send the full
    // raw WUPS frame (header+payload+checksum) as the MQTT payload so the
    // panel's wupsproto.ts decoder sees byte-for-byte what the bus sees.
    wupsPublishTelemetryStatus(f.seq, s);
    return;
  }

  // net.downlink — ESP32 wraps each MQTT-arrived message in a net.downlink
  // EVENT to RP2040. The wrapper payload is hdr (6 B) + topic[topic_len] +
  // payload[payload_len]; the inner `payload` is itself a complete WUPS frame
  // emitted by the panel backend. Deframe it (Fletcher-8 verify) and recurse
  // back into this same dispatcher — recursion is intentional: the inner
  // frame may carry any class/op we already handle (ui.beep, power.*, etc.)
  // so we don't want to duplicate the dispatch table.
  if (f.cls == WUPS_CLASS_NET && f.op == WUPS_OP_NET_DOWNLINK &&
      (f.flags & WUPS_FLAG_EVENT) &&
      f.len >= sizeof(wups_net_downlink_v1_hdr_t)) {
    wups_net_downlink_v1_hdr_t dl;
    memcpy(&dl, f.payload, sizeof(dl));
    if (dl.version != 1) return;
    size_t need = sizeof(dl) + dl.topic_len + dl.payload_len;
    if (need > f.len) return;

    const uint8_t* inner = f.payload + sizeof(dl) + dl.topic_len;
    uint16_t inner_pl_len = dl.payload_len;
    if (inner_pl_len < WUPS_FRAMING_BYTES) return;
    if (inner[0] != WUPS_SYNC1 || inner[1] != WUPS_SYNC2) return;
    uint16_t inner_len = (uint16_t)inner[8] | ((uint16_t)inner[9] << 8);
    if (inner_len > WUPS_MAX_PAYLOAD) return;
    if ((size_t)WUPS_FRAMING_BYTES + inner_len != inner_pl_len) return;
    if (inner[WUPS_HEADER_BYTES + inner_len + 2] != WUPS_END1) return;
    if (inner[WUPS_HEADER_BYTES + inner_len + 3] != WUPS_END2) return;

    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < (size_t)WUPS_HEADER_BYTES + inner_len; i++) {
      a = (uint8_t)(a + inner[i]);
      b = (uint8_t)(b + a);
    }
    if (a != inner[WUPS_HEADER_BYTES + inner_len] ||
        b != inner[WUPS_HEADER_BYTES + inner_len + 1]) return;

    WupsFrame ifr;
    ifr.dst   = inner[2];
    ifr.src   = inner[3];
    ifr.cls   = inner[4];
    ifr.op    = inner[5];
    ifr.flags = inner[6];
    ifr.seq   = inner[7];
    ifr.len   = inner_len;
    if (inner_len) memcpy(ifr.payload, inner + WUPS_HEADER_BYTES, inner_len);

    dbgOut.print(F("[net.downlink] cls=0x"));
    dbgOut.print(ifr.cls, HEX);
    dbgOut.print(F(" op=0x"));
    dbgOut.print(ifr.op, HEX);
    dbgOut.print(F(" flags=0x"));
    dbgOut.print(ifr.flags, HEX);
    dbgOut.print(F(" len="));
    dbgOut.println(ifr.len);

    wups_on_local_frame(inbound_port, ifr);
    return;
  }

  // system.log (cls=SYSTEM op=SYS_LOG, EVENT) — CH32X uses this for raw
  // register dumps and diagnostic snapshots. Header is 4 bytes (version,
  // level, text_len, reserved), then `text_len` ASCII bytes (no NUL).
  // Forward verbatim to the debug stream so `pio device monitor` on the
  // probe UART can read it.
  if (f.cls == WUPS_CLASS_SYSTEM && f.op == WUPS_OP_SYS_LOG && f.len >= 4) {
    uint8_t text_len = f.payload[2];
    if ((uint16_t)4 + text_len > f.len) return;
    dbgOut.print(F("[CH32X log] "));
    dbgOut.write(f.payload + 4, text_len);
    dbgOut.println();
    return;
  }

  // ui.beep → play a tone on the buzzer. Lets a remote operator (web
  // panel, local CLI, …) verify the round-trip path reaches *this*
  // device — sound is end-to-end proof the command landed on the right
  // RP2040 and the dispatcher decoded it correctly.
  //
  // freq_hz == 0 or dur_ms == 0 → use sensible defaults so a minimal
  // {"op":"beep"} request still produces an audible chirp.
  if (f.cls == WUPS_CLASS_UI && f.op == WUPS_OP_UI_BEEP &&
      (f.flags & WUPS_FLAG_REQ) &&
      f.len >= sizeof(wups_ui_beep_v1_t)) {
    wups_ui_beep_v1_t b;
    memcpy(&b, f.payload, sizeof(b));
    if (b.version == 1) {
      uint16_t freq = b.freq_hz ? b.freq_hz : 1500;
      uint16_t dur  = b.dur_ms  ? b.dur_ms  : 150;
      // Clamp duration so a misbehaving caller can't lock the buzzer
      // (tone() is non-blocking on this core but loop() still does a
      // 50 ms delay, so a long active tone is mostly fine — cap at 5 s
      // as a safety net).
      if (dur > 5000) dur = 5000;
      tone(BUZZER_PIN, freq, dur);
    }

    uint8_t out_port = WUPS_PORT_NONE;
    if      (f.src == WUPS_ADDR_RPI)   out_port = WUPS_PORT_RPI;
    else if (f.src == WUPS_ADDR_CH32X) out_port = WUPS_PORT_CH32X;
    else if (f.src == WUPS_ADDR_ESP32) out_port = WUPS_PORT_ESP32;
    if (out_port != WUPS_PORT_NONE) {
      wups_send_seq(out_port, f.src, WUPS_CLASS_UI, WUPS_OP_UI_BEEP,
                    WUPS_FLAG_RESP, f.seq, nullptr, 0);
    }
    return;
  }

  // system.ping → respond with uptime + fw_version.
  if (f.cls == WUPS_CLASS_SYSTEM && f.op == WUPS_OP_SYS_PING &&
      (f.flags & WUPS_FLAG_REQ)) {
    wups_sys_pong_v1_t pong;
    pong.version    = 1;
    pong.reserved   = 0;
    pong.fw_version = (uint16_t)((1u << 8) | 0u); /* 1.0 — bump on release */
    pong.uptime_ms  = (uint32_t)millis();

    uint8_t out_port = WUPS_PORT_NONE;
    if      (f.src == WUPS_ADDR_RPI)   out_port = WUPS_PORT_RPI;
    else if (f.src == WUPS_ADDR_CH32X) out_port = WUPS_PORT_CH32X;
    else if (f.src == WUPS_ADDR_ESP32) out_port = WUPS_PORT_ESP32;
    if (out_port != WUPS_PORT_NONE) {
      wups_send_seq(out_port, f.src, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_PING,
                    WUPS_FLAG_RESP, f.seq, &pong, sizeof(pong));
    }
    return;
  }

  // Other classes/ops: ignored in v1.
}

// Hand off a payload to the ESP32 for MQTT publication.
//
// Wire shape (per common/protocol.h `wups_net_publish_v1_hdr_t`):
//   [version=1][qos][retain][topic_len][payload_len_le][topic[topic_len]][payload[payload_len]]
//
// The ESP32 implements ADR-0002 / ADR-0004 — if `topic` is a bare subtopic
// like "telemetry" / "event" / "cmd/response", the ESP32 prepends the
// per-device prefix `t/{iccid}/` before publishing. The RP2040 stays
// SIM-agnostic (it has no ICCID) — it picks the *kind*, the ESP32 picks
// the *destination*.
//
// Caller owns `topic` and `payload`; they're copied into the WUPS frame.
// Returns true if the request was queued onto UART1.
static bool wupsRequestPublish(const char* topic, uint8_t qos, uint8_t retain,
                               const uint8_t* payload, uint16_t payload_len) {
  size_t topic_len = strlen(topic);
  if (topic_len == 0 || topic_len > 200) return false;
  size_t total = sizeof(wups_net_publish_v1_hdr_t) + topic_len + payload_len;
  if (total > WUPS_MAX_PAYLOAD) return false;

  uint8_t buf[WUPS_MAX_PAYLOAD];
  wups_net_publish_v1_hdr_t hdr;
  hdr.version     = 1;
  hdr.qos         = qos;
  hdr.retain      = retain;
  hdr.topic_len   = (uint8_t)topic_len;
  hdr.payload_len = payload_len;
  memcpy(buf, &hdr, sizeof(hdr));
  memcpy(buf + sizeof(hdr), topic, topic_len);
  if (payload_len) memcpy(buf + sizeof(hdr) + topic_len, payload, payload_len);

  wups_send(WUPS_PORT_ESP32, WUPS_ADDR_ESP32,
            WUPS_CLASS_NET, WUPS_OP_NET_PUBLISH, WUPS_FLAG_REQ,
            buf, (uint16_t)total);
  return true;
}

// Build a full WUPS frame (sync + header + payload + Fletcher-8 + end)
// in-place. Used to package telemetry / event / cmd-response payloads so
// the panel's wupsproto.ts decoder sees the same bytes the bus saw.
// Returns total frame length written, or 0 on overflow.
static size_t wupsBuildFrame(uint8_t* out, size_t out_cap,
                             uint8_t dst, uint8_t src,
                             uint8_t cls, uint8_t op,
                             uint8_t flags, uint8_t seq,
                             const void* payload, uint16_t payload_len) {
  size_t need = (size_t)WUPS_FRAMING_BYTES + payload_len;
  if (out_cap < need) return 0;
  out[0] = WUPS_SYNC1;
  out[1] = WUPS_SYNC2;
  out[2] = dst;
  out[3] = src;
  out[4] = cls;
  out[5] = op;
  out[6] = flags;
  out[7] = seq;
  out[8] = (uint8_t)(payload_len & 0xFFu);
  out[9] = (uint8_t)((payload_len >> 8) & 0xFFu);
  if (payload_len) memcpy(out + 10, payload, payload_len);
  uint8_t a = 0, b = 0;
  for (size_t i = 2; i < 10u + (size_t)payload_len; ++i) {
    a = (uint8_t)(a + out[i]);
    b = (uint8_t)(b + a);
  }
  out[10 + payload_len]     = a;
  out[10 + payload_len + 1] = b;
  out[10 + payload_len + 2] = WUPS_END1;
  out[10 + payload_len + 3] = WUPS_END2;
  return need;
}

// Wrap a cached CH32X power.status into a WUPS frame (src=CH32X preserved
// for the panel's audit trail) and ship it to the ESP32 as net.publish
// onto the "telemetry" subtopic.
static void wupsPublishTelemetryStatus(uint8_t seq, const wups_power_status_v1_t& s) {
  uint8_t frame[WUPS_FRAMING_BYTES + sizeof(wups_power_status_v1_t)];
  size_t n = wupsBuildFrame(frame, sizeof(frame),
                            WUPS_ADDR_BROADCAST, WUPS_ADDR_CH32X,
                            WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                            WUPS_FLAG_EVENT, seq, &s, sizeof(s));
  if (n == 0) return;
  wupsRequestPublish("telemetry", /*qos=*/0, /*retain=*/0, frame, (uint16_t)n);
}

// Send `system.hello` broadcast on boot so other nodes can discover us.
static void wupsSendHelloBcast(void) {
  wups_sys_hello_v1_t h;
  h.version       = 1;
  h.proto_version = WUPS_PROTO_VERSION;
  h.node_addr     = WUPS_ADDR_RP2040;
  h.reserved      = 0;
  h.fw_version    = (uint16_t)((1u << 8) | 0u);
  h.caps_classes  = WUPS_CAP_SYSTEM | WUPS_CAP_UI;
  h.build_id      = 0;
  // Broadcast goes to every reachable MCU port (and USB-CDC if a host is
  // attached). The router handles fan-out to all ports for us — but for
  // hello we want to reach every link, not just the address-mapped one,
  // so we issue three explicit sends with DST=BROADCAST.
  wups_send(WUPS_PORT_RPI,   WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
            WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, &h, sizeof(h));
  wups_send(WUPS_PORT_CH32X, WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
            WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, &h, sizeof(h));
  wups_send(WUPS_PORT_ESP32, WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
            WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, &h, sizeof(h));
}

// Check buttons with debounce, returns: -1=left, 0=none, +1=right
static int8_t checkButtons() {
  static unsigned long lastDebounceLeft = 0;
  static unsigned long lastDebounceRight = 0;

  unsigned long now = millis();
  int8_t result = 0;

  bool btnLeft = digitalRead(BTN_LEFT_PIN);
  bool btnRight = digitalRead(BTN_RIGHT_PIN);

  // Left button - detect falling edge (HIGH to LOW)
  if (btnLeft == LOW && lastBtnLeftState == HIGH) {
    if (now - lastDebounceLeft >= DEBOUNCE_MS) {
      result = -1;
      lastDebounceLeft = now;
    }
  }
  lastBtnLeftState = btnLeft;

  // Right button - detect falling edge
  if (btnRight == LOW && lastBtnRightState == HIGH) {
    if (now - lastDebounceRight >= DEBOUNCE_MS) {
      result = 1;
      lastDebounceRight = now;
    }
  }
  lastBtnRightState = btnRight;

  return result;
}

void setup() {
  // Power-hold
  pinMode(RP_HOLD_VDD_PIN, OUTPUT);
  digitalWrite(RP_HOLD_VDD_PIN, HIGH);

  // OLED reset pin
  pinMode(RP_UI_GPIO_PIN, OUTPUT);
  digitalWrite(RP_UI_GPIO_PIN, HIGH);

  // Buzzer pin
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Button pins (active LOW with internal pullup)
  pinMode(BTN_LEFT_PIN, INPUT_PULLUP);
  pinMode(BTN_RIGHT_PIN, INPUT_PULLUP);

  // GPIO18, 19 - unused, set as input with pullup (not floating)
  pinMode(GPIO18_PIN, INPUT_PULLUP);
  pinMode(GPIO19_PIN, INPUT_PULLUP);

  // GPIO26, 27 - unused ADC pins, drive LOW
  pinMode(GPIO26_PIN, OUTPUT);
  digitalWrite(GPIO26_PIN, LOW);
  pinMode(GPIO27_PIN, OUTPUT);
  digitalWrite(GPIO27_PIN, LOW);

  // ADC resolution
  analogReadResolution(ADC_BITS);

  Serial.begin(115200);
  dbgSerial.begin(DBG_BAUD);
  while (!Serial && millis() < 3000); // Wait for USB Serial (max 3s)
  dbgOut.println(F("Web3 Pi UPS RP2040 boot"));

  // UART0 to CH32X: RX on GPIO17, TX on GPIO16, binary protocol v1.
  Serial1.setRX(UART0_RX_PIN);
  Serial1.setTX(GPIO16_PIN);
  Serial1.setFIFOSize(256);  // Increase RX buffer (default is 32)
  Serial1.begin(921600);
  // Bump GPIO16 drive 4 mA -> 12 mA + slewfast for cleaner edges through patch wire.
  // PADS reg layout: [0]=SLEWFAST, [1]=SCHMITT, [2]=PDE, [3]=PUE, [5:4]=DRIVE, [6]=IE, [7]=OD
  // Touch only DRIVE (5:4) and SLEWFAST (0); leave SCHMITT, PDE, PUE, IE alone.
  volatile uint32_t* gpio16_pads = (volatile uint32_t*)(0x4001c000u + 0x04u + 16u * 4u);
  *gpio16_pads = (*gpio16_pads & ~0x31u) | 0x31u;
  dbgOut.println(F("UART0 bidir on GPIO17/16 @ 921600, GPIO16 drive=12mA"));

  // UART1 to M.2 ESP32 — hardware Serial2 with CTS/RTS flow control. setCTS
  // and setRTS must be called before begin(); arduino-pico enables HW flow
  // control automatically when both pins are configured.
  Serial2.setTX(UART1_TX_PIN);
  Serial2.setRX(UART1_RX_PIN);
  Serial2.setCTS(UART1_CTS_PIN);
  Serial2.setRTS(UART1_RTS_PIN);
  Serial2.setFIFOSize(256);
  Serial2.begin(UART1_BAUD);
  dbgOut.println(F("UART1 bidir on GPIO20(TX)/21(RX) + CTS22/RTS23 @ 921600"));

  // Wire the binary router up to all three streams. Bytes arriving on any
  // of these now feed wups_router_drain() in loop() and dispatch via
  // wups_on_local_frame() (defined above).
  wups_router_init(&Serial, &Serial1, &Serial2);
  wupsSendHelloBcast();

  // Init I2C for OLED
  WireCustom.begin();
  WireCustom.setClock(400000);

  // Start OLED
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    dbgOut.println(F("SSD1306 init failed!"));
    while (true) { delay(1000); }
  }

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(11, 2);
  oled.print(F("Web3 Pi"));
  oled.setTextSize(2);
  oled.setCursor(14, 14);
  oled.print(F("UPS"));
  oled.display();

  // Play startup melody
  playStartupMelody();

  // Initialize interaction time for auto-return
  lastInteractionTime = millis();

  // Start stabilization timer - suppress alerts until ADC/EMA settle
  startupEndTime = millis() + STARTUP_STABILIZE_MS;
}

void loop() {
  // Drain commands from the host service (USB-CDC) and the Probe UART (J350)
  // and forward both to CH32X. Each source has its own framing state.
  // Drain USB-CDC, UART0 (CH32X) and UART1 (ESP32) into per-port deframers,
  // route forwarded frames, and dispatch local frames to wups_on_local_frame().
  wups_router_drain();

  // Shovel as much of the debug ring buffer as the SerialPIO HW FIFO can
  // accept, then return. Non-blocking, runs every loop iteration (~50 ms).
  dbgRing.pump(dbgSerial);

  // Read ADC values (single sample, EMA filtering handles noise)
  int rawBattVolt = analogRead(ADC_BATT_VOLT_PIN);
  int rawVbusOut  = analogRead(ADC_VBUS_OUT_PIN);

  // Convert to voltage (mV)
  float adcLsb_mV = (VREF * 1000.0f) / ADC_MAX;

  // Calculate battery voltage from divider (mV)
  // Vbat = VADC / DIVIDER_RATIO
  float battAdc_mV = rawBattVolt * adcLsb_mV;
  int battVolt_mV = (int)(battAdc_mV / BATT_DIVIDER_RATIO + 0.5f);

  // Calculate VBUS output voltage from divider (mV)
  float vbusAdc_mV = rawVbusOut * adcLsb_mV;
  vbus_out_mV = (int)(vbusAdc_mV / VBUS_DIVIDER_RATIO + 0.5f);

  // CH32X bytes are pulled from Serial1 by wups_router_drain() (called at the
  // top of loop()). Out-of-frame bytes (e.g. CH32X printf debug strings on
  // USART2) are silently dropped by the deframer in v1. If we ever need them
  // back, the router can grow a per-port "stray byte sink".

  // --- Button handling ---
  // On the Power Control screen the buttons trigger ups.power.* commands.
  // On every other screen they navigate left/right.
  int8_t btnAction = checkButtons();
  if (btnAction != 0) {
    lastInteractionTime = millis();

    if (currentScreen == SCREEN_POWER_CTRL) {
      // Power Control screen: send a binary REQ to CH32X. No NEED_ACK in v1,
      // so we don't wait for a response — confirmation comes via the next
      // power.status push (vbus_out_mV transitioning).
      uint8_t op  = (btnAction < 0) ? WUPS_OP_PWR_DISABLE : WUPS_OP_PWR_ENABLE;
      uint8_t seq = wups_send(WUPS_PORT_CH32X, WUPS_ADDR_CH32X,
                              WUPS_CLASS_POWER, op, WUPS_FLAG_REQ,
                              nullptr, 0);
      dbgOut.print(btnAction < 0 ? F("[btn] LEFT -> power.disable seq=")
                                 : F("[btn] RIGHT -> power.enable seq="));
      dbgOut.println(seq);
    } else {
      if (btnAction > 0) {
        // RIGHT pressed - next screen (wrap)
        currentScreen = (currentScreen + 1) % SCREEN_COUNT;
      } else {
        // LEFT pressed - previous screen (wrap)
        currentScreen = (currentScreen + SCREEN_COUNT - 1) % SCREEN_COUNT;
      }
    }
    tone(BUZZER_PIN, 1000, 20);
  }

  // Auto-return to home screen after timeout
  if (currentScreen != 0 && (millis() - lastInteractionTime >= AUTO_RETURN_MS)) {
    currentScreen = 0;
  }

  // --- Dashboard display ---
  static uint8_t animPhase = 0;
  animPhase++;

  // Use own ADC measurement for battery voltage (primary source)
  int batteryVoltage_mV = battVolt_mV;

  // Apply EMA filtering to battery voltage
  if (filtered_batt_mV < 0) {
    // First reading - initialize
    filtered_batt_mV = (float)batteryVoltage_mV;
  } else {
    filtered_batt_mV = EMA_ALPHA * batteryVoltage_mV + (1.0f - EMA_ALPHA) * filtered_batt_mV;
  }

  // Update ui.bv with filtered value (for compatibility)
  ui.bv = (int)(filtered_batt_mV + 0.5f);

  // Calculate SOC from filtered battery voltage, then apply adaptive EMA:
  // - Snap immediately on first reading or large changes (battery plug/unplug)
  // - Smooth small oscillations from LUT boundary crossings
  int rawSoc = voltageToSoc(ui.bv);
  if (filtered_soc < 0 || abs(rawSoc - (int)filtered_soc) > SOC_SNAP_THRESHOLD) {
    filtered_soc = (float)rawSoc;
  } else {
    filtered_soc = SOC_EMA_ALPHA * rawSoc + (1.0f - SOC_EMA_ALPHA) * filtered_soc;
  }
  int soc = (int)(filtered_soc + 0.5f);
  ui.soc = soc;

  // CH32X power.status is forwarded to RPi inside wups_on_local_frame() at
  // the moment of arrival (preserving SRC=CH32X). Augmenting with RP2040's
  // bv/SOC/vo would require a separate v2 power.aggregate op — deferred.
  // For now we simply consume `newFrameReceived` as a "data updated" flag.
  newFrameReceived = false;

  // --- Startup stabilization gate ---
  // During first 3 seconds, let ADC/EMA settle before making decisions
  // Splash screen (set in setup) remains visible on OLED
  if (!startupComplete) {
    if (millis() >= startupEndTime) {
      startupComplete = true;
      previousPowerGood = (ui.pg == 1);
      lastLowBatteryBeep = millis();
      displayCs = ui.cs;
      noBatteryDebounced = (lastFrameTime > 0) ? (ui.bp == 0)
                         : (ui.bv > 10000 || ui.bv < 5000);
    } else {
      delay(50);
      return;
    }
  }

  // Battery presence detection:
  // Primary: use bp (battery present) from MP2762A charger IC (hardware UVLO threshold)
  // Fallback: voltage-based detection when no UART data received yet
  bool rawNoBattery;
  if (lastFrameTime > 0) {
    rawNoBattery = (ui.bp == 0);
  } else {
    rawNoBattery = (ui.bv > 10000) || (ui.bv < 5000);
  }

  // Debounce battery presence to prevent display flicker
  if (rawNoBattery != noBatteryDebounced) {
    noBatteryCounter++;
    if (noBatteryCounter >= NO_BATTERY_DEBOUNCE) {
      noBatteryDebounced = rawNoBattery;
      noBatteryCounter = 0;
    }
  } else {
    noBatteryCounter = 0;
  }
  bool noBattery = noBatteryDebounced;

  // Smooth displayed charge state to prevent CHG/FUL flicker at end-of-charge
  // (MP2762A toggles between CC and CV states when battery is nearly full)
  if (ui.cs != displayCs) {
    csChangeCounter++;
    if (csChangeCounter >= CS_CHANGE_DEBOUNCE) {
      displayCs = ui.cs;
      csChangeCounter = 0;
    }
  } else {
    csChangeCounter = 0;
  }

  oled.clearDisplay();

  // Check for invalid charger: power connected but voltage too low or garbage
  // pg=1 means power is present
  // vi < 8000 means ~5V (non-PD or PD at 5V only - not enough for 26W)
  // vi > 21000 means garbage/saturated ADC (charger not working properly)
  bool badCharger = (ui.pg == 1 && (ui.vi < 8000 || ui.vi > 21000));

  if (badCharger) {
    // Buzzer alert logic
    unsigned long now = millis();
    if (!badChargerAlertPlayed) {
      // First detection - play error sound sequence
      playErrorSound();
      badChargerAlertPlayed = true;
      lastReminderTime = now;
    } else if (now - lastReminderTime >= BAD_PSU_REMINDER_INTERVAL_MS) {
      // Periodic reminder beep
      playReminderBeep();
      lastReminderTime = now;
    }

    // Warning screen - bad charger (no PD) - takes priority over all screens
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    // Flashing "!" icon
    if ((animPhase / 8) % 2 == 0) {
      oled.setCursor(0, 0);
      oled.print(F("!"));
    }

    oled.setCursor(10, 0);
    oled.print(F("BAD PSU"));

    oled.setCursor(0, 12);
    oled.print(F("Need PD"));
    oled.setCursor(0, 22);
    oled.print(F("26W min"));
  } else {
    // Reset alert state when charger is OK or disconnected
    badChargerAlertPlayed = false;

    // Draw current screen based on navigation
    switch (currentScreen) {
      case 0:
        drawScreenHome(soc, noBattery, animPhase);
        break;
      case 1:
        drawScreenPower();
        break;
      case 2:
        drawScreenBattery();
        break;
      case 3:
        drawScreenPDInfo();
        break;
      case SCREEN_PD_DIAG:
        drawScreenPDDiag();
        break;
      case SCREEN_POWER_PATH:
        drawScreenPowerPath();
        break;
      case SCREEN_POWER_CTRL:
        drawScreenPowerCtrl();
        break;
    }
  }

  oled.display();

  // --- Power transition detection (charger disconnected -> battery power) ---
  bool currentPowerGood = (ui.pg == 1);

  // Detect transition from power connected to battery power
  if (previousPowerGood && !currentPowerGood && !powerLossAlertPlayed) {
    playPowerLossAlarm();
    powerLossAlertPlayed = true;
    lastLowBatteryBeep = millis();  // Reset low battery timer
  }

  // Reset alert flag when power is restored
  if (currentPowerGood) {
    powerLossAlertPlayed = false;
  }

  previousPowerGood = currentPowerGood;

  // --- Low battery warning beeps (only on battery power) ---
  if (!currentPowerGood && !noBattery && !badCharger) {
    unsigned long now = millis();

    if (soc < 10) {
      // Critical battery: beep every 5 seconds
      if (now - lastLowBatteryBeep >= CRITICAL_BATTERY_INTERVAL_MS) {
        playCriticalBatteryBeep();
        lastLowBatteryBeep = now;
      }
    } else if (soc < 20) {
      // Low battery: beep every 30 seconds
      if (now - lastLowBatteryBeep >= LOW_BATTERY_INTERVAL_MS) {
        playLowBatteryBeep();
        lastLowBatteryBeep = now;
      }
    }
  }

  delay(50);
}
