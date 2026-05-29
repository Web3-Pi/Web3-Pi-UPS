/*
 * ADR-0012 — OLED system menu state machine.
 *
 * See oled_menu.h for the design. This file holds:
 *   - the current screen / cursor state,
 *   - text generation for each screen (sent via ui.trust_prompt mode=2),
 *   - dispatch of button events and exit gestures,
 *   - hook points into backend_mode (request_switch / factory_reset).
 */

#include "oled_menu.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../../common/protocol.h"
#include "backend_mode.h"
#include "cmdauth_arkiv.h"
#include "esp_system.h"
#include "http_cfg.h"
#include "wups_link.h"

#define TAG "oled_menu"

/* Exit gesture — hold LEFT (single button) for this long. Matches the
 * activation gesture (hold RIGHT 2s in main.cpp) for symmetry: 2 s in,
 * 2 s out. Falls between the fingerprint-confirm 5 s used by the claim
 * flow and the home-screen auto-return 15 s. */
#define EXIT_HOLD_SECS  2u

/* Idle timeout — if no button event arrives for this long, the menu
 * closes itself. Prevents the device staying on a menu page after the
 * user wanders off. */
#define IDLE_TIMEOUT_MS 60000u

typedef enum {
    /* Root menu: shows distinct categories so the user can see at a
     * glance that this isn't "just a mode selector". Drill from here.
     *
     * Cursor opens on `Debug` (item 0): a deliberately non-destructive
     * default so a stray RIGHT press right after activation can't flip
     * the backend mode. Hold LEFT 2s closes the menu without picking
     * anything. */
    SCR_ROOT = 0,
    SCR_MODE,             /* MQTT / ARKIV / HTTP */
    SCR_FACTORY_RESET,    /* confirm screen      */
    SCR_REGEN_ARKIV,      /* confirm screen — re-roll device Arkiv wallet */
    SCR_HTTP_KEY,         /* show HTTP-mode secret + Back/New                */
    SCR_HTTP_REGEN,       /* confirm screen — re-roll HTTP-mode secret       */
} menu_screen_t;

static struct {
    bool          active;
    menu_screen_t screen;
    uint8_t       cursor;       /* highlighted row, 0-based */
    uint32_t      nonce;        /* binds our trust_prompt to its result */
    uint32_t      last_button_ms;
} S;

/* Items per screen. The 64x32 OLED fits 4 rows at default font size. */
static uint8_t screen_item_count(menu_screen_t s)
{
    switch (s) {
        case SCR_ROOT:          return 6;  /* Debug / Mode / HTTP Key / Reset / Wallet / Back */
        case SCR_MODE:          return 4;  /* MQTT / ARKIV / HTTP / Back  */
        case SCR_FACTORY_RESET: return 2;  /* Back / Wipe                 */
        case SCR_REGEN_ARKIV:   return 2;  /* Back / Regen                */
        case SCR_HTTP_KEY:      return 2;  /* Back / New key              */
        case SCR_HTTP_REGEN:    return 2;  /* Back / Regen                */
        default:                return 0;
    }
}

/* Render the current screen into `out` (NUL-terminated). The text goes
 * to the RP2040 verbatim — keep each line ≤ 10 chars (64 / 6 px) and
 * keep total under ~200 B to fit the trust_prompt payload cap. */
