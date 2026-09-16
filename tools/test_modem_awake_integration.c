#include "modem_awake_policy.h"

#include <stdarg.h>
#include <stdint.h>
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
enum { ESP_OK = 0, GPIO_MODE_OUTPUT = 2, GPIO_PULLUP_DISABLE = 0,
       GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0,
       MODEM_FAIL_NONE = 0, MODEM_FAIL_AWAKE = 91 };
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define MODEM_TAG "modem"
#define pdMS_TO_TICKS(ms) (ms)

typedef enum {
    PSM_ENABLED, EDRX_ENABLED, PSM_TIMEOUT, EDRX_TIMEOUT,
    LEGACY_UNSUPPORTED, EMPTY_ACTIVE_READBACK
} failure_kind;

static struct {
    unsigned gpio_calls, fail_gpio_call;
    uint64_t low_latch, output_enabled;
    unsigned success_attempt, attempt, psm_reads, edrx_reads, rf_off_calls;
    unsigned delay_calls, at_calls;
    uint64_t elapsed_ms;
    failure_kind failure;
    bool fail_rf_off;
    char trace[16][32];
    char log[4096];
    size_t log_length;
} host;
static int s_fail_stage;
/* OTA UART selection is exercised by test_modem_uart_boot_policy.py. */
static void modem_select_boot_baud(void) {}

static void log_message(const char *tag, const char *format, ...)
{
    (void)tag;
    CHECK(host.log_length < sizeof(host.log));
    va_list args;
    va_start(args, format);
    int n = vsnprintf(host.log + host.log_length,
                      sizeof(host.log) - host.log_length, format, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n < sizeof(host.log) - host.log_length);
    host.log_length += (size_t)n;
}
#define ESP_LOGI(...) log_message(__VA_ARGS__)
#define ESP_LOGW(...) log_message(__VA_ARGS__)
#define ESP_LOGE(...) log_message(__VA_ARGS__)

static esp_err_t gpio_result(void)
{
    ++host.gpio_calls;
    return host.gpio_calls == host.fail_gpio_call
               ? -(esp_err_t)(100 + host.gpio_calls) : ESP_OK;
}

static esp_err_t gpio_set_level(int pin, unsigned level)
{
    CHECK(pin == 1 || pin == 5); /* Actual board PWRKEY and DTR, never RI. */
    CHECK(level == 0);
    CHECK(host.output_enabled == 0); /* Latch must be written BEFORE enable. */
    CHECK(host.at_calls == 0);
    esp_err_t result = gpio_result();
    if (result == ESP_OK) host.low_latch |= UINT64_C(1) << pin;
    return result;
}

static esp_err_t gpio_config(const gpio_config_t *config)
{
    const uint64_t wanted = (UINT64_C(1) << 1) | (UINT64_C(1) << 5);
    CHECK(config != NULL);
    CHECK(config->pin_bit_mask == wanted);
    CHECK((host.low_latch & wanted) == wanted);
    CHECK(config->mode == GPIO_MODE_OUTPUT);
    CHECK(config->pull_up_en == GPIO_PULLUP_DISABLE);
    CHECK(config->pull_down_en == GPIO_PULLDOWN_DISABLE);
    CHECK(config->intr_type == GPIO_INTR_DISABLE);
    CHECK(host.at_calls == 0);
    esp_err_t result = gpio_result();
    if (result == ESP_OK) host.output_enabled = config->pin_bit_mask;
    return result;
}

static void vTaskDelay(unsigned ticks)
{
    CHECK(ticks == 1000);
    CHECK(host.rf_off_calls == 0); /* No reset/RF transition between attempts. */
    CHECK(host.attempt > 0 && host.attempt < 3);
    ++host.delay_calls;
    host.elapsed_ms += ticks;
}

/* Adapter boundary: deliver complete AT replies to the REAL verifier. Unknown
 * commands are fatal, so adding a setter/reset to retry code cannot pass. */
