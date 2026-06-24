/*
 * ADR-0012 (revised) — RP2040-local persisted UI settings. See ui_settings.h.
 */
#include "ui_settings.h"
#include <EEPROM.h>

/* Buzzer pin mirrors main.cpp / trust_ui.cpp (GPIO15). Kept local so the
 * mute gate lives entirely in this module. */
static constexpr uint8_t BUZZER_PIN = 15;

/* SSD1306 contrast per brightness level. Level 0 stays usable (not blanked);
 * level 3 is the panel's max. */
static const uint8_t kContrast[UI_BRIGHTNESS_LEVELS] = { 5, 10, 15, 32, 64, 128 };

/* Persisted blob. `magic`+`version` guard against reading uninitialised or
 * stale-layout flash. Sound defaults ON, brightness defaults to brightest. */
#define UI_SETTINGS_MAGIC   0x57555053UL  /* 'W''U''P''S' */
#define UI_SETTINGS_VERSION 1

struct StoredSettings {
  uint32_t magic;
  uint8_t  version;
  uint8_t  brightness;   /* level index 0..UI_BRIGHTNESS_LEVELS-1 */
  uint8_t  sound;        /* 0 = muted, 1 = on */
  uint8_t  reserved;
};

static StoredSettings s = {
  UI_SETTINGS_MAGIC, UI_SETTINGS_VERSION, UI_BRIGHTNESS_DEFAULT, 1, 0
};

void ui_settings_begin(void) {
  EEPROM.begin(256);
  StoredSettings tmp;
  EEPROM.get(0, tmp);
  if (tmp.magic == UI_SETTINGS_MAGIC && tmp.version == UI_SETTINGS_VERSION) {
    s = tmp;
    if (s.brightness >= UI_BRIGHTNESS_LEVELS) s.brightness = UI_BRIGHTNESS_DEFAULT;
    s.sound = s.sound ? 1 : 0;
  } else {
    /* First boot or different layout: seed defaults into flash so the next
     * boot reads a valid blob. */
    EEPROM.put(0, s);
    EEPROM.commit();
  }
}

void ui_settings_commit(void) {
  EEPROM.put(0, s);
  EEPROM.commit();
}

uint8_t ui_settings_brightness_level(void) {
  return s.brightness;
}

void ui_settings_set_brightness_level(uint8_t level) {
  if (level >= UI_BRIGHTNESS_LEVELS) level = UI_BRIGHTNESS_LEVELS - 1;
  s.brightness = level;
}

uint8_t ui_settings_brightness_contrast(void) {
  uint8_t lv = s.brightness;
  if (lv >= UI_BRIGHTNESS_LEVELS) lv = UI_BRIGHTNESS_LEVELS - 1;
  return kContrast[lv];
}

void ui_settings_apply_brightness(Adafruit_SSD1306& oled) {
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(ui_settings_brightness_contrast());
}

bool ui_settings_sound_enabled(void) {
  return s.sound != 0;
}

void ui_settings_set_sound_enabled(bool enabled) {
  s.sound = enabled ? 1 : 0;
}

void ui_settings_reset_defaults(void) {
  /* Same values as the static initializer / first-boot seed: brightest,
   * sound on. Persist immediately so the defaults survive the reboot. */
  s.magic      = UI_SETTINGS_MAGIC;
  s.version    = UI_SETTINGS_VERSION;
  s.brightness = UI_BRIGHTNESS_DEFAULT;
  s.sound      = 1;
  s.reserved   = 0;
  ui_settings_commit();
}

void ui_settings_beep(uint16_t freq_hz, uint16_t dur_ms) {
  if (!s.sound) return;
  tone(BUZZER_PIN, freq_hz, dur_ms);
}