static void render_screen(char *out, size_t cap)
{
    const wups_backend_mode_t cur = backend_mode_get();
    switch (S.screen) {
        case SCR_ROOT: {
            /* 6 items on a 4-row OLED → scroll a 4-row window that keeps the
             * highlighted item visible with a little context above it. */
            const char *labels[6] = {
                "Debug",
                "Mode",
                "HTTP Key",
                "Reset",
                "Wallet",   /* short for "Regen Arkiv Wallet" */
                "Back",
            };
            const int n = 6;
            int first = 0;
            if (S.cursor >= 3) first = (int)S.cursor - 2;
            if (first > n - 4) first = n - 4;   /* don't scroll past the end */
            if (first < 0) first = 0;
            int written = 0;
            for (int row = 0; row < 4 && first + row < n; ++row) {
                int idx = first + row;
                written += snprintf(out + written,
                                    written < (int)cap ? cap - (size_t)written : 0,
                                    "%c%s%s",
                                    idx == S.cursor ? '>' : ' ',
                                    labels[idx],
                                    row == 3 ? "" : "\n");
            }
            break;
        }
        case SCR_MODE:
            /* `*` = active mode. Cursor 0 is intentionally MQTT (not Back)
             * because the user came in here specifically to flip the mode. */
            snprintf(out, cap,
                     "%cMQTT  %c\n"
                     "%cARKIV %c\n"
                     "%cHTTP  %c\n"
                     "%cBack",
                     S.cursor == 0 ? '>' : ' ',
                     cur == WUPS_BACKEND_MODE_MQTT  ? '*' : ' ',
                     S.cursor == 1 ? '>' : ' ',
                     cur == WUPS_BACKEND_MODE_ARKIV ? '*' : ' ',
                     S.cursor == 2 ? '>' : ' ',
                     cur == WUPS_BACKEND_MODE_HTTP  ? '*' : ' ',
                     S.cursor == 3 ? '>' : ' ');
            break;
        case SCR_HTTP_KEY: {
            /* Show the HTTP-mode shared secret so the operator can type it
             * into their server. Two groups of 8 (with a mid-space) fit the
             * 64-px width. http_cfg generates one on first read. */
            char code[HTTP_CFG_SECRET_CHARS + 1];
            if (http_cfg_get_secret(code, sizeof code) != ESP_OK) {
                snprintf(out, cap, "HTTP KEY\nERROR\n%cBack", '>');
                break;
            }
            snprintf(out, cap,
                     "%.4s %.4s\n"
                     "%.4s %.4s\n"
                     "%cBack\n"
                     "%cNew key",
                     code, code + 4, code + 8, code + 12,
                     S.cursor == 0 ? '>' : ' ',
                     S.cursor == 1 ? '>' : ' ');
            break;
        }
        case SCR_HTTP_REGEN:
            /* Back-first safety: regen invalidates the code the operator
             * already configured on their server. */
            snprintf(out, cap,
                     "NEW HTTP\nKEY?\n"
                     "%cBack\n"
                     "%cRegen",
                     S.cursor == 0 ? '>' : ' ',
                     S.cursor == 1 ? '>' : ' ');
            break;
        case SCR_FACTORY_RESET:
            /* Cursor opens on Back (item 0) so a confirm screen drilled
             * into by accident doesn't wipe the device with one further
             * press. Wipe is item 1 — needs a deliberate LEFT + RIGHT. */
            snprintf(out, cap,
                     "FACTORY\n"
                     "RESET ALL?\n"
                     "%cBack\n"
                     "%cWipe",
                     S.cursor == 0 ? '>' : ' ',
                     S.cursor == 1 ? '>' : ' ');
            break;
        case SCR_REGEN_ARKIV:
            /* Same Back-first safety as Factory Reset — regen invalidates
             * the existing on-chain identity (nonce, claim binding). */
            snprintf(out, cap,
                     "NEW WALLET\n"
                     "ARKIV ADDR?\n"
                     "%cBack\n"
                     "%cRegen",
                     S.cursor == 0 ? '>' : ' ',
                     S.cursor == 1 ? '>' : ' ');
            break;
    }
}

/* Push the current screen to the RP2040. The trust_ui on the other end
 * replaces any in-progress prompt with this one, so re-sending is the
 * documented way to update what's on screen. */
static void push_screen(void)
{
    if (!S.active) return;
    char text[200];
    render_screen(text, sizeof text);
    /* `confirm_secs = EXIT_HOLD_SECS` tells the RP2040 the both-button
     * hold length needed to exit (mode=2 menu reinterprets the existing
     * countdown timer as the back/exit gesture). */
    wups_link_trust_prompt(WUPS_TRUST_PROMPT_MODE_MENU,
                           EXIT_HOLD_SECS,
                           S.nonce,
                           text);
    S.last_button_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Send a ui.set_screen REQ to the RP2040 so the dashboard switches
 * away from the home screen to one of the developer pages. Used by the
 * Debug item — the RP2040 trust_ui side relinquishes the OLED in its
 * set_screen handler, so we don't have to coordinate a teardown frame. */
static void send_set_screen(uint8_t screen_idx)
{
    wups_ui_set_screen_v1_t s = { .version = 1, .screen = screen_idx };
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_UI, WUPS_OP_UI_SET_SCREEN,
                   WUPS_FLAG_REQ, &s, sizeof(s));
}

