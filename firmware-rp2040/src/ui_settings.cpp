/*
 * ADR-0012 (revised) — RP2040-local persisted UI settings. See ui_settings.h.
 */
#include "ui_settings.h"
#include <EEPROM.h>

/* Buzzer pin mirrors main.cpp / trust_ui.cpp (GPIO15). Kept local so the
 * mute gate lives entirely in this module. */
static constexpr uint8_t BUZZER_PIN = 15;

/* SSD1306 contrast per brightness level. Level 0 stays usable (not blanked)
 * and is the factory default (burn-in mitigation); level 5 is the max. */
static const uint8_t kContrast[UI_BRIGHTNESS_LEVELS] = { 5, 10, 15, 32, 64, 128 };

/* Persisted blob. `magic`+`version` guard against reading uninitialised or
 * stale-layout flash. Sound defaults ON, brightness defaults to dimmest.
 *
 * Version history (layout unchanged so far — the version also carries
 * one-shot migrations of the stored values):
 *   1  rp2040 <= 1.2.3: brightness default was level 3 ("Lvl 4/6").
 *   2  rp2040 1.2.4: default lowered to level 0 for burn-in mitigation; a
 *      v1 blob is migrated in place — brightness forced to the new default,
 *      sound preserved (the fleet never set brightness deliberately, owner
 *      decision 2026-09-15). */
#define UI_SETTINGS_MAGIC   0x57555053UL  /* 'W''U''P''S' */
#define UI_SETTINGS_VERSION 2

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
  } else if (tmp.magic == UI_SETTINGS_MAGIC && tmp.version == 1) {
    /* v1 -> v2 migration: same layout, only the brightness policy changed.
     * Keep the user's sound setting, drop brightness to the new default and
     * persist once so the next boot takes the fast path. */
    s.version    = UI_SETTINGS_VERSION;
    s.brightness = UI_BRIGHTNESS_DEFAULT;
    s.sound      = tmp.sound ? 1 : 0;
    s.reserved   = 0;
    EEPROM.put(0, s);
    EEPROM.commit();
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
  /* Same values as the static initializer / first-boot seed: dimmest,
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
