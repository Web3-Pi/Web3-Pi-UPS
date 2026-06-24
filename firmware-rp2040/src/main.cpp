#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SerialPIO.h>
#include <string.h>
#include "wups_proto.h"
#include "wups_router.h"
#include "trust_ui.h"
#include "ui_settings.h"
#include "local_menu.h"

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
// Screen strip (power.status v2 debug screens). Home (0) is the dashboard;
// 1..4 are the developer readouts split along the v2 struct sections.
constexpr uint8_t SCREEN_HOME    = 0;
constexpr uint8_t SCREEN_INPUT   = 1;   // VIN / input PD contract / source
constexpr uint8_t SCREEN_OUTPUT  = 2;   // Vout RP2040 vs CH32X / output PD / Ilim
constexpr uint8_t SCREEN_BATTERY = 3;   // Vbat RP2040 vs CH32X / mode / charge current
constexpr uint8_t SCREEN_SYSTEM  = 4;   // uptime / temps / fault bitmap
constexpr uint8_t SCREEN_COUNT   = 5;
constexpr unsigned long AUTO_RETURN_MS = 20000;  // Auto-return to home after 20s

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

// --- Cached CH32X power.status ---
// CH32X emits a power.status frame every 1 s. wups_on_local_frame() projects
// fields into the `ui` snapshot below. The v2 redesign is NOT a prefix-
// compatible superset of v1, so we cache the RAW payload (version-agnostic)
// for the republish / uplink paths — forwarding the decoded struct would
// truncate the v2 tail. Last_Power_Status / Last_Pwr_V2 are kept as decoded
// snapshots for convenience but MUST NOT gate forwarding.
wups_power_status_v1_t Last_Power_Status = {};
static wups_power_status_v2_t Last_Pwr_V2 = {};
unsigned long Last_Power_Status_Ms = 0;

// Raw last-accepted power.status payload (any version) for republish/uplink.
static uint8_t  Last_Pwr_Raw[WUPS_MAX_PAYLOAD];
static uint16_t Last_Pwr_Raw_Len = 0;

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
    // From CH32X power.status v2 (raw mV / mA, 0 = N/A sentinel for PD fields):
    int pd_in_mV, pd_in_mA;     // HUSB238 input contract (0 = no contract)
    int pd_out_mV, pd_out_mA;   // output PD contract to the Pi (0 = rail off)
    int vbus_out_ch_mV;         // CH32X PA0 VBUS_OUT measurement (mV)
    int vsys_mV;                // MP2762A VSYS rail (mV)
    int iin_mA;                 // MP2762A charger input current (mA)
    int temp_lm_dC;             // LM75B board temp (deci-Celsius)
    int temp_mp_dC;             // MP2762A junction temp (deci-Celsius, -32768 = N/A)
    int usb_c_attach;           // 1 if HUSB238 reports a PD contract present
    int soc_ch;                 // SOC from CH32X vbat (0..100, LUT)
    int iout_limit_mA;          // TPS55289 current LIMIT (mA)
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

// --- ESP32 backend-menu hand-off (ADR-0012, revised) ---
// The LEFT-hold gesture now opens the RP2040-LOCAL menu (local_menu.*). The
// ESP32 backend menu is reached from there via the "Network" item, which
// broadcasts the activation ui.button_event and arms this deadline. The M.2
// module (ESP32+modem) is optional, so we may have no peer to answer: if no
// trust_prompt arrives within MENU_RESPONSE_TIMEOUT_MS we reopen the local
// menu with a "No modem" notice. menu_pending_deadline_ms = 0 means no
// hand-off in flight; non-zero is the absolute millis() to time out at.
static uint32_t menu_pending_deadline_ms = 0;
// Generous so a BUSY-but-present ESP32 (mid TLS / Arkiv telemetry submit) isn't
// falsely declared "No modem". A genuinely absent/wedged M.2 just waits this
// long before the notice — a better trade-off than spurious "No modem" flashes.
constexpr uint32_t MENU_RESPONSE_TIMEOUT_MS = 4000;

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
  if (!ui_settings_sound_enabled()) return;  // muted: skip the blocking delays too
  // Three rapid high-pitched beeps
  for (int i = 0; i < 6; i++) {
    ui_settings_beep(2400, 100);
    delay(150);
  }
  
}

// Play gentle reminder beep - blocking
static void playReminderBeep() {
  if (!ui_settings_sound_enabled()) return;
  ui_settings_beep(2400, 80);
  delay(50);
  ui_settings_beep(2400, 80);
  delay(50);
}

// Play startup melody - pleasant ascending tones
static void playStartupMelody() {
  if (!ui_settings_sound_enabled()) return;
  // C5, E5, G5, C6 - major chord arpeggio
  const int notes[] = {523, 659, 784, 1047};
  const int durations[] = {100, 100, 100, 200};

  for (int i = 0; i < 4; i++) {
    ui_settings_beep(notes[i], durations[i]);
    delay(durations[i] + 30);
  }
  noTone(BUZZER_PIN);
}

// Play power loss alarm - alarming descending tones
static void playPowerLossAlarm() {
  if (!ui_settings_sound_enabled()) return;
  // Descending urgent pattern
  for (int i = 0; i < 3; i++) {
    ui_settings_beep(1500, 150);
    delay(180);
    ui_settings_beep(1000, 150);
    delay(180);
    ui_settings_beep(700, 200);
    delay(300);
  }
  noTone(BUZZER_PIN);
}

// Play low battery warning beep - single short beep
static void playLowBatteryBeep() {
  if (!ui_settings_sound_enabled()) return;
  ui_settings_beep(1800, 100);
  delay(100);
  noTone(BUZZER_PIN);
}

