#include "modem.h"
#include "modem_radio_policy.h"
#include "modem_awake_policy.h"
#include "modem_support_diag.h"
#include "modem_diag_clock.h"
#include "mqtt.h"
#include "identity.h"
#include "backend_mode.h"
#include "http_backend.h"
#include "wups_link.h"
#include "cmdauth_arkiv.h"
#include "arkiv_tlm.h"
#include "arkiv_ws.h"
#include "arkiv_rpc.h"
#include "fw_ota.h"
#include "../../common/protocol.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_netif_sntp.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "esp_netif_net_stack.h" /* esp_netif_get_netif_impl */
#include "lwip/netif.h"          /* mib2_counters for net.status byte counts */
#include "lwip/sockets.h"        /* UDP DNS probes (uplink-watchdog classifier) */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define MODEM_TAG "modem"

/* W3P MODEM V1 (M.2) pinout — ESP32-S3FH4R2 ↔ SIM7080G UART1.
 * (Was LilyGo T-SIM7080G-S3: PWRKEY=41, DTR=42, RI=3, RX=4, TX=5. GPIO3 is a
 *  strapping pin and is left NC on the M.2 card.) DTR is held low to keep
 *  the AT UART awake; RI is routed but not handled here. */
#define MODEM_PWR_GPIO     1   /* ESP_PWRKEY    → SIM7080G PWRKEY */
#define MODEM_TX_GPIO      2   /* ESP_UART1_TXD : ESP TX → Modem RX */
#define MODEM_RX_GPIO      4   /* ESP_UART1_RXD : ESP RX ← Modem TX */
#define MODEM_DTR_GPIO     5   /* ESP_UART1_DTR -> SIM7080G via TXB0108 */
#define MODEM_RI_GPIO      6   /* ESP_UART1_RI  (not driven in SW) */

#define MODEM_UART        UART_NUM_1
#define MODEM_BAUD        115200

/* Post-power-on settle before AT is reliable. SIM7080G Ton(uart) = 1.8 s
 * (HW Design V1.05, Table 9); we keep a conservative 5 s so the SIM/AT stack
 * is fully up, and esp_modem's sync (20×500 ms) absorbs any extra cold-start. */
#define MODEM_BOOT_DELAY_MS  5000

/* --- Bench bring-up diagnostic (set 0 for normal/production builds) -------
 * When 1: skip esp_modem/PPP entirely and run a raw UART probe that writes
 * "AT\r\n" on UART1 (TX=GPIO2 / RX=GPIO4) and hexdumps any reply, and NEVER
 * power-cycles the modem — so a healthy, network-searching modem stays ON for
 * measurement. Used to validate the TXB0108 level-translator power
 * (LTE_VDD_EXT) bodge on the W3P MODEM V1 card:
 *   RX TIMEOUT / 0 bytes -> UART path dead (translator unpowered / wiring)
 *   "AT\r\r\nOK\r\n"      -> path alive, translator working. */
#define MODEM_UART_DIAG   0

/* Select the fleet APN once after reading the SIM identity. Registration
 * failures must not rotate away from the SIM's configured APN. The first
 * DCE is created before ICCID is available, so its cached PDP context must
 * be synchronized with this selection before RF resume and PPP setup. */
static const char *s_apn = "sensor.net";
static bool s_apn_seeded = false;

#define EVT_GOT_IP        BIT0
#define EVT_LOST_IP       BIT1
#define EVT_PPP_FAIL      BIT2
#define EVT_MQTT_DOWN     BIT3   /* 0.8.7: early supervisor wake on MQTT drop */
#define EVT_PPP_CHANGED   (EVT_GOT_IP | EVT_LOST_IP | EVT_PPP_FAIL)

/* Supervisor backoff: start short, cap at 60s. After this many consecutive
 * bring-up failures we give the modem a full PWRKEY power cycle, since the
 * radio side can wedge in ways AT-only recovery can't fix. */
#define PPP_BACKOFF_MIN_MS       1000
#define PPP_BACKOFF_MAX_MS      60000
#define PPP_FAILS_BEFORE_PWRCYCLE   5
#define PPP_GOT_IP_TIMEOUT_MS   60000

/* PWRKEY hold time. 1.3 s satisfies both the SIM7080G power-ON (>= 1.0 s)
 * and power-OFF (>= 1.2 s) hold specs — the old 1.0 s pulse could boot a
 * cold modem but was too short to power a running one OFF, which silently
 * defeated every "power-cycle" escalation (the pulse was a no-op). */
#define MODEM_PWRKEY_HOLD_MS     1300

/* How long to keep probing AT after a PWRKEY on-pulse before concluding the
 * modem is really silent. SIM7080G boot-to-AT-ready runs up to ~12-15 s on
 * some firmware revisions; probing for only a few seconds and then pulsing
 * PWRKEY "again" powers the freshly booting modem straight back off (seen
 * on bench 2026-07-22). */
#define MODEM_BOOT_PROBE_MS      25000

/* How long a booted modem gets to register to the network (CEREG/CGREG)
 * before the bring-up round is failed. Cat-M network search after a cold
 * boot routinely runs tens of seconds. */
#define MODEM_REG_WAIT_MS        90000

/* While bring-up keeps failing on registration timeout, power-cycle only
 * after this many consecutive timeouts (instead of the fast 2/5 threshold):
 * each cycle restarts the search from scratch. */
#define REG_TIMEOUT_PWRCYCLE_EVERY   5
/* After the off-pulse, let the modem finish its power-down sequence
 * (Toff ~1.8 s per HW design) before pulsing it back on. */
#define MODEM_PWROFF_SETTLE_MS   5000

/* Post-PPP uplink watchdog (field incident 2026-07: "zombie PDP" — the PDP
 * context looked active network-side but data was black-holed for 33 min and
 * the firmware had no way out; only a manual network-side "Reset connection"
 * recovered it). While PPP is up we tick every 30 s: poll signal quality
 * (CMUX sessions), emit net.status, and check that the ACTIVE backend's
 * uplink is actually alive. If PPP holds an IP but the uplink hasn't been
 * healthy for UPLINK_DEAD_SECS, tear down and escalate: first trip resets
 * the module (AT+CFUN=1,1 = fresh network attach + fresh PDP, the
 * device-side equivalent of that manual reset); if the uplink never became
 * healthy before the next trip, full PWRKEY power-cycle instead. */
#define PPP_SUPERVISE_TICK_MS   30000
#define UPLINK_DEAD_SECS          300
#define HTTP_UPLINK_FRESH_SECS     90   /* 3 missed 30 s POST cadences = unhealthy */
#define ARKIV_UPLINK_FRESH_SECS   120   /* 4 missed 30 s telemetry writes = unhealthy */

/* Belt-and-braces re-sends of the OLED alert CLEAR after recovery — the
 * ui.display_msg frame is unACKed on a no-flow-control UART, so a single
 * lost clear used to latch the "NO NETWORK" banner forever. */
#define ALERT_CLEAR_RESENDS         3
#define ALERT_CLEAR_RESEND_S       60

/* net.status telemetry cadence (CMUX sessions only — the DATA fallback has
 * no AT channel to poll). Emit every NET_STATUS_EMIT_PERIOD_S, or right
 * away when the state changes or rssi moves by more than the delta. */
#define NET_STATUS_EMIT_PERIOD_S   60
#define NET_STATUS_RSSI_DELTA_DB    3

/* net.status `state` codes (see wups_net_status_v1_t in protocol.h:
 * 0=off 1=init 2=net_attach 3=ppp_up 4=mqtt_up 5=err). State 4 is used
 * generically for "active backend uplink is up" in all three modes. */
#define NET_STATE_PPP_UP  3
#define NET_STATE_MQTT_UP 4

/* Consecutive CMUX-entry failures (each from a freshly booted, network-
 * REGISTERED modem — an unregistered one can't dial and is counted as a
 * normal bring-up failure instead) before this boot falls back to plain DATA
 * mode — the uplink must never depend on CMUX. RSSI polling is silently
 * disabled in fallback. One retry is re-armed every CMUX_FALLBACK_RETRY_S so
 * a transient double-failure can't cost an always-on unit months of
 * telemetry; a genuinely CMUX-broken modem pays one extra reset per day. */
#define CMUX_ENTRY_FAILS_MAX  2
#define CMUX_FALLBACK_RETRY_S  (24 * 3600)

/* Backend-outage hold (probes OK, uplink dead): after this long raise the
 * OLED alert anyway — WITHOUT modem resets — so a genuinely wedged client
 * is never silent forever. Long on purpose: routine backend redeploys must
 * not beep at customers. */
#define UPLINK_HOLD_ALERT_S  (6 * UPLINK_DEAD_SECS)

/* 0.8.7 — independent internet probe used ONLY to classify an
 * uplink-watchdog trip: a dead backend behind a healthy internet must not
 * put the fleet into a modem-reset cycle (2026-07-21 zombie-PDP lesson in
 * reverse — resets only help when the PDP data path itself is broken). A
 * fresh OK verdict is cached briefly so a long outage probes once per
 * cache window, not every tick. */
#define INET_PROBE_TIMEOUT_MS 4000
#define INET_PROBE_CACHE_S     120

static EventGroupHandle_t s_modem_evt;
static esp_netif_t       *s_ppp_netif;
static esp_modem_dce_t   *s_dce;
static bool               s_iccid_known;       /* set true once AT+CCID populated identity */
static bool               s_initial_connectivity_checked;

void modem_notify_mqtt_down(void)
{
    if (s_modem_evt) {
        xEventGroupSetBits(s_modem_evt, EVT_MQTT_DOWN);
    }
}

/* PPP lifecycle state: event bits are wakeups, never an unordered history
 * of success/failure. The default event loop serializes the two callbacks;
 * this short lock also orders their observations against supervisor actions.
 * No SDK, logging or FreeRTOS event-group call is made while holding it.
 * attempt/sequence identify LOCAL observations, not SDK session identifiers:
 * esp-netif keeps one netif and its LOST_IP timer across DCE recreation. */
typedef enum {
    PPP_PREPARING = 0,  /* no new dial yet; previous-session events ignored */
    PPP_STARTING,       /* dialing, awaiting the first address */
    PPP_UP,
    PPP_DOWN,
    PPP_STOPPING,       /* intentional teardown has committed */
} ppp_phase_t;

typedef enum {
    PPP_OBS_NONE = 0,
    PPP_OBS_GOT_IP,
    PPP_OBS_LOST_IP,
    PPP_OBS_ERROR,
} ppp_observation_t;

typedef struct {
    ppp_phase_t phase;
    ppp_observation_t observation;
    int32_t error;
    uint32_t attempt;
    uint32_t sequence;
    bool had_ip;
} ppp_event_state_t;

static portMUX_TYPE s_ppp_event_lock = portMUX_INITIALIZER_UNLOCKED;
static ppp_event_state_t s_ppp_state = {.phase = PPP_STOPPING};
/* END PPP lifecycle state */

static ppp_event_state_t ppp_events_snapshot(void)
{
    portENTER_CRITICAL(&s_ppp_event_lock);
    ppp_event_state_t state = s_ppp_state;
    portEXIT_CRITICAL(&s_ppp_event_lock);
    return state;
}

/* Called from OTA and diagnostics as well as the supervisor. */
bool modem_ppp_is_up(void)
{
    return ppp_events_snapshot().phase == PPP_UP;
}

static void ppp_events_begin_attempt(void)
{
    /* Clear notifications before resetting the state. A late wakeup from a
     * previous callback is harmless: decisions always read the state below. */
    xEventGroupClearBits(s_modem_evt, EVT_PPP_CHANGED);
    portENTER_CRITICAL(&s_ppp_event_lock);
    s_ppp_state.attempt++;
    s_ppp_state.sequence++;
    s_ppp_state.phase = PPP_PREPARING;
    s_ppp_state.observation = PPP_OBS_NONE;
    s_ppp_state.error = 0;
    s_ppp_state.had_ip = false;
    portEXIT_CRITICAL(&s_ppp_event_lock);
}

static void ppp_events_start_dial(void)
{
    /* Before either CMUX or DATA can start PPP, never after GOT_IP. Events
     * during registration belonged to a netif with no new PPP session. */
    portENTER_CRITICAL(&s_ppp_event_lock);
    s_ppp_state.phase = PPP_STARTING;
    s_ppp_state.observation = PPP_OBS_NONE;
    s_ppp_state.error = 0;
    s_ppp_state.had_ip = false;
    s_ppp_state.sequence++;
    portEXIT_CRITICAL(&s_ppp_event_lock);
}

