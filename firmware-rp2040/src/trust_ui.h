/*
 * Track 2 / ADR-0011 §10.1/§10.4 — RP2040 trust-anchor front end (P2-4c).
 *
 * The OLED (SSD1306 0x3C) and the two buttons (GPIO13/14) physically
 * exist ONLY on the RP2040, so the Paranoic owner-binding confirm has to
 * be rendered + gestured here. This is a scoped, owner-approved exception
 * to the Track-1 "RP2040 unchanged" Decision C: the RP2040 stays a DUMB
 * renderer — it shows the ASCII the ESP32 sends, runs the 5 s both-button
 * hold, and reports the outcome. It NEVER parses the fingerprint /
 * claim-code and is NEVER the authority; the ESP32 verifies the owner
 * signature and owns the binding (cmdauth_arkiv). The in-board UART link
 * is the trust boundary.
 *
 * Wire contract: common/protocol.h ui.trust_prompt (0x05, ESP32→RP2040
 * REQ) / ui.trust_result (0x06, RP2040→ESP32 RESP). Non-blocking: a tick
 * is driven from loop(); while a prompt is active the trust UI owns the
 * display and the buttons, and the normal dashboard is suppressed.
 */
#ifndef TRUST_UI_H
#define TRUST_UI_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

/* Called from wups_on_local_frame() when a ui.trust_prompt REQ is
 * delivered to this node. `payload`/`len` is the raw WUPS payload;
 * `req_seq` is the REQ SEQ to echo on the ui.trust_result RESP. A new
 * prompt always replaces any in-progress one (e.g. claim-code screen →
 * owner fingerprint confirm). */
void trust_ui_on_prompt(const uint8_t* payload, uint16_t len, uint8_t req_seq);

/* True while the trust UI owns the OLED + buttons. loop() must skip the
 * normal screen/navigation while this holds. */
bool trust_ui_active(void);

/* Tick once per loop() iteration (~50 ms). `btnLeftDown`/`btnRightDown`
 * are the raw, active-low-decoded button levels (true = pressed). Renders
 * the trust screen, runs the both-button hold, and emits ui.trust_result
 * when the gate resolves (confirmed / timeout). No-op when inactive. */
void trust_ui_tick(Adafruit_SSD1306& oled, bool btnLeftDown,
                    bool btnRightDown);

#endif /* TRUST_UI_H */
