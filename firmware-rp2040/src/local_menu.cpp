/*
 * ADR-0012 (revised) — RP2040-LOCAL universal OLED menu. See local_menu.h.
 */
#include "local_menu.h"
#include "ui_settings.h"
#include "wups_proto.h"
#include "wups_router.h"
#include <stdio.h>
#include <string.h>

/* 64x32 OLED, default GFX font: 6 px advance, 8 px tall → 10 cols × 4 rows.
 * Keep every rendered line ≤ 10 chars (the leading cursor '>' counts). */
static constexpr uint8_t SCR_COLS = 10;
static constexpr uint8_t SCR_ROWS = 4;

/* Idle auto-close — matches the ESP32 menu's 60 s so behaviour is uniform. */
static constexpr uint32_t LM_IDLE_MS    = 60000;
/* "No modem" notice dwell after a failed Network hand-off. */
static constexpr uint32_t LM_NOTICE_MS  = 1800;
/* Output rail considered "on" above this measured voltage. */
static constexpr int      LM_OUT_ON_MV  = 3000;

/* Root item order. NUM_ROOT must match the labels built in render_root(). */
enum {
  ROOT_BRIGHT = 0,
  ROOT_SOUND,
  ROOT_INFO,
  ROOT_OUTPUT,
  ROOT_NETWORK,
  ROOT_EXIT,
  NUM_ROOT
};

enum {
  LM_ROOT = 0,
  LM_BRIGHT,
  LM_INFO,
  LM_OUTPUT,
  LM_OUTPUT_CONFIRM,
};

static struct {
  bool     active;
  uint8_t  screen;          /* LM_*                                      */
  uint8_t  cursor;          /* root cursor 0..NUM_ROOT-1                 */
  uint8_t  sub;             /* sub-cursor for Output / confirm screens   */
  bool     prev_left;
  bool     prev_right;
  bool     btn_init;        /* phantom-edge suppression on the first tick */
  uint32_t last_input_ms;
  uint32_t notice_until_ms; /* 0 = no transient notice                   */
} S;

/* --- helpers -------------------------------------------------------------- */

/* Paint a verbatim 4-row text buffer ('\n' = row break, clipped at 10 cols). */
static void draw4(Adafruit_SSD1306& oled, const char* text) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  uint8_t row = 0, col = 0;
  for (const char* p = text; *p && row < SCR_ROWS; ++p) {
    if (*p == '\n') { row++; col = 0; continue; }
    if (col >= SCR_COLS) continue;
    oled.setCursor(col * 6, row * 8);
    oled.write((uint8_t)*p);
    col++;
  }
  oled.display();
}

/* Window a list into 4 rows keeping the cursor visible (mirrors oled_menu.c
 * on the ESP32). Prefixes the highlighted row with '>'. */
static void render_list(char* out, size_t cap,
                        const char* const* labels, int n, int cursor) {
  int first = 0;
  if (cursor >= 3) first = cursor - 2;
  if (first > n - (int)SCR_ROWS) first = n - (int)SCR_ROWS;
  if (first < 0) first = 0;
  int w = 0;
  for (int row = 0; row < (int)SCR_ROWS && first + row < n; ++row) {
    int idx = first + row;
    w += snprintf(out + w, w < (int)cap ? cap - (size_t)w : 0,
                  "%c%s%s",
                  idx == cursor ? '>' : ' ',
                  labels[idx],
                  row == (int)SCR_ROWS - 1 ? "" : "\n");
  }
}

static void emit_power(uint8_t op) {
  wups_send(WUPS_PORT_CH32X, WUPS_ADDR_CH32X, WUPS_CLASS_POWER, op,
            WUPS_FLAG_REQ, nullptr, 0);
}

/* --- rendering ------------------------------------------------------------ */