static void ppp_events_begin_stop(void)
{
    portENTER_CRITICAL(&s_ppp_event_lock);
    s_ppp_state.phase = PPP_STOPPING;
    s_ppp_state.sequence++;
    portEXIT_CRITICAL(&s_ppp_event_lock);
}

static bool ppp_events_take_loss(void)
{
    /* Recheck and commit together. A newer GOT before this decision wins;
     * a GOT after it cannot resurrect a DCE already selected for teardown. */
    portENTER_CRITICAL(&s_ppp_event_lock);
    bool lost = s_ppp_state.phase == PPP_DOWN;
    if (lost) {
        s_ppp_state.phase = PPP_STOPPING;
        s_ppp_state.sequence++;
    }
    portEXIT_CRITICAL(&s_ppp_event_lock);
    return lost;
}

static bool ppp_events_observe(ppp_observation_t observation, int32_t error)
{
    portENTER_CRITICAL(&s_ppp_event_lock);
    ppp_phase_t before = s_ppp_state.phase;
    bool accepted = before != PPP_STOPPING && before != PPP_PREPARING;
    s_ppp_state.sequence++;
    if (accepted) {
        s_ppp_state.observation = observation;
        s_ppp_state.error = error;
        if (observation == PPP_OBS_GOT_IP) {
            s_ppp_state.phase = PPP_UP;
            s_ppp_state.had_ip = true;
        } else if (observation == PPP_OBS_ERROR || s_ppp_state.had_ip) {
            s_ppp_state.phase = PPP_DOWN;
        }
        /* LOST before this attempt's first GOT is not evidence that a fresh
         * connection failed. The previous netif's 120s timer can fire even
         * during the new dial. Keep waiting for GOT or the bounded timeout.
         * Do not let such LOST erase a preceding explicit terminal error. */
    }
    ppp_event_state_t state = s_ppp_state;
    portEXIT_CRITICAL(&s_ppp_event_lock);
    ESP_LOGI(MODEM_TAG,
             "PPP event attempt=%u seq=%u obs=%d error=%ld phase=%d->%d %s",
             (unsigned)state.attempt, (unsigned)state.sequence,
             (int)observation, (long)error, (int)before, (int)state.phase,
             accepted ? "applied" : "ignored outside active dial/session");
    if (accepted) {
        EventBits_t wake = observation == PPP_OBS_GOT_IP ? EVT_GOT_IP :
                          observation == PPP_OBS_LOST_IP ? EVT_LOST_IP : EVT_PPP_FAIL;
        xEventGroupSetBits(s_modem_evt, wake);
    }
    return accepted;
}

static ppp_phase_t ppp_events_wait_for_link(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        const int64_t remaining = deadline - esp_timer_get_time();
        portENTER_CRITICAL(&s_ppp_event_lock);
        ppp_phase_t phase = s_ppp_state.phase;
        if (phase == PPP_DOWN || (remaining <= 0 && phase != PPP_UP)) {
            s_ppp_state.phase = PPP_STOPPING;
            s_ppp_state.sequence++;
        }
        portEXIT_CRITICAL(&s_ppp_event_lock);
        if (phase == PPP_UP || phase == PPP_DOWN) return phase;
        if (remaining <= 0) return PPP_STARTING; /* timeout, no IP */
        TickType_t wait = pdMS_TO_TICKS((uint32_t)((remaining + 999) / 1000));
        if (wait == 0) wait = 1;
        (void)xEventGroupWaitBits(s_modem_evt, EVT_PPP_CHANGED,
                                  pdTRUE, pdFALSE, wait);
    }
}

/* CMUX session state. `s_cmux_active` = the current DCE runs PPP + AT
 * multiplexed (so we may poll AT while data flows). `s_cmux_dirty` = a CMUX
 * entry attempt failed and the modem's framing state is unknown — teardown
 * must PWRKEY-cycle it back to a known-fresh boot. `s_cmux_entry_fails`
 * counts consecutive failed entries (see CMUX_ENTRY_FAILS_MAX);
 * `s_cmux_fallback_since_s` timestamps the fallback for the daily retry. */
static bool     s_cmux_active;
static bool     s_cmux_dirty;
static int      s_cmux_entry_fails;
static uint32_t s_cmux_fallback_since_s;

/* Uplink-watchdog escalation: consecutive trips with no healthy uplink in
 * between. 1st trip → AT+CFUN=1,1 module reset; 2nd+ → PWRKEY power-cycle.
 * Reset to 0 whenever the uplink is seen healthy. */
static int s_uplink_trips;

/* Bring-up CSQ (converted to dBm; 0 = unknown) — seeds the first net.status
 * emitted right after GOT_IP, before the supervision loop's first poll. */
static int8_t s_bringup_rssi_dbm;

/* What ppp_teardown_dce() must do besides destroying the DCE. NORMAL still
 * module-resets a CMUX session (fresh-boot guarantee, see below); the other
 * two are the uplink watchdog escalation ladder. */
typedef enum {
    TEARDOWN_NORMAL = 0,    /* link dropped by itself / bring-up failure    */
    TEARDOWN_MODULE_RESET,  /* watchdog trip #1 — AT+CFUN=1,1               */
    TEARDOWN_PWRCYCLE,      /* watchdog trip #2+ — full PWRKEY power-cycle  */
} teardown_action_t;

static inline uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* --- degraded-state surfacing -------------------------------------------
 * Cellular bring-up already self-heals (supervisor: AT retry → backoff →
 * modem PWRKEY power-cycle). When it stays broken ACROSS repeated
 * power-cycles we raise a visible alert on the RP2040 (OLED + buzzer) via
 * ui.display_msg, naming the stage that failed, and clear it on the next
 * successful IP. We never reset the ESP32 — a marginal SIM contact / weak
 * antenna is not fixable by any reset, so we alert the human and keep
 * retrying slowly (auto-recovers when the contact/signal comes back). */
/* Surface the visible alert after this many consecutive bring-up failures
 * (~a minute at the per-attempt timings below) — fast enough that a no-SIM /
 * no-signal unit tells the user quickly, high enough that a 1-2 s transient
 * doesn't false-alarm. Modem PWRKEY power-cycle recovery runs in parallel;
 * a successful IP clears the alert. */
#define MODEM_FAILS_BEFORE_ALERT  4

typedef enum {
    MODEM_FAIL_NONE = 0,
    MODEM_FAIL_AT,     /* modem never answered AT                    */
    MODEM_FAIL_RADIO,   /* strict LTE-M / B3+B20 policy not verified */
    MODEM_FAIL_AWAKE,   /* CSCLK / PSM / CAT-M eDRX off not verified */
    MODEM_FAIL_SIM,    /* AT ok, but no SIM/ICCID (CPIN/CCID failed) */
    MODEM_FAIL_NET,    /* SIM ok, but no registration / no PPP IP    */
    MODEM_FAIL_UPLINK, /* 0.8.7: internet fine, backend unreachable  */
} modem_fail_t;

static modem_fail_t s_fail_stage;
static int          s_fails_since_ok;   /* consecutive bring-up failures; 0 on IP */
static bool         s_alert_active;
static int          s_alert_clear_pending;  /* recovery clears still to re-send */

static const char *modem_fail_msg(modem_fail_t f)
{
    switch (f) {
    case MODEM_FAIL_RADIO:  return "RADIO CONFIG";
    case MODEM_FAIL_AWAKE:  return "MODEM CONFIG";
    case MODEM_FAIL_SIM:    return "SIM ERROR";
    case MODEM_FAIL_NET:    return "NO NETWORK";
    case MODEM_FAIL_UPLINK: return "NO UPLINK";
    case MODEM_FAIL_AT:  /* fallthrough */
    default:             return "MODEM FAIL";
    }
}

/* Push a short alert string to the RP2040 OLED/buzzer (ui.display_msg).
 * text_len==0 is the CLEAR sentinel. Sent over UART2 (wups_link), which is
 * a separate link from the modem UART1 and is mutex-protected, so calling
 * this from the supervisor task is safe. */
static void modem_ui_alert(const char *msg)
{
    /* wups_ui_display_msg_v1_hdr_t { u8 ver=1, u8 line, u8 text_len, u8 rsv } + text */
    uint8_t buf[4 + 24];
    size_t tl = msg ? strlen(msg) : 0;
    if (tl > 24) tl = 24;
    buf[0] = 1; buf[1] = 0; buf[2] = (uint8_t)tl; buf[3] = 0;
    if (tl) memcpy(buf + 4, msg, tl);
    wups_link_send(WUPS_ADDR_RP2040, WUPS_CLASS_UI, WUPS_OP_UI_DISPLAY_MSG,
                   WUPS_FLAG_REQ, buf, (uint16_t)(4 + tl));
}

static void modem_ui_alert_clear(void) { modem_ui_alert(NULL); }

/* --- net.status telemetry ------------------------------------------------ */

/* Last emitted net.status snapshot — drives the change-triggered emits. */
static int8_t   s_ns_last_rssi;
static uint8_t  s_ns_last_state;
static uint32_t s_ns_last_emit_s;

/* CSQ 0..31 → dBm (-113 + 2*csq); 99 / out of range → 0 (= unknown on the
 * wire, matching the "leave 0 if not known" convention of the struct). */
static int8_t csq_to_dbm(int csq)
{
    if (csq < 0 || csq > 31) return 0;
    return (int8_t)(-113 + 2 * csq);
}

/* Full AT replies: esp_modem_at() returns only the last non-terminal line
 * (and caps it at 128 bytes), losing CAT-M in the two-line CBANDCFG reply.
 * This collector is owned only by ppp_sup. esp_modem_command() serializes
 * commands and detaches its callback under line_lock before returning.
 * The inflatable DTE option is required for cumulative replies also in CMUX.
 * Logging is restricted to these radio/status commands: never credentials. */
