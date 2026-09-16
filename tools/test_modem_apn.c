#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = -2,
       ESP_MODEM_DCE_SIM7070 = 7070, ESP_MODEM_FLOW_CONTROL_NONE = 0,
       ESP_MODEM_MODE_CMUX = 1, ESP_MODEM_MODE_DATA = 2, UART_NUM_1 = 1,
       MODEM_FAIL_NONE, MODEM_FAIL_AT, MODEM_FAIL_RADIO, MODEM_FAIL_AWAKE,
       MODEM_FAIL_SIM, MODEM_FAIL_NET };
typedef struct {
    struct { int tx_io_num, rx_io_num, rts_io_num, cts_io_num,
                 flow_control, port_num, baud_rate; } uart_config;
} esp_modem_dte_config_t;
typedef struct { const char *apn; } esp_modem_dce_config_t;
typedef struct { char copied_apn[32]; } esp_modem_dce_t;
typedef bool (*radio_at_fn)(void *, const char *, char *, size_t, unsigned);
#define ESP_MODEM_DTE_DEFAULT_CONFIG() { .uart_config = {0} }
#define ESP_MODEM_DCE_DEFAULT_CONFIG(value) { .apn = (value) }
#define MODEM_TAG "modem"
#define pdMS_TO_TICKS(ms) (ms)

static esp_modem_dce_t *s_dce;
static int netif, *s_ppp_netif = &netif;
static int s_fail_stage, s_bringup_rssi_dbm, s_cmux_entry_fails, s_reg_timeout_streak;
static bool s_iccid_known, s_cmux_active, s_cmux_dirty;
static int64_t s_cmux_fallback_since_s;
typedef struct { int port, tx_gpio, rx_gpio; } modem_uart_baud_config_t;
static const modem_uart_baud_config_t s_modem_uart_config = {1, 2, 4};

static struct {
    esp_modem_dce_t dce;
    const char *sim_reply, *expected_apn;
    char iccid[24], initial_cached_apn[32], requested_apn[32], modem_apn[32];
    unsigned create_calls, identity_reads, cache_updates, apn_at_calls;
    unsigned rf_resume_calls, reg_calls, dial_calls, mode_calls;
    unsigned seed_logs, fixed_logs, warning_logs;
    bool rf_off, fail_sync, fail_apn_at, fail_sim_read, reject_identity, unregistered;
    bool fail_baud, baud_ready;
    int mode;
    int64_t clock_us;
} host;

static bool modem_uart_baud_prepare(const modem_uart_baud_config_t *config, int baud)
{
    CHECK(config == &s_modem_uart_config && baud == CONFIG_WUPS_MODEM_UART_BAUD);
    CHECK(host.create_calls == 0 && host.mode_calls == 0);
    host.baud_ready = !host.fail_baud;
    return host.baud_ready;
}

static void log_message(const char *tag, const char *format, ...)
{
    (void)tag;
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    CHECK(length >= 0 && (size_t)length < sizeof(text));
    if (strstr(text, "APN selected from ICCID list:")) ++host.seed_logs;
    if (strstr(text, "APN fixed by build profile:")) ++host.fixed_logs;
    if (strstr(text, "AT+CGDCONT failed")) ++host.warning_logs;
}
#define ESP_LOGI(...) log_message(__VA_ARGS__)
#define ESP_LOGW(...) log_message(__VA_ARGS__)
#define ESP_LOGE(...) log_message(__VA_ARGS__)

