/*
 * Track 2 / ADR-0011 §10.1/§10.4 — RP2040 trust-anchor front end (P2-4c).
 * See trust_ui.h for the design + the Decision-C exception rationale.
 */
#include "trust_ui.h"
#include "wups_router.h"
#include "wups_proto.h"
#include "ui_settings.h"
#include <string.h>

/* 64x32 OLED, default GFX font: 6 px advance, 8 px tall. */
static constexpr uint8_t  SCR_COLS   = 10;   /* 64 / 6 (1 px slack)     */
static constexpr uint8_t  SCR_ROWS   = 4;    /* 32 / 8                  */
static constexpr uint8_t  MAX_LINES  = 44;   /* >= wrap of a 232 B text */
static constexpr uint8_t  BUZZER_PIN = 15;   /* mirrors main.cpp        */

/* RP2040-side UX timing. The ESP32 waits TRUST_WAIT_MS (180 s) for the
 * result, so resolve well before that. */
static constexpr uint32_t CONFIRM_TIMEOUT_MS = 150000; /* → result=timeout */
static constexpr uint32_t DISPLAY_TTL_MS     = 90000;  /* claim-code idle  */
static constexpr uint32_t PAGE_MS            = 3500;   /* auto-page cadence*/
static constexpr uint8_t  RELEASE_GRACE      = 2;      /* ~100 ms debounce */

/* result codes (common/protocol.h ui.trust_result). */
static constexpr uint8_t  RES_CONFIRMED = 0;
static constexpr uint8_t  RES_TIMEOUT   = 1;

static struct {
    bool     active;
    uint8_t  mode;          /* 0 = fingerprint confirm, 1 = claim-code,
                               2 = system menu (ADR-0012)                */
    uint8_t  confirm_secs;  /* 0 = display-only (no result expected)     */
    uint32_t nonce;
    uint8_t  req_seq;
    uint32_t t0_ms;         /* when this prompt became active            */
    uint32_t hold_start_ms; /* 0 = not currently holding both buttons    */
    uint8_t  release_miss;  /* consecutive non-both samples (debounce)   */
    /* ADR-0012 — edge-detect state for mode=2. Initialised on the first
     * mode=2 tick with the live button levels so a button still held
     * from the activation gesture doesn't fire a phantom "press" event
     * the moment menu opens. */
    bool     menu_btn_initialized;
    bool     menu_prev_left;
    bool     menu_prev_right;
    uint16_t text_len;
    char     text[241];
} S;

bool trust_ui_active(void) { return S.active; }

void trust_ui_force_close(void)
{
    S.active = false;
    S.hold_start_ms = 0;
    S.release_miss  = 0;
}

static void send_result(uint8_t result)
{
    wups_ui_trust_result_v1_t r;
    r.version     = 1;
    r.result      = result;
    r.reserved[0] = 0;
    r.reserved[1] = 0;
    r.nonce       = S.nonce;
    /* RESP echoes the REQ SEQ; the ESP32 correlates by nonce. */
    wups_send_seq(WUPS_PORT_ESP32, WUPS_ADDR_ESP32, WUPS_CLASS_UI,
                  WUPS_OP_UI_TRUST_RESULT, WUPS_FLAG_RESP, S.req_seq,
                  &r, sizeof(r));
}

void trust_ui_on_prompt(const uint8_t* payload, uint16_t len, uint8_t req_seq)
{
    if (len < sizeof(wups_ui_trust_prompt_v1_hdr_t)) return;
    wups_ui_trust_prompt_v1_hdr_t h;
    memcpy(&h, payload, sizeof(h));
    if (h.version != 1) return;

    uint16_t tlen = h.text_len;
    if ((uint32_t)sizeof(h) + tlen > len) return;          /* truncated */
    if (tlen > sizeof(S.text) - 1) tlen = sizeof(S.text) - 1;

    memcpy(S.text, payload + sizeof(h), tlen);
    S.text[tlen]     = '\0';
    S.text_len       = tlen;
    S.mode           = h.mode;
    S.confirm_secs   = h.confirm_secs;
    S.nonce          = h.nonce;
    S.req_seq        = req_seq;
    S.t0_ms          = millis();
    S.hold_start_ms  = 0;
    S.release_miss   = 0;
    /* Mode=2 (menu) re-syncs prev-button state on the next tick so the
     * fingers still on the activation key don't generate phantom edges.
     * Other modes don't read prev-state, so the flag is harmless there. */
    S.menu_btn_initialized = false;
    S.active         = true;
}