#if !CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED
#error "Full modem diagnostics require cumulative CMUX response buffering"
#endif
static struct {
    char data[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    bool invalid;
} s_radio_reply;

static esp_err_t radio_reply_cb(uint8_t *data, size_t length)
{
    if (!data) {
        s_radio_reply.invalid = length != 0;
        return length ? ESP_FAIL : ESP_ERR_NOT_FINISHED;
    }
    if (length >= sizeof(s_radio_reply.data) || memchr(data, '\0', length)) {
        s_radio_reply.invalid = true;
        return ESP_FAIL;
    }
    for (size_t i = 0; i < length; ++i) {
        if ((data[i] < 32 && data[i] != '\r' && data[i] != '\n' && data[i] != '\t') ||
            data[i] > 126) {
            s_radio_reply.invalid = true;
            return ESP_FAIL;
        }
    }
    memcpy(s_radio_reply.data, data, length);
    s_radio_reply.data[length] = '\0';
    s_radio_reply.length = length;
    modem_radio_response_status_t status =
        modem_radio_response_status(s_radio_reply.data, length);
    if (status == MODEM_RADIO_RESPONSE_ERROR) return ESP_FAIL;
    return status == MODEM_RADIO_RESPONSE_OK ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

static bool radio_at(void *context, const char *command, char *response,
                     size_t capacity, unsigned timeout_ms)
{
    (void)context;
    char wire[80];
    int n = snprintf(wire, sizeof(wire), "%s\r", command);
    if (!response || capacity == 0 || n < 0 || (size_t)n >= sizeof(wire)) return false;
    response[0] = '\0';
    memset(&s_radio_reply, 0, sizeof(s_radio_reply));
    modem_diag_clock_log(command);
    esp_err_t rc = esp_modem_command(s_dce, wire, radio_reply_cb, timeout_ms);
    bool complete = rc == ESP_OK && !s_radio_reply.invalid &&
        s_radio_reply.length < capacity &&
        modem_radio_response_status(s_radio_reply.data, s_radio_reply.length) ==
            MODEM_RADIO_RESPONSE_OK;
    ESP_LOGI(MODEM_TAG, "AT reply begin [%s] rc=%s complete=%d bytes=%u",
             command, esp_err_to_name(rc), complete, (unsigned)s_radio_reply.length);
    /* One timestamped line per response line. Preserve every field, remove
     * only CR/LF framing. Reject binary replies instead of injecting controls. */
    if (!s_radio_reply.invalid) {
        const char *p = s_radio_reply.data;
        while (*p) {
            size_t len = strcspn(p, "\r\n");
            if (len) ESP_LOGI(MODEM_TAG, "AT [%s] %.*s", command, (int)len, p);
            p += len;
            while (*p == '\r' || *p == '\n') ++p;
        }
    }
    ESP_LOGI(MODEM_TAG, "AT reply end [%s]%s", command,
             s_radio_reply.invalid ? " INVALID/OVERSIZE" : "");
    if (complete) memcpy(response, s_radio_reply.data, s_radio_reply.length + 1);
    return complete;
}

/* Read-only support diagnostics share the supervisor's AT ownership. Check
 * again before every command: OTA or PPP loss can arrive during a snapshot.
 * Before PPP there is no data stream; live sessions require the CMUX channel. */
static bool modem_support_diag_ready(void *context)
{
    const bool live_ppp = *(const bool *)context;
    if (!s_dce || fw_ota_in_progress()) return false;
    if (!live_ppp) return true;
    return s_cmux_active && modem_ppp_is_up();
}

static void modem_support_snapshot(bool live_ppp)
{
    modem_support_diag_run(s_dce, modem_support_diag_ready, &live_ppp);
}

/* Network-provided values can lag registration. Retry only the two
 * read-only checks; never rewrite settings or reset RF between attempts.
 * At most 3 * (2 * 3 s) + 2 * 1 s waiting, then the usual paced retry. */
static bool modem_awake_check_before_ppp(void)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (modem_awake_verify_active(radio_at, NULL)) {
            ESP_LOGI(MODEM_TAG, "awake policy verified: CSCLK=0 PSM=0 CAT-M eDRX=0");
            return true;
        }
        ESP_LOGW(MODEM_TAG, "awake runtime readback not confirmed (%u/3)", attempt + 1);
        if (attempt + 1 < 3) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    char reply[MODEM_RADIO_RESPONSE_CAPACITY];
    (void)radio_at(NULL, "AT+CFUN=4", reply, sizeof(reply), 10000);
    ESP_LOGE(MODEM_TAG, "awake policy runtime state unknown/enabled; PPP blocked, RF-off requested");
    s_fail_stage = MODEM_FAIL_AWAKE;
    return false;
}

static void modem_network_snapshot(const char *reason)
{
    char reply[MODEM_RADIO_RESPONSE_CAPACITY];
    ESP_LOGI(MODEM_TAG, "network snapshot: %s", reason);
    (void)radio_at(NULL, "AT+CEREG?", reply, sizeof(reply), 3000);
    (void)radio_at(NULL, "AT+COPS?", reply, sizeof(reply), 3000);
    (void)radio_at(NULL, "AT+CPSI?", reply, sizeof(reply), 3000);
}

/* Optional RSRP/RSRQ from AT+CPSI? (Cat-M form ends ...,<RSRQ>,<RSRP>,
 * <RSSI>,<RSSNR>). Parsed defensively: SIM7080G FW revisions differ on
 * whether RSRP/RSRQ come in dB(m) or tenths, so out-of-range raw values are
 * re-tried as tenths and anything still implausible is dropped silently.
 * Outputs are left untouched (0 = unknown) on any surprise. */
static void poll_cpsi(int8_t *rsrp_out, int8_t *rsrq_out)
{
    char out[MODEM_RADIO_RESPONSE_CAPACITY] = {0};
    if (!radio_at(NULL, "AT+CPSI?", out, sizeof(out), 3000)) return;
    /* Full replies can include unrelated comma-separated URCs. Restrict the
     * signal parser to the CPSI line before looking at its numeric tail. */
    char *cpsi = NULL;
    for (char *line = out; *line;) {
        size_t len = strcspn(line, "\r\n");
        if (strncmp(line, "+CPSI:", 6) == 0) {
            line[len] = '\0';
            cpsi = line;
            break;
        }
        line += len;
        while (*line == '\r' || *line == '\n') ++line;
    }
    if (!cpsi || !strstr(cpsi, "LTE")) return; /* no service / other RAT */

    /* Collect the integer value of every comma field, keep the tail. */
    long vals[20];
    int  n = 0;
    for (char *p = strchr(cpsi, ','); p && n < 20; p = strchr(p + 1, ',')) {
        vals[n++] = strtol(p + 1, NULL, 10);
    }
    if (n < 4) return;
    long rsrq = vals[n - 4];   /* ...,<RSRQ>,<RSRP>,<RSSI>,<RSSNR> */
    long rsrp = vals[n - 3];
    if (rsrp <= -440 && rsrp >= -1560) rsrp /= 10;   /* tenths variant */
    if (rsrq <= -35  && rsrq >= -340)  rsrq /= 10;
    if (rsrp <= -44 && rsrp >= -128) *rsrp_out = (int8_t)rsrp;
    if (rsrq <= -3  && rsrq >= -34)  *rsrq_out = (int8_t)rsrq;
}

/* Build a FULL framed WUPS net.status EVENT (byte-identical to the frames
 * the RP2040 relays via net.publish) and hand it to the active backend.
 * Self-emitted frames never pass wups_link's handle_net_publish, so the
 * per-backend routing that lives there is mirrored here explicitly. */
static void emit_net_status(uint8_t state, int8_t rssi_dbm,
                            int8_t rsrp, int8_t rsrq)
{
    wups_net_status_v2_t st = {0};
    st.version  = 2;
    st.state    = state;
    st.rssi_dBm = rssi_dbm;
    st.rsrp_dBm = rsrp;
    st.rsrq_dB  = rsrq;
    /* v2 tail — ESP32<->RP2040 sys-link health (wups_link.h). Lets the
     * panel tell a dead inter-MCU link from a quiet one (2026-08-20). */
    st.sys_frames_rx = wups_link_frames_rx();
    st.sys_resync    = wups_link_resync_count();
    uint32_t age_s   = wups_link_frame_age_s();
    st.sys_link_age_s = (age_s > 0xFFFFu) ? 0xFFFFu : (uint16_t)age_s;
    esp_netif_ip_info_t ip;
    if (s_ppp_netif && esp_netif_get_ip_info(s_ppp_netif, &ip) == ESP_OK) {
        st.ip_addr = ip.ip.addr;            /* already network byte order */
    }
#if MIB2_STATS
    /* Per-netif octet counters maintained by lwIP PPP (ifinoctets in ppp.c,
     * ifoutoctets in pppos.c) — cumulative for the lifetime of the esp_netif,
     * i.e. since boot, surviving PPP re-dials. TX includes PPP/HDLC framing. */
    struct netif *lwip_netif =
        s_ppp_netif ? (struct netif *)esp_netif_get_netif_impl(s_ppp_netif) : NULL;
    if (lwip_netif) {
        st.bytes_tx = lwip_netif->mib2_counters.ifoutoctets;
        st.bytes_rx = lwip_netif->mib2_counters.ifinoctets;
    }
#endif
    /* errors left 0 — not tracked on this side. */

    uint8_t frame[WUPS_FRAMING_BYTES + sizeof(st)];
    uint16_t flen = wups_link_render_frame(frame, sizeof(frame),
                                           WUPS_ADDR_BROADCAST,
                                           WUPS_CLASS_NET, WUPS_OP_NET_STATUS,
                                           WUPS_FLAG_EVENT, &st, sizeof(st));
    if (!flen) return;

    switch (backend_mode_get()) {
    case WUPS_BACKEND_MODE_MQTT:
        /* The owner copies this freshly generated existing frame, including
         * before topic initialization, and selects occasional QoS 1 probes. */
        mqtt_publish_net_status(frame, flen);
        break;
    case WUPS_BACKEND_MODE_HTTP:
        http_backend_observe_telemetry_frame(frame, flen);
        break;
    case WUPS_BACKEND_MODE_ARKIV:
        /* Same gate as wups_link's telemetry snoop — the emit task idles
         * until claimed anyway, this just keeps the cache semantics equal. */
        if (cmdauth_arkiv_claim_state() == ARKIV_CLAIMED) {
            arkiv_tlm_observe_frame(frame, flen);
        }
        break;
    default:
        break;
    }

    s_ns_last_rssi   = rssi_dbm;
    s_ns_last_state  = state;
    s_ns_last_emit_s = now_s();
}

esp_err_t modem_init(void)
{
    /* Preload LOW before enabling either output: PWRKEY stays released
     * through Q501; DTR wakes a UART left in CSCLK=1 by an earlier image.
     * Keep DTR LOW across modem resets/redials. It does not wake PSM. */
    esp_err_t err = gpio_set_level(MODEM_PWR_GPIO, 0);
    if (err != ESP_OK) return err;
    err = gpio_set_level(MODEM_DTR_GPIO, 0);
    if (err != ESP_OK) return err;
    gpio_config_t pin_cfg = {
        .pin_bit_mask = (1ULL << MODEM_PWR_GPIO) | (1ULL << MODEM_DTR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&pin_cfg);
    if (err != ESP_OK) return err;
    ESP_LOGI(MODEM_TAG, "GPIO%d PWRKEY released, GPIO%d DTR held LOW; UART owned by esp_modem",
             MODEM_PWR_GPIO, MODEM_DTR_GPIO);
    return ESP_OK;
}

/* Single PWRKEY toggle (through the inverting transistor). PWRKEY is a
 * level-edge toggle, not an on-only signal — the same pulse boots an off
 * modem and powers a running one off (hold >= 1.2 s). */
static void pwrkey_pulse(void)
{
    gpio_set_level(MODEM_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_HOLD_MS));
    gpio_set_level(MODEM_PWR_GPIO, 0);
}

esp_err_t modem_power_on(void)
{
    /* Pulse sequence per LilyGo example & SIM7080G hardware design. */
    ESP_LOGI(MODEM_TAG, "pulsing PWRKEY (GPIO%d) for %d ms to power modem on...",
             MODEM_PWR_GPIO, MODEM_PWRKEY_HOLD_MS);
    pwrkey_pulse();
    ESP_LOGI(MODEM_TAG, "PWRKEY released — modem boot in progress");
    return ESP_OK;
}

/* Probe AT on a temporary raw UART. Returns true if the modem answered "OK".
 * The UART driver is removed afterwards so esp_modem can claim UART1 cleanly.
 * NB: a modem left in CMUX framing (ESP soft-reboot mid-session) is alive
 * but will NOT answer a plain AT — callers must treat "silent" as "off OR
 * unreachable", never as proof of power-off. */
static void probe_escape_data_mode(void)
{
    /* If the modem is stuck in PPP/data mode (the ESP rebooted mid-session —
     * the modem keeps its own data session up), pull it back to command mode
     * with the "+++" escape: ~1 s of UART idle, "+++", ~1 s idle. Harmless when
     * the modem is off or already in command mode. (Does NOT rescue a modem
     * left in CMUX framing — see the note above.) */
    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_write_bytes(MODEM_UART, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_flush_input(MODEM_UART);
}

/* Probe for up to `window_ms` after the initial "+++" escape. A modem that is
 * still booting needs the long window (see MODEM_BOOT_PROBE_MS); long windows
 * re-run the escape once at half-time in case the modem finished booting into
 * a resumed data session after the first escape already went by. */
static bool modem_probe_at_alive_ms(uint32_t window_ms)
{
    uart_config_t cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(MODEM_UART, 512, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    uart_param_config(MODEM_UART, &cfg);
    uart_set_pin(MODEM_UART, MODEM_TX_GPIO, MODEM_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    probe_escape_data_mode();

    uint8_t buf[64];
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)window_ms * 1000;
    const int64_t half_us     = esp_timer_get_time() + (int64_t)window_ms * 500;
    bool re_escaped = (window_ms < 10000);  /* only long windows re-escape */
    bool alive = false;
    for (;;) {
        uart_flush_input(MODEM_UART);
        uart_write_bytes(MODEM_UART, "AT\r\n", 4);
        int n = uart_read_bytes(MODEM_UART, buf, sizeof(buf) - 1, pdMS_TO_TICKS(300));
        if (n > 0) {
            buf[n] = '\0';
            if (strstr((char *)buf, "OK")) alive = true;
        }
        if (alive || esp_timer_get_time() >= deadline_us) break;
        if (!re_escaped && esp_timer_get_time() >= half_us) {
            probe_escape_data_mode();
            re_escaped = true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    uart_driver_delete(MODEM_UART);
    return alive;
}

static bool modem_probe_at_alive(void)
{
    return modem_probe_at_alive_ms(1500);   /* ≈ the classic 5×300 ms sweep */
}

/* Full PWRKEY power-cycle that converges on a freshly BOOTED modem no matter
 * what state it started in. `known_running` short-circuits the AT probe for
 * callers that know the modem is alive (PPP was up moments ago) — necessary
 * because a modem in CMUX framing is alive yet silent to a plain-AT probe. */
static void modem_power_cycle(bool known_running)
{
    if (known_running || modem_probe_at_alive()) {
        ESP_LOGW(MODEM_TAG, "power-cycling modem: PWRKEY off-pulse, settle, on-pulse");
        pwrkey_pulse();                                  /* >= 1.2 s hold = power off */
        vTaskDelay(pdMS_TO_TICKS(MODEM_PWROFF_SETTLE_MS));
        modem_power_on();
        vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_DELAY_MS));
        return;
    }
    /* Silent: powered off, or running but unreachable over plain AT (left in
     * CMUX framing). PWRKEY is a blind toggle, so pulse once and re-probe —
     * an off modem is now booting and answers; a CMUX-wedged one just powered
     * off and needs the second pulse to boot fresh. The re-probe MUST be
     * patient (MODEM_BOOT_PROBE_MS): a short probe declares a still-booting
     * modem "silent" and the second pulse then powers it straight back off. */
    modem_power_on();
    if (modem_probe_at_alive_ms(MODEM_BOOT_PROBE_MS)) {
        return;
    }
    ESP_LOGW(MODEM_TAG, "still silent %d s after PWRKEY pulse — pulsing again "
                        "(modem was likely ON in CMUX framing and is off now)",
             MODEM_BOOT_PROBE_MS / 1000);
    modem_power_on();
    if (!modem_probe_at_alive_ms(MODEM_BOOT_PROBE_MS)) {
        ESP_LOGE(MODEM_TAG, "modem still silent after a full PWRKEY cycle — "
                            "possible hardware fault; leaving recovery to the "
                            "supervisor retry ladder");
    }
}

void modem_ensure_on(void)
{
    /* The modem's VBAT (PP3800_SYS) is always-on and independent of the ESP,
     * so the modem stays powered across ESP resets/reflashes. A blind PWRKEY
     * pulse here would toggle a healthy, running modem OFF (PWRKEY is a
     * level-edge toggle, not an on-only signal). So probe AT first; only
     * touch PWRKEY when the modem is actually silent. */
    if (modem_probe_at_alive()) {
        ESP_LOGI(MODEM_TAG, "modem already powered (AT answered) — skipping PWRKEY pulse");
        return;
    }
    /* Silent = off, or on but unreachable (an ESP soft-reboot can leave it
     * in CMUX framing). modem_power_cycle's silent branch converges on a
     * fresh boot in both cases. */
    ESP_LOGI(MODEM_TAG, "modem silent — powering on via PWRKEY");
    modem_power_cycle(false);
}

/* --- esp_netif PPP event hooks ------------------------------------------- */

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT ||
        (id != IP_EVENT_PPP_GOT_IP && id != IP_EVENT_PPP_LOST_IP) ||
        !data || !s_ppp_netif) return;
    /* Both PPP IP events use ip_event_got_ip_t, including the delayed
     * esp-netif LOST_IP timer. Never apply another interface's events. */
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    if (e->esp_netif != s_ppp_netif) return;

    switch (id) {
    case IP_EVENT_PPP_GOT_IP: {
        if (!ppp_events_observe(PPP_OBS_GOT_IP, 0)) return;
        ESP_LOGI(MODEM_TAG, "PPP got IP: " IPSTR " gw=" IPSTR " mask=" IPSTR,
                 IP2STR(&e->ip_info.ip),
                 IP2STR(&e->ip_info.gw),
                 IP2STR(&e->ip_info.netmask));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "PPP DNS main: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        if (esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "PPP DNS backup: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        break;
    }
    case IP_EVENT_PPP_LOST_IP:
        if (!ppp_events_observe(PPP_OBS_LOST_IP, 0)) return;
        ESP_LOGW(MODEM_TAG, "PPP lost IP");
        break;
    default:
        break;
    }
}

static void on_netif_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != NETIF_PPP_STATUS || !data || !s_ppp_netif) return;
    /* IDF 6.0.2 on_ppp_status_changed posts errors 1..12 with an
     * esp_netif_t* payload (NOT ip_event_got_ip_t). NONE and phase changes
     * are not failures. PPPERR_CONNECT normally arrives via LOST_IP instead.
     * The separate CONNECT_FAILED(0x200) post in this SDK has a different,
     * malformed payload; do not treat its copied bytes as a netif pointer.
     * A start that produces no valid event remains bounded by the IP wait. */
    if (id < NETIF_PPP_ERRORPARAM || id > NETIF_PPP_ERRORLOOPBACK) return;
    esp_netif_t *event_netif;
    memcpy(&event_netif, data, sizeof(event_netif));
    if (event_netif != s_ppp_netif) return;
    if (ppp_events_observe(PPP_OBS_ERROR, id)) {
        ESP_LOGW(MODEM_TAG, "PPP terminal error %ld", (long)id);
    } else if (id == NETIF_PPP_ERRORUSER) {
        ESP_LOGI(MODEM_TAG, "PPP user disconnect outside active dial/session (expected stop)");
    }
}