static bool radio_at(void *context, const char *command, char *response,
                     size_t capacity, unsigned timeout_ms)
{
    CHECK(context == NULL);
    CHECK(response != NULL && capacity == MODEM_RADIO_RESPONSE_CAPACITY);
    CHECK(host.at_calls < sizeof(host.trace) / sizeof(host.trace[0]));
    CHECK(strlen(command) < sizeof(host.trace[0]));
    strcpy(host.trace[host.at_calls++], command);
    const char *reply = "\r\nOK\r\n";
    bool complete = true;
    if (!strcmp(command, "AT+CFUN=4")) {
        CHECK(timeout_ms == 10000);
        CHECK(host.attempt == 3 && host.delay_calls == 2);
        CHECK(host.rf_off_calls == 0);
        ++host.rf_off_calls;
        complete = !host.fail_rf_off;
        host.elapsed_ms += complete ? 1 : timeout_ms;
    } else {
        CHECK(timeout_ms == 3000);
        CHECK(host.rf_off_calls == 0);
        if (!strcmp(command, "AT+CPSMRDP")) {
            ++host.attempt;
            ++host.psm_reads;
            CHECK(host.attempt <= 3);
            bool pending = host.success_attempt == 0 || host.attempt < host.success_attempt;
            reply = "\r\n+CPSMRDP: 0,20,3600,20,3600,0\r\nOK\r\n";
            if (pending && host.failure == PSM_ENABLED)
                reply = "\r\n+CPSMRDP: 1\r\nOK\r\n";
            if (pending && host.failure == PSM_TIMEOUT) complete = false;
        } else if (!strcmp(command, "AT+CEDRXRDP")) {
            ++host.edrx_reads;
            bool pending = host.success_attempt == 0 || host.attempt < host.success_attempt;
            reply = "\r\n+CEDRXRDP: 0\r\nOK\r\n";
            if (pending && host.failure == EDRX_ENABLED)
                reply = "\r\n+CEDRXRDP: 4,\"0001\",\"0010\",\"0011\"\r\nOK\r\n";
            if (pending && host.failure == EDRX_TIMEOUT) complete = false;
            if (pending && host.failure == LEGACY_UNSUPPORTED) {
                reply = "\r\nERROR\r\n";
                complete = false;
            }
            if (pending && host.failure == EMPTY_ACTIVE_READBACK)
                reply = "\r\nOK\r\n";
        } else {
            fprintf(stderr, "Unexpected AT mutation/command: %s\n", command);
            CHECK(false);
        }
        /* For the max-time case both queries consume their whole allowance. */
        host.elapsed_ms += host.failure == EDRX_TIMEOUT ? timeout_ms
                            : complete ? 1 : timeout_ms;
    }
    CHECK(strlen(reply) < capacity);
    strcpy(response, reply);
    return complete;
}

#include "modem_awake_integration.inc"

static void reset_host(void)
{
    memset(&host, 0, sizeof(host));
    s_fail_stage = MODEM_FAIL_NONE;
}

static void test_gpio_init(void)
{
    reset_host();
    CHECK(modem_init() == ESP_OK);
    CHECK(host.gpio_calls == 3);
    CHECK(host.output_enabled == ((UINT64_C(1) << 1) | (UINT64_C(1) << 5)));
    CHECK(host.at_calls == 0 && host.delay_calls == 0);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        reset_host();
        host.fail_gpio_call = failure;
        CHECK(modem_init() == -(esp_err_t)(100 + failure));
        CHECK(host.gpio_calls == failure); /* Stop at the failing SDK call. */
        CHECK(host.output_enabled == 0);
        CHECK(host.at_calls == 0 && host.delay_calls == 0);
    }
}

static void test_verification(failure_kind failure, unsigned success_attempt,
                              bool fail_rf_off)
{
    reset_host();
    host.failure = failure;
    host.success_attempt = success_attempt;
    host.fail_rf_off = fail_rf_off;
    CHECK(modem_awake_check_before_ppp() == (success_attempt != 0));
    unsigned attempts = success_attempt ? success_attempt : 3;
    unsigned edrx_reads = (failure == PSM_ENABLED || failure == PSM_TIMEOUT)
                              ? (success_attempt ? 1 : 0) : attempts;
    CHECK(host.attempt == attempts && host.psm_reads == attempts);
    CHECK(host.edrx_reads == edrx_reads);
    CHECK(host.delay_calls == attempts - 1);
    CHECK(host.gpio_calls == 0); /* Verification cannot pulse PWRKEY/DTR. */
    CHECK(host.rf_off_calls == (success_attempt ? 0u : 1u));
    CHECK(host.at_calls == attempts + edrx_reads + host.rf_off_calls);
    CHECK(s_fail_stage == (success_attempt ? MODEM_FAIL_NONE : MODEM_FAIL_AWAKE));
    CHECK(host.elapsed_ms <= 30000); /* 3 x 6s reads + 2 x 1s + final 10s. */
    CHECK(strstr(host.log, "awake policy verified") != NULL || !success_attempt);
    if (!success_attempt) {
        CHECK(!strcmp(host.trace[host.at_calls - 1], "AT+CFUN=4"));
        CHECK(strstr(host.log, "PPP blocked") != NULL);
        CHECK(strstr(host.log, "awake policy verified") == NULL);
    }
}

int main(void)
{
    test_gpio_init();
    test_verification(PSM_ENABLED, 1, false);
    test_verification(PSM_ENABLED, 2, false);
    test_verification(EDRX_ENABLED, 3, false);
    test_verification(PSM_TIMEOUT, 2, false);
    test_verification(EDRX_TIMEOUT, 3, false);
    test_verification(PSM_ENABLED, 0, false);
    test_verification(EDRX_ENABLED, 0, false);
    test_verification(LEGACY_UNSUPPORTED, 0, false);
    test_verification(EMPTY_ACTIVE_READBACK, 0, false);
    test_verification(EDRX_TIMEOUT, 0, true);
    printf("modem_awake_integration: PASS (%u checks; actual init + retry functions, real active verifier)\n", checks);
    return 0;
}
