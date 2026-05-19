/*
 * Track 2 / ADR-0011 §10.1/§10.4 — RP2040 trust-anchor front end (P2-4c).
 * See trust_ui.h for the design + the Decision-C exception rationale.
 */
#include "trust_ui.h"
#include "wups_router.h"
#include "wups_proto.h"
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
    uint8_t  mode;          /* 0 = fingerprint confirm, 1 = claim-code   */
    uint8_t  confirm_secs;  /* 0 = display-only (no result expected)     */
    uint32_t nonce;
    uint8_t  req_seq;
    uint32_t t0_ms;         /* when this prompt became active            */
    uint32_t hold_start_ms; /* 0 = not currently holding both buttons    */
    uint8_t  release_miss;  /* consecutive non-both samples (debounce)   */
    uint16_t text_len;
    char     text[241];
} S;

bool trust_ui_active(void) { return S.active; }

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
    /* Bottom progress bar (multi-page only) so the user knows to wait
     * for the rest of the text to cycle around. */
    if (pages > 1)
        oled.fillRect(0, 31, (int)((page + 1) * 64 / pages), 1,
                      SSD1306_WHITE);
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
            w[c++] = (char)('1' + idx);
            w[c++] = ':';
            while (*p && *p != ' ' && c < SCR_COLS) w[c++] = *p++;
            while (*p && *p != ' ') ++p;     /* skip any overflow */
            w[c] = '\0';
            oled.setCursor(0, idx * 8);
            oled.print(w);
        }
    }
    /* 2-page progress bar (same affordance as draw_paged). */
    oled.fillRect(0, 31, (int)((page + 1) * 64 / 2), 1, SSD1306_WHITE);
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

void trust_ui_tick(Adafruit_SSD1306& oled, bool btnLeftDown,
                    bool btnRightDown)
{
    if (!S.active) return;
    uint32_t now = millis();

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
            tone(BUZZER_PIN, 2000, 120);
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
    draw_paged(oled);
}