static const char *esp_err_to_name(esp_err_t result)
{
    return result == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static esp_modem_dce_t *esp_modem_new_dev(int model,
                                         const esp_modem_dte_config_t *dte,
                                         const esp_modem_dce_config_t *config,
                                         const void *ppp_netif)
{
    CHECK(model == ESP_MODEM_DCE_SIM7070 && dte != NULL && ppp_netif == &netif);
    CHECK(host.baud_ready && dte->uart_config.baud_rate == CONFIG_WUPS_MODEM_UART_BAUD);
    CHECK(config != NULL && strlen(config->apn) < sizeof(host.dce.copied_apn));
    /* The real SDK copies config->apn into PdpContext at construction. */
    strcpy(host.dce.copied_apn, config->apn);
    strcpy(host.initial_cached_apn, config->apn);
    ++host.create_calls;
    return &host.dce;
}

static esp_err_t esp_modem_sync(esp_modem_dce_t *dce)
{
    CHECK(dce == &host.dce);
    return ESP_OK;
}

static esp_err_t esp_modem_set_apn(esp_modem_dce_t *dce, const char *apn)
{
    CHECK(dce == &host.dce && host.create_calls == 1);
    CHECK(host.rf_off && host.rf_resume_calls == 0);
    CHECK(host.apn_at_calls == 0 && host.mode_calls == 0 && host.dial_calls == 0);
    CHECK(s_iccid_known && !strcmp(apn, host.expected_apn));
    CHECK(strlen(apn) < sizeof(dce->copied_apn));
    ++host.cache_updates;
    if (host.fail_sync) return ESP_FAIL;
    /* Models the public API's replacement of the cached PDP, without AT. */
    strcpy(dce->copied_apn, apn);
    return ESP_OK;
}

static esp_err_t esp_modem_at(esp_modem_dce_t *dce, const char *command,
                              char *response, unsigned timeout_ms)
{
    CHECK(dce == &host.dce && timeout_ms > 0);
    if (!strcmp(command, "AT+CMEE=2")) {
        CHECK(response == NULL);
    } else if (!strcmp(command, "AT+CGMR")) {
        CHECK(response != NULL);
        strcpy(response, "SIM7080 host fixture");
    } else if (!strcmp(command, "AT+CPIN?")) {
        CHECK(response != NULL);
        strcpy(response, "+CPIN: READY\r\nOK\r\n");
    } else if (!strcmp(command, "AT+CCID") || !strcmp(command, "AT+CICCID")) {
        CHECK(response != NULL && host.rf_off);
        ++host.identity_reads;
        if (host.fail_sim_read) return ESP_FAIL;
        CHECK(strlen(host.sim_reply) < 128);
        strcpy(response, host.sim_reply);
    } else if (!strncmp(command, "AT+CGDCONT=", 11)) {
        char expected[80];
        snprintf(expected, sizeof(expected), "AT+CGDCONT=1,\"IP\",\"%s\"", host.expected_apn);
        CHECK(!strcmp(command, expected));
        CHECK(response == NULL && timeout_ms == 3000);
        CHECK(host.rf_off && host.rf_resume_calls == 0);
        CHECK(host.cache_updates == 1 && host.mode_calls == 0 && !host.fail_sync);
        CHECK(!strcmp(dce->copied_apn, host.expected_apn));
        ++host.apn_at_calls;
        strcpy(host.requested_apn, host.expected_apn);
        if (host.fail_apn_at) return ESP_FAIL;
        strcpy(host.modem_apn, host.expected_apn);
    } else {
        fprintf(stderr, "Unexpected AT command: %s\n", command);
        CHECK(false);
    }
    return ESP_OK;
}

static esp_err_t esp_modem_get_imei(esp_modem_dce_t *dce, char *value)
{
    CHECK(dce == &host.dce && value != NULL);
    strcpy(value, "123456789012345");
    return ESP_OK;
}
static esp_err_t esp_modem_get_imsi(esp_modem_dce_t *dce, char *value)
{
    return esp_modem_get_imei(dce, value);
}
static esp_err_t esp_modem_get_module_name(esp_modem_dce_t *dce, char *value)
{
    return esp_modem_get_imei(dce, value);
}
static void identity_set_imei(const char *value) { CHECK(value != NULL); }
static const char *identity_iccid(void) { return host.iccid; }
static esp_err_t identity_set_iccid(const char *value)
{
    CHECK(value != NULL && strlen(value) < sizeof(host.iccid));
    if (host.reject_identity) return ESP_FAIL;
    CHECK(strlen(value) == 19);
    strcpy(host.iccid, value);
    return ESP_OK;
}
static esp_err_t esp_modem_get_signal_quality(esp_modem_dce_t *dce, int *rssi, int *ber)
{
    CHECK(dce == &host.dce && rssi != NULL && ber != NULL);
    *rssi = 20;
    *ber = 0;
    return ESP_OK;
}
static int csq_to_dbm(int csq) { return -113 + 2 * csq; }
static bool radio_at(void *ctx, const char *cmd, char *reply, size_t cap, unsigned ms)
{
    (void)ctx; (void)cmd; (void)reply; (void)cap; (void)ms;
    CHECK(false); /* Radio policy is a separately tested boundary here. */
    return false;
}
static bool modem_radio_prepare(radio_at_fn callback, void *ctx)
{
    CHECK(callback == radio_at && ctx == NULL && host.rf_resume_calls == 0);
    host.rf_off = true;
    return true;
}
static bool modem_awake_prepare(radio_at_fn callback, void *ctx)
{
    CHECK(callback == radio_at && ctx == NULL && host.rf_off);
    return true;
}
static bool modem_radio_resume(radio_at_fn callback, void *ctx)
{
    CHECK(callback == radio_at && ctx == NULL && host.rf_off);
    CHECK(host.cache_updates == 1 && host.apn_at_calls == 1 && !host.fail_sync);
    CHECK(!strcmp(host.dce.copied_apn, host.expected_apn));
    CHECK(!strcmp(host.requested_apn, host.expected_apn));
    if (!host.fail_apn_at) CHECK(!strcmp(host.modem_apn, host.expected_apn));
    ++host.rf_resume_calls;
    host.rf_off = false;
    return true;
}
static void modem_network_snapshot(const char *reason) { CHECK(reason != NULL); }
static void modem_support_snapshot(bool force) { CHECK(!force); }
static bool modem_awake_check_before_ppp(void)
{
    CHECK(host.rf_resume_calls == 1 && host.reg_calls > 0 && !host.unregistered);
    return true;
}
static int modem_reg_stat(void)
{
    CHECK(host.rf_resume_calls == 1 && !host.rf_off);
    ++host.reg_calls;
    return host.unregistered ? 2 : 5;
}
static int64_t esp_timer_get_time(void) { return host.clock_us; }
static int64_t now_s(void) { return host.clock_us / 1000000; }
static void vTaskDelay(unsigned ms) { host.clock_us += (int64_t)ms * 1000; }
static void ppp_events_start_dial(void)
{
    CHECK(host.rf_resume_calls == 1 && host.reg_calls > 0 && !host.unregistered);
    ++host.dial_calls;
}
static esp_err_t esp_modem_set_mode(esp_modem_dce_t *dce, int mode)
{
    CHECK(dce == &host.dce && host.dial_calls == 1);
    CHECK(mode == ESP_MODEM_MODE_CMUX || mode == ESP_MODEM_MODE_DATA);
    CHECK(!strcmp(dce->copied_apn, host.requested_apn));
    CHECK(!strcmp(dce->copied_apn, host.expected_apn));
    /* SDK setup_data_mode() sends the copied PDP APN again before ATD. */
    strcpy(host.modem_apn, dce->copied_apn);
    host.mode = mode;
    ++host.mode_calls;
    return ESP_OK;
}

#include "modem_apn_bringup.inc"

static const char *boot_apn;

/* Expected SIM-based APN in normal builds; fixed artifacts must override
 * it for both matching and opposite-profile ICCIDs in all scenarios. */
static const char *profile_apn(const char *automatic_apn)
{
    return WUPS_FIXED_APN[0] ? WUPS_FIXED_APN : automatic_apn;
}

static void cold_boot(const char *reply, const char *expected)
{
    memset(&host, 0, sizeof(host));
    host.sim_reply = reply;
    host.expected_apn = profile_apn(expected);
    strcpy(host.modem_apn, "previous.stored.apn");
    s_dce = NULL;
    s_apn = boot_apn;
    s_apn_seeded = false;
    s_iccid_known = false;
    s_fail_stage = MODEM_FAIL_NONE;
    s_cmux_active = s_cmux_dirty = false;
    s_cmux_entry_fails = s_reg_timeout_streak = 0;
    s_cmux_fallback_since_s = 0;
}

static void next_attempt(void)
{
    /* Only per-attempt host observations reset. Production boot state,
     * selected APN and SIM identity deliberately survive this new DCE. */
    host.create_calls = host.identity_reads = host.cache_updates = host.apn_at_calls = 0;
    host.rf_resume_calls = host.reg_calls = host.dial_calls = host.mode_calls = 0;
    host.seed_logs = host.fixed_logs = host.warning_logs = 0;
    host.rf_off = false;
    s_fail_stage = MODEM_FAIL_NONE;
    s_dce = NULL;
    s_cmux_active = s_cmux_dirty = false;
}

static void check_success(const char *initial_apn, bool first_attempt, bool data_mode)
{
    CHECK(ppp_bringup_dce() == ESP_OK);
    CHECK(s_fail_stage == MODEM_FAIL_NONE && s_apn_seeded && s_iccid_known);
    CHECK(!strcmp(host.initial_cached_apn, profile_apn(initial_apn)));
    CHECK(!strcmp(s_apn, host.expected_apn));
    CHECK(!strcmp(host.modem_apn, host.expected_apn));
    CHECK(host.identity_reads == (first_attempt ? 1u : 0u));
    CHECK(host.seed_logs == (first_attempt && !WUPS_FIXED_APN[0] ? 1u : 0u));
    CHECK(host.fixed_logs == (first_attempt && WUPS_FIXED_APN[0] ? 1u : 0u));
    CHECK(host.cache_updates == 1 && host.apn_at_calls == 1);
    CHECK(host.rf_resume_calls == 1 && host.mode_calls == 1);
    CHECK(host.mode == (data_mode ? ESP_MODEM_MODE_DATA : ESP_MODEM_MODE_CMUX));
}

static void test_profiles(void)
{
    static const char *const original_sim_replies[] = {
        "+CCID: 8988228066614189920\r\nOK\r\n",
        "+ICCID: 8988280666000338870\r\nOK\r\n",
        "8988280666000338871\r\nOK\r\n",
        "+CCID: 8988228066618136967\r\nOK\r\n",
        "+CCID: 8988228066618136966\r\nOK\r\n",
        /* The real ICCID parser normalizes the optional twentieth digit. */
        "+CCID: 89882280666141899201\r\nOK\r\n",
    };
    for (size_t i = 0; i < sizeof(original_sim_replies) / sizeof(original_sim_replies[0]); ++i) {
        cold_boot(original_sim_replies[i], "iot.1nce.net");
        check_success("sensor.net", true, false);
        next_attempt();
        /* Retries reuse the identity; a later AT response cannot reseed. */
        host.sim_reply = "+CCID: 8988228066614189921\r\nOK\r\n";
        check_success("iot.1nce.net", false, false);
    }
    cold_boot("+CCID: 8988228066614189921\r\nOK\r\n", "sensor.net");
    check_success("sensor.net", true, false); /* No prefix matching. */
    next_attempt();
    check_success("sensor.net", false, false);
    cold_boot("+CCID: 8988307000000000001\r\nOK\r\n", "sensor.net");
    s_cmux_entry_fails = CMUX_ENTRY_FAILS_MAX;
    check_success("sensor.net", true, true);
    cold_boot(original_sim_replies[0], "iot.1nce.net");
    s_cmux_entry_fails = CMUX_ENTRY_FAILS_MAX;
    check_success("sensor.net", true, true);
}

static void test_registration_retry(void)
{
    const char *replies[] = { "+CCID: 8988228066614189920\r\nOK\r\n",
                             "+CCID: 8988307000000000001\r\nOK\r\n" };
    const char *apns[] = { "iot.1nce.net", "sensor.net" };
    for (unsigned profile = 0; profile < 2; ++profile) {
        cold_boot(replies[profile], apns[profile]);
        host.unregistered = true;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            CHECK(ppp_bringup_dce() == ESP_FAIL);
            CHECK(s_fail_stage == MODEM_FAIL_NET && s_apn_seeded);
            CHECK(!strcmp(s_apn, profile_apn(apns[profile])));
            CHECK(!strcmp(host.initial_cached_apn,
                          attempt ? profile_apn(apns[profile]) : boot_apn));
            CHECK(!strcmp(host.dce.copied_apn, profile_apn(apns[profile])));
            CHECK(!strcmp(host.requested_apn, profile_apn(apns[profile])));
            CHECK(host.reg_calls > 1 && host.dial_calls == 0 && host.mode_calls == 0);
            CHECK(s_reg_timeout_streak == (int)attempt + 1);
            next_attempt();
        }
        host.unregistered = false;
        check_success(apns[profile], false, false);
        CHECK(s_reg_timeout_streak == 0);
    }
}

