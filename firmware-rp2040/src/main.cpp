#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SerialPIO.h>

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
// so writing a 250-byte status JSON straight to it would stall loop() for
// ~22 ms (or forever if the PIO TX SM stops draining). DbgRing absorbs bursts
// in 1 KB of RAM, drops bytes silently when full, and is drained in loop()
// only as fast as the PIO HW FIFO has room — fully non-blocking.
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
// DTR is deasserted — without this guard a 290-byte status JSON freezes
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
constexpr uint8_t GPIO20_PIN = 20;   // GPIO20 - not used, set as input pullup
constexpr uint8_t UART0_RX_PIN = 17; // GPIO17 - UART0 RX (status + responses from CH32X)

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
constexpr uint8_t SCREEN_COUNT = 5;
constexpr uint8_t SCREEN_POWER_CTRL = 4;  // last screen — buttons act as power on/off
constexpr unsigned long AUTO_RETURN_MS = 15000;  // Auto-return to home after 15s

// --- Bad charger alert state ---
bool badChargerAlertPlayed = false;
unsigned long lastReminderTime = 0;

// --- Battery power loss detection ---
bool previousPowerGood = false;     // Initialized after startup stabilization
bool powerLossAlertPlayed = false;  // Prevent repeated alerts

// --- Low battery warning state ---
unsigned long lastLowBatteryBeep = 0;

unsigned long lastJsonTime = 0;  // Track when last JSON was received

// --- Screen navigation state ---
uint8_t currentScreen = 0;
unsigned long lastInteractionTime = 0;
bool lastBtnLeftState = HIGH;
bool lastBtnRightState = HIGH;

// --- UART JSON data ---
char uartBuffer[256];
int uartIdx = 0;

// --- UART debug message buffer ---
char dbgBuffer[128];
int dbgIdx = 0;

// --- Command framing from a host-side stream -> CH32X ---
// Hosts write single-line JSON commands like {"cmd":"ups.power.cycle","id":7}
// to either USB-CDC (Pi 5 service) or the Probe UART (J350). We accumulate
// brace-balanced frames and forward verbatim to Serial1 (CH32X). One state
// struct per source so the two streams can interleave bytes safely.
struct CmdRxState {
  char buffer[256];
  int  idx = 0;
  bool inJson = false;
};
CmdRxState hostRx;
CmdRxState dbgRx;
uint32_t nextCmdId = 1;
volatile uint32_t cmdFramesForwarded = 0;  // diagnostic — read via SWD to confirm forwarding works
int json_t = 0;    // temperature
int json_vs = 0;   // voltage source
int json_is = 0;   // current source
int json_vr = 0;   // voltage regulated
int json_ir = 0;   // current regulated
int json_cs = 0;   // charge state (0-3)
int json_pd = 0;   // PD negotiation status (OUTPUT side, not input!)
int json_pdo = 0;  // Power Delivery Object (OUTPUT side)
int json_pg = 0;   // Power Good (1 = power connected)
int json_vi = 0;   // Input voltage (mV) from MP2762A
int json_ci = 0;   // Charge current (mA) from MP2762A
int json_cf = 0;   // Charger fault flags from MP2762A
int json_bp = 0;   // Battery present (0/1)
int json_vb = 0;   // Battery voltage (mV) from remote MCU ADC
int json_vbc = 0;  // Battery voltage (mV) from MP2762A charger IC
int json_role = 0;    // Current role (0=SINK, 1=SOURCE)
int json_snk_ok = 0;  // SINK negotiation success
int json_snk_v = 0;   // Negotiated SINK voltage (0.1V)
int json_snk_i = 0;   // Negotiated SINK current (0.1A)

// --- Battery values calculated from ADC (for compatibility) ---
int json_bv = 0;   // battery voltage (mV) - from ADC
int json_soc = 0;  // state of charge (%) - calculated from voltage

// --- VBUS output voltage from ADC ---
int vbus_out_mV = 0;  // USB-C VBUS output voltage (mV) from ADC3

// --- Filtered battery voltage (EMA with alpha=0.1 for ~30s smoothing) ---
float filtered_batt_mV = -1.0f;   // From ADC, -1 = not initialized
constexpr float EMA_ALPHA = 0.1f;