// Play critical battery warning beep - double short beep
static void playCriticalBatteryBeep() {
  if (!ui_settings_sound_enabled()) return;
  ui_settings_beep(2000, 80);
  delay(120);
  ui_settings_beep(2000, 80);
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
static const __FlashStringHelper* batteryModeLabel(bool mains, int cs);

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
  oled.print(batteryModeLabel(ui.pg, displayCs));
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

// Map (mains-present, charge-state) to a 3-char mode label. On battery (no
// mains) the MP2762A is unpowered and reports cs=0 — that is a genuine
// discharge. On mains, cs=0 means "not charging" (idle / pack full), which
// must NOT be shown as discharge. Used by Home and the Battery screen.
static const __FlashStringHelper* batteryModeLabel(bool mains, int cs) {
  if (!mains) return F("DSC");           // no mains -> discharging
  switch (cs) {
    case 1:  return F("PRE");
    case 2:  return F("CHG");
    case 3:  return F("FUL");
    default: return F("IDL");            // mains present, not charging
  }
}

// Screen 1 (INPUT): VIN, input PD contract (HUSB238), derived power, source.
// 64x32, 6x8 font => max 10 chars per line at y=0/8/16/24.
static void drawScreenInput() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // row0: "IN  %2d.%dV" — VIN whole.frac (e.g. "IN  12.3V")
  oled.setCursor(0, 0);
  oled.print(F("IN "));
  int vi_w = ui.vi / 1000;
  if (vi_w < 10) oled.print(F(" "));
  oled.print(vi_w);
  oled.print(F("."));
  oled.print((ui.vi % 1000) / 100);
  oled.print(F("V"));

  // row1: input PD contract, or "PD N/A" when no contract negotiated.
  oled.setCursor(0, 8);
  if (ui.pd_in_mV > 0) {
    int v = ui.pd_in_mV / 1000;
    oled.print(F("PD "));
    if (v < 10) oled.print(F(" "));
    oled.print(v);
    oled.print(F("V"));
    oled.print(ui.pd_in_mA / 1000);
    oled.print(F("."));
    oled.print((ui.pd_in_mA % 1000) / 100);
    oled.print(F("A"));
  } else {
    oled.print(F("PD N/A"));
  }

  // row2: contract power (V*A, integer watts), only when a contract exists.
  oled.setCursor(0, 16);
  if (ui.pd_in_mV > 0) {
    int w = (ui.pd_in_mV / 1000) * (ui.pd_in_mA / 1000);
    oled.print(F("="));
    oled.print(w);
    oled.print(F("W"));
  }

  // row3: which source is feeding us.
  oled.setCursor(0, 24);
  if (ui.usb_c_attach) {
    oled.print(F("SRC:USB-C"));
  } else if (ui.pg) {
    oled.print(F("SRC:BARR"));
  } else {
    oled.print(F("SRC:OFF"));
  }

  drawScreenIndicator(SCREEN_INPUT);
}

// Screen 2 (OUTPUT): RP2040 vs CH32X VBUS_OUT, output PD contract, Ilimit.
static void drawScreenOutput() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // row0: output rail voltage (RP2040's own ADC).
  oled.setCursor(0, 0);
  oled.print(F("V "));
  oled.print(vbus_out_mV / 1000);
  oled.print(F("."));
  oled.print((vbus_out_mV % 1000) / 100);
  oled.print(F("V"));

  // row1: output PD contract to the Pi, or "PD N/A" when the rail is off.
  oled.setCursor(0, 8);
  if (ui.pd_out_mV > 0) {
    int v = ui.pd_out_mV / 1000;
    oled.print(F("PD "));
    if (v < 10) oled.print(F(" "));
    oled.print(v);
    oled.print(F("V"));
    oled.print(ui.pd_out_mA / 1000);
    oled.print(F("."));
    oled.print((ui.pd_out_mA % 1000) / 100);
    oled.print(F("A"));
  } else {
    oled.print(F("PD N/A"));
  }

  // row2: TPS55289 current limit (mA -> whole.frac A).
  oled.setCursor(0, 16);
  oled.print(F("Ilim  "));
  oled.print(ui.iout_limit_mA / 1000);
  oled.print(F("."));
  oled.print((ui.iout_limit_mA % 1000) / 100);
  oled.print(F("A"));

  drawScreenIndicator(SCREEN_OUTPUT);
}

// Screen 3 (BATTERY): RP2040 vs CH32X Vbat/SOC, charge mode, charge current.
static void drawScreenBattery() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // row0: battery voltage + SOC (RP2040's own ADC). "V %d.%dV%2d%%"
  oled.setCursor(0, 0);
  oled.print(F("V "));
  oled.print(ui.bv / 1000);
  oled.print(F("."));
  oled.print((ui.bv % 1000) / 100);
  oled.print(F("V "));
  if (ui.soc < 10) oled.print(F(" "));
  oled.print(ui.soc);
  oled.print(F("%"));

  // row1: charge/discharge mode (mains-aware: cs=0 on mains is "not charging",
  // not discharge).
  oled.setCursor(0, 8);
  oled.print(F("Mode:"));
  oled.print(batteryModeLabel(ui.pg, ui.cs));

  // row2: charge current (mA -> whole.frac A; absolute value for sign-safe
  // single-digit fraction). "Ich %d.%dA"
  int ich = abs(ui.ci);
  oled.setCursor(0, 16);
  oled.print(F("Ich "));
  oled.print(ich / 1000);
  oled.print(F("."));
  oled.print((ich % 1000) / 100);
  oled.print(F("A"));

  drawScreenIndicator(SCREEN_BATTERY);
}

