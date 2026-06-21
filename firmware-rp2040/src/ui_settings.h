/*
 * ADR-0012 (revised) — RP2040-local persisted UI settings.
 *
 * Brightness + sound are the two universal settings the local OLED menu
 * exposes (see local_menu.h). They live here because they outlive a single
 * menu session and must survive a reboot — there was no persistence layer
 * on the RP2040 before, so this module owns a tiny flash-backed store via
 * the earlephilhower EEPROM emulation (one 4 KB sector, written only on a
 * deliberate change — never in a hot path).
 *
 * ui_settings_beep() is the single point of truth for muting: every firmware
 * beep routes through it, so the "sound off" setting silences button clicks,
 * melodies, AND the power/battery alarms (owner decision 2026-06-21 — full
 * mute). Flip the gate in ui_settings_beep() if alarms should ever be exempt.
 */
#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

/* Brightness is a small set of discrete levels (a 2-button menu can only
 * cycle, not slide). Level index 0..UI_BRIGHTNESS_LEVELS-1 maps to an
 * SSD1306 contrast value in ui_settings.cpp. Default = brightest. */
#define UI_BRIGHTNESS_LEVELS  4
#define UI_BRIGHTNESS_DEFAULT  (UI_BRIGHTNESS_LEVELS - 1)

/* Load settings from flash (or seed defaults on first boot / bad magic).
 * Call once in setup() before the first beep and before applying brightness. */
void ui_settings_begin(void);

/* Persist the in-RAM settings to flash. Called only on a deliberate change
 * (sound toggle, leaving the brightness screen) to spare the flash. */
void ui_settings_commit(void);

uint8_t ui_settings_brightness_level(void);
void    ui_settings_set_brightness_level(uint8_t level);
/* The SSD1306 contrast byte (0..255) for the current level. */
uint8_t ui_settings_brightness_contrast(void);
/* Push the current brightness to the panel (SETCONTRAST). */
void    ui_settings_apply_brightness(Adafruit_SSD1306& oled);

bool ui_settings_sound_enabled(void);
void ui_settings_set_sound_enabled(bool enabled);

/* Buzzer helper that honours ui_settings_sound_enabled(). All firmware
 * beeps go through this — see the header comment. No-op when muted. */
void ui_settings_beep(uint16_t freq_hz, uint16_t dur_ms);

#endif /* UI_SETTINGS_H */