/* Greedy word-wrap into fixed-width rows, honouring '\n' as a hard break
 * and hard-splitting words longer than the screen. Returns line count. */
static uint8_t wrap_lines(char out[MAX_LINES][SCR_COLS + 1])
{
    uint8_t nl = 0, col = 0;
    const char* p = S.text;
    while (*p && nl < MAX_LINES) {
        if (*p == '\n') {
            out[nl][col] = '\0';
            nl++; col = 0; p++;
            continue;
        }
        /* measure the next word */
        const char* w = p;
        uint8_t wl = 0;
        while (w[wl] && w[wl] != ' ' && w[wl] != '\n') wl++;

        if (col != 0 && col + 1 + wl > SCR_COLS) {  /* word won't fit */
            out[nl][col] = '\0';
            nl++; col = 0;
            if (nl >= MAX_LINES) break;
        }
        if (wl > SCR_COLS) {                         /* hard-split */
            for (uint8_t i = 0; i < wl; ++i) {
                if (col == SCR_COLS) {
                    out[nl][col] = '\0'; nl++; col = 0;
                    if (nl >= MAX_LINES) return nl;
                }
                out[nl][col++] = p[i];
            }
            p += wl;
        } else {
            if (col != 0 && col < SCR_COLS) out[nl][col++] = ' ';
            for (uint8_t i = 0; i < wl && col < SCR_COLS; ++i)
                out[nl][col++] = p[i];
            p += wl;
        }
        while (*p == ' ') p++;
    }
    if (nl < MAX_LINES) { out[nl][col] = '\0'; nl++; }
    return nl;
}

static void draw_paged(Adafruit_SSD1306& oled)
{
    static char lines[MAX_LINES][SCR_COLS + 1];
    uint8_t nl = wrap_lines(lines);
    uint8_t pages = (nl + SCR_ROWS - 1) / SCR_ROWS;
    if (pages == 0) pages = 1;
    uint8_t page = (uint8_t)((millis() / PAGE_MS) % pages);

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    for (uint8_t r = 0; r < SCR_ROWS; ++r) {
        uint8_t li = page * SCR_ROWS + r;
        if (li >= nl) break;
        oled.setCursor(0, r * 8);
        oled.print(lines[li]);
    }
    oled.display();
}

/* Claim-code screen (display-only, §10.4). The ESP32 sends exactly
 * "ID:<iccid>\n<w1 w2 ...>". A generic word-wrap looked ragged (the long
 * ICCID bled a stray word onto the ID page), so render two fixed pages:
 *
 *   page 0:  ID:                page 1:  1:<word1>
 *            <iccid 0..9>                2:<word2>
 *            <iccid 10..>                3:<word3>
 *                                        4:<word4>
 *
 * Falls back to the generic pager if the text isn't in that shape. */
static void draw_claim_code(Adafruit_SSD1306& oled)
{
    const char* nl = strchr(S.text, '\n');
    if (!nl || strncmp(S.text, "ID:", 3) != 0) { draw_paged(oled); return; }

    const char* id   = S.text + 3;          /* digits after "ID:"      */
    size_t      idlen = (size_t)(nl - id);
    const char* words = nl + 1;             /* space-separated words   */

    uint8_t page = (uint8_t)((millis() / PAGE_MS) % 2);
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    if (page == 0) {
        oled.setCursor(0, 0);
        oled.print(F("ID:"));
        uint8_t row = 1;
        for (size_t i = 0; i < idlen && row < SCR_ROWS; i += SCR_COLS, ++row) {
            char chunk[SCR_COLS + 1];
            size_t n = idlen - i;
            if (n > SCR_COLS) n = SCR_COLS;
            memcpy(chunk, id + i, n);
            chunk[n] = '\0';
            oled.setCursor(0, row * 8);
            oled.print(chunk);
        }
    } else {
        const char* p = words;
        for (uint8_t idx = 0; idx < SCR_ROWS && *p; ++idx) {
            while (*p == ' ') ++p;
            if (!*p) break;
            char w[SCR_COLS + 1];
            uint8_t c = 0;
            /* Uppercase the BIP39 word — far more legible on the 64x32
             * OLED. Display-only: the contract is the word→index map, and
             * the Panel lowercases input, so case never affects bytes.
             * No "n:" prefix — one word per line reads cleaner. */
            while (*p && *p != ' ' && c < SCR_COLS) {
                char ch = *p++;
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                w[c++] = ch;
            }
            while (*p && *p != ' ') ++p;     /* skip any overflow */
            w[c] = '\0';
            oled.setCursor(0, idx * 8);
            oled.print(w);
        }
    }
    oled.display();
}