static void render(Adafruit_SSD1306& oled, const LocalMenuCtx& ctx) {
  char buf[96];
  switch (S.screen) {
    case LM_ROOT: {
      char snd[12];
      snprintf(snd, sizeof snd, "Sound %s",
               ui_settings_sound_enabled() ? "ON" : "OFF");
      const char* labels[NUM_ROOT] = {
        "Bright", snd, "Info", "Output", "Network", "Exit"
      };
      render_list(buf, sizeof buf, labels, NUM_ROOT, S.cursor);
      break;
    }
    case LM_BRIGHT: {
      uint8_t lv = ui_settings_brightness_level();
      char cells[UI_BRIGHTNESS_LEVELS + 1];
      for (int i = 0; i < UI_BRIGHTNESS_LEVELS; ++i)
        cells[i] = (i <= lv) ? '#' : '-';
      cells[UI_BRIGHTNESS_LEVELS] = '\0';
      snprintf(buf, sizeof buf, "BRIGHT\nLvl %d/%d\n[%s]\nR+  Lok",
               lv + 1, UI_BRIGHTNESS_LEVELS, cells);
      break;
    }
    case LM_INFO: {
      char up[12];
      uint32_t m = ctx.uptime_s / 60UL;
      if (m < 100UL)        snprintf(up, sizeof up, "UP %lum", (unsigned long)m);
      else if (m / 60 < 100) snprintf(up, sizeof up, "UP %luh", (unsigned long)(m / 60));
      else                  snprintf(up, sizeof up, "UP %lud", (unsigned long)(m / 1440));
      char tlm[12], tmp[12];
      if (ctx.power_fresh)
        snprintf(tlm, sizeof tlm, "Tlm %dC", ctx.temp_lm_dC / 10);
      else
        snprintf(tlm, sizeof tlm, "Tlm N/A");
      if (ctx.power_fresh && ctx.temp_mp_dC != -32768)
        snprintf(tmp, sizeof tmp, "Tmp %dC", ctx.temp_mp_dC / 10);
      else
        snprintf(tmp, sizeof tmp, "Tmp N/A");
      snprintf(buf, sizeof buf, "%s\n%s\n%s\nL/R back", up, tlm, tmp);
      break;
    }
    case LM_OUTPUT: {
      bool on = ctx.vbus_out_mV > LM_OUT_ON_MV;
      snprintf(buf, sizeof buf, "OUTPUT\n st: %s\n%cBack\n%c%s",
               on ? "ON" : "OFF",
               S.sub == 0 ? '>' : ' ',
               S.sub == 1 ? '>' : ' ',
               on ? "Turn OFF" : "Turn ON");
      break;
    }
    case LM_OUTPUT_CONFIRM: {
      snprintf(buf, sizeof buf, "CUT PWR\nTO PI?\n%cBack\n%cConfirm",
               S.sub == 0 ? '>' : ' ',
               S.sub == 1 ? '>' : ' ');
      break;
    }
    default:
      buf[0] = '\0';
      break;
  }
  draw4(oled, buf);
}

static void draw_notice(Adafruit_SSD1306& oled) {
  draw4(oled, "NO MODEM\nM.2 card\nmissing?");
}

/* --- input ---------------------------------------------------------------- */

/* Apply one button edge to the state machine. May close the menu (Exit /
 * Network hand-off) — callers must re-check local_menu_active() afterwards. */