// --- Filtered SOC (adaptive EMA: snap on large changes, smooth small oscillations) ---
float filtered_soc = -1.0f;
constexpr float SOC_EMA_ALPHA = 0.05f;
constexpr int SOC_SNAP_THRESHOLD = 3;  // Snap immediately if SOC changes by more than 3%
bool newJsonReceived = false;

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

// Debug mode - raw UART passthrough (set to 1 to enable)
#define DEBUG_RAW_UART 0

// Extract integer value for a given key from JSON
// Returns true if found and valid, false otherwise
bool extractInt(const char* json, const char* key, int& outVal) {
  char searchKey[16];
  snprintf(searchKey, sizeof(searchKey), "\"%s\":", key);

  const char* p = strstr(json, searchKey);
  if (p == nullptr) return false;

  p += strlen(searchKey);

  // Skip whitespace
  while (*p == ' ' || *p == '\t') p++;

  // Check if we have a valid number start
  if (*p != '-' && (*p < '0' || *p > '9')) return false;

  outVal = atoi(p);
  return true;
}

// Parse JSON - only updates values that are found and valid
void parseJson(const char* json) {
  // Basic validation: must start with { and contain at least one :
  if (json[0] != '{' || strchr(json, ':') == nullptr) {
    dbgOut.println(F("JSON: invalid format"));
    return;
  }

  int newVal;
  if (extractInt(json, "t", newVal)) json_t = newVal;
  if (extractInt(json, "vs", newVal)) json_vs = newVal;
  if (extractInt(json, "is", newVal)) json_is = newVal;
  if (extractInt(json, "vr", newVal)) json_vr = newVal;
  if (extractInt(json, "ir", newVal)) json_ir = newVal;
  if (extractInt(json, "cs", newVal)) json_cs = newVal;
  if (extractInt(json, "pd", newVal)) json_pd = newVal;
  if (extractInt(json, "pdo", newVal)) json_pdo = newVal;
  if (extractInt(json, "pg", newVal)) json_pg = newVal;
  if (extractInt(json, "vi", newVal)) json_vi = newVal;
  if (extractInt(json, "ci", newVal)) json_ci = newVal;
  if (extractInt(json, "cf", newVal)) json_cf = newVal;
  if (extractInt(json, "bp", newVal)) json_bp = newVal;
  if (extractInt(json, "vb", newVal)) json_vb = newVal;
  if (extractInt(json, "vbc", newVal)) json_vbc = newVal;
  if (extractInt(json, "role", newVal)) json_role = newVal;
  if (extractInt(json, "snk_ok", newVal)) json_snk_ok = newVal;
  if (extractInt(json, "snk_v", newVal)) json_snk_v = newVal;
  if (extractInt(json, "snk_i", newVal)) json_snk_i = newVal;
}

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
  oled.print(json_bv / 1000);
  oled.print(F("."));
  oled.print((json_bv % 1000) / 100);
  oled.print(F("V"));

  // Bottom line: input voltage + output voltage
  oled.setCursor(0, 25);
  // Input voltage (vi)
  if (json_vi > 0) {
    oled.print(json_vi / 1000);
    oled.print(F("."));
    oled.print((json_vi % 1000) / 100);
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
  oled.print(json_vs / 10);
  oled.print(F("."));
  oled.print(json_vs % 10);
  oled.print(F("V "));
  oled.print(json_is / 10);
  oled.print(F("."));
  oled.print(json_is % 10);
  oled.print(F("A"));

  // Line 2: Header
  oled.setCursor(0, 16);
  oled.print(F("PWR RD:"));

  // Line 3: Voltage READ, Current READ
  oled.setCursor(0, 24);
  oled.print(json_vr / 10);
  oled.print(F("."));
  oled.print(json_vr % 10);
  oled.print(F("V "));
  oled.print(json_ir / 10);
  oled.print(F("."));
  oled.print(json_ir % 10);
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
  oled.print(json_t / 10);
  oled.print(F("."));
  oled.print(abs(json_t % 10));
  oled.print(F("C"));

  // Line 1: Battery voltage (from ADC)
  oled.setCursor(0, 8);
  oled.print(F("Bat:"));
  oled.print(json_bv / 1000);
  oled.print(F("."));
  int frac = (json_bv % 1000) / 10;
  if (frac < 10) oled.print(F("0"));
  oled.print(frac);
  oled.print(F("V"));

  // Line 2: Charger fault flags (hex)
  oled.setCursor(0, 16);
  oled.print(F("CF:0x"));
  if (json_cf < 16) oled.print(F("0"));
  oled.print(json_cf, HEX);

  // Line 3: Charge current
  oled.setCursor(0, 24);
  oled.print(F("CI:"));
  oled.print(json_ci);
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
  oled.print(json_pd);

  // Line 2: PDO Index
  oled.setCursor(0, 16);
  oled.print(F("PDO:"));
  oled.print(json_pdo);

  // Line 3: Output voltage (measured from ADC3)
  oled.setCursor(0, 24);
  oled.print(F("Vo:"));
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  oled.print((vbus_out_mV % 1000) / 100);
  oled.print(F("V"));

  drawScreenIndicator(3);
}