/* Fingerprint screen (§10.1 trust-anchor confirm). The ESP32 sends text
 * "<w1> <w2> <w3> <w4> chk:<HHHH>" (4 BIP39 words + 16-bit checksum, see
 * arkiv_claim.c owner_fingerprint). Rendering through the generic paged
 * wrapper looked ragged (mixed case, words split across lines, the chk
 * suffix orphaned on the last page) — match the claim-code treatment:
 * fixed two pages, uppercase, one word per line.
 *
 *   page 0:  WORD1                page 1:  CHK:
 *            WORD2                          <hex at textSize 2>
 *            WORD3
 *            WORD4
 *
 * Falls back to the generic pager if the text isn't in the expected
 * "<words> chk:<hex>" shape. */
static void draw_fingerprint(Adafruit_SSD1306& oled)
{
    const char* chk_marker = strstr(S.text, " chk:");
    if (!chk_marker) { draw_paged(oled); return; }
    const char* words_end = chk_marker;
    const char* chk       = chk_marker + 5;   /* skip " chk:" */

    uint8_t page = (uint8_t)((millis() / PAGE_MS) % 2);
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);

    if (page == 0) {
        const char* p = S.text;
        for (uint8_t idx = 0; idx < SCR_ROWS && p < words_end; ++idx) {
            while (p < words_end && *p == ' ') ++p;
            if (p >= words_end) break;
            char w[SCR_COLS + 1];
            uint8_t c = 0;
            /* Uppercase, mirroring claim-code (display-only; the contract
             * is the word→index map, the verifying side already lowercases). */
            while (p < words_end && *p != ' ' && c < SCR_COLS) {
                char ch = *p++;
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                w[c++] = ch;
            }
            while (p < words_end && *p != ' ') ++p;   /* skip overflow */
            w[c] = '\0';
            oled.setCursor(0, idx * 8);
            oled.print(w);
        }
    } else {
        oled.setCursor(0, 0);
        oled.print(F("CHK:"));
        oled.setTextSize(2);
        oled.setCursor(0, 12);
        /* First 4 hex chars of the checksum, uppercase. Stop early on any
         * unexpected separator so we don't bleed past the field. */
        char chk4[5] = {0};
        for (uint8_t i = 0; i < 4; ++i) {
            char ch = chk[i];
            if (!ch || ch == ' ' || ch == '\n') break;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            chk4[i] = ch;
        }
        oled.print(chk4);
    }
    oled.display();
}

static void draw_countdown(Adafruit_SSD1306& oled, uint8_t secs_left)
{
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(2, 0);
    oled.print(F("HOLD BOTH"));
    oled.setTextSize(3);
    oled.setCursor(26, 9);
    oled.print(secs_left);
    oled.display();
}

/* ADR-0012 menu rendering — verbatim text, no countdown, no '\n' magic
 * (the ESP32 already formats short lines that fit the 64x32 OLED). */
static void draw_menu(Adafruit_SSD1306& oled)
{
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    uint8_t row = 0;
    uint8_t col = 0;
    for (const char* p = S.text; *p && row < SCR_ROWS; ++p) {
        if (*p == '\n') {
            row++; col = 0;
            continue;
        }
        if (col >= SCR_COLS) continue;   /* clip; ESP32 keeps lines short */
        oled.setCursor(col * 6, row * 8);
        oled.write((uint8_t)*p);
        col++;
    }
    oled.display();
}

/* ADR-0012 — broadcast a ui.button_event so the ESP32 menu state
 * machine can react. button: 0=left, 1=right; action: 0=press, 1=release,
 * 2=long. Sent BROADCAST so the (otherwise dumb) protocol stays
 * symmetric with the existing event family. */