/* --- SNTP time sync ------------------------------------------------------ */

/*
 * TLS cert validation needs an accurate wall clock; without it, mbedTLS
 * thinks every Let's Encrypt cert is "not yet valid" because the chip
 * boots at epoch=0 (1970). We hit pool.ntp.org over PPP and block until
 * we have a real time, or `timeout_ms` elapses.
 */
static esp_err_t wait_for_time_sync(uint32_t timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    /* A late sync after the initial wait times out must still anchor logs. */
    cfg.sync_cb = modem_diag_clock_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(MODEM_TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    int retry = 0;
    const int retry_count = (int)(timeout_ms / 500);
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500)) != ESP_OK) {
        if (++retry > retry_count) {
            ESP_LOGW(MODEM_TAG, "SNTP sync timed out after %u ms", (unsigned)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
    }

    time_t now = 0;
    time(&now);
    struct tm tm;
    gmtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    ESP_LOGI(MODEM_TAG, "SNTP synced: %s (epoch=%lld)", buf, (long long)now);
    return ESP_OK;
}

/* --- HTTP GET smoke test ------------------------------------------------ */

/*
 * Capture the first chunk of the response body so we have proof in the log
 * that real bytes flowed in from the network (not just a 200 OK status).
 */
static char   s_http_body_preview[128];
static size_t s_http_body_preview_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    size_t remaining = sizeof(s_http_body_preview) - 1 - s_http_body_preview_len;
    if (remaining == 0 || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t n = (size_t)evt->data_len < remaining ? (size_t)evt->data_len : remaining;
    memcpy(s_http_body_preview + s_http_body_preview_len, evt->data, n);
    s_http_body_preview_len += n;
    s_http_body_preview[s_http_body_preview_len] = '\0';
    return ESP_OK;
}

static void run_http_get_test(void)
{
    /* Purpose-built connectivity endpoint (the check every Android phone
     * uses; returns 204 with no body — ~1 KB less LTE data per boot than
     * the old example.com page, whose operators explicitly discourage
     * production use). Failure only logs — nothing is gated on this. */
    static const char *URL = "http://connectivitycheck.gstatic.com/generate_204";

    ESP_LOGI(MODEM_TAG, "--- HTTP GET %s over PPP (esp_http_client) ---", URL);

    s_http_body_preview_len = 0;
    s_http_body_preview[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = URL,
        .timeout_ms    = 15000,
        .event_handler = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(MODEM_TAG, "esp_http_client_init failed");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int64_t len = esp_http_client_get_content_length(client);
        ESP_LOGI(MODEM_TAG,
                 "HTTP %d, content_length=%lld, captured %u byte(s) of body",
                 status, len, (unsigned)s_http_body_preview_len);
        if (s_http_body_preview_len > 0) {
            /* Strip embedded newlines so the log line stays readable. */
            for (size_t i = 0; i < s_http_body_preview_len; i++) {
                if (s_http_body_preview[i] == '\n' || s_http_body_preview[i] == '\r') {
                    s_http_body_preview[i] = ' ';
                }
            }
            ESP_LOGI(MODEM_TAG, "HTTP body[0..%u]: %.*s%s",
                     (unsigned)s_http_body_preview_len,
                     (int)s_http_body_preview_len, s_http_body_preview,
                     (size_t)len > s_http_body_preview_len ? " ..." : "");
        }
    } else {
        ESP_LOGE(MODEM_TAG, "esp_http_client_perform failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* --- DCE bring-up / teardown -------------------------------------------- */

/* EPS/GPRS registration status — "+CEREG: <n>,<stat>": 1 = registered home,
 * 5 = registered roaming, 2 = searching, 3 = denied, 0 = idle; -1 = no
 * parsable answer. Registration means the ATD*99# dial inside the mode
 * switch can be expected to succeed. */
static int modem_reg_stat(void)
{
    static const char *cmds[] = { "AT+CEREG?", "AT+CGREG?" };
    int stat = -1;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        char out[128] = {0};   /* >= CONFIG_ESP_MODEM_C_API_STR_MAX */
        if (esp_modem_at(s_dce, cmds[i], out, 3000) != ESP_OK) continue;
        const char *comma = strchr(out, ',');
        if (!comma) continue;
        stat = atoi(comma + 1);
        if (stat == 1 || stat == 5) return stat;
    }
    return stat;
}

/* Consecutive bring-up failures caused by a registration timeout — used to
 * exempt them from the fast power-cycle threshold (a PWRKEY cycle restarts
 * the modem's network search from scratch, so cycling every 2 failures
 * mid-search loops forever; seen on bench 2026-07-22). */
static int s_reg_timeout_streak = 0;

/* Create the DCE, sync at AT level, log identity, and switch to data (PPP)
 * mode. On success the DCE is owned by `s_dce` and the PPP layer is racing
 * to acquire an IP — caller waits for the ordered PPP lifecycle state. */
static esp_err_t ppp_bringup_dce(void)
{
    /* DTE = Data Terminal Equipment side (us) — UART parameters. */
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.tx_io_num   = MODEM_TX_GPIO;
    dte_cfg.uart_config.rx_io_num   = MODEM_RX_GPIO;
    dte_cfg.uart_config.rts_io_num  = -1;
    dte_cfg.uart_config.cts_io_num  = -1;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_cfg.uart_config.port_num   = MODEM_UART;
    dte_cfg.uart_config.baud_rate  = MODEM_BAUD;

    /* DCE = Data Circuit-terminating Equipment side (the modem). The 1nce
     * SIM auto-provisions the radio APN, but the application PPP context
     * still needs an explicit APN at PDP-context activation time. */
    esp_modem_dce_config_t dce_cfg =
        ESP_MODEM_DCE_DEFAULT_CONFIG(s_apn);

    /* SIM7080G isn't a separate DCE class; SIM7070 covers the same AT set
     * (the SIM7070/SIM7080/SIM7090 family share commands and the V1.05 AT
     * Manual is one document for all three). */
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7070,
                              &dte_cfg, &dce_cfg, s_ppp_netif);
    if (!s_dce) {
        ESP_LOGE(MODEM_TAG, "esp_modem_new_dev failed");
        return ESP_FAIL;
    }
    ESP_LOGI(MODEM_TAG, "DCE created (SIM7070 class)");

    /* Probe the modem at AT level a few times so we know it's awake before
     * we tell esp_modem to switch to PPP/data mode. esp_modem starts in
     * COMMAND mode, so AT works. */
    bool synced = false;
    for (int i = 0; i < 20; i++) {
        if (esp_modem_sync(s_dce) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "modem responsive (after %d AT retries)", i);
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!synced) {
        ESP_LOGE(MODEM_TAG, "modem did not respond to AT after 20 tries");
        s_fail_stage = MODEM_FAIL_AT;
        return ESP_ERR_TIMEOUT;
    }

    /* Verbose CME error reporting, so AT+CPIN?/AT+CCID failures below log the
     * actual cause ("SIM not inserted" vs "SIM busy" vs a wedged modem)
     * instead of a bare rc=-1. Best-effort — ignore result and response. */
    esp_modem_at(s_dce, "AT+CMEE=2", NULL, 1000);

    if (!modem_radio_prepare(radio_at, NULL)) {
        ESP_LOGE(MODEM_TAG, "radio policy failed; PPP blocked, RF-off requested");
        s_fail_stage = MODEM_FAIL_RADIO;
        return ESP_FAIL;
    }

    if (!modem_awake_prepare(radio_at, NULL)) {
        ESP_LOGE(MODEM_TAG, "awake policy configuration not confirmed; PPP blocked, RF-off requested");
        s_fail_stage = MODEM_FAIL_AWAKE;
        return ESP_FAIL;
    }

    /* A few sanity-check at-level reads before going to data mode. */
    char buf[64] = {0};
    if (esp_modem_get_imei(s_dce, buf) == ESP_OK) {
        ESP_LOGI(MODEM_TAG, "IMEI: %s", buf);
        identity_set_imei(buf);
    }
    if (esp_modem_get_imsi(s_dce, buf) == ESP_OK)        ESP_LOGI(MODEM_TAG, "IMSI: %s", buf);
    if (esp_modem_get_module_name(s_dce, buf) == ESP_OK) ESP_LOGI(MODEM_TAG, "module: %s", buf);
    {
        /* Modem firmware revision, once per bring-up — makes field modem FW
         * versions visible in logs (they differ per production batch and
         * change AT quirks, e.g. AT+CCID vs AT+CICCID). Best-effort. */
        char rev[128] = {0};   /* >= CONFIG_ESP_MODEM_C_API_STR_MAX */
        if (esp_modem_at(s_dce, "AT+CGMR", rev, 3000) == ESP_OK && rev[0]) {
            ESP_LOGI(MODEM_TAG, "modem FW revision: %s", rev);
        }
    }
    int rssi = 99, ber = 99;
    s_bringup_rssi_dbm = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(MODEM_TAG, "signal: rssi=%d ber=%d", rssi, ber);
        s_bringup_rssi_dbm = csq_to_dbm(rssi);
    }

    /* Read SIM ICCID — this becomes the device identity (MQTT username +
     * topic prefix per ADR-0002). The exact response format varies between
     * SIM7080G firmware revisions ("+CCID: 8988...", "+ICCID: 8988...", or
     * just "8988...\r\nOK"), so we extract by scanning for the first run of
     * digits and capping at 20. Identity is set ONCE per boot; subsequent
     * PPP cycles preserve it. */
    if (s_iccid_known) {
        ESP_LOGI(MODEM_TAG, "ICCID already known: %s", identity_iccid());
    } else {
        /* Wait for the SIM stack to report READY before reading ICCID. AT
         * answers several seconds before "+CPIN: READY" on a cold or
         * re-seated SIM, and a single AT+CCID shot then loses the race —
         * that was the intermittent bench failure. Poll CPIN for ~6 s. */
        bool sim_ready = false;
        for (int i = 0; i < 12 && !sim_ready; i++) {
            char pin[128] = {0};   /* >= CONFIG_ESP_MODEM_C_API_STR_MAX */
            if (esp_modem_at(s_dce, "AT+CPIN?", pin, 2000) == ESP_OK &&
                strstr(pin, "READY")) {
                ESP_LOGI(MODEM_TAG, "SIM ready (CPIN READY after %d polls)", i);
                sim_ready = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!sim_ready) {
            ESP_LOGW(MODEM_TAG, "CPIN not READY after ~6s "
                                "(SIM not inserted / bad contact?) — trying CCID anyway");
        }

        /* Read ICCID with retries: CPIN can flip READY a beat before the
         * EF-ICCID file is readable, and SIM7080G FW revisions differ on
         * AT+CCID vs AT+CICCID — so retry and try the alias once. */
        char at_out[128] = {0};
        esp_err_t at_err = ESP_FAIL;
        for (int attempt = 0; attempt < 5; attempt++) {
            at_out[0] = '\0';
            at_err = esp_modem_at(s_dce, "AT+CCID", at_out, 3000);
            if (at_err == ESP_OK && at_out[0]) break;
            if (attempt == 2) {                 /* mid-way: try the alias once */
                at_out[0] = '\0';
                at_err = esp_modem_at(s_dce, "AT+CICCID", at_out, 3000);
                if (at_err == ESP_OK && at_out[0]) break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (at_err == ESP_OK && at_out[0]) {
            ESP_LOGI(MODEM_TAG, "AT+CCID raw: %s", at_out);
            char iccid[24] = {0};
            size_t out_idx = 0;
            bool started = false;
            for (size_t i = 0; at_out[i] != '\0' && out_idx + 1 < sizeof(iccid); ++i) {
                char c = at_out[i];
                if (isdigit((unsigned char)c)) {
                    iccid[out_idx++] = c;
                    started = true;
                } else if (started) {
                    break;   /* stop at first non-digit so we don't slurp "OK" */
                }
            }
            iccid[out_idx] = '\0';
            /* ICCID can be 19 or 20 digits — the 20th (when present) is a
             * Luhn check digit; strip it so what the panel claims matches
             * what the device publishes (1NCE/panel use the 19-digit form). */
            if (out_idx == 20) {
                ESP_LOGI(MODEM_TAG, "trimming Luhn check digit: %s -> %.*s",
                         iccid, 19, iccid);
                iccid[19] = '\0';
            }
            if (identity_set_iccid(iccid) == ESP_OK) {
                s_iccid_known = true;
            }
        } else {
            /* Persistent SIM read failure. Fail the whole bring-up so the
             * supervisor counts it (toward the power-cycle + degraded alert)
             * and retries — instead of racing on to PPP, which needs the SIM
             * anyway and would just time out silently. */
            ESP_LOGE(MODEM_TAG,
                     "AT+CCID failed after retries: rc=%d out='%s' "
                     "(SIM not inserted / bad contact?)", (int)at_err, at_out);
            s_fail_stage = MODEM_FAIL_SIM;
            return ESP_FAIL;
        }
    }

    /* The original fleet is this exact five-card set. All other cards use
     * sensor.net. Identity is fixed for the boot, and retries retain its APN
     * even when registration fails. Do not seed from an invalid identity. */
    if (!s_iccid_known) {
        ESP_LOGE(MODEM_TAG, "ICCID invalid — refusing to select an APN");
        s_fail_stage = MODEM_FAIL_SIM;
        return ESP_FAIL;
    }
    if (!s_apn_seeded) {
        static const char *const old_batch_iccids[] = {
            "8988228066614189920",
            "8988280666000338870",
            "8988280666000338871",
            "8988228066618136967",
            "8988228066618136966",
        };
        const char *iccid = identity_iccid();
        for (size_t i = 0;
             i < sizeof(old_batch_iccids) / sizeof(old_batch_iccids[0]); ++i) {
            if (strcmp(iccid, old_batch_iccids[i]) == 0) {
                s_apn = "iot.1nce.net";
                break;
            }
        }
        s_apn_seeded = true;
        ESP_LOGI(MODEM_TAG, "APN selected from ICCID list: %s", s_apn);
    }

    /* esp_modem copied the APN when the DCE was created, before the first
     * ICCID read. Updating only CGDCONT would let setup_data_mode() restore
     * that stale APN after registration. Update both on every new DCE. */
    esp_err_t apn_err = esp_modem_set_apn(s_dce, s_apn);
    if (apn_err != ESP_OK) {
        ESP_LOGE(MODEM_TAG, "DCE APN synchronization failed: %s",
                 esp_err_to_name(apn_err));
        s_fail_stage = MODEM_FAIL_NET;
        return apn_err;
    }

    /* Program the attach APN BEFORE waiting for registration. The LTE attach
     * carries the default-bearer (PDN) request built from CGDCONT cid 1, so
     * the APN must be in place now — esp_modem itself only sends CGDCONT in
     * setup_data_mode(), i.e. after registration succeeds, which is too late
     * for networks that reject the attach on a wrong/stale APN (new SIM
     * batch, bench 2026-08-18). Best-effort: on failure the modem attaches
     * with whatever CGDCONT its NVRAM holds, as before. */
    {
        char apn_cmd[64];
        snprintf(apn_cmd, sizeof apn_cmd, "AT+CGDCONT=1,\"IP\",\"%s\"",
                 s_apn);
        if (esp_modem_at(s_dce, apn_cmd, NULL, 3000) == ESP_OK) {
            ESP_LOGI(MODEM_TAG, "attach APN set: %s", s_apn);
        } else {
            ESP_LOGW(MODEM_TAG, "AT+CGDCONT failed — attach uses modem's stored APN");
        }
    }

    /* Common gate covers CMUX and its plain-DATA fallback on every retry. */
    if (!modem_radio_resume(radio_at, NULL)) {
        ESP_LOGE(MODEM_TAG, "radio readback/resume failed; PPP blocked, RF-off requested");
        s_fail_stage = MODEM_FAIL_RADIO;
        return ESP_FAIL;
    }
    ESP_LOGI(MODEM_TAG, "radio policy verified: LTE-M only, CAT-M bands B3+B20, no NB-IoT fallback");
    modem_network_snapshot("radio ready / before registration");

    /* Switch to CMUX mode — PPP (data channel) and an AT command channel run
     * concurrently, which is what lets the post-GOT_IP supervision loop poll
     * CSQ/CPSI and issue AT+CFUN resets while the link is up. Entered ONCE
     * per modem boot, from command mode, after the CCID gate. The SIM7070/
     * 7080 family cannot RE-enter CMUX on a running modem (esp-protocols
     * issue #659 — the second entry gets NO CARRIER), so every teardown of a
     * CMUX session ends in a module reset / power-cycle that guarantees the
     * next entry starts from a freshly booted modem. If entry fails
     * CMUX_ENTRY_FAILS_MAX times in a row (fresh, registered boots each),
     * fall back to plain DATA mode so the uplink never depends on CMUX —
     * RSSI polling is then silently disabled until the daily retry. */
    if (s_cmux_entry_fails >= CMUX_ENTRY_FAILS_MAX &&
        now_s() - s_cmux_fallback_since_s >= CMUX_FALLBACK_RETRY_S) {
        ESP_LOGI(MODEM_TAG, "daily CMUX retry — re-arming one entry attempt");
        s_cmux_entry_fails = CMUX_ENTRY_FAILS_MAX - 1;
    }
    /* Both modes need a registered network to verify negotiated sleep state. */
    /* Both CMUX and DATA end in the ATD*99# dial, which fails whenever
     * the modem isn't network-registered — a routine LTE outage, not a
     * CMUX defect. Gate on registration so only failures of a registered
     * modem count toward the DATA fallback. Crucially: WAIT for it. A
     * freshly booted modem needs tens of seconds of UNINTERRUPTED Cat-M
     * network search; failing fast here fed the 2-fail power-cycle,
     * which restarted the search every ~45 s and looped forever
     * (bench 2026-07-22). */
    {
        const int64_t reg_deadline =
            esp_timer_get_time() + (int64_t)MODEM_REG_WAIT_MS * 1000;
        int stat = -1, poll = 0;
        bool registered = false;
        for (;;) {
            stat = modem_reg_stat();
            registered = (stat == 1 || stat == 5);
            if (registered || esp_timer_get_time() >= reg_deadline) break;
            if ((poll++ % 3) == 0) {
                int csq = 99, ber = 99;
                (void)esp_modem_get_signal_quality(s_dce, &csq, &ber);
                ESP_LOGI(MODEM_TAG,
                         "waiting for network registration (stat=%d rssi=%d)...",
                         stat, csq);
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        if (!registered) {
            ESP_LOGW(MODEM_TAG, "no network registration within %d s "
                                "(last stat=%d) — failing bring-up before "
                                "the mode switch",
                     MODEM_REG_WAIT_MS / 1000, stat);
            modem_network_snapshot("registration timeout");
            s_fail_stage = MODEM_FAIL_NET;
            s_reg_timeout_streak++;
            ESP_LOGW(MODEM_TAG, "next bring-up keeps fixed APN \"%s\"",
                     s_apn);
            return ESP_FAIL;
        }
        s_reg_timeout_streak = 0;
    }
    if (!modem_awake_check_before_ppp()) return ESP_FAIL;
    modem_network_snapshot("registered / before PPP");
    modem_support_snapshot(false);
    esp_err_t err;
    ppp_events_start_dial();
    if (s_cmux_entry_fails < CMUX_ENTRY_FAILS_MAX) {
        ESP_LOGI(MODEM_TAG, "switching modem to CMUX mode (PPP + AT channel)...");
        err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX);
        if (err != ESP_OK) {
            s_cmux_entry_fails++;
            s_cmux_dirty = true;   /* framing state unknown — teardown must power-cycle */
            ESP_LOGE(MODEM_TAG, "esp_modem_set_mode(CMUX) failed: %s (%d/%d)",
                     esp_err_to_name(err), s_cmux_entry_fails, CMUX_ENTRY_FAILS_MAX);
            if (s_cmux_entry_fails >= CMUX_ENTRY_FAILS_MAX) {
                s_cmux_fallback_since_s = now_s();
                ESP_LOGW(MODEM_TAG, "CMUX entry failed %d consecutive times — "
                                    "falling back to plain DATA mode "
                                    "(RSSI polling disabled; retry in ~24 h)",
                         s_cmux_entry_fails);
            }
            return err;
        }
        s_cmux_active = true;
        s_cmux_entry_fails = 0;
    } else {
        /* CMUX fallback — plain data (PPP) mode, no AT channel. */
        ESP_LOGI(MODEM_TAG, "switching modem to PPP/data mode (CMUX fallback)...");
        ESP_LOGW(MODEM_TAG, "periodic AT diagnostics unavailable in DATA mode; see pre-PPP snapshot");
        err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        if (err != ESP_OK) {
            ESP_LOGE(MODEM_TAG, "esp_modem_set_mode(DATA) failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* Tear down the DCE and free its UART driver so the next bring-up can claim
 * the port cleanly, plus whatever modem-state repair `action` demands.
 *
 * CMUX sessions are special: we must NEVER exit CMUX on a running modem
 * (esp-protocols #659 — the SIM7070/7080 family answers a re-entry with NO
 * CARRIER), so instead of dropping back to command mode we reset the module
 * over the still-open CMUX command channel (AT+CFUN=1,1). That both
 * guarantees the next bring-up's CMUX entry starts from a freshly booted
 * modem AND performs a fresh network attach + fresh PDP — the device-side
 * equivalent of a network-side "Reset connection". If the AT channel is
 * dead too, escalate to a PWRKEY power-cycle. */
static void ppp_teardown_dce(teardown_action_t action)
{
    /* Commit intentional stop before any command can provoke PPP_ERRORUSER.
     * Late IP callbacks cannot resurrect this DCE during destroy/backoff. */
    ppp_events_begin_stop();
    bool fresh_boot_wait = false;               /* modem rebooting after CFUN=1,1 */
    bool pwrcycle = (action == TEARDOWN_PWRCYCLE);
    bool was_cmux = s_cmux_active;

    if (s_dce) {
        if (s_cmux_active && !pwrcycle) {
            /* NULL response buffer — the text is unused, and esp_modem_at
             * copies up to CONFIG_ESP_MODEM_C_API_STR_MAX (128) bytes into
             * whatever buffer it is given, so a short stack buffer here
             * would overflow when teardown-window URCs pile onto the reply. */
            if (esp_modem_at(s_dce, "AT+CFUN=1,1", NULL, 3000) == ESP_OK) {
                ESP_LOGI(MODEM_TAG, "AT+CFUN=1,1 sent — module resetting "
                                    "(fresh attach + PDP, fresh CMUX next round)");
                fresh_boot_wait = true;
            } else {
                ESP_LOGW(MODEM_TAG, "AT+CFUN=1,1 failed on the CMUX command "
                                    "channel — escalating to PWRKEY power-cycle");
                pwrcycle = true;
            }
        } else if (!s_cmux_active && !s_cmux_dirty) {
            /* Plain DATA session (CMUX fallback) — best-effort exit from data
             * mode first; we don't care if it fails (likely the link is
             * already down). Skipped after a FAILED CMUX entry (s_cmux_dirty):
             * the DCE then believes it's in CMUX mode with PPP never started,
             * so the COMMAND transition would block ~30 s in
             * wait_until_ppp_exits — the power-cycle below repairs it instead. */
            esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
            if (err != ESP_OK) {
                ESP_LOGW(MODEM_TAG, "set_mode(COMMAND) on teardown failed (%s) — "
                                    "continuing with destroy",
                         esp_err_to_name(err));
                if (action == TEARDOWN_MODULE_RESET) {
                    /* The promised module reset can't be delivered over AT —
                     * escalate so the watchdog trip isn't silently a no-op. */
                    pwrcycle = true;
                }
            } else if (action == TEARDOWN_MODULE_RESET) {
                if (esp_modem_at(s_dce, "AT+CFUN=1,1", NULL, 3000) == ESP_OK) {
                    ESP_LOGI(MODEM_TAG, "AT+CFUN=1,1 sent — module resetting");
                    fresh_boot_wait = true;
                } else {
                    ESP_LOGW(MODEM_TAG, "AT+CFUN=1,1 failed — escalating to "
                                        "PWRKEY power-cycle");
                    pwrcycle = true;
                }
            }
        }
        esp_modem_destroy(s_dce);
        s_dce = NULL;
        ESP_LOGI(MODEM_TAG, "DCE destroyed");
    }
    s_cmux_active = false;

    if (pwrcycle || s_cmux_dirty) {
        /* `known_running` only when this was a live CMUX session (PPP was up
         * moments ago); after a failed CMUX entry the state is anyone's
         * guess, so let the probe-based convergence sort it out. */
        modem_power_cycle(was_cmux && !s_cmux_dirty);
        s_cmux_dirty = false;
    } else if (fresh_boot_wait) {
        /* CFUN=1,1 reboots the module; esp_modem_sync's 20×500 ms retry on
         * the next bring-up absorbs any remainder. */
        vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_DELAY_MS));
    }
}

/* --- post-PPP uplink watchdog -------------------------------------------- */

/* Uplink health of the ACTIVE backend (see the accessors in mqtt.h /
 * http_backend.h / arkiv_ws.h). `up` = the backend link is demonstrably
 * alive right now (drives the net.status state field); `wd_ok` = the uplink
 * watchdog should treat this tick as healthy. They differ only where no
 * uplink is expected at all (Arkiv mode before the device is claimed, HTTP
 * mode before an endpoint is configured): nothing to supervise → wd_ok but
 * not up. */
/* One minimal DNS/UDP query ("web3pi.io" A IN, RD set) to `server_ip:53`.
 * Success = any response with our transaction ID and the QR bit — we don't
 * care about the answer (NXDOMAIN/SERVFAIL count too), only that real bytes
 * crossed the internet and came back (~30 B out / ~100 B in). The QNAME is
 * our own domain purely as good netiquette; nothing depends on it
 * resolving. ICMP is deliberately not used: LTE-M carrier NATs commonly
 * drop it, which would misclassify a healthy network as dead. */
static bool dns_probe_one(const char *server_ip, int timeout_ms)
{
    static const uint8_t qtail[] = {
        6, 'w', 'e', 'b', '3', 'p', 'i', 2, 'i', 'o', 0, /* QNAME     */
        0x00, 0x01,                                      /* QTYPE A   */
        0x00, 0x01,                                      /* QCLASS IN */
    };
    uint8_t pkt[12 + sizeof(qtail)] = { 0 };
    uint16_t txid = (uint16_t)(esp_timer_get_time() & 0xffff);
    pkt[0] = (uint8_t)(txid >> 8);
    pkt[1] = (uint8_t)(txid & 0xff);
    pkt[2] = 0x01; /* RD */
    pkt[5] = 0x01; /* QDCOUNT = 1 */
    memcpy(pkt + 12, qtail, sizeof(qtail));

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    bool ok = false;
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) == 1 &&
        sendto(fd, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dst, sizeof(dst)) == (int)sizeof(pkt)) {
        uint8_t resp[160];
        int n = recv(fd, resp, sizeof(resp), 0);
        ok = n >= 12 && resp[0] == pkt[0] && resp[1] == pkt[1] &&
             (resp[2] & 0x80) != 0;
    }
    close(fd);
    return ok;
}

/* Two independent anycast resolvers — one confirming reply is enough. */
static bool inet_probe(void)
{
    return dns_probe_one("1.1.1.1", INET_PROBE_TIMEOUT_MS) ||
           dns_probe_one("8.8.8.8", INET_PROBE_TIMEOUT_MS);
}

/* `ota_ok` (0.8.7): may this tick count as the "demonstrably healthy
 * uplink" that marks a pending-verify OTA image valid? Everywhere it
 * matches the pre-0.8.7 wd_ok values (legacy semantics preserved for the
 * unclaimed-Arkiv / unconfigured-HTTP postures, whose UART-flashed updates
 * must not be left to auto-rollback) — EXCEPT the new MQTT auth-refused
 * clause: a unit the broker is refusing must not confirm an OTA image on
 * network reachability alone. */
static void uplink_health(bool *up, bool *wd_ok, bool *ota_ok)
{
    switch (backend_mode_get()) {
    case WUPS_BACKEND_MODE_MQTT:
        /* CONNECT alone is not evidence that an uplink publication reached
         * the broker. The owner also invalidates proof during OTA transfer. */
        *up = mqtt_publication_proof_fresh();
        *wd_ok = *up;
        *ota_ok = *up;
        if (!*up && mqtt_auth_refused()) {
            /* Broker reachable, TLS fine, CONNACK refused our credentials:
             * the unit is merely unclaimed (or its secret was rotated) and
             * the network is provably healthy — a modem reset cannot help.
             * Same posture as unclaimed Arkiv below; without it factory
             * units beeped "NO UPLINK" and reset the modem forever while
             * simply waiting for their first claim (2026-08-21). */
            *wd_ok = true;
        }
        break;
    case WUPS_BACKEND_MODE_HTTP: {
        if (!http_backend_is_configured()) {
            /* Endpoint not set yet (guided VPS setup / net.config pending) —
             * same "nothing to supervise" posture as unclaimed Arkiv below,
             * or the watchdog would reset a healthy modem forever. */
            *up = false;
            *wd_ok = true;
            *ota_ok = true;     /* legacy: UART-flashed update must confirm */
            break;
        }
        uint32_t last = http_backend_last_success_s();
        *up = last != 0 && (now_s() - last) <= HTTP_UPLINK_FRESH_SECS;
        *wd_ok = *up;
        *ota_ok = *up;
        break;
    }
    case WUPS_BACKEND_MODE_ARKIV:
        if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) {
            *up = false;
            *wd_ok = true;      /* unclaimed: no uplink expected, never trip */
            *ota_ok = true;     /* legacy: UART-flashed update must confirm */
        } else {
            /* RPC round-trips prove the uplink (telemetry every 30 s, cmd
             * poll every 5 s while the WS is down). The WS subscription is
             * only the low-latency cmd push — a refused/404 WS (e.g. a
             * placeholder-token CI build; field incident 2026-08-16: the
             * old `*wd_ok = ws_subscribed` put the unit in a permanent
             * CFUN/PWRKEY reset cycle with a "NO UPLINK" banner while
             * writes were landing on-chain) must never trip the modem
             * watchdog on its own. */
            uint32_t last = arkiv_rpc_last_success_s();
            bool rpc_fresh =
                last != 0 && (now_s() - last) <= ARKIV_UPLINK_FRESH_SECS;
            *up = rpc_fresh || arkiv_ws_subscribed();
            *wd_ok = *up;
            *ota_ok = *up;
        }
        break;
    default:
        *up = false;
        *wd_ok = true;          /* unknown mode — never trip */
        *ota_ok = true;
        break;
    }
}

/* Runs while PPP is up (and ONLY then — bring-up/backoff never get here).
 * Every PPP_SUPERVISE_TICK_MS: check uplink health, poll signal quality and
 * emit net.status (CMUX sessions), and pace the alert-clear re-sends.
 * Returns when the link drops by itself (TEARDOWN_NORMAL) or when the
 * uplink watchdog trips (module reset / power-cycle per s_uplink_trips). */
static teardown_action_t supervise_uplink(void)
{
    uint32_t last_healthy_s  = now_s();  /* watchdog timer starts at GOT_IP */
    uint32_t last_clear_s    = now_s();  /* GOT_IP path just sent a clear    */
    uint32_t last_radio_snapshot_s = now_s();
    uint32_t last_radio_poll_s = now_s();
    bool radio_snapshot_pending = false;
    int8_t last_rsrp = 0, last_rsrq = 0;
    uint32_t last_probe_ok_s = 0;        /* internet-probe OK verdict cache  */

    for (;;) {
        /* MQTT pacing belongs to its independent owner; this task only
         * supervises PPP and wakes early to observe a reported MQTT drop. */
        EventBits_t bits = xEventGroupWaitBits(s_modem_evt,
                                               EVT_PPP_CHANGED | EVT_MQTT_DOWN,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(PPP_SUPERVISE_TICK_MS));
        if (ppp_events_take_loss()) {
            ESP_LOGW(MODEM_TAG, "PPP loss: last radio snapshot %us ago; see preceding AT replies",
                     (unsigned)(now_s() - last_radio_snapshot_s));
            ESP_LOGW(MODEM_TAG, "PPP link lost — tearing down DCE");
            return TEARDOWN_NORMAL;
        }
        if (bits & EVT_MQTT_DOWN) {
            /* Early wake only — the MQTT owner performs recovery. */
            radio_snapshot_pending = true;
        }
        uint32_t now = now_s();

        /* Fast idempotent request only. SDK start/retry/reconnect/revive and
         * their backoff execute in the MQTT owner, never in the PPP task. */
        if (backend_mode_get() == WUPS_BACKEND_MODE_MQTT && s_iccid_known) {
            (void)mqtt_client_start();
        }

        /* OTA-1 — while a firmware download runs, the uplink is deliberately
         * busy (MQTT may go quiet under the TLS transfer) and a watchdog
         * teardown would kill it. Freeze the watchdog timer, count no trips,
         * escalate nothing; genuine PPP loss (the EVT bits above) still ends
         * the session and fails the download on its own. */
        if (fw_ota_in_progress()) {
            last_healthy_s = now_s();
            continue;
        }

        bool uplink_up = false, wd_healthy = false, ota_proof = false;
        uplink_health(&uplink_up, &wd_healthy, &ota_proof);
        if (wd_healthy) {
            last_healthy_s = now;
            /* OTA-1 rollback — first demonstrably healthy uplink marks a
             * pending-verify OTA image valid (one-shot, no-op otherwise).
             * Gated on ota_proof, NOT wd_ok: an auth-refused unit must not
             * confirm an image on network reachability alone. */
            if (ota_proof) {
                fw_ota_mark_uplink_healthy();
            }
            if (s_uplink_trips != 0 || s_alert_active) {
                /* Deferred recovery bookkeeping (see the GOT_IP branch):
                 * only a demonstrably healthy uplink ends an escalation.
                 * Also covers the hold-alert (backend outage) case, which
                 * raises the alert with zero trips. */
                ESP_LOGI(MODEM_TAG, "uplink healthy again — resetting trip escalation");
                s_uplink_trips = 0;
                s_fails_since_ok = 0;
                s_fail_stage = MODEM_FAIL_NONE;
                if (s_alert_active) {
                    s_alert_active = false;
                    modem_ui_alert_clear();
                    s_alert_clear_pending = ALERT_CLEAR_RESENDS;
                    last_clear_s = now;
                    ESP_LOGI(MODEM_TAG, "uplink recovered — clearing OLED/buzzer alert");
                }
            }
        } else if (s_alert_active && s_fail_stage == MODEM_FAIL_UPLINK) {
            /* Hold alert survived a PPP re-dial (last_healthy_s re-armed, so
             * the hold branch won't re-assert for another UPLINK_DEAD_SECS):
             * keep refreshing it here, or the RP2040's 5-min non-refresh
             * auto-clear would drop the banner mid-outage. */
            modem_ui_alert(modem_fail_msg(MODEM_FAIL_UPLINK));
        }

        /* Signal-quality poll — CMUX sessions only (the DATA fallback has no
         * AT channel while PPP runs). rsrp/rsrq stay 0 (unknown) unless
         * AT+CPSI? parses cleanly. */
        int8_t rssi = s_ns_last_rssi, rsrp = last_rsrp, rsrq = last_rsrq;
        if (s_cmux_active && now - last_radio_poll_s >= 30) {
            last_radio_poll_s = now;
            int csq = 99, ber = 99;
            if (esp_modem_get_signal_quality(s_dce, &csq, &ber) == ESP_OK) {
                rssi = csq_to_dbm(csq);
            }
            bool support_due = false;
            /* Existing CPSI cadence (30 s); COPS/CEREG once per minute,
             * or on MQTT drop, rate limited to one snapshot per 30 s.
             * No AT is sent on a live plain-DATA PPP channel. */
            if (now - last_radio_snapshot_s >= 60 ||
                (radio_snapshot_pending && now - last_radio_snapshot_s >= 30)) {
                char reply[MODEM_RADIO_RESPONSE_CAPACITY];
                ESP_LOGI(MODEM_TAG, "network snapshot: %s",
                         radio_snapshot_pending ? "MQTT disconnected" : "periodic");
                (void)radio_at(NULL, "AT+CEREG?", reply, sizeof(reply), 3000);
                (void)radio_at(NULL, "AT+COPS?", reply, sizeof(reply), 3000);
                last_radio_snapshot_s = now;
                support_due = true;
                radio_snapshot_pending = false;
            }
            rsrp = rsrq = 0;
            poll_cpsi(&rsrp, &rsrq);
            last_rsrp = rsrp;
            last_rsrq = rsrq;
            /* Collect the radio snapshot first. Optional APN/PSM/eDRX reads
             * have a bounded batch and yield to OTA/PPP loss per command.
             * Failures are diagnostic only, never a recovery trigger. */
            if (support_due) modem_support_snapshot(true);
        }
        /* Events can arrive while a bounded AT read is in progress. Do not
         * run stale health/probe/reset decisions after optional diagnostics. */
        if (ppp_events_take_loss()) {
            ESP_LOGW(MODEM_TAG, "PPP loss during diagnostics — yielding to recovery");
            return TEARDOWN_NORMAL;
        }
        if (fw_ota_in_progress()) {
            last_healthy_s = now_s();
            continue;
        }
        now = now_s();
        uint8_t state = uplink_up ? NET_STATE_MQTT_UP : NET_STATE_PPP_UP;
        int rssi_delta = (int)rssi - (int)s_ns_last_rssi;
        if (state != s_ns_last_state ||
            abs(rssi_delta) > NET_STATUS_RSSI_DELTA_DB ||
            now - s_ns_last_emit_s >= NET_STATUS_EMIT_PERIOD_S) {
            /* Also anchors the DATA fallback, which has no live AT channel. */
            modem_diag_clock_log("net.status");
            emit_net_status(state, rssi, rsrp, rsrq);
        }

        /* Belt-and-braces alert-clear re-sends: the single clear sent at
         * GOT_IP rides an unACKed UART frame, so repeat it a few times once
         * the uplink is demonstrably healthy. The RP2040 ignores redundant
         * clears. */
        if (s_alert_clear_pending > 0 && wd_healthy &&
            now - last_clear_s >= ALERT_CLEAR_RESEND_S) {
            modem_ui_alert_clear();
            s_alert_clear_pending--;
            last_clear_s = now;
        }

        /* Preserve the existing disconnected-uplink recovery policy: a DNS
         * response holds escalation; failed probes permit the established
         * reset ladder. Neither result identifies the operator/backend root
         * cause. Connected/degraded MQTT and a stalled worker are held below. */
        if (now - last_healthy_s >= UPLINK_DEAD_SECS) {
            /* A live MQTT session with stale publication proof, or an SDK
             * worker stuck in its own call, is handled by the independent
             * MQTT monitor. A modem reset cannot safely unstick that task.
             * Keep reporting the outage without counting it as healthy. */
            mqtt_health_snapshot_t mqtt_health = {0};
            bool mqtt_owned_fault = false;
            if (backend_mode_get() == WUPS_BACKEND_MODE_MQTT) {
                mqtt_get_health(&mqtt_health);
                mqtt_owned_fault = mqtt_health.connected || mqtt_health.worker_stalled;
            }
            /* The hold must never cover a client that cannot even attempt
             * unless its owner is itself stalled (handled separately above).
             * Otherwise preserve the previous missing-client escalation. */
            bool client_missing = (backend_mode_get() == WUPS_BACKEND_MODE_MQTT &&
                                   !mqtt_sdk_is_started());
            bool inet_ok = (now - last_probe_ok_s < INET_PROBE_CACHE_S);
            if (!mqtt_owned_fault && !inet_ok && !client_missing && inet_probe()) {
                last_probe_ok_s = now;
                inet_ok = true;
                ESP_LOGW(MODEM_TAG,
                         "uplink dead %us but internet probes answer — "
                         "holding modem recovery (cause not yet established)",
                         (unsigned)(now - last_healthy_s));
            }
            if (mqtt_owned_fault || (inet_ok && !client_missing)) {
                /* Long hold (wedged client / marathon backend outage):
                 * surface the alert WITHOUT resetting the modem, so the
                 * unit is never silent forever. Re-asserted every tick —
                 * the RP2040 auto-clears a non-refreshed alert after 5 min
                 * and does not re-beep on a repeated identical text. */
                if (now - last_healthy_s >= UPLINK_HOLD_ALERT_S) {
                    if (!s_alert_active) {
                        ESP_LOGE(MODEM_TAG,
                                 "uplink dead %us (%s) — raising 'NO UPLINK' "
                                 "alert without modem reset",
                                 (unsigned)(now - last_healthy_s),
                                 mqtt_owned_fault ? "MQTT owner/proof degraded"
                                                  : "internet probes answer");
                    }
                    s_fail_stage = MODEM_FAIL_UPLINK;
                    s_alert_active = true;
                    modem_ui_alert(modem_fail_msg(s_fail_stage));
                }
                continue;
            }
            if (ppp_events_take_loss())
                return TEARDOWN_NORMAL;
            if (fw_ota_in_progress()) {
                last_healthy_s = now_s();
                continue;
            }
            if (s_cmux_active) modem_network_snapshot("uplink recovery");
            /* The final AT snapshot can overlap a new OTA or a real loss. */
            if (ppp_events_take_loss())
                return TEARDOWN_NORMAL;
            if (fw_ota_in_progress()) {
                last_healthy_s = now_s();
                continue;
            }
            now = now_s();
            s_uplink_trips++;
            s_fail_stage = MODEM_FAIL_NET;
            s_fails_since_ok++;
            ESP_LOGE(MODEM_TAG,
                     "uplink DEAD for %us with PPP up (zombie PDP?) — trip #%d, "
                     "%s",
                     (unsigned)(now - last_healthy_s), s_uplink_trips,
                     s_uplink_trips >= 2 ? "escalating to PWRKEY power-cycle"
                                         : "resetting module (AT+CFUN=1,1)");
            return s_uplink_trips >= 2 ? TEARDOWN_PWRCYCLE
                                       : TEARDOWN_MODULE_RESET;
        }
    }
}

/* --- supervisor task ----------------------------------------------------- */

/*
 * Long-lived task. Brings PPP up, runs first-boot smoke tests + starts the
 * MQTT client, then watches for PPP_LOST_IP / PPP_FAIL and tears down +
 * recreates the DCE. The MQTT owner keeps the client across PPP cycles and
 * owns reconnect timing; this task never waits for an MQTT SDK operation.
 *
 * Backoff: doubles from PPP_BACKOFF_MIN_MS up to PPP_BACKOFF_MAX_MS.
 * If we hit PPP_FAILS_BEFORE_PWRCYCLE bring-ups in a row without an IP we
 * pulse PWRKEY (full hardware power cycle) to recover from a wedged radio.
 */
static void ppp_supervisor_task(void *arg)
{
    (void)arg;

    /* esp_modem requires a default event loop + esp_netif initialized. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Set up the PPP netif (one-shot — kept across DCE recreation). */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    assert(s_ppp_netif);

    s_modem_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                               on_netif_ppp_status, NULL));

    uint32_t backoff_ms   = PPP_BACKOFF_MIN_MS;
    int      consecutive_fails = 0;

    for (;;) {
        ppp_events_begin_attempt();

        teardown_action_t teardown = TEARDOWN_NORMAL;
        esp_err_t err = ppp_bringup_dce();
        if (err == ESP_OK) {
            ppp_phase_t phase = ppp_events_wait_for_link(PPP_GOT_IP_TIMEOUT_MS);

            if (phase == PPP_UP) {
                ESP_LOGI(MODEM_TAG, "PPP up — TCP/IP stack is on the cellular interface");
                consecutive_fails = 0;
                backoff_ms = PPP_BACKOFF_MIN_MS;
                if (s_uplink_trips == 0 && s_fail_stage != MODEM_FAIL_UPLINK) {
                    /* NOTE: a zero-trip NO-UPLINK hold alert (backend outage
                     * with healthy internet) is deliberately NOT cleared
                     * here — PPP-up proves nothing about the backend; the
                     * supervision loop's wd-healthy branch clears it. */
                    s_fails_since_ok = 0;
                    s_fail_stage = MODEM_FAIL_NONE;
                    if (s_alert_active) {
                        s_alert_active = false;
                        modem_ui_alert_clear();
                        /* The clear frame is unACKed — schedule re-sends from
                         * the supervision loop once the uplink is healthy, so
                         * a single lost UART frame can't latch the banner. */
                        s_alert_clear_pending = ALERT_CLEAR_RESENDS;
                        ESP_LOGI(MODEM_TAG, "modem recovered — clearing OLED/buzzer alert");
                    }
                } else {
                    /* Mid uplink-watchdog escalation: a zombie PDP hands out
                     * IPs just fine, so PPP-up alone proves nothing. Recovery
                     * bookkeeping (fail counters, alert clear) moves to the
                     * supervision loop's uplink-healthy branch — otherwise a
                     * persistent zombie would reset the alert machinery every
                     * cycle and never surface. */
                    ESP_LOGW(MODEM_TAG,
                             "PPP up after uplink trip #%d — waiting for the "
                             "uplink itself before declaring recovery",
                             s_uplink_trips);
                }

                if (s_cmux_active) modem_network_snapshot("PPP up");
                if (ppp_events_take_loss()) goto stop_ppp;

                if (!s_initial_connectivity_checked) {
                    /* Do this once per boot, independently of asynchronous
                     * MQTT startup; a pending request must not rerun probes. */
                    s_initial_connectivity_checked = true;
                    wait_for_time_sync(15000);
                    if (ppp_events_take_loss()) goto stop_ppp;
                    run_http_get_test();
                }
                if (ppp_events_take_loss()) goto stop_ppp;

                const wups_backend_mode_t mode = backend_mode_get();
                if (mode == WUPS_BACKEND_MODE_HTTP) {
                    /* Idempotent across PPP reconnects. */
                    ESP_LOGI(MODEM_TAG, "starting HTTP control-mode backend...");
                    http_backend_start();
                } else if (mode != WUPS_BACKEND_MODE_MQTT) {
                    ESP_LOGI(MODEM_TAG,
                             "skipping MQTT client start — backend mode is %s",
                             backend_mode_name(mode));
                } else if (!s_iccid_known) {
                    ESP_LOGE(MODEM_TAG,
                             "ICCID unknown — refusing to start MQTT. "
                             "Check SIM card / AT+CCID handling.");
                } else if (mqtt_client_start() != ESP_OK) {
                    ESP_LOGW(MODEM_TAG, "MQTT start request unavailable — will retry");
                }

                /* First net.status of the session, seeded from the CSQ read
                 * during bring-up (the supervision loop refreshes it on CMUX
                 * sessions). The MQTT owner caches the frame even while
                 * asynchronous topic/client initialization is pending. */
                modem_diag_clock_log("PPP ready");
                emit_net_status(NET_STATE_PPP_UP, s_bringup_rssi_dbm, 0, 0);

                /* Supervise until the link drops or the uplink watchdog
                 * trips (see supervise_uplink). Either way, we tear down
                 * and rebuild. */
                teardown = supervise_uplink();
            } else if (phase == PPP_DOWN) {
                ESP_LOGE(MODEM_TAG, "PPP setup failed");
                s_fail_stage = MODEM_FAIL_NET;   /* ICCID was read; radio/network side */
                consecutive_fails++;
                s_fails_since_ok++;
            } else {
                ESP_LOGE(MODEM_TAG, "PPP setup timed out (%d ms)",
                         PPP_GOT_IP_TIMEOUT_MS);
                s_fail_stage = MODEM_FAIL_NET;
                consecutive_fails++;
                s_fails_since_ok++;
            }
        } else {
            consecutive_fails++;
            s_fails_since_ok++;
        }

stop_ppp:
        ppp_teardown_dce(teardown);

        /* When degraded, power-cycle the modem more often: a hot-inserted SIM
         * or restored signal is only picked up on a modem re-init, so frequent
         * cycles make recovery-after-fix fast (~a minute) instead of waiting
         * out the full backoff. */
        int pwrcycle_thresh = s_alert_active ? 2 : PPP_FAILS_BEFORE_PWRCYCLE;
        if (s_reg_timeout_streak > 0) {
            /* Failing on registration timeout: a power-cycle would restart
             * the modem's network search from scratch, so it is almost
             * always counterproductive — keep it only as a rare last resort
             * against a wedged radio stack. */
            pwrcycle_thresh = REG_TIMEOUT_PWRCYCLE_EVERY;
        }
        if (s_fail_stage != MODEM_FAIL_RADIO && s_fail_stage != MODEM_FAIL_AWAKE &&
            consecutive_fails >= pwrcycle_thresh) {
            ESP_LOGW(MODEM_TAG,
                     "%d bring-up failures — power-cycling modem",
                     consecutive_fails);
            /* Probe-based full cycle that ends with a freshly booted modem
             * whatever state it wedged in (incl. boot delays). */
            modem_power_cycle(false);
            consecutive_fails = 0;
            s_reg_timeout_streak = 0;   /* fresh boot = fresh search budget */
        }

        /* Surface / re-assert the visible alert once we've failed enough
         * times in a row — independent of the power-cycle cadence above, so
         * the operator is told in ~a minute rather than after several slow
         * cycles. The modem power-cycle recovery keeps running in parallel;
         * a successful IP clears the alert. Re-sent each round so a rebooted
         * RP2040 re-shows it. */
        if (s_fails_since_ok >= MODEM_FAILS_BEFORE_ALERT) {
            const char *m = modem_fail_msg(s_fail_stage);
            if (!s_alert_active) {
                s_alert_active = true;
                ESP_LOGE(MODEM_TAG,
                         "modem DEGRADED (%d fails) — surfacing '%s' on OLED/buzzer",
                         s_fails_since_ok, m);
            }
            modem_ui_alert(m);
        }

        /* Cap the backoff short while degraded so we keep retrying (and
         * re-cycling) frequently until the fault clears; normal cap otherwise. */
        uint32_t backoff_cap = s_alert_active && s_fail_stage != MODEM_FAIL_RADIO &&
                              s_fail_stage != MODEM_FAIL_AWAKE
            ? 10000u : PPP_BACKOFF_MAX_MS;
        if (backoff_ms > backoff_cap) backoff_ms = backoff_cap;
        ESP_LOGI(MODEM_TAG, "backing off %u ms before retry",
                 (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > backoff_cap) backoff_ms = backoff_cap;
    }
}

#if MODEM_UART_DIAG
/* Send one AT command on the raw UART and log the reply on a single line.
 * Used only for bench bring-up (MODEM_UART_DIAG). */
static void diag_send_at(const char *cmd, int wait_ms)
{
    static uint8_t buf[512];
    uart_flush_input(MODEM_UART);
    uart_write_bytes(MODEM_UART, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART, "\r\n", 2);
    int n = uart_read_bytes(MODEM_UART, buf, sizeof(buf) - 1, pdMS_TO_TICKS(wait_ms));
    if (n > 0) {
        buf[n] = '\0';
        for (int i = 0; i < n; i++) if (buf[i] == '\r' || buf[i] == '\n') buf[i] = ' ';
        ESP_LOGI(MODEM_TAG, "DIAG %-10s -> [%d] %s", cmd, n, (char *)buf);
    } else {
        ESP_LOGW(MODEM_TAG, "DIAG %-10s -> TIMEOUT (0 bytes)", cmd);
    }
}

/* Raw UART AT-sweep diagnostic: owns UART1 directly (no esp_modem), never
 * power-cycles the modem. Confirms the level-translator path and reports SIM,
 * signal, ICCID and registration so we can see exactly where bring-up stops. */
static void raw_uart_diag_task(void *arg)
{
    (void)arg;
    uart_config_t cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MODEM_UART, 2048, 0, 0, NULL, 0);
    uart_param_config(MODEM_UART, &cfg);
    uart_set_pin(MODEM_UART, MODEM_TX_GPIO, MODEM_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGW(MODEM_TAG, "MODEM_UART_DIAG: raw AT sweep on UART%d TX=GPIO%d RX=GPIO%d @ %d "
                        "(no esp_modem, no power-cycle)",
             MODEM_UART, MODEM_TX_GPIO, MODEM_RX_GPIO, MODEM_BAUD);
    bool scanned = false;  /* one-shot AT+COPS=? — blocks the loop ~2.5 min */
    for (;;) {
        ESP_LOGI(MODEM_TAG, "================ DIAG AT sweep ================");
        diag_send_at("AT",        1000);  /* sanity / link alive            */
        diag_send_at("ATE0",      1000);  /* echo off for cleaner replies   */
        diag_send_at("AT+CPIN?",  2000);  /* SIM ready? (READY / SIM PIN..) */
        diag_send_at("AT+CICCID", 2000);  /* SIM7080G ICCID command         */
        diag_send_at("AT+CCID",   2000);  /* legacy ICCID command (compare) */
        diag_send_at("AT+CIMI",   2000);  /* IMSI — proves SIM file access  */
        diag_send_at("AT+CRSM=176,12258,0,0,10", 3000); /* read EF_ICCID raw */
        diag_send_at("AT+CSQ",    2000);  /* signal (rssi,ber); 99=unknown  */
        diag_send_at("AT+CGREG?", 2000);  /* GPRS registration             */
        diag_send_at("AT+CEREG?", 2000);  /* LTE/EPS registration          */
        diag_send_at("AT+COPS?",  3000);  /* operator                      */
        diag_send_at("AT+CGNAPN", 3000);  /* network-assigned APN          */
        diag_send_at("AT+CNMP?",  1000);  /* preferred network mode        */
        diag_send_at("AT+CMNB?",  1000);  /* Cat-M / NB-IoT preference     */
        diag_send_at("AT+CFUN?",  1000);  /* RF functionality (4 = radio OFF — same
                                           * signature as a dead antenna path) */
        diag_send_at("AT+CBANDCFG?", 2000); /* configured Cat-M / NB bands */
        diag_send_at("AT+CPSI?",  2000);  /* serving cell / NO SERVICE     */
        if (!scanned) {
            scanned = true;
            ESP_LOGW(MODEM_TAG, "DIAG: one-shot AT+COPS=? full network scan "
                                "(up to 150 s — the definitive RF-path test)");
            diag_send_at("AT+COPS=?", 150000);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* MODEM_UART_DIAG */

void modem_at_pass_through_start(void)
{
#if MODEM_UART_DIAG
    /* Bench bring-up: raw AT sweep instead of PPP/esp_modem. Modem stays ON. */
    xTaskCreate(raw_uart_diag_task, "uart_diag", 4096, NULL, 5, NULL);
    ESP_LOGW(MODEM_TAG, "MODEM_UART_DIAG active — raw UART AT sweep (PPP/esp_modem disabled)");
#else
    /* Despite the legacy name, this now spawns the PPP supervisor task
     * (espressif/esp_modem). It brings PPP up, starts esp-mqtt on first
     * success, and then runs forever — re-creating the DCE whenever PPP
     * drops so production devices don't go silent on a transient cellular
     * outage. */
    xTaskCreate(ppp_supervisor_task, "ppp_sup", 8192, NULL, 5, NULL);
    ESP_LOGI(MODEM_TAG, "PPP supervisor task started (esp_modem)");
#endif
}
