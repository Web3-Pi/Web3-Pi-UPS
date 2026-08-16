#include "modem.h"
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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define MODEM_TAG "modem"

/* W3P MODEM V1 (M.2) pinout — ESP32-S3FH4R2 ↔ SIM7080G UART1.
 * (Was LilyGo T-SIM7080G-S3: PWRKEY=41, DTR=42, RI=3, RX=4, TX=5. GPIO3 is a
 *  strapping pin and is left NC on the M.2 card.) Only PWRKEY/TX/RX are used
 *  in software; DTR/RI are routed but not driven here. */
#define MODEM_PWR_GPIO     1   /* ESP_PWRKEY    → SIM7080G PWRKEY */
#define MODEM_TX_GPIO      2   /* ESP_UART1_TXD : ESP TX → Modem RX */
#define MODEM_RX_GPIO      4   /* ESP_UART1_RXD : ESP RX ← Modem TX */
#define MODEM_DTR_GPIO     5   /* ESP_UART1_DTR (not driven in SW) */
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

#define APN_1NCE_IOT      "iot.1nce.net"

#define EVT_GOT_IP        BIT0
#define EVT_LOST_IP       BIT1
#define EVT_PPP_FAIL      BIT2

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

static EventGroupHandle_t s_modem_evt;
static esp_netif_t       *s_ppp_netif;
static esp_modem_dce_t   *s_dce;
static bool               s_iccid_known;       /* set true once AT+CCID populated identity */
static bool               s_mqtt_started;

/* PPP holds an IP right now — single-word read, safe from any task. Used by
 * fw_ota to refuse an update with no link (OTA-1). */
static volatile bool      s_ppp_up;

bool modem_ppp_is_up(void) { return s_ppp_up; }

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
    MODEM_FAIL_SIM,    /* AT ok, but no SIM/ICCID (CPIN/CCID failed) */
    MODEM_FAIL_NET,    /* SIM ok, but no registration / no PPP IP    */
} modem_fail_t;

static modem_fail_t s_fail_stage;
static int          s_fails_since_ok;   /* consecutive bring-up failures; 0 on IP */
static bool         s_alert_active;
static int          s_alert_clear_pending;  /* recovery clears still to re-send */

