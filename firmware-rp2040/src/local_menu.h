/*
 * ADR-0012 (revised) — RP2040-LOCAL universal OLED menu.
 *
 * The original system menu (oled_menu.c) lives on the ESP32: the RP2040 was
 * a dumb renderer for ESP32-pushed ui.trust_prompt text. Consequence: with
 * no M.2 module (ESP32 + LTE-M modem) there was no menu at all.
 *
 * This module gives the RP2040 its OWN menu, built from local state, so the
 * universal settings work with or without the modem:
 *   - Bright   : OLED brightness (SSD1306 contrast), persisted
 *   - Sound    : buzzer mute on/off, persisted
 *   - Info     : uptime + board/charger temperature (read-only)
 *   - Output   : enable/disable the power rail to the Pi (with a confirm
 *                screen before cutting power — destructive)
 *   - Network  : hand off to the ESP32 backend menu when the M.2 module is
 *                present (forwards the activation gesture); shows "No modem"
 *                otherwise
 *   - Exit     : close the menu
 *
 * Activation: held LEFT on the home screen opens this menu (see main.cpp).
 * Navigation mirrors the ESP32 menu so muscle memory carries over:
 *   LEFT  = move cursor down (wrap) / toggle the active option
 *   RIGHT = select / activate the highlighted item
 *
 * While open, this menu owns the OLED + buttons exactly like trust_ui does
 * (main.cpp's loop early-returns to local_menu_tick), so the two never fight
 * over the display. The local menu only opens while trust_ui is inactive.
 */
#ifndef LOCAL_MENU_H
#define LOCAL_MENU_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

/* Live values the menu needs, sampled by the caller each tick so this module
 * stays decoupled from main.cpp's globals. */
struct LocalMenuCtx {
  uint32_t uptime_s;     /* RP2040 uptime (seconds)                         */
  int      temp_lm_dC;   /* LM75B board temp (deci-°C)                      */
  int      temp_mp_dC;   /* MP2762A junction temp (deci-°C, -32768 = N/A)   */
  bool     power_fresh;  /* a CH32X power.status arrived recently           */
  int      vbus_out_mV;  /* measured output rail (mV) — output on/off state */
};

void local_menu_open(void);
void local_menu_close(void);
bool local_menu_active(void);

/* Re-open at the root with a transient "No modem" notice on top — called by
 * main.cpp when a Network hand-off to the ESP32 times out. */
void local_menu_note_no_modem(void);

/* Drive once per loop() iteration (~50 ms) while active. `leftDown`/
 * `rightDown` are the active-low-decoded button levels (true = pressed). */
void local_menu_tick(Adafruit_SSD1306& oled, bool leftDown, bool rightDown,
                     const LocalMenuCtx& ctx);

/* Implemented in main.cpp: broadcast the activation gesture to the ESP32,
 * arm the response timeout, and close this menu so trust_ui can take over
 * the OLED if the ESP32 answers. Invoked by the "Network" item. */
void local_menu_host_open_esp32(void);

#endif /* LOCAL_MENU_H */
