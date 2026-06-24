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
#include "arkiv_tlm.h"
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
    SCR_ARKIV_WALLET,     /* Arkiv wallet submenu: Address/Balance/Regen/Back*/
    SCR_ARKIV_ADDR,       /* show the device's Arkiv fund address (read-only)*/
    SCR_ARKIV_BALANCE,    /* show the device wallet GLM balance (read-only)  */
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

/* Result text for SCR_ARKIV_BALANCE. The balance query is network/blocking,
 * so it runs once in activate_current() and stashes the formatted line here;
 * render_screen() then just shows it (the screen has no cursor navigation —
 * any press returns). Two short lines fit the 64x32 OLED, ≤ 10 chars each. */
static char s_balance_text[40];

/* Items per screen. The 64x32 OLED fits 4 rows at default font size. */
static uint8_t screen_item_count(menu_screen_t s)
{
    switch (s) {
        case SCR_ROOT:          return 6;  /* Debug / Mode / HTTP Key / Reset / Wallet / Back */
        case SCR_MODE:          return 4;  /* MQTT / ARKIV / HTTP / Back  */
        case SCR_FACTORY_RESET: return 2;  /* Back / Wipe                 */
        case SCR_REGEN_ARKIV:   return 2;  /* Back / Regen                */
        case SCR_ARKIV_WALLET:  return 4;  /* Address/Balance/Regen/Back  */
        case SCR_ARKIV_ADDR:    return 1;  /* read-only — any press = Back */
        case SCR_ARKIV_BALANCE: return 1;  /* read-only — any press = Back */
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
        case SCR_ARKIV_WALLET:
            /* Submenu: view the fund address (read-only), check the on-chain
             * GLM balance, or re-roll the key. 4 items = exactly 4 OLED rows. */
            snprintf(out, cap,
                     "%cAddress\n"
                     "%cBalance\n"
                     "%cRegen\n"
                     "%cBack",
                     S.cursor == 0 ? '>' : ' ',
                     S.cursor == 1 ? '>' : ' ',
                     S.cursor == 2 ? '>' : ' ',
                     S.cursor == 3 ? '>' : ' ');
            break;
        case SCR_ARKIV_ADDR: {
            /* The device's own Arkiv (Braga) wallet — the owner funds THIS with
             * GLM so it can pay gas to publish telemetry. Shown on the OLED for
             * the cold-start bootstrap (owner has the device physically; no
             * backend / on-chain write needed first, §4.6). 20-byte EOA = 40 hex;
             * at ~10 chars/line that's exactly 4 rows, so there is no room for a
             * "0x"/label/Back — the owner prepends 0x, and any button returns
             * (handled in oled_menu_on_button_event). */
            if (!cmdauth_arkiv_ready()) {
                snprintf(out, cap, "No Arkiv\nwallet yet");
                break;
            }
            const uint8_t *a = cmdauth_arkiv_device_addr();
            static const char H[] = "0123456789abcdef";
            char hex[41];
            for (int i = 0; i < 20; ++i) {
                hex[2 * i]     = H[a[i] >> 4];
                hex[2 * i + 1] = H[a[i] & 0x0F];
            }
            hex[40] = '\0';
            /* Raw 0x-address. The RP2040 (MODE_WALLET) renders a legible hex
             * page (slashed zeros) alternating with a QR built from this exact
             * string — so no row-splitting here. */
            snprintf(out, cap, "0x%s", hex);
            break;
        }
        case SCR_ARKIV_BALANCE:
            /* Read-only result of the eth_getBalance query run in activate
             * (or the re-entry from the address page). s_balance_text already
             * holds the two formatted lines ("BALANCE\n<n> GLM" / an error). */
            snprintf(out, cap, "%s", s_balance_text);
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
    /* The fund-address screen uses MODE_WALLET (RP2040 draws hex + QR); every
     * other screen is a plain MODE_MENU list. Both forward button presses, so
     * navigation (incl. "any press = Back" on the address page) works the same.
     * `confirm_secs = EXIT_HOLD_SECS` is the both-button hold length to exit. */
    const uint8_t mode = (S.screen == SCR_ARKIV_ADDR)
                             ? WUPS_TRUST_PROMPT_MODE_WALLET
                             : WUPS_TRUST_PROMPT_MODE_MENU;
    wups_link_trust_prompt(mode, EXIT_HOLD_SECS, S.nonce, text);
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

/* Format the device wallet's cached balance into s_balance_text for
 * SCR_ARKIV_BALANCE. Non-blocking, no RPC — reads the value the arkiv_tlm task
 * refreshes (see arkiv_tlm_cached_balance_wei). Wei → GLM (18 decimals), 6
 * fractional digits. Laid out on three lines — "BALANCE" / "<n>.<6>" / "GLM" —
 * because "<n>.<6> GLM" (12 chars) overflows the 64x32 OLED's ~10-char line. */
static void fetch_balance_text(void)
{
    if (!cmdauth_arkiv_ready()) {
        snprintf(s_balance_text, sizeof s_balance_text, "No Arkiv\nwallet yet");
        return;
    }
    /* Read the cached balance refreshed by the arkiv_tlm task (6 KB stack,
     * RPC-safe). We must NOT do eth_getBalance here: this runs on the 4 KB
     * wups_rx button task and a blocking HTTP+TLS RPC overflows its stack
     * (the device rebooted). The cache populates after the first telemetry
     * submit (~30 s) and refreshes every few minutes. */
    uint64_t wei = 0;
    if (!arkiv_tlm_cached_balance_wei(&wei)) {
        snprintf(s_balance_text, sizeof s_balance_text, "BALANCE\nwait tlm");
        return;
    }
    /* 1 GLM = 1e18 wei. Split into integer + 6-decimal fraction without
     * floating point: micro-GLM = wei / 1e12 keeps 6 decimals exactly. */
    const uint64_t WEI_PER_GLM   = 1000000000000000000ULL; /* 1e18 */
    const uint64_t WEI_PER_MICRO = 1000000000000ULL;       /* 1e12 */
    uint64_t whole = wei / WEI_PER_GLM;
    uint64_t micro = (wei % WEI_PER_GLM) / WEI_PER_MICRO;   /* 0..999999 */
    snprintf(s_balance_text, sizeof s_balance_text,
             "BALANCE\n%" PRIu64 ".%06" PRIu64 "\nGLM", whole, micro);
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
                case 4:  /* Wallet → Arkiv wallet submenu (address / regen) */
                    S.screen = SCR_ARKIV_WALLET;
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
                /* Drop the RP2040-local settings (brightness/sound) back to
                 * defaults too, so the reset is a complete "as-new". Sent
                 * before the ESP32 reboots; harmless no-op on an un-updated
                 * RP2040 (unknown op is ignored). */
                wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_UI,
                               WUPS_OP_UI_LOCAL_RESET, 0, NULL, 0);
                vTaskDelay(pdMS_TO_TICKS(80));  /* let the frame flush over UART */
                backend_mode_factory_reset();  /* regens wallet, wipes nvs, reboots */
            }
            return false;
        case SCR_REGEN_ARKIV:
            if (S.cursor == 0) {
                /* Back → Arkiv wallet submenu, land on Regen (item 2) */
                S.screen = SCR_ARKIV_WALLET;
                S.cursor = 2;
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
        case SCR_ARKIV_WALLET:
            switch (S.cursor) {
                case 0:  /* Address → read-only fund-address screen */
                    S.screen = SCR_ARKIV_ADDR;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 1:  /* Balance → query on-chain GLM, then show result.
                          * The query is blocking (~RPC round-trip) so flash a
                          * "checking…" screen first, exactly like the mode
                          * switch does for its reboot wait. */
                    push_transition_screen("BALANCE\nchecking\nplease\nwait...");
                    fetch_balance_text();
                    S.screen = SCR_ARKIV_BALANCE;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 2:  /* Regen → re-roll confirm screen */
                    S.screen = SCR_REGEN_ARKIV;
                    S.cursor = 0;
                    push_screen();
                    break;
                case 3:  /* Back → root, land on Wallet */
                    S.screen = SCR_ROOT;
                    S.cursor = 4;
                    push_screen();
                    break;
            }
            return false;
        case SCR_ARKIV_ADDR:
            /* Read-only — any select returns to the wallet submenu. */
            S.screen = SCR_ARKIV_WALLET;
            S.cursor = 0;
            push_screen();
            return false;
        case SCR_ARKIV_BALANCE:
            /* Read-only — any select returns to the wallet submenu, landing
             * on Balance (item 1) so a re-check is one press away. */
            S.screen = SCR_ARKIV_WALLET;
            S.cursor = 1;
            push_screen();
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

    /* The read-only fund-address page has no on-screen Back (the 40-hex address
     * fills all 4 rows) — either button returns to the wallet submenu. */
    if (S.screen == SCR_ARKIV_ADDR) {
        S.screen = SCR_ARKIV_WALLET;
        S.cursor = 0;
        push_screen();
        return;
    }

    /* The read-only balance result has no on-screen Back either — either
     * button returns to the wallet submenu, landing on Balance (item 1) so a
     * re-check is one press away (matches the activate-path behaviour). */
    if (S.screen == SCR_ARKIV_BALANCE) {
        S.screen = SCR_ARKIV_WALLET;
        S.cursor = 1;
        push_screen();
        return;
    }

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
