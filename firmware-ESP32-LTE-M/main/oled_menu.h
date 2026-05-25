#pragma once

/*
 * ADR-0012 — OLED system menu controller (ESP32 side).
 *
 * The OLED + 2 buttons live on the RP2040 (Decision-C exception, see
 * trust_ui.h on the RP2040 side). For the system menu we reuse the
 * trust_ui rendering surface: the ESP32 sends a `ui.trust_prompt`
 * with mode=2 (WUPS_TRUST_PROMPT_MODE_MENU), the RP2040 displays the
 * text verbatim, and individual short button presses get forwarded to
 * us as `ui.button_event` broadcasts. Holding both buttons for
 * `confirm_secs` exits the menu (the RP2040 sends ui.trust_result
 * with result=CONFIRMED).
 *
 * The menu state machine — which screen is showing, which item is
 * highlighted, what selection does — lives in this module. On any
 * navigation we re-send a trust_prompt with the new text; on a
 * selection we fire the configured action (backend_mode_request_switch
 * or backend_mode_factory_reset).
 *
 * Activation is gesture-driven: the user holds both buttons for ~3 s
 * on the home screen. The RP2040 detects this and sends a single
 * `ui.button_event` with button=0xFF action=2 (long), which is our
 * agreed activation signal. (The button=0xFF encoding distinguishes
 * the activation gesture from a real button press, and stays
 * backwards-compatible with the existing button field set.)
 */

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True if the menu is currently active (a trust_prompt mode=2 was sent
 * and the user hasn't exited yet). External code can poll this to
 * suppress conflicting display traffic. */
bool oled_menu_active(void);

/* Open the root menu. Sends an initial trust_prompt mode=2 with the
 * "Main menu" screen text. Idempotent — calling while the menu is
 * already active is a no-op (caller can rely on a stable session). */
void oled_menu_open(void);

/* Close the menu immediately. Doesn't send a teardown frame to the
 * RP2040 (the existing trust_ui timeout / next prompt handles that).
 * Used internally after a selection that ends in a reboot. */
void oled_menu_close(void);

/* Handle a button event from the RP2040. `button` is 0=left, 1=right,
 * 0xFF=activation gesture; `action` is 0=press, 1=release, 2=long.
 * No-op when the menu isn't active (except for the activation gesture
 * which opens it). */
void oled_menu_on_button_event(uint8_t button, uint8_t action);

/* Handle a ui.trust_result frame for the menu's nonce. Called from the
 * trust-result dispatch path; matches by nonce against our active
 * prompt and closes the menu on `CONFIRMED` (= exit gesture) or
 * `TIMEOUT`. No-op if the result is for a non-menu prompt. */
void oled_menu_on_trust_result(uint32_t nonce, uint8_t result);

#ifdef __cplusplus
}
#endif