// Screen 4 (SYSTEM): RP2040 uptime, both temps, fault bitmap.
static void drawScreenSystem() {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // row0: compact RP2040 uptime. "UP %s"
  //   < 100 min -> "%dm"; < 100 h -> "%dh%02dm" (or "%dh" once >= 10 h to
  //   stay <= 10 chars); else "%dd".
  unsigned long up_s = millis() / 1000UL;
  unsigned long up_m = up_s / 60UL;
  oled.setCursor(0, 0);
  oled.print(F("UP "));
  if (up_m < 100UL) {
    oled.print(up_m);
    oled.print(F("m"));
  } else {
    unsigned long up_h = up_m / 60UL;
    if (up_h < 100UL) {
      oled.print(up_h);
      oled.print(F("h"));
      if (up_h < 10UL) {
        // "UP 9h59m" = 8 chars — room for minutes.
        unsigned long rem_m = up_m % 60UL;
        if (rem_m < 10UL) oled.print(F("0"));
        oled.print(rem_m);
        oled.print(F("m"));
      }
    } else {
      oled.print(up_h / 24UL);
      oled.print(F("d"));
    }
  }

  // row1: LM75B board temp (deci-C -> whole C). "Tlm %3dC"
  oled.setCursor(0, 8);
  oled.print(F("Tlm "));
  oled.print(ui.temp_lm_dC / 10);
  oled.print(F("C"));

  // row2: MP2762A junction temp, "N/A" when unpowered (-32768 sentinel).
  oled.setCursor(0, 16);
  if (ui.temp_mp_dC == -32768) {
    oled.print(F("Tmp  N/A"));
  } else {
    oled.print(F("Tmp "));
    oled.print(ui.temp_mp_dC / 10);
    oled.print(F("C"));
  }

  // row3: fault bitmap as 4 hex digits. "Flt:%04X"
  oled.setCursor(0, 24);
  oled.print(F("Flt:"));
  unsigned int flt = (unsigned int)(ui.cf & 0xFFFF);
  for (int sh = 12; sh >= 0; sh -= 4) {
    oled.print((flt >> sh) & 0xF, HEX);
  }

  drawScreenIndicator(SCREEN_SYSTEM);
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
static void wupsPublishTelemetryStatus(uint8_t seq, const uint8_t* payload, uint16_t len);
static void wupsPublishHostStatus(uint8_t seq, const wups_host_status_v1_t& s);
static void wupsPublishCmdResponse(uint8_t cls, uint8_t op, uint8_t seq, uint8_t code);
static void wupsPublishEventFrame(const WupsFrame& f);

// Cellular uplink is metered: the M.2 modem runs on a ~500 MB/mo LTE data
// plan, sized from measured packet sizes. The CH32X pushes power.status
// every 1 s (needed locally for the OLED / alarms / SOC EMA and the
// Last_Power_Status cache), but we only relay it to the MQTT backend every
// TELEMETRY_UPLINK_INTERVAL_MS. Exception: a change in input-power state
// (grid <-> battery) or in the charger fault bitmap forces an immediate
// publish, so the cloud panel sees an outage/fault within seconds rather
// than up to 30 s later. This throttle is cloud-visibility only — local
// buzzer/OLED alarms and the RPi-side shutdown decision run off the 1 s
// stream and are unaffected. The on-demand power.status REQ path (panel
// "Request status" button) also stays immediate; it is user-triggered and
// rare, so it does not meaningfully spend the data budget.
static const uint32_t TELEMETRY_UPLINK_INTERVAL_MS = 30000;
static uint32_t Last_Telemetry_Uplink_Ms = 0;
static bool     Telemetry_Uplink_Primed  = false;  // first frame after boot?
static bool     Last_Uplink_Pg           = false;  // input "good" at last uplink
static uint16_t Last_Uplink_Faults       = 0;      // charger faults at last uplink

void wups_on_local_frame(uint8_t inbound_port, const WupsFrame& f) {
  // power.event / host.event (op 0x10, EVENT) — alert-class state changes
  // (mains lost/restored, charger fault, host shutdown-imminent, …). The
  // ESP32 uplinks ONLY net.publish-wrapped payloads, so the raw broadcast
  // the router already forwarded to its port dies there — this wrap is the
  // only path an event has to the panel's event log. Re-encoded verbatim
  // (SRC preserved) onto the "event" subtopic. inbound_port guard: a frame
  // that arrived FROM the ESP32 (net.downlink re-injection) must not bounce
  // straight back out as a fresh uplink.
  if ((f.flags & WUPS_FLAG_EVENT) && inbound_port != WUPS_PORT_ESP32 &&
      ((f.cls == WUPS_CLASS_POWER && f.op == WUPS_OP_PWR_EVENT) ||
       (f.cls == WUPS_CLASS_HOST  && f.op == WUPS_OP_HOST_EVENT))) {
    wupsPublishEventFrame(f);
    return;
  }

  // CH32X power.status — cache and project into the `ui` snapshot so the
  // OLED / alarm code keeps working unchanged. The wire format is version-
  // dispatched on payload[0]: v2 (the current CH32X firmware) carries split
  // INPUT/OUTPUT PD contracts + real VSYS/IIN; v1 is the legacy fallback.
  // We forward/uplink the RAW payload (not a decoded struct) so the v2 tail
  // is never truncated.
  if (f.cls == WUPS_CLASS_POWER && f.op == WUPS_OP_PWR_STATUS &&
      f.src == WUPS_ADDR_CH32X && f.len >= 1) {
    const uint8_t pwrVersion = f.payload[0];
    bool     decoded   = false;
    bool     pgNow     = false;   // input "good" — set in whichever branch decodes
    uint16_t faultsNow = 0;       // fault bitmap — set in whichever branch decodes

    if (pwrVersion == 2 && f.len >= sizeof(wups_power_status_v2_t)) {
      // --- power.status v2 ---
      wups_power_status_v2_t s2;
      memcpy(&s2, f.payload, sizeof(s2));
      Last_Pwr_V2 = s2;

      // Temperature: prefer MP2762A junction temp, but it reads -32768 when
      // the charger is unpowered — fall back to LM75B board temp then.
      ui.t  = (s2.temp_mp_dC == -32768) ? s2.temp_lm_dC
                                        : max((int)s2.temp_mp_dC, (int)s2.temp_lm_dC);
      ui.cs = s2.charge_state;
      ui.pg = (s2.vbus_in_mV > 5000) ? 1 : 0;
      ui.vi = s2.vbus_in_mV;
      ui.ci = s2.ichg_mA;
      ui.cf = s2.faults;
      ui.bp = (s2.flags & WUPS_PWR2_FLAG_BATT_PRESENT) ? 1 : 0;
      ui.vb  = s2.vbat_mV;
      ui.vbc = s2.vbat_mV;
      ui.vr  = (int)(s2.vout_read_mV / 100);
      ui.ir  = (int)(s2.iout_limit_mA / 100);
      ui.vs  = (int)(s2.vout_set_mV  / 100);
      ui.is  = (int)(s2.iout_limit_mA / 100);
      // New v2 fields.
      ui.pd_in_mV     = s2.pd_in_mV;
      ui.pd_in_mA     = s2.pd_in_mA;
      ui.pd_out_mV    = s2.pd_out_mV;
      ui.pd_out_mA    = s2.pd_out_mA;
      ui.vbus_out_ch_mV = s2.vbus_out_mV;
      ui.vsys_mV      = s2.vsys_mV;
      ui.iin_mA       = s2.iin_mA;
      ui.temp_lm_dC   = s2.temp_lm_dC;
      ui.temp_mp_dC   = s2.temp_mp_dC;
      ui.usb_c_attach = (s2.flags & WUPS_PWR2_FLAG_USB_C_ATTACH) ? 1 : 0;
      ui.iout_limit_mA = s2.iout_limit_mA;
      ui.soc_ch       = voltageToSoc(s2.vbat_mV);

      pgNow     = (s2.vbus_in_mV > 5000);
      faultsNow = s2.faults;
      decoded   = true;
    } else if (pwrVersion == 1 && f.len >= sizeof(wups_power_status_v1_t)) {
      // --- power.status v1 (legacy fallback, mapping unchanged) ---
      wups_power_status_v1_t s;
      memcpy(&s, f.payload, sizeof(s));
      Last_Power_Status = s;

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

      pgNow     = (s.vbus_in_mV > 5000);
      faultsNow = s.faults;
      decoded   = true;
    }

    if (!decoded) return;  // unknown version or short frame — drop

    // Cache the RAW payload (version-agnostic) for the on-demand republish
    // and the throttled uplink — forwarding a decoded struct would truncate
    // the v2 tail.
    if (f.len <= sizeof(Last_Pwr_Raw)) {
      memcpy(Last_Pwr_Raw, f.payload, f.len);
      Last_Pwr_Raw_Len = f.len;
    }
    Last_Power_Status_Ms = millis();
    newFrameReceived = true;
    lastFrameTime    = millis();

    // Mirror parsed power.status to the debug stream (USB-CDC + Probe UART
    // via J350) so the firmware author can read CH32X telemetry remotely
    // without unplugging the bench setup. One line per frame, fixed column
    // order so it's easy to grep / diff between runs. For v2, Vsys/Iin come
    // from the real dedicated fields (not the v1 pd_contract aliasing).
    dbgOut.print(F("[t="));
    dbgOut.print(millis() / 1000);
    dbgOut.print(F("s] v="));
    dbgOut.print(pwrVersion);
    dbgOut.print(F(" cs="));
    dbgOut.print(ui.cs);
    dbgOut.print(F(" pg="));
    dbgOut.print(pgNow ? 1 : 0);
    dbgOut.print(F(" Vin="));
    dbgOut.print(ui.vi);
    if (pwrVersion == 2) {
      dbgOut.print(F("mV Vsys="));
      dbgOut.print(Last_Pwr_V2.vsys_mV);
      dbgOut.print(F("mV Iin="));
      dbgOut.print(Last_Pwr_V2.iin_mA);
    } else {
      dbgOut.print(F("mV Vsys="));
      dbgOut.print(Last_Power_Status.pd_contract_mV);   // diag-aliased (v1)
      dbgOut.print(F("mV Iin="));
      dbgOut.print(Last_Power_Status.pd_contract_mA);   // diag-aliased (v1)
    }
    dbgOut.print(F("mA Vbat="));
    dbgOut.print(ui.vbc);
    dbgOut.print(F("mV Ichg="));
    dbgOut.print(ui.ci);
    dbgOut.print(F("mA Vout="));
    dbgOut.print((pwrVersion == 2) ? ui.vbus_out_ch_mV : (ui.vr * 100));
    dbgOut.print(F("mV T="));
    dbgOut.print(ui.t);
    dbgOut.print(F("dC f=0x"));
    dbgOut.println((unsigned int)faultsNow, HEX);

    // --- DIAG (temporary): ADC comparison — RP2040 own ADC (ui.bv, drives
    // the OLED SOC) vs CH32X PA5 (ui.vbc). Lets us see on one line which
    // source is closer to a bench multimeter. Remove once the SOC source is
    // settled. 1 Hz (one per CH32X power.status frame). grep tag: [ADCcmp]
    dbgOut.print(F("[ADCcmp] RP2040_bv="));
    dbgOut.print(ui.bv);
    dbgOut.print(F("mV soc="));
    dbgOut.print(ui.soc);
    dbgOut.print(F("% | CH32X_vbat="));
    dbgOut.print(ui.vbc);
    dbgOut.print(F("mV | delta(RP-CH)="));
    dbgOut.print(ui.bv - ui.vbc);
    dbgOut.println(F("mV"));

    // Push-mode aggregate to RPi: forward the RAW payload on USB-CDC with
    // SRC=CH32X preserved (so the v2 tail survives). RPi service decodes and
    // treats it as authoritative.
    wups_send_with_src(WUPS_PORT_RPI, WUPS_ADDR_RPI, WUPS_ADDR_CH32X,
                       WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                       WUPS_FLAG_EVENT, f.seq, f.payload, f.len);

    // Drive the cellular uplink via net.publish to the ESP32. ESP32 is a
    // dumb pipe to MQTT — it forwards `topic`+`payload` to the broker and
    // auto-prefixes relative subtopics with `t/{iccid}/`. We send the full
    // raw WUPS frame (header+payload+checksum) as the MQTT payload so the
    // panel's wupsproto.ts decoder sees byte-for-byte what the bus sees.
    //
    // Throttled to TELEMETRY_UPLINK_INTERVAL_MS to stay inside the LTE data
    // budget, with an immediate publish whenever the input-power state or
    // the charger fault bitmap changes (see notes at the static decls).
    {
      const uint32_t nowMs = millis();
      bool publishNow;
      if (!Telemetry_Uplink_Primed) {
        publishNow = true;                  // first frame: announce we're online
      } else if (pgNow != Last_Uplink_Pg || faultsNow != Last_Uplink_Faults) {
        publishNow = true;                  // power state / fault change
      } else {
        // Unsigned subtraction is millis()-rollover safe.
        publishNow = (uint32_t)(nowMs - Last_Telemetry_Uplink_Ms)
                         >= TELEMETRY_UPLINK_INTERVAL_MS;
      }
      if (publishNow) {
        wupsPublishTelemetryStatus(f.seq, f.payload, f.len);
        Last_Telemetry_Uplink_Ms = nowMs;
        Last_Uplink_Pg           = pgNow;
        Last_Uplink_Faults       = faultsNow;
        Telemetry_Uplink_Primed  = true;
      }
    }
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

    dbgOut.print(F("[net.downlink] dst=0x"));
    dbgOut.print(ifr.dst, HEX);
    dbgOut.print(F(" cls=0x"));
    dbgOut.print(ifr.cls, HEX);
    dbgOut.print(F(" op=0x"));
    dbgOut.print(ifr.op, HEX);
    dbgOut.print(F(" flags=0x"));
    dbgOut.print(ifr.flags, HEX);
    dbgOut.print(F(" len="));
    dbgOut.println(ifr.len);

    // Re-inject through the router so broadcast/unicast frames are also
    // forwarded to CH32X / RPi as required (e.g. ups.power.* lives on CH32X).
    wups_route_frame(inbound_port, ifr);
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

  // power.status REQ — panel's "Request status frame" button. CH32X
  // already pushes power.status as an EVENT every 1 s and we cache it
  // in Last_Power_Status; just republish that cached frame on MQTT
  // telemetry immediately so the panel's Live-telemetry tile refreshes
  // without a CH32X round trip. Max staleness ≈ 1 s — fine for Vbat /
  // Iout / temperature granularity.
  if (f.cls == WUPS_CLASS_POWER && f.op == WUPS_OP_PWR_STATUS &&
      (f.flags & WUPS_FLAG_REQ)) {
    if (Last_Power_Status_Ms != 0 && Last_Pwr_Raw_Len != 0) {
      // Republish the RAW cached payload (any version) so the v2 tail is
      // preserved end-to-end.
      wupsPublishTelemetryStatus(f.seq, Last_Pwr_Raw, Last_Pwr_Raw_Len);
    }
    wupsPublishCmdResponse(f.cls, f.op, f.seq, /*code=*/0);
    return;
  }

  // power.{enable,disable,cycle,reset} REQ — RP2040 has no local action
  // here; the router has already forwarded the broadcast frame to CH32X
  // over UART0 by the time we're called. CH32X executes the command but
  // is a leaf node on UART (no MQTT path of its own), so RP2040 stands in
  // and publishes a best-effort RESP to t/{iccid}/cmd/response. The UART
  // link is 921600 8N1 point-to-point with HW flow control — the forward
  // is virtually never lost, and there's no easier round-trip ack to wait
  // for than this one.
  if (f.cls == WUPS_CLASS_POWER && (f.flags & WUPS_FLAG_REQ) &&
      (f.op == WUPS_OP_PWR_ENABLE  ||
       f.op == WUPS_OP_PWR_DISABLE ||
       f.op == WUPS_OP_PWR_CYCLE   ||
       f.op == WUPS_OP_PWR_RESET)) {
    wupsPublishCmdResponse(f.cls, f.op, f.seq, /*code=*/0);
    return;
  }

  // ui.trust_prompt (Track 2 / ADR-0011 §10.1/§10.4) — the ESP32 drives
  // the Paranoic owner-binding gate. The OLED + 2 buttons are RP2040-only,
  // so the physical confirm lives here (scoped, owner-approved Decision-C
  // exception). We are a dumb renderer: hand it to trust_ui, which takes
  // over the display + buttons and emits ui.trust_result. No CMD response
  // forwarding — this is an internal ESP32↔RP2040 round-trip.
  if (f.cls == WUPS_CLASS_UI && f.op == WUPS_OP_UI_TRUST_PROMPT &&
      (f.flags & WUPS_FLAG_REQ)) {
    // trust_ui is about to take the OLED. Close the RP2040-local menu first
    // so it can't silently resume (possibly on a destructive confirm screen)
    // with a phantom button edge when trust_ui later ends. No-op on the
    // normal "Network" hand-off (already closed) and on remote claim prompts.
    if (local_menu_active()) local_menu_close();
    trust_ui_on_prompt(f.payload, f.len, f.seq);
    return;
  }

  // ADR-0012 — system.hello from the ESP32 means the ESP32 just (re)booted.
  // Any in-progress trust_ui session (claim flow, system menu) is now
  // orphaned — its state machine lives on the ESP32, which has just lost
  // it. Force the OLED back to the home dashboard so the user sees the
  // device respond. Also resets currentScreen so debug screens don't
  // linger after a backend-mode-switch reboot.
  if (f.cls == WUPS_CLASS_SYSTEM && f.op == WUPS_OP_SYS_HELLO &&
      f.src == WUPS_ADDR_ESP32) {
    if (trust_ui_active()) trust_ui_force_close();
    currentScreen = 0;
    lastInteractionTime = millis();
    return;
  }

  // ADR-0012 — ui.set_screen forces a particular dashboard screen.
  // Used by the system menu's "Debug" entry: it puts the RP2040 on one
  // of the developer screens (1=INPUT, 2=OUTPUT, 3=BATTERY, 4=SYSTEM)
  // and the user can navigate them with the normal LEFT/RIGHT shorts
  // (which now run on every screen, Home included). Auto-return to home
  // (20s idle) brings the user back to the dashboard / menu activation
  // gesture window.
  if (f.cls == WUPS_CLASS_UI && f.op == WUPS_OP_UI_SET_SCREEN &&
      f.len >= sizeof(wups_ui_set_screen_v1_t)) {
    wups_ui_set_screen_v1_t s;
    memcpy(&s, f.payload, sizeof(s));
    if (s.version == 1 && s.screen < SCREEN_COUNT) {
      currentScreen = s.screen;
      lastInteractionTime = millis();
      // The system menu was just closed by the ESP32 — make sure the
      // RP2040 trust_ui side doesn't keep ownership of the OLED.
      // trust_ui's own per-prompt session timeout would also handle
      // this, but explicitly relinquishing avoids a ~3 s overlap.
      trust_ui_force_close();
    }
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
      ui_settings_beep(freq, dur);
    }

    uint8_t out_port = WUPS_PORT_NONE;
    if      (f.src == WUPS_ADDR_RPI)   out_port = WUPS_PORT_RPI;
    else if (f.src == WUPS_ADDR_CH32X) out_port = WUPS_PORT_CH32X;
    else if (f.src == WUPS_ADDR_ESP32) out_port = WUPS_PORT_ESP32;
    if (out_port != WUPS_PORT_NONE) {
      wups_send_seq(out_port, f.src, WUPS_CLASS_UI, WUPS_OP_UI_BEEP,
                    WUPS_FLAG_RESP, f.seq, nullptr, 0);
    }
    wupsPublishCmdResponse(f.cls, f.op, f.seq, /*code=*/0);
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

  // host.status — RPi host service pushes CPU temp / mem / disk / load /
  // uptime every few seconds (drives the OLED's host screen). Re-publish
  // it verbatim onto the MQTT "telemetry" subtopic so the panel's
  // wupsproto decoder sees the same bytes the bus sees. SRC=RPI is kept
  // for the panel's audit trail. Local-consume only — do NOT retransmit
  // onto the other MCU ports.
  if (f.cls == WUPS_CLASS_HOST && f.op == WUPS_OP_HOST_STATUS &&
      f.src == WUPS_ADDR_RPI &&
      f.len >= sizeof(wups_host_status_v1_t)) {
    wups_host_status_v1_t hs;
    memcpy(&hs, f.payload, sizeof(hs));
    if (hs.version != 1) return;
    wupsPublishHostStatus(f.seq, hs);
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

// Publish a one-byte cmd-response RESP back to the panel on
// t/{iccid}/cmd/response. The panel's dispatcher matches by SEQ (resolved
// via Redis to the originating commandId) and reads payload[0] as a
// result code (0 = success, non-zero = failure → "code_N"). DST=RPI is
// arbitrary — the panel only cares about SEQ + payload.
static void wupsPublishCmdResponse(uint8_t cls, uint8_t op, uint8_t seq,
                                   uint8_t code) {
  uint8_t frame[WUPS_FRAMING_BYTES + 1];
  size_t n = wupsBuildFrame(frame, sizeof(frame),
                            WUPS_ADDR_RPI, WUPS_ADDR_RP2040,
                            cls, op, WUPS_FLAG_RESP, seq, &code, 1);
  if (n == 0) return;
  wupsRequestPublish("cmd/response", /*qos=*/1, /*retain=*/0, frame, (uint16_t)n);
}

// Wrap a CH32X power.status RAW payload (any version — v1 or v2) into a WUPS
// frame (src=CH32X preserved for the panel's audit trail) and ship it to the
// ESP32 as net.publish onto the "telemetry" subtopic. Taking raw bytes (not a
// decoded struct) keeps the v2 tail intact end-to-end; the panel's wupsproto
// decoder version-dispatches on payload[0] just like we do.
static void wupsPublishTelemetryStatus(uint8_t seq, const uint8_t* payload, uint16_t len) {
  if (payload == nullptr || len == 0 || len > WUPS_MAX_PAYLOAD) return;
  uint8_t frame[WUPS_FRAMING_BYTES + WUPS_MAX_PAYLOAD];
  size_t n = wupsBuildFrame(frame, sizeof(frame),
                            WUPS_ADDR_BROADCAST, WUPS_ADDR_CH32X,
                            WUPS_CLASS_POWER, WUPS_OP_PWR_STATUS,
                            WUPS_FLAG_EVENT, seq, payload, len);
  if (n == 0) return;
  wupsRequestPublish("telemetry", /*qos=*/0, /*retain=*/0, frame, (uint16_t)n);
}

// Wrap an RPi-sourced host.status into a WUPS frame (src=RPI preserved for
// the panel's audit trail) and ship it to the ESP32 as net.publish onto
// the "telemetry" subtopic — same path/QoS as power.status.
static void wupsPublishHostStatus(uint8_t seq, const wups_host_status_v1_t& s) {
  uint8_t frame[WUPS_FRAMING_BYTES + sizeof(wups_host_status_v1_t)];
  size_t n = wupsBuildFrame(frame, sizeof(frame),
                            WUPS_ADDR_BROADCAST, WUPS_ADDR_RPI,
                            WUPS_CLASS_HOST, WUPS_OP_HOST_STATUS,
                            WUPS_FLAG_EVENT, seq, &s, sizeof(s));
  if (n == 0) return;
  wupsRequestPublish("telemetry", /*qos=*/0, /*retain=*/0, frame, (uint16_t)n);
}

// Wrap an alert-class event frame (power.event / host.event) verbatim and
// ship it onto the "event" subtopic. SRC is preserved so the panel's
// dispatcher attributes the event to the MCU that raised it. QoS 1 —
// these are rare state-transition frames; losing one leaves a hole in the
// event log, unlike the periodic telemetry stream where the next sample
// heals any gap.
static void wupsPublishEventFrame(const WupsFrame& f) {
  uint8_t frame[WUPS_FRAMING_BYTES + 64];
  size_t n = wupsBuildFrame(frame, sizeof(frame),
                            f.dst, f.src, f.cls, f.op,
                            f.flags, f.seq, f.payload, f.len);
  if (n == 0) return;
  wupsRequestPublish("event", /*qos=*/1, /*retain=*/0, frame, (uint16_t)n);
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

// ADR-0012 (revised) — the local menu's "Network" item hands off to the
// ESP32 backend menu (Mode / HTTP Key / Wallet / Reset). We reuse the
// original activation path: broadcast ui.button_event{0xFF, long}, which the
// ESP32 opens its menu on, then close the local menu and arm a response
// timeout. If the ESP32 answers, its trust_prompt makes trust_ui take over
// the OLED; if nothing answers (no M.2 module / wedged ESP32), the timeout
// in loop() reopens the local menu with a "No modem" notice.
void local_menu_host_open_esp32(void) {
  wups_ui_button_event_v1_t e;
  e.version  = 1;
  e.button   = 0xFF;   // activation gesture sentinel (ADR-0012)
  e.action   = 2;      // long
  e.reserved = 0;
  wups_send(WUPS_PORT_ESP32, WUPS_ADDR_BROADCAST,
            WUPS_CLASS_UI, WUPS_OP_UI_BUTTON_EVENT,
            WUPS_FLAG_EVENT, &e, sizeof(e));
  ui_settings_beep(1200, 80);
  menu_pending_deadline_ms = millis() + MENU_RESPONSE_TIMEOUT_MS;
  local_menu_close();  // release the OLED; trust_ui takes over if ESP32 answers
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

  // Load persisted UI settings (brightness + sound mute) before the first
  // beep / brightness apply. Seeds defaults on first boot.
  ui_settings_begin();

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

  // UART1 to M.2 ESP32. HW flow control (CTS/RTS) was disabled 2026-05-25
  // — the handshake misbehaved across ESP32-only reboots and left the
  // RP2040↔ESP32 link permanently desynced (resync counter climbed every
  // frame, telemetry only worked after a full power-cycle). Pin
  // assignments for CTS/RTS retained as INPUT/OUTPUT pullups in case we
  // want to re-enable later, but setCTS/setRTS aren't called so
  // arduino-pico runs Serial2 without HW handshake.
  Serial2.setTX(UART1_TX_PIN);
  Serial2.setRX(UART1_RX_PIN);
  Serial2.setFIFOSize(256);
  Serial2.begin(UART1_BAUD);
  // Park the previous CTS/RTS pins as inputs with pullups so they don't
  // float and accidentally drive anything on the ESP32 side.
  pinMode(UART1_CTS_PIN, INPUT_PULLUP);
  pinMode(UART1_RTS_PIN, INPUT_PULLUP);
  dbgOut.println(F("UART1 bidir on GPIO20(TX)/21(RX) @ 921600 (no HW flow ctrl)"));

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

  // Apply persisted brightness so the splash already shows at the user's level.
  ui_settings_apply_brightness(oled);

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

  // Track 2 / ADR-0011 §10.1/§10.4 — the Paranoic owner-binding gate is a
  // security override: while a ui.trust_prompt is active the trust UI owns
  // the OLED and both buttons, and the normal dashboard / navigation is
  // fully suppressed. Buttons are active-LOW with pullups (LOW = pressed).
  if (trust_ui_active()) {
    trust_ui_tick(oled,
                  digitalRead(BTN_LEFT_PIN)  == LOW,
                  digitalRead(BTN_RIGHT_PIN) == LOW);
    delay(50);
    return;
  }

  // ADR-0012 (revised) — the RP2040-LOCAL universal menu owns the OLED +
  // buttons while open, exactly like trust_ui above. Built from local state,
  // so it works with or without the M.2 module. Only opens while trust_ui is
  // inactive (checked above), so the two never fight over the display.
  if (local_menu_active()) {
    LocalMenuCtx ctx;
    ctx.uptime_s    = millis() / 1000UL;
    ctx.temp_lm_dC  = ui.temp_lm_dC;
    ctx.temp_mp_dC  = ui.temp_mp_dC;
    ctx.power_fresh = (Last_Power_Status_Ms != 0) &&
                      ((uint32_t)(millis() - Last_Power_Status_Ms) < 3000u);
    // Read the output rail live so the Output screen reflects a just-issued
    // enable/disable (the main ADC read below is skipped while we early-return).
    {
      float lsb_mV = (VREF * 1000.0f) / ADC_MAX;
      ctx.vbus_out_mV =
          (int)(analogRead(ADC_VBUS_OUT_PIN) * lsb_mV / VBUS_DIVIDER_RATIO + 0.5f);
    }
    local_menu_tick(oled,
                    digitalRead(BTN_LEFT_PIN)  == LOW,
                    digitalRead(BTN_RIGHT_PIN) == LOW,
                    ctx);
    delay(50);
    return;
  }

  // ADR-0012 — system-menu activation gesture: hold LEFT for
  // MENU_ACTIVATION_HOLD_MS on the home screen. One-button gesture
  // (ergonomically easiest). Sends a single ui.button_event{
  // button=0xFF, action=long} broadcast which the ESP32 interprets as
  // "open system menu". Exit isn't a hold — once the menu is open the
  // user selects "Back" (short press, just like any other nav).
  //
  // Short LEFT presses now also navigate the screen strip from Home (see
  // the button-handling block below), so the hold must be distinguished
  // from a tap WITHOUT being cancelled the instant a tap nav fires. We
  // latch on the hold itself: the episode is gated on currentScreen==0 at
  // the START of the hold (s_home_at_start), then driven purely by the
  // LEFT held-state — so a short LEFT that bumps currentScreen off Home
  // mid-hold doesn't reset the timer. s_menu_fired one-shots per episode;
  // both latches clear on LEFT release. s_left_consumed tells the nav
  // block below to skip the LEFT-release tap (the press became a hold).
  static bool s_left_consumed = false;   // shared with nav block below
  {
    static uint32_t s_left_start_ms  = 0;
    static bool     s_menu_fired     = false;
    static bool     s_home_at_start  = false;
    constexpr uint32_t MENU_ACTIVATION_HOLD_MS = 2000;
    bool left_down = digitalRead(BTN_LEFT_PIN) == LOW;
    if (left_down) {
      uint32_t now_ms = millis();
      if (s_left_start_ms == 0) {
        s_left_start_ms = now_ms;
        s_home_at_start = (currentScreen == 0);  // only Home arms the gesture
      }
      if (s_home_at_start && !s_menu_fired &&
          (now_ms - s_left_start_ms) >= MENU_ACTIVATION_HOLD_MS) {
        // ADR-0012 (revised): the LEFT-hold now opens the RP2040-LOCAL
        // universal menu (brightness / sound / info / output), which works
        // with or without the M.2 module. The ESP32 backend menu (Mode /
        // HTTP Key / Wallet / Reset) is reached from inside it via the
        // "Network" item — see local_menu_host_open_esp32(). The
        // local_menu_active() branch at the top of loop() takes over the
        // OLED on the next iteration.
        local_menu_open();
        ui_settings_beep(1200, 80);
        s_menu_fired    = true;
        s_left_consumed = true;   // this LEFT became a hold — no tap nav
      }
    } else {
      s_left_start_ms = 0;
      s_menu_fired    = false;
      s_home_at_start = false;
    }
  }

  // ADR-0012 (revised) — "Network" hand-off result. local_menu_host_open_esp32()
  // broadcast the activation gesture, armed this deadline, and closed the
  // local menu. If trust_ui took over, the ESP32 answered — clear silently.
  // Otherwise (no M.2 module / wedged ESP32) reopen the local menu and flash
  // a "No modem" notice so the user isn't dropped back to the dashboard
  // wondering what happened.
  if (menu_pending_deadline_ms != 0) {
    if (trust_ui_active()) {
      menu_pending_deadline_ms = 0;
    } else if ((int32_t)(millis() - menu_pending_deadline_ms) >= 0) {
      menu_pending_deadline_ms = 0;
      ui_settings_beep(1500, 80);
      local_menu_open();
      local_menu_note_no_modem();
    }
  }

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
  // Short LEFT/RIGHT presses cycle the screen strip on EVERY screen,
  // including Home: RIGHT = next, LEFT = previous, both wrap around
  // SCREEN_COUNT. This supersedes the old ADR-0012 home no-op — the owner
  // wants the v2 debug screens reachable directly from Home.
  //
  // RIGHT has no hold meaning, so it navigates on the press edge (snappy).
  // LEFT doubles as the system-menu hold gesture, so to keep both working
  // cleanly on Home we navigate LEFT on its RELEASE edge and only when the
  // press did NOT become a hold (s_left_consumed, set by the gesture block
  // above). That way a quick LEFT tap goes back one screen while a 2 s LEFT
  // hold opens the menu without first flicking the display to SYSTEM.
  // trust_ui owns the buttons while the menu / claim flow is up, so this
  // block is skipped entirely in that mode (early return at top of loop()).
  int8_t btnAction = checkButtons();
  if (btnAction > 0) {
    // RIGHT pressed - next screen (wrap)
    lastInteractionTime = millis();
    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    ui_settings_beep(1000, 20);
  }

  // LEFT release-edge nav: previous screen (wrap), unless this LEFT episode
  // already fired the system-menu hold gesture.
  {
    static bool s_left_prev_down = false;
    bool left_down = digitalRead(BTN_LEFT_PIN) == LOW;
    if (s_left_prev_down && !left_down) {       // release edge
      if (!s_left_consumed) {
        lastInteractionTime = millis();
        currentScreen = (currentScreen + SCREEN_COUNT - 1) % SCREEN_COUNT;
        ui_settings_beep(1000, 20);
      }
      s_left_consumed = false;   // re-arm for the next LEFT episode
    }
    s_left_prev_down = left_down;
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
    } else {
      delay(50);
      return;
    }
  }

  // Battery-presence detection removed (owner decision 2026-06-21): the
  // MP2762A presence signal is unreliable on this hardware — it falsely
  // reports "no battery" when the pack is full and mains is present — and
  // presence checking is not wanted. Always treat a battery as present.
  bool noBattery = false;

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
      case SCREEN_HOME:
        drawScreenHome(soc, noBattery, animPhase);
        break;
      case SCREEN_INPUT:
        drawScreenInput();
        break;
      case SCREEN_OUTPUT:
        drawScreenOutput();
        break;
      case SCREEN_BATTERY:
        drawScreenBattery();
        break;
      case SCREEN_SYSTEM:
        drawScreenSystem();
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