// Screen 4: Power Control — LEFT disables VBUS_OUT, RIGHT re-enables it.
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

// Send a JSON command to CH32X over Serial1.
// Returns the auto-assigned command id so the caller can match responses.
static uint32_t sendUpsCommand(const char* cmd) {
  uint32_t id = nextCmdId++;
  Serial1.print(F("{\"cmd\":\""));
  Serial1.print(cmd);
  Serial1.print(F("\",\"id\":"));
  Serial1.print(id);
  Serial1.println(F("}"));
  return id;
}

// Drain a command stream: collect brace-balanced frames and forward verbatim
// to CH32X. Bytes outside frames are ignored.
static void drainCmdStream(Stream& src, CmdRxState& st) {
  while (src.available()) {
    char c = (char)src.read();
    if (c == '{') {
      st.idx = 0;
      st.buffer[st.idx++] = c;
      st.inJson = true;
    } else if (st.inJson) {
      if (st.idx >= (int)sizeof(st.buffer) - 1) {
        // Frame too long: discard
        st.idx = 0;
        st.inJson = false;
        continue;
      }
      st.buffer[st.idx++] = c;
      if (c == '}') {
        st.buffer[st.idx] = '\0';
        Serial1.print(st.buffer);
        Serial1.print(F("\r\n"));
        cmdFramesForwarded++;
        st.idx = 0;
        st.inJson = false;
      }
    }
  }
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

  // GPIO18, 19, 20 - set as input with pullup (not floating)
  pinMode(GPIO18_PIN, INPUT_PULLUP);
  pinMode(GPIO19_PIN, INPUT_PULLUP);
  pinMode(GPIO20_PIN, INPUT_PULLUP);

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

  // UART0 to CH32X: RX on GPIO17 (status JSON + responses), TX on GPIO16 (commands)
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
  drainCmdStream(Serial, hostRx);
  drainCmdStream(dbgSerial, dbgRx);

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

  // Read UART1
  while (Serial1.available()) {
    char c = Serial1.read();

#if DEBUG_RAW_UART
    Serial.print(c);
#else

    // Normal mode: parse JSON and debug messages
    if (c == '{') {
      // Start of JSON
      uartIdx = 0;
      uartBuffer[uartIdx++] = c;
      dbgIdx = 0; // Cancel any debug message in progress
    }
    else if (uartIdx > 0 && uartIdx < (int)sizeof(uartBuffer) - 1) {
      // Inside JSON
      uartBuffer[uartIdx++] = c;

      if (c == '}') {
        uartBuffer[uartIdx] = '\0';
        parseJson(uartBuffer);
        newJsonReceived = true;  // Will output merged JSON later
        lastJsonTime = millis();
        uartIdx = 0;
      }
    }
    else if (uartIdx >= (int)sizeof(uartBuffer) - 1) {
      // JSON buffer overflow
      uartIdx = 0;
    }
    else if (uartIdx == 0) {
      // Not inside JSON - handle debug messages
      if (c == '\n') {
        // End of debug line
        dbgBuffer[dbgIdx] = '\0';
        if (dbgIdx > 0) {
          dbgOut.println(dbgBuffer);
        }
        dbgIdx = 0;
      }
      else if (c != '\r' && dbgIdx < (int)sizeof(dbgBuffer) - 1) {
        // Accumulate debug characters (skip \r)
        dbgBuffer[dbgIdx++] = c;
      }
      else if (dbgIdx >= (int)sizeof(dbgBuffer) - 1) {
        // Debug buffer overflow - flush
        dbgBuffer[dbgIdx] = '\0';
        dbgOut.println(dbgBuffer);
        dbgIdx = 0;
      }
    }
#endif
  }

  // --- Button handling ---
  // On the Power Control screen the buttons trigger ups.power.* commands.
  // On every other screen they navigate left/right.
  int8_t btnAction = checkButtons();
  if (btnAction != 0) {
    lastInteractionTime = millis();

    if (currentScreen == SCREEN_POWER_CTRL) {
      if (btnAction < 0) {
        uint32_t id = sendUpsCommand("ups.power.disable");
        dbgOut.print(F("[btn] LEFT -> ups.power.disable id="));
        dbgOut.println(id);
      } else {
        uint32_t id = sendUpsCommand("ups.power.enable");
        dbgOut.print(F("[btn] RIGHT -> ups.power.enable id="));
        dbgOut.println(id);
      }
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

  // Update json_bv with filtered value (for compatibility)
  json_bv = (int)(filtered_batt_mV + 0.5f);

  // Calculate SOC from filtered battery voltage, then apply adaptive EMA:
  // - Snap immediately on first reading or large changes (battery plug/unplug)
  // - Smooth small oscillations from LUT boundary crossings
  int rawSoc = voltageToSoc(json_bv);
  if (filtered_soc < 0 || abs(rawSoc - (int)filtered_soc) > SOC_SNAP_THRESHOLD) {
    filtered_soc = (float)rawSoc;
  } else {
    filtered_soc = SOC_EMA_ALPHA * rawSoc + (1.0f - SOC_EMA_ALPHA) * filtered_soc;
  }
  int soc = (int)(filtered_soc + 0.5f);
  json_soc = soc;

  // Forward incoming JSON from CH32X to USB-CDC.
  // - Status frames: augment with RP2040-measured bv/SOC/bd/vo before re-emitting.
  // - Command responses (contain "resp":): forward verbatim, no augmentation.
  if (newJsonReceived) {
    bool isResponse = (strstr(uartBuffer, "\"resp\":") != nullptr);
    int len = strlen(uartBuffer);

    if (isResponse) {
      dbgOut.println(uartBuffer);
    } else if (len > 0 && uartBuffer[len - 1] == '}') {
      uartBuffer[len - 1] = '\0';  // Remove closing brace
      dbgOut.print(uartBuffer);
      dbgOut.print(F(",\"bv\":"));
      dbgOut.print(json_bv);
      dbgOut.print(F(",\"SOC\":"));
      dbgOut.print(soc);
      dbgOut.print(F(",\"bd\":"));
      dbgOut.print(soc);
      dbgOut.print(F(",\"vo\":"));
      dbgOut.print(vbus_out_mV);
      dbgOut.println(F("}"));
    } else {
      dbgOut.println(uartBuffer);  // Fallback: print as-is
    }
    newJsonReceived = false;
  }

  // --- Startup stabilization gate ---
  // During first 3 seconds, let ADC/EMA settle before making decisions
  // Splash screen (set in setup) remains visible on OLED
  if (!startupComplete) {
    if (millis() >= startupEndTime) {
      startupComplete = true;
      previousPowerGood = (json_pg == 1);
      lastLowBatteryBeep = millis();
      displayCs = json_cs;
      noBatteryDebounced = (lastJsonTime > 0) ? (json_bp == 0)
                         : (json_bv > 10000 || json_bv < 5000);
    } else {
      delay(50);
      return;
    }
  }

  // Battery presence detection:
  // Primary: use bp (battery present) from MP2762A charger IC (hardware UVLO threshold)
  // Fallback: voltage-based detection when no UART data received yet
  bool rawNoBattery;
  if (lastJsonTime > 0) {
    rawNoBattery = (json_bp == 0);
  } else {
    rawNoBattery = (json_bv > 10000) || (json_bv < 5000);
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
  if (json_cs != displayCs) {
    csChangeCounter++;
    if (csChangeCounter >= CS_CHANGE_DEBOUNCE) {
      displayCs = json_cs;
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
  bool badCharger = (json_pg == 1 && (json_vi < 8000 || json_vi > 21000));

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
      case SCREEN_POWER_CTRL:
        drawScreenPowerCtrl();
        break;
    }
  }

  oled.display();

  // --- Power transition detection (charger disconnected -> battery power) ---
  bool currentPowerGood = (json_pg == 1);

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