static const char *modem_fail_msg(modem_fail_t f)
{
    switch (f) {
    case MODEM_FAIL_SIM: return "SIM ERROR";
    case MODEM_FAIL_NET: return "NO NETWORK";
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

/* Optional RSRP/RSRQ from AT+CPSI? (Cat-M form ends ...,<RSRQ>,<RSRP>,
 * <RSSI>,<RSSNR>). Parsed defensively: SIM7080G FW revisions differ on
 * whether RSRP/RSRQ come in dB(m) or tenths, so out-of-range raw values are
 * re-tried as tenths and anything still implausible is dropped silently.
 * Outputs are left untouched (0 = unknown) on any surprise. */
static void poll_cpsi(int8_t *rsrp_out, int8_t *rsrq_out)
{
    char out[160] = {0};
    if (esp_modem_at(s_dce, "AT+CPSI?", out, 3000) != ESP_OK || !out[0]) return;
    if (!strstr(out, "LTE")) return;        /* "NO SERVICE" / GSM / parse guard */

    /* Collect the integer value of every comma field, keep the tail. */
    long vals[20];
    int  n = 0;
    for (char *p = strchr(out, ','); p && n < 20; p = strchr(p + 1, ',')) {
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
    wups_net_status_v1_t st = {0};
    st.version  = 1;
    st.state    = state;
    st.rssi_dBm = rssi_dbm;
    st.rsrp_dBm = rsrp;
    st.rsrq_dB  = rsrq;
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
    case WUPS_BACKEND_MODE_MQTT: {
        const char *topic = mqtt_topic_telemetry();
        if (topic[0]) {
            (void)mqtt_publish_raw(topic, frame, flen, 0, 0);
        }
        break;
    }
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
    /* PWRKEY GPIO: default LOW (= PWRKEY released through inverting transistor). */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << MODEM_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&pwr_cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(MODEM_PWR_GPIO, 0);

    ESP_LOGI(MODEM_TAG, "GPIO%d (PWRKEY) configured; UART will be owned by esp_modem",
             MODEM_PWR_GPIO);
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
    if (base != IP_EVENT) return;

    switch (id) {
    case IP_EVENT_PPP_GOT_IP: {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
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
        s_ppp_up = true;
        xEventGroupSetBits(s_modem_evt, EVT_GOT_IP);
        break;
    }
    case IP_EVENT_PPP_LOST_IP:
        ESP_LOGW(MODEM_TAG, "PPP lost IP");
        s_ppp_up = false;
        xEventGroupSetBits(s_modem_evt, EVT_LOST_IP);
        break;
    default:
        break;
    }
}

static void on_netif_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != NETIF_PPP_STATUS) return;
    /* PHASE_DEAD = link broken */
    if (id == NETIF_PPP_ERRORUSER) {
        ESP_LOGW(MODEM_TAG, "PPP error from user / disconnect");
        xEventGroupSetBits(s_modem_evt, EVT_PPP_FAIL);
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
    static const char *URL = "http://example.com/";

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
 * to acquire an IP — caller waits on EVT_GOT_IP / EVT_PPP_FAIL. */
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
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(APN_1NCE_IOT);

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
    esp_err_t err;
    if (s_cmux_entry_fails < CMUX_ENTRY_FAILS_MAX) {
        /* The CMUX transition ends in the ATD*99# dial, which fails whenever
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
                s_fail_stage = MODEM_FAIL_NET;
                s_reg_timeout_streak++;
                return ESP_FAIL;
            }
            s_reg_timeout_streak = 0;
        }
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
    s_ppp_up = false;
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
static void uplink_health(bool *up, bool *wd_ok)
{
    switch (backend_mode_get()) {
    case WUPS_BACKEND_MODE_MQTT:
        *up = mqtt_is_connected();
        *wd_ok = *up;
        break;
    case WUPS_BACKEND_MODE_HTTP: {
        if (!http_backend_is_configured()) {
            /* Endpoint not set yet (guided VPS setup / net.config pending) —
             * same "nothing to supervise" posture as unclaimed Arkiv below,
             * or the watchdog would reset a healthy modem forever. */
            *up = false;
            *wd_ok = true;
            break;
        }
        uint32_t last = http_backend_last_success_s();
        *up = last != 0 && (now_s() - last) <= HTTP_UPLINK_FRESH_SECS;
        *wd_ok = *up;
        break;
    }
    case WUPS_BACKEND_MODE_ARKIV:
        if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) {
            *up = false;
            *wd_ok = true;      /* unclaimed: no uplink expected, never trip */
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
        }
        break;
    default:
        *up = false;
        *wd_ok = true;          /* unknown mode — never trip */
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
    uint32_t last_healthy_s = now_s();   /* watchdog timer starts at GOT_IP */
    uint32_t last_clear_s   = now_s();   /* GOT_IP path just sent a clear    */

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_modem_evt,
                                               EVT_LOST_IP | EVT_PPP_FAIL,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(PPP_SUPERVISE_TICK_MS));
        if (bits & (EVT_LOST_IP | EVT_PPP_FAIL)) {
            ESP_LOGW(MODEM_TAG, "PPP link lost — tearing down DCE");
            return TEARDOWN_NORMAL;
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

        bool uplink_up = false, wd_healthy = false;
        uplink_health(&uplink_up, &wd_healthy);
        uint32_t now = now_s();
        if (wd_healthy) {
            last_healthy_s = now;
            /* OTA-1 rollback — first demonstrably healthy uplink marks a
             * pending-verify OTA image valid (one-shot, no-op otherwise). */
            fw_ota_mark_uplink_healthy();
            if (s_uplink_trips != 0) {
                /* Deferred recovery bookkeeping (see the GOT_IP branch):
                 * only a demonstrably healthy uplink ends an escalation. */
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
        }

        /* Signal-quality poll — CMUX sessions only (the DATA fallback has no
         * AT channel while PPP runs). rsrp/rsrq stay 0 (unknown) unless
         * AT+CPSI? parses cleanly. */
        int8_t rssi = s_ns_last_rssi, rsrp = 0, rsrq = 0;
        if (s_cmux_active) {
            int csq = 99, ber = 99;
            if (esp_modem_get_signal_quality(s_dce, &csq, &ber) == ESP_OK) {
                rssi = csq_to_dbm(csq);
            }
            poll_cpsi(&rsrp, &rsrq);
        }
        uint8_t state = uplink_up ? NET_STATE_MQTT_UP : NET_STATE_PPP_UP;
        int rssi_delta = (int)rssi - (int)s_ns_last_rssi;
        if (state != s_ns_last_state ||
            abs(rssi_delta) > NET_STATUS_RSSI_DELTA_DB ||
            now - s_ns_last_emit_s >= NET_STATUS_EMIT_PERIOD_S) {
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

        /* The watchdog itself: PPP holds an IP but the uplink has been dead
         * for UPLINK_DEAD_SECS — the zombie-PDP signature. Count it on the
         * existing NET-stage alert machinery and escalate. */
        if (now - last_healthy_s >= UPLINK_DEAD_SECS) {
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
 * recreates the DCE. esp-mqtt has its own reconnect timer, so it stays
 * started across PPP cycles and reconnects on its own once a route exists.
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
        /* Clear any stale event bits from a previous iteration so we don't
         * trip on a LOST_IP that was already serviced. */
        xEventGroupClearBits(s_modem_evt,
                             EVT_GOT_IP | EVT_LOST_IP | EVT_PPP_FAIL);

        teardown_action_t teardown = TEARDOWN_NORMAL;
        esp_err_t err = ppp_bringup_dce();
        if (err == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(
                s_modem_evt,
                EVT_GOT_IP | EVT_PPP_FAIL,
                pdFALSE, pdFALSE,
                pdMS_TO_TICKS(PPP_GOT_IP_TIMEOUT_MS));

            if (bits & EVT_GOT_IP) {
                ESP_LOGI(MODEM_TAG, "PPP up — TCP/IP stack is on the cellular interface");
                consecutive_fails = 0;
                backoff_ms = PPP_BACKOFF_MIN_MS;
                if (s_uplink_trips == 0) {
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

                if (!s_mqtt_started) {
                    /* Wall-clock time, needed by TLS cert validity check. */
                    wait_for_time_sync(15000);

                    /* End-to-end proof from C: hit a public HTTP server
                     * through lwIP → PPP → modem → 1nce → internet. */
                    run_http_get_test();

                    /* ADR-0012 — only start the EMQX client when this
                     * device is actually in MQTT mode. In Arkiv/HTTP mode
                     * the chain/user-endpoint is the only uplink and an
                     * extra MQTT client would just burn LTE data. */
                    const wups_backend_mode_t mode = backend_mode_get();
                    if (mode == WUPS_BACKEND_MODE_HTTP) {
                        /* HTTP-2 (§4.18a) — start the HTTP control-mode task.
                         * It self-paces POSTs to the user-hosted endpoint and
                         * needs no broker. Idempotent across PPP reconnects. */
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
                    } else {
                        ESP_LOGI(MODEM_TAG, "starting MQTT client...");
                        if (mqtt_client_start() == ESP_OK) {
                            s_mqtt_started = true;
                        } else {
                            ESP_LOGE(MODEM_TAG, "mqtt_client_start failed");
                        }
                    }
                } else {
                    ESP_LOGI(MODEM_TAG, "PPP reconnected — esp-mqtt will resume on its own");
                }

                /* First net.status of the session, seeded from the CSQ read
                 * during bring-up (the supervision loop refreshes it on CMUX
                 * sessions). Emitted after the backend start above so the
                 * MQTT telemetry topic is populated. */
                emit_net_status(NET_STATE_PPP_UP, s_bringup_rssi_dbm, 0, 0);

                /* Supervise until the link drops or the uplink watchdog
                 * trips (see supervise_uplink). Either way, we tear down
                 * and rebuild. */
                teardown = supervise_uplink();
            } else if (bits & EVT_PPP_FAIL) {
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
        if (consecutive_fails >= pwrcycle_thresh) {
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
        uint32_t backoff_cap = s_alert_active ? 10000u : PPP_BACKOFF_MAX_MS;
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