static void send_button_event(uint8_t button, uint8_t action)
{
    wups_ui_button_event_v1_t e;
    e.version  = 1;
    e.button   = button;
    e.action   = action;
    e.reserved = 0;
    wups_send(WUPS_PORT_ESP32, WUPS_ADDR_BROADCAST,
              WUPS_CLASS_UI, WUPS_OP_UI_BUTTON_EVENT,
              WUPS_FLAG_EVENT, &e, sizeof(e));
}

void trust_ui_tick(Adafruit_SSD1306& oled, bool btnLeftDown,
                    bool btnRightDown)
{
    if (!S.active) return;
    uint32_t now = millis();

    /* ADR-0012 — menu mode. The ESP32 owns navigation state; we just
     * render the latest text, forward short button presses as
     * ui.button_event broadcasts, and treat a hold-LEFT (or hold-both
     * as a fallback) of `confirm_secs` as the exit gesture (sent back
     * as trust_result CONFIRMED so the ESP32 closes its menu state).
     * Exit was originally hold-both-3s, but the user reported the
     * two-button press is awkward — hold-LEFT (single button) is the
     * primary gesture now. */
    if (S.mode == 2) {
        if (now - S.t0_ms > CONFIRM_TIMEOUT_MS) {
            send_result(RES_TIMEOUT);
            S.active = false;
            return;
        }

        /* First tick of a new menu prompt — sync prev-button levels
         * with reality so the activation key still being held doesn't
         * fire a phantom edge. */
        if (!S.menu_btn_initialized) {
            S.menu_prev_left       = btnLeftDown;
            S.menu_prev_right      = btnRightDown;
            S.menu_btn_initialized = true;
            draw_menu(oled);
            return;
        }

        /* Edge-detect short presses and forward as nav events. There's
         * no hold gesture in menu mode any more — exit is the "Back"
         * item, a normal short-press selection. We only emit the press
         * edge (action=0); release (action=1) would be redundant and
         * the menu state machine only acts on action=0 anyway. */
        if (btnLeftDown && !S.menu_prev_left)   send_button_event(0, 0);
        if (btnRightDown && !S.menu_prev_right) send_button_event(1, 0);
        S.menu_prev_left  = btnLeftDown;
        S.menu_prev_right = btnRightDown;

        draw_menu(oled);
        return;
    }

    /* Display-only (claim-code, §10.4): no result, auto-dismiss when the
     * ESP32 stops refreshing it (it resends every ~60 s while UNCLAIMED). */
    if (S.confirm_secs == 0) {
        if (now - S.t0_ms > DISPLAY_TTL_MS) { S.active = false; return; }
        draw_claim_code(oled);
        return;
    }

    /* Fingerprint confirm (§10.1). */
    if (now - S.t0_ms > CONFIRM_TIMEOUT_MS) {
        send_result(RES_TIMEOUT);
        S.active = false;
        return;
    }

    bool both = btnLeftDown && btnRightDown;
    if (both) {
        S.release_miss = 0;
        if (S.hold_start_ms == 0) S.hold_start_ms = now;
        uint32_t held = now - S.hold_start_ms;
        uint32_t need = (uint32_t)S.confirm_secs * 1000u;
        if (held >= need) {
            ui_settings_beep(2000, 120);
            send_result(RES_CONFIRMED);
            S.active = false;
            return;
        }
        uint8_t left = (uint8_t)((need - held + 999) / 1000);
        draw_countdown(oled, left);
        return;
    }

    /* Not (or no longer) holding both. Ride out contact bounce for a few
     * samples before abandoning an in-progress hold. */
    if (S.hold_start_ms != 0) {
        if (++S.release_miss <= RELEASE_GRACE) {
            uint32_t held = now - S.hold_start_ms;
            uint32_t need = (uint32_t)S.confirm_secs * 1000u;
            uint8_t left = (uint8_t)((need - held + 999) / 1000);
            draw_countdown(oled, left);
            return;
        }
        S.hold_start_ms = 0;
    }
    /* §10.1 fingerprint mode uses a dedicated 2-page layout (uppercase
     * words + a textSize-2 CHK page). draw_fingerprint falls back to
     * draw_paged if the text isn't shaped "<words> chk:<hex>". */
    draw_fingerprint(oled);
}