static void test_failures(void)
{
    cold_boot("+CCID: 8988228066618136966\r\nOK\r\n", "iot.1nce.net");
    host.fail_baud = true;
    CHECK(ppp_bringup_dce() == ESP_FAIL);
    CHECK(s_fail_stage == MODEM_FAIL_AT && s_dce == NULL);
    CHECK(host.create_calls == 0 && host.dial_calls == 0 && host.rf_resume_calls == 0);
    next_attempt();
    host.fail_baud = false;
    check_success("sensor.net", true, false);

    cold_boot("+CCID: 8988228066614189920\r\nOK\r\n", "iot.1nce.net");
    host.fail_sync = true;
    CHECK(ppp_bringup_dce() == ESP_FAIL);
    CHECK(s_fail_stage == MODEM_FAIL_NET && host.cache_updates == 1);
    CHECK(host.apn_at_calls == 0 && host.rf_resume_calls == 0 && host.mode_calls == 0);
    CHECK(host.rf_off && !strcmp(host.dce.copied_apn, profile_apn("sensor.net")));
    CHECK(s_apn_seeded && !strcmp(s_apn, profile_apn("iot.1nce.net")));
    next_attempt();
    host.fail_sync = false;
    check_success("iot.1nce.net", false, false);

    cold_boot("+CCID: 8988228066614189920\r\nOK\r\n", "iot.1nce.net");
    host.fail_apn_at = true;
    if (WUPS_FIXED_APN[0]) {
        CHECK(ppp_bringup_dce() == ESP_FAIL);
        CHECK(s_fail_stage == MODEM_FAIL_NET && host.rf_off);
        CHECK(host.cache_updates == 1 && host.apn_at_calls == 1);
        CHECK(host.rf_resume_calls == 0 && host.dial_calls == 0 && host.mode_calls == 0);
        CHECK(!strcmp(host.requested_apn, WUPS_FIXED_APN));
        CHECK(!strcmp(host.modem_apn, "previous.stored.apn"));
        next_attempt();
        host.fail_apn_at = false;
        check_success(WUPS_FIXED_APN, false, false);
    } else {
        check_success("sensor.net", true, false);
        CHECK(host.warning_logs == 1); /* Existing best-effort attach policy. */
    }

    cold_boot("malformed SIM response", "sensor.net");
    host.reject_identity = true;
    CHECK(ppp_bringup_dce() == ESP_FAIL);
    CHECK(s_fail_stage == MODEM_FAIL_SIM && !s_apn_seeded && !s_iccid_known);
    CHECK(host.cache_updates == 0 && host.apn_at_calls == 0 && host.rf_resume_calls == 0);
    next_attempt();
    host.reject_identity = false;
    host.sim_reply = "+CCID: 8988228066614189920\r\nOK\r\n";
    host.expected_apn = profile_apn("iot.1nce.net");
    check_success("sensor.net", true, false);

    cold_boot("", "sensor.net");
    host.fail_sim_read = true;
    CHECK(ppp_bringup_dce() == ESP_FAIL);
    CHECK(s_fail_stage == MODEM_FAIL_SIM && !s_apn_seeded && !s_iccid_known);
    CHECK(host.cache_updates == 0 && host.apn_at_calls == 0 && host.rf_resume_calls == 0);
}

int main(void)
{
    boot_apn = s_apn;
    CHECK(!strcmp(boot_apn, profile_apn("sensor.net")));
    test_profiles();
    test_registration_retry();
    test_failures();
    printf("modem_apn: %u checks PASS (profile %s; production bring-up, cross-profile SIM, first boot/retry, cache/AT order and failure gates)\n",
           checks, WUPS_FIXED_APN[0] ? WUPS_FIXED_APN : "auto ICCID");
    return 0;
}