/* Push an in-flight "we're doing something, please wait" screen via
 * another ui.trust_prompt mode=2 with the same nonce. Without it the
 * OLED freezes on the last menu page during the ~10 s mode-switch /
 * factory-reset reboot, which looks like a hang. The 200 ms delay
 * gives the RP2040 time to deframe + render the new prompt before
 * the network stack goes away under us. */
static void push_transition_screen(const char *text)
{
    if (!S.active) return;
    wups_link_trust_prompt(WUPS_TRUST_PROMPT_MODE_MENU,
                           EXIT_HOLD_SECS,
                           S.nonce,
                           text);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* Perform the action bound to the highlighted item. Returns true if
 * the menu should close after this action (the action itself usually
 * reboots so the close is mostly cosmetic). */
static bool activate_current(void)
{
    switch (S.screen) {
        case SCR_ROOT:
            switch (S.cursor) {
                case 0:  /* Debug — jump RP2040 to the Power screen and
                          *         close the menu so LEFT/RIGHT shorts
                          *         cycle the dashboard pages again. */
                    ESP_LOGI(TAG, "menu → debug screens");
                    send_set_screen(1);  /* SCREEN_POWER on RP2040 */
                    oled_menu_close();
                    break;
                case 1:  /* Mode submenu */
                    S.screen = SCR_MODE;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 2:  /* HTTP Key → show/regenerate the HTTP-mode secret */
                    S.screen = SCR_HTTP_KEY;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 3:  /* Reset → confirm screen */
                    S.screen = SCR_FACTORY_RESET;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 4:  /* Wallet → regen Arkiv confirm screen */
                    S.screen = SCR_REGEN_ARKIV;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 5:  /* Back / exit menu */
                    ESP_LOGI(TAG, "menu → back (exit)");
                    /* Tell the RP2040 to drop back to the home dashboard
                     * and release the OLED. The trust_ui session is
                     * dismissed by set_screen on that side. */
                    send_set_screen(0);
                    oled_menu_close();
                    break;
            }
            return false;
        case SCR_MODE: {
            switch (S.cursor) {
                case 0:  /* MQTT  */
                case 1:  /* ARKIV */
                {
                    const wups_backend_mode_t target =
                        S.cursor == 0 ? WUPS_BACKEND_MODE_MQTT
                                      : WUPS_BACKEND_MODE_ARKIV;
                    /* Show "switching" feedback so the user doesn't see
                     * the menu page frozen during the ~10 s reboot.
                     * 10-char-per-line cap (64 px / 6 px default font);
                     * "MODE: ARKIV" overflowed by one — split across
                     * lines instead. */
                    char wait_text[64];
                    snprintf(wait_text, sizeof wait_text,
                             "SWITCHING\nTO %s\nPlease\nwait...",
                             S.cursor == 0 ? "MQTT" : "ARKIV");
                    push_transition_screen(wait_text);
                    ESP_LOGI(TAG, "menu → switch to %s",
                             backend_mode_name(target));
                    backend_mode_request_switch(target);
                    break;
                }
                case 2:  /* HTTP control mode (plan HTTP-2) */
                {
                    push_transition_screen("SWITCHING\nTO HTTP\nPlease\nwait...");
                    ESP_LOGI(TAG, "menu → switch to http");
                    backend_mode_request_switch(WUPS_BACKEND_MODE_HTTP);
                    break;
                }
                case 3:  /* Back → root */
                    S.screen = SCR_ROOT;
                    S.cursor = 1;  /* land back on "Mode" so re-entry is easy */
                    push_screen();
                    break;
            }
            return false;
        }
        case SCR_FACTORY_RESET:
            if (S.cursor == 0) {
                /* Back → root */
                S.screen = SCR_ROOT;
                S.cursor = 2;  /* land on "Reset" so retry is easy */
                push_screen();
            } else {
                ESP_LOGW(TAG, "menu → factory reset confirmed");
                push_transition_screen("FACTORY\nRESET\nPlease\nwait...");
                backend_mode_factory_reset();  /* reboots */
            }
            return false;
        case SCR_REGEN_ARKIV:
            if (S.cursor == 0) {
                /* Back → root */
                S.screen = SCR_ROOT;
                S.cursor = 3;  /* land on "Wallet" so retry is easy */
                push_screen();
            } else {
                ESP_LOGW(TAG, "menu → regenerate Arkiv wallet confirmed");
                push_transition_screen("REGEN\nWALLET\nPlease\nwait...");
                esp_err_t err = cmdauth_arkiv_regenerate_wallet();
                if (err == ESP_OK) {
                    ESP_LOGW(TAG, "wallet regenerated — rebooting to load new key");
                    vTaskDelay(pdMS_TO_TICKS(200));  /* flush logs */
                    esp_restart();
                } else {
                    ESP_LOGE(TAG, "wallet regen failed: %s — staying in menu",
                             esp_err_to_name(err));
                    /* Push a brief error screen and let the user retry. */
                    push_transition_screen("REGEN\nFAILED\nRetry?");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    push_screen();   /* back to the confirm screen */
                }
            }
            return false;
        case SCR_HTTP_KEY:
            if (S.cursor == 0) {
                /* Back → root, land on "HTTP Key" */
                S.screen = SCR_ROOT;
                S.cursor = 2;
                push_screen();
            } else {
                /* New key → confirm screen */
                S.screen = SCR_HTTP_REGEN;
                S.cursor = 0;
                push_screen();
            }
            return false;
        case SCR_HTTP_REGEN:
            if (S.cursor == 0) {
                /* Back → show the (unchanged) key again */
                S.screen = SCR_HTTP_KEY;
                S.cursor = 0;
                push_screen();
            } else {
                ESP_LOGW(TAG, "menu → regenerate HTTP secret confirmed");
                esp_err_t err = http_cfg_regenerate_secret();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "HTTP secret regen failed: %s",
                             esp_err_to_name(err));
                }
                /* No reboot needed — the next POST reads the new key. Show it. */
                S.screen = SCR_HTTP_KEY;
                S.cursor = 0;
                push_screen();
            }
            return false;
    }
    return false;
}

