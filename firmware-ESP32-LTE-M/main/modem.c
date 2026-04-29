#include "modem.h"
#include "mqtt.h"

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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define MODEM_TAG "modem"

#define MODEM_PWR_GPIO    41
#define MODEM_DTR_GPIO    42
#define MODEM_RI_GPIO      3
#define MODEM_RX_GPIO      4   /* ESP32 RX ← Modem TX */
#define MODEM_TX_GPIO      5   /* ESP32 TX → Modem RX */

#define MODEM_UART        UART_NUM_1
#define MODEM_BAUD        115200

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

static EventGroupHandle_t s_modem_evt;
static esp_netif_t       *s_ppp_netif;
static esp_modem_dce_t   *s_dce;

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

esp_err_t modem_power_on(void)
{
    /* Pulse sequence per LilyGo example & SIM7080G hardware design. */
    ESP_LOGI(MODEM_TAG, "pulsing PWRKEY (GPIO%d) for 1s to power modem on...", MODEM_PWR_GPIO);
    gpio_set_level(MODEM_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(MODEM_PWR_GPIO, 0);
    ESP_LOGI(MODEM_TAG, "PWRKEY released — modem boot in progress");
    return ESP_OK;
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
        xEventGroupSetBits(s_modem_evt, EVT_GOT_IP);
        break;
    }
    case IP_EVENT_PPP_LOST_IP:
        ESP_LOGW(MODEM_TAG, "PPP lost IP");
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
        return ESP_ERR_TIMEOUT;
    }

    /* A few sanity-check at-level reads before going to data mode. */
    char buf[64] = {0};
    if (esp_modem_get_imei(s_dce, buf) == ESP_OK)        ESP_LOGI(MODEM_TAG, "IMEI: %s", buf);
    if (esp_modem_get_imsi(s_dce, buf) == ESP_OK)        ESP_LOGI(MODEM_TAG, "IMSI: %s", buf);
    if (esp_modem_get_module_name(s_dce, buf) == ESP_OK) ESP_LOGI(MODEM_TAG, "module: %s", buf);
    int rssi = 99, ber = 99;
    if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(MODEM_TAG, "signal: rssi=%d ber=%d", rssi, ber);
    }

    /* Switch to data (PPP) mode — esp_modem now drives UART and lwIP picks up. */
    ESP_LOGI(MODEM_TAG, "switching modem to PPP/data mode...");
    esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(MODEM_TAG, "esp_modem_set_mode(DATA) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* Tear down the DCE and free its UART driver so the next bring-up can claim
 * the port cleanly. Best-effort exit from data mode first; we don't care if
 * it fails (likely the link is already down). */
static void ppp_teardown_dce(void)
{
    if (!s_dce) return;
    esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGW(MODEM_TAG, "set_mode(COMMAND) on teardown failed (%s) — "
                            "continuing with destroy",
                 esp_err_to_name(err));
    }
    esp_modem_destroy(s_dce);
    s_dce = NULL;
    ESP_LOGI(MODEM_TAG, "DCE destroyed");
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

    bool     mqtt_started = false;
    uint32_t backoff_ms   = PPP_BACKOFF_MIN_MS;
    int      consecutive_fails = 0;

    for (;;) {
        /* Clear any stale event bits from a previous iteration so we don't
         * trip on a LOST_IP that was already serviced. */
        xEventGroupClearBits(s_modem_evt,
                             EVT_GOT_IP | EVT_LOST_IP | EVT_PPP_FAIL);

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

                if (!mqtt_started) {
                    /* Wall-clock time, needed by TLS cert validity check. */
                    wait_for_time_sync(15000);

                    /* End-to-end proof from C: hit a public HTTP server
                     * through lwIP → PPP → modem → 1nce → internet. */
                    run_http_get_test();

                    ESP_LOGI(MODEM_TAG, "starting MQTT client...");
                    if (mqtt_client_start() == ESP_OK) {
                        mqtt_started = true;
                    } else {
                        ESP_LOGE(MODEM_TAG, "mqtt_client_start failed");
                    }
                } else {
                    ESP_LOGI(MODEM_TAG, "PPP reconnected — esp-mqtt will resume on its own");
                }

                /* Block until the link drops. PPP_LOST_IP is the normal
                 * disconnect path; PPP_FAIL covers user-initiated/auth
                 * errors. Either way, we tear down and rebuild. */
                xEventGroupWaitBits(s_modem_evt,
                                    EVT_LOST_IP | EVT_PPP_FAIL,
                                    pdFALSE, pdFALSE,
                                    portMAX_DELAY);
                ESP_LOGW(MODEM_TAG, "PPP link lost — tearing down DCE");
            } else if (bits & EVT_PPP_FAIL) {
                ESP_LOGE(MODEM_TAG, "PPP setup failed");
                consecutive_fails++;
            } else {
                ESP_LOGE(MODEM_TAG, "PPP setup timed out (%d ms)",
                         PPP_GOT_IP_TIMEOUT_MS);
                consecutive_fails++;
            }
        } else {
            consecutive_fails++;
        }

        ppp_teardown_dce();

        if (consecutive_fails >= PPP_FAILS_BEFORE_PWRCYCLE) {
            ESP_LOGW(MODEM_TAG,
                     "%d consecutive bring-up failures — power-cycling modem",
                     consecutive_fails);
            /* PWRKEY pulse on a powered modem toggles it off, then on again.
             * The 8-second post-PWRKEY boot delay matches main.c's initial
             * wait so the SIM7080G has time to be ready for AT. */
            modem_power_on();
            vTaskDelay(pdMS_TO_TICKS(8000));
            consecutive_fails = 0;
        }

        ESP_LOGI(MODEM_TAG, "backing off %u ms before retry",
                 (unsigned)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms *= 2;
        if (backoff_ms > PPP_BACKOFF_MAX_MS) backoff_ms = PPP_BACKOFF_MAX_MS;
    }
}

void modem_at_pass_through_start(void)
{
    /* Despite the legacy name, this now spawns the PPP supervisor task
     * (espressif/esp_modem). It brings PPP up, starts esp-mqtt on first
     * success, and then runs forever — re-creating the DCE whenever PPP
     * drops so production devices don't go silent on a transient cellular
     * outage. */
    xTaskCreate(ppp_supervisor_task, "ppp_sup", 8192, NULL, 5, NULL);
    ESP_LOGI(MODEM_TAG, "PPP supervisor task started (esp_modem)");
}