static void handle_edge(Adafruit_SSD1306& oled, const LocalMenuCtx& ctx,
                        bool leftEdge, bool rightEdge) {
  switch (S.screen) {
    case LM_ROOT:
      if (leftEdge) {
        S.cursor = (uint8_t)((S.cursor + 1) % NUM_ROOT);
      } else if (rightEdge) {
        switch (S.cursor) {
          case ROOT_BRIGHT:  S.screen = LM_BRIGHT; break;
          case ROOT_SOUND:
            ui_settings_set_sound_enabled(!ui_settings_sound_enabled());
            ui_settings_commit();
            break;
          case ROOT_INFO:    S.screen = LM_INFO; break;
          case ROOT_OUTPUT:  S.screen = LM_OUTPUT; S.sub = 0; break;
          case ROOT_NETWORK: local_menu_host_open_esp32(); break; /* closes menu */
          case ROOT_EXIT:    local_menu_close(); break;
        }
      }
      break;

    case LM_BRIGHT:
      if (rightEdge) {
        uint8_t lv = (uint8_t)((ui_settings_brightness_level() + 1)
                               % UI_BRIGHTNESS_LEVELS);
        ui_settings_set_brightness_level(lv);
        ui_settings_apply_brightness(oled);   /* live preview, no flash write */
      } else if (leftEdge) {
        ui_settings_commit();                  /* persist on leave */
        S.screen = LM_ROOT;
        S.cursor = ROOT_BRIGHT;
      }
      break;

    case LM_INFO:
      if (leftEdge || rightEdge) {
        S.screen = LM_ROOT;
        S.cursor = ROOT_INFO;
      }
      break;

    case LM_OUTPUT: {
      bool on = ctx.vbus_out_mV > LM_OUT_ON_MV;
      if (leftEdge) {
        S.sub ^= 1;
      } else if (rightEdge) {
        if (S.sub == 0) {                      /* Back */
          S.screen = LM_ROOT;
          S.cursor = ROOT_OUTPUT;
        } else if (on) {                       /* Turn OFF → confirm first */
          S.screen = LM_OUTPUT_CONFIRM;
          S.sub = 0;
        } else {                               /* Turn ON → safe, no confirm */
          emit_power(WUPS_OP_PWR_ENABLE);
        }
      }
      break;
    }

    case LM_OUTPUT_CONFIRM:
      if (leftEdge) {
        S.sub ^= 1;
      } else if (rightEdge) {
        if (S.sub == 1) emit_power(WUPS_OP_PWR_DISABLE);
        S.screen = LM_OUTPUT;
        S.sub = 0;
      }
      break;
  }
}

/* --- public API ----------------------------------------------------------- */

bool local_menu_active(void) { return S.active; }

void local_menu_open(void) {
  S.active          = true;
  S.screen          = LM_ROOT;
  S.cursor          = 0;
  S.sub             = 0;
  S.btn_init        = false;       /* sync button levels on first tick */
  S.last_input_ms   = millis();
  S.notice_until_ms = 0;
}

void local_menu_close(void) {
  // Persist any pending settings change (e.g. brightness adjusted, then the
  // menu idled out without crossing the LM_BRIGHT leave-edge that commits).
  // EEPROM.put() diffs the buffer and commit() no-ops when nothing changed,
  // so this is free when there is nothing to save.
  ui_settings_commit();
  S.active = false;
}

void local_menu_note_no_modem(void) {
  if (!S.active) return;
  S.screen          = LM_ROOT;
  S.cursor          = ROOT_NETWORK;  /* land back on Network for an easy retry */
  S.sub             = 0;
  S.btn_init        = false;
  S.last_input_ms   = millis();
  // Clamp away the 0 sentinel ("no notice") so the splash isn't silently
  // skipped at the single millis() value where now+LM_NOTICE_MS wraps to 0.
  uint32_t until    = millis() + LM_NOTICE_MS;
  S.notice_until_ms = until ? until : 1;
}

void local_menu_tick(Adafruit_SSD1306& oled, bool leftDown, bool rightDown,
                     const LocalMenuCtx& ctx) {
  if (!S.active) return;
  uint32_t now = millis();

  /* First tick after (re)open: adopt the live button levels so the LEFT key
   * still held from the activation gesture doesn't fire a phantom edge. */
  if (!S.btn_init) {
    S.prev_left  = leftDown;
    S.prev_right = rightDown;
    S.btn_init   = true;
    if (S.notice_until_ms) { draw_notice(oled); return; }
    render(oled, ctx);
    return;
  }

  bool leftEdge  = leftDown  && !S.prev_left;
  bool rightEdge = rightDown && !S.prev_right;
  S.prev_left  = leftDown;
  S.prev_right = rightDown;

  /* Transient "No modem" notice owns the screen but doesn't block timing. */
  if (S.notice_until_ms) {
    if ((int32_t)(now - S.notice_until_ms) < 0) { draw_notice(oled); return; }
    S.notice_until_ms = 0;
  }

  if (leftEdge || rightEdge) {
    S.last_input_ms = now;
    ui_settings_beep(1000, 20);               /* click feedback (honours mute) */
    handle_edge(oled, ctx, leftEdge, rightEdge);
    if (!S.active) return;                     /* Exit / Network closed us */
  } else if ((uint32_t)(now - S.last_input_ms) > LM_IDLE_MS) {
    local_menu_close();                        /* wandered off → drop to home */
    return;
  }

  render(oled, ctx);
}