/* --- public API ----------------------------------------------------------- */

bool oled_menu_active(void)
{
    return S.active;
}

void oled_menu_open(void)
{
    if (S.active) return;
    /* Random nonce so a late trust_result for a previous menu session
     * (or anything else) doesn't accidentally match ours. */
    S.nonce = esp_random();
    S.active = true;
    S.screen = SCR_ROOT;
    S.cursor = 0;
    ESP_LOGI(TAG, "menu opened (nonce=%" PRIu32 ")", S.nonce);
    push_screen();
}

void oled_menu_close(void)
{
    if (!S.active) return;
    S.active = false;
    ESP_LOGI(TAG, "menu closed");
}

void oled_menu_on_button_event(uint8_t button, uint8_t action)
{
    /* Activation gesture (button=0xFF, action=long): the RP2040 sends
     * exactly one of these when the user holds LEFT on the home screen
     * for the activation duration. If a previous menu session was left
     * dangling (e.g. on the RP2040 side after a backend-mode reboot the
     * panel's HELLO handler missed), force-close and reopen — the user
     * is asking for a fresh menu and we shouldn't make them retry. */
    if (button == 0xFF && action == 2u) {
        if (S.active) oled_menu_close();
        oled_menu_open();
        return;
    }

    if (!S.active) return;

    /* Only act on "press" (action=0) for short presses. action=1
     * (release) is informational; action=2 (long) is reserved for the
     * RP2040's exit-gesture path (which arrives as trust_result). */
    if (action != 0u) return;

    const uint8_t n = screen_item_count(S.screen);
    if (n == 0) return;

    if (button == 0u) {
        /* LEFT = move cursor DOWN (with wrap). Down feels more natural
         * than up — items are listed top-to-bottom so "advance through
         * the list" reads as moving downward. */
        S.cursor = (uint8_t)((S.cursor + 1u) % n);
        push_screen();
    } else if (button == 1u) {
        /* RIGHT = select. The current item either drills down, runs an
         * action, or no-ops. Selection vs scrolling is RIGHT because
         * navigation here is a single-axis list — pairing LEFT=up,
         * RIGHT=select feels more natural than the alternative on the
         * 2-button hardware and matches how the home screen treats
         * RIGHT (the action button on Power Control). */
        activate_current();
    }
    /* Unknown button code: ignore. */
}

void oled_menu_on_trust_result(uint32_t nonce, uint8_t result)
{
    if (!S.active) return;
    if (nonce != S.nonce) return;
    /* Both CONFIRMED (exit hold succeeded) and TIMEOUT close the menu —
     * the user either deliberately backed out or stopped interacting.
     * CANCELLED isn't currently emitted by the RP2040 in mode=2 but is
     * the obvious "anything else" close. */
    (void)result;
    oled_menu_close();
}
