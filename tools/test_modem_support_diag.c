#include "modem_support_diag.h"
#include "esp_err.h"

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

struct esp_modem_dce_wrap { int unused; };
static struct esp_modem_dce_wrap dce;
static const char *const allowed[] = {
    "AT+CGDCONT?\r", "AT+CPSMS?\r", "AT+CPSMRDP\r",
    "AT+CEDRXS?\r", "AT+CEDRX?\r", "AT+CEDRXRDP\r",
    "AT+CSCLK?\r", "AT+CPSMCFG?\r", "AT+CPSMCFGEXT?\r",
};
#define NCOMMANDS (sizeof(allowed) / sizeof(allowed[0]))
#define MAX_CALLS 64u
#define FLAG_LOSS 1u
#define FLAG_OTA 2u
#define FLAG_DATA 4u

typedef enum { NORMAL, TIMEOUT, FALSE_OK, NULL_DATA } action_kind;
typedef struct {
    const char *text;
    size_t length;
    size_t fragment;
    unsigned duration_ms;
    unsigned flags_after;
    bool empty_callback;
    action_kind kind;
} action;

static action actions[MAX_CALLS];
static unsigned indices[MAX_CALLS], timeouts[MAX_CALLS], calls, clock_calls;
static char clock_commands[MAX_CALLS][32];
static unsigned clock_cost_ms[MAX_CALLS], clock_flags_after[MAX_CALLS];
static unsigned flags;
static int64_t now_us;
static char logs[131072];
static size_t log_length;

static void reset_fake(void)
{
    memset(actions, 0, sizeof(actions));
    memset(indices, 0, sizeof(indices));
    memset(timeouts, 0, sizeof(timeouts));
    memset(clock_commands, 0, sizeof(clock_commands));
    memset(clock_cost_ms, 0, sizeof(clock_cost_ms));
    memset(clock_flags_after, 0, sizeof(clock_flags_after));
    calls = clock_calls = flags = 0;
    now_us = 1000000;
    logs[0] = '\0';
    log_length = 0;
}

int64_t esp_timer_get_time(void) { return now_us; }

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "OTHER";
    }
}

void test_log(const char *tag, const char *format, ...)
{
    CHECK(tag != NULL);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(logs + log_length, sizeof(logs) - log_length, format, args);
    va_end(args);
    CHECK(n >= 0 && (size_t)n + 2 < sizeof(logs) - log_length);
    log_length += (size_t)n;
    logs[log_length++] = '\n';
    logs[log_length] = '\0';
}

void modem_diag_clock_log(const char *command)
{
    CHECK(clock_calls < MAX_CALLS);
    CHECK(strlen(command) < sizeof(clock_commands[0]));
    CHECK(strchr(command, '\r') == NULL && strchr(command, '\n') == NULL);
    strcpy(clock_commands[clock_calls], command);
    now_us += (int64_t)clock_cost_ms[clock_calls] * 1000;
    flags |= clock_flags_after[clock_calls];
    ++clock_calls;
}

static bool ready(void *context)
{
    CHECK(context == &flags);
    return flags == 0;
}

esp_err_t esp_modem_command(esp_modem_dce_t *device, const char *command,
                            esp_err_t (*callback)(uint8_t *, size_t), uint32_t timeout_ms)
{
    CHECK(device == &dce && callback != NULL);
    CHECK(calls < MAX_CALLS);
    CHECK(flags == 0); /* Never issue a command after loss, OTA or DATA. */
    CHECK(timeout_ms > 0 && timeout_ms <= MODEM_SUPPORT_DIAG_COMMAND_MS);
    unsigned index = 0;
    while (index < NCOMMANDS && strcmp(command, allowed[index])) ++index;
    CHECK(index < NCOMMANDS); /* Exact allowlist rejects every setter. */
    CHECK(clock_calls > calls);
    CHECK(strlen(clock_commands[clock_calls - 1]) + 1 == strlen(command));
    CHECK(!memcmp(clock_commands[clock_calls - 1], command, strlen(command) - 1));
    indices[calls] = index;
    timeouts[calls] = timeout_ms;
    action a = actions[calls++];
    unsigned duration = a.duration_ms < timeout_ms ? a.duration_ms : timeout_ms;
    now_us += (int64_t)duration * 1000;
    esp_err_t result = ESP_ERR_NOT_FINISHED;
    if (a.empty_callback) CHECK(callback(NULL, 0) == ESP_ERR_NOT_FINISHED);
    if (a.kind == NULL_DATA) {
        result = callback(NULL, 1);
    } else {
        const char *text = a.text ? a.text : "\r\nOK\r\n";
        size_t length = a.length ? a.length : strlen(text);
        size_t step = a.fragment ? a.fragment : length;
        for (size_t n = step < length ? step : length;;) {
            result = callback((uint8_t *)text, n);
            if (result != ESP_ERR_NOT_FINISHED || n == length) break;
            n = length - n > step ? n + step : length;
        }
    }
    flags |= a.flags_after;
    if (a.kind == TIMEOUT) {
        CHECK(result == ESP_ERR_NOT_FINISHED);
        return ESP_ERR_TIMEOUT;
    }
    if (a.kind == FALSE_OK) return ESP_OK;
    CHECK(result == ESP_OK || result == ESP_FAIL);
    return result;
}

static void run(void) { modem_support_diag_run(&dce, ready, &flags); }

static void expect_order(unsigned first, unsigned count)
{
    CHECK(calls == count);
    for (unsigned i = 0; i < count; ++i) CHECK(indices[i] == first + i);
}

static unsigned occurrences(const char *text)
{
    unsigned count = 0;
    for (const char *p = logs; (p = strstr(p, text)) != NULL; p += strlen(text)) ++count;
    return count;
}

static void finish_tail(unsigned first)
{
    reset_fake();
    run();
    expect_order(first, (unsigned)NCOMMANDS - first);
    reset_fake();
}

static void test_full(void)
{
    char response[4096];
    size_t used = 0;
    for (unsigned i = 1; i <= 15; ++i) {
        int n = snprintf(response + used, sizeof(response) - used,
                         "+CGDCONT: %u,\"IPV4V6\",\"ROW%02u-long-access-point-name\","
                         "\"0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\",0,0,0\r\n", i, i);
        CHECK(n > 0 && (size_t)n < sizeof(response) - used);
        used += (size_t)n;
    }
    CHECK(used > 512);
    strcpy(response + used, "OK\r\n");
    actions[0] = (action){.text=response, .fragment=7, .empty_callback=true};
    actions[1] = (action){.text="AT+CPSMS?\r\n+CPSMS: 0,,,\"01100000\",\"00000000\"\r\nOK\r\n", .fragment=1};
    run();
    expect_order(0, NCOMMANDS);
    CHECK(clock_calls == NCOMMANDS);
    for (unsigned i = 1; i <= 15; ++i) {
        char marker[16];
        snprintf(marker, sizeof(marker), "ROW%02u-", i);
        CHECK(occurrences(marker) == 1); /* Cumulative fragments are not appended twice. */
    }
    CHECK(strstr(logs, "status=OVERFLOW") == NULL);
    CHECK(strstr(logs, "+CPSMS: 0,,,") != NULL);
    reset_fake();
    run();
    expect_order(0, NCOMMANDS); /* Completed snapshots always restart at the first command. */
}

static void test_errors(void)
{
    actions[0].text = "\r\nERROR\r\n";
    actions[1].text = "+CME ERROR: 3\r\n";
    actions[2].text = "+CMS ERROR: 500\r\n";
    run();
    expect_order(0, NCOMMANDS);
    CHECK(occurrences("status=ERROR") == 6);
    CHECK(strstr(logs, "AT [AT+CGDCONT?] ERROR") != NULL);
    CHECK(strstr(logs, "+CME ERROR: 3") != NULL);
    CHECK(strstr(logs, "+CMS ERROR: 500") != NULL);
}

static void test_timeout(void)
{
    actions[0] = (action){.text="+CGDCONT: 1,\"IP\",\"partial\"\r\nO", .fragment=5,
                          .kind=TIMEOUT, .duration_ms=1500};
    run();
    expect_order(0, 1);
    CHECK(strstr(logs, "INCOMPLETE/TIMEOUT") != NULL);
    CHECK(strstr(logs, "partial") != NULL);
    finish_tail(1);
    actions[0] = (action){.text="+CGDCONT: partial\r\n", .kind=FALSE_OK};
    run();
    expect_order(0, 1);
    CHECK(strstr(logs, "INCOMPLETE/TRANSPORT") != NULL);
    finish_tail(1);
}

static void test_invalid(void)
{
    static const char nul[] = "SAFE\r\nBAD\0LINE\r\nOK\r\n";
    static const char escape[] = "SAFE\r\nBAD\x1b[31m\r\nOK\r\n";
    static const char high[] = "SAFE\r\nBAD\x80\r\nOK\r\n";
    static const char conflict[] = "SAFE\r\nOK\r\nERROR\r\n";
    const char *bad[] = {nul, escape, high, conflict};
    const size_t sizes[] = {sizeof(nul)-1, sizeof(escape)-1, sizeof(high)-1, sizeof(conflict)-1};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        actions[0] = (action){.text=bad[i], .length=sizes[i]};
        run();
        expect_order(0, 1);
        CHECK(strstr(logs, "status=INVALID") != NULL);
        CHECK(strstr(logs, "AT [AT+CGDCONT?] SAFE") != NULL);
        CHECK(strchr(logs, '\x1b') == NULL);
        CHECK(strchr(logs, '\x80') == NULL);
        finish_tail(1);
    }
    actions[0].kind = NULL_DATA;
    run();
    expect_order(0, 1);
    CHECK(strstr(logs, "status=INVALID") != NULL);
    finish_tail(1);
}

static void test_bounds(void)
{
    char response[MODEM_SUPPORT_DIAG_CAPACITY + 1];
    memset(response, 'X', sizeof(response));
    memcpy(response, "BOUND-START-", 12);
    const char suffix[] = "-BOUND-END\nOK\r\n";
    memcpy(response + MODEM_SUPPORT_DIAG_CAPACITY - 1 - (sizeof(suffix) - 1),
           suffix, sizeof(suffix) - 1);
    actions[0] = (action){.text=response, .length=MODEM_SUPPORT_DIAG_CAPACITY - 1};
    run();
    expect_order(0, NCOMMANDS);
    CHECK(strstr(logs, "BOUND-START-") != NULL && strstr(logs, "-BOUND-END") != NULL);
    CHECK(strstr(logs, "status=OVERFLOW") == NULL);
    reset_fake();
    actions[0] = (action){.text=response, .length=MODEM_SUPPORT_DIAG_CAPACITY};
    run();
    expect_order(0, 1);
    CHECK(strstr(logs, "status=OVERFLOW/TRUNCATED") != NULL);
    CHECK(strstr(logs, "captured=8191") != NULL);
    CHECK(strstr(logs, "BOUND-START-") != NULL);
    finish_tail(1);
}

static void test_deadline(void)
{
    for (unsigned i = 0; i < NCOMMANDS; ++i) actions[i].duration_ms = 1500;
    run();
    expect_order(0, 4);
    CHECK(now_us == 7000000);
    reset_fake();
    for (unsigned i = 0; i < NCOMMANDS; ++i) actions[i].duration_ms = 1500;
    run();
    expect_order(4, 4);
    CHECK(now_us == 7000000);
    finish_tail(8);
    for (unsigned i = 0; i < 4; ++i) actions[i].duration_ms = 1300;
    actions[4] = (action){.text="+PARTIAL: 1\r\n", .kind=TIMEOUT, .duration_ms=1500};
    run();
    expect_order(0, 5);
    CHECK(timeouts[4] == 800);
    CHECK(now_us == 7000000);
    finish_tail(5);
}

static void test_readiness(void)
{
    modem_support_diag_run(NULL, ready, &flags);
    modem_support_diag_run(&dce, NULL, &flags);
    CHECK(calls == 0 && clock_calls == 0);
    const unsigned reasons[] = {FLAG_LOSS, FLAG_OTA, FLAG_DATA};
    for (size_t i = 0; i < sizeof(reasons)/sizeof(reasons[0]); ++i) {
        flags = reasons[i];
        run();
        CHECK(calls == 0 && clock_calls == 0);
        flags = 0;
        actions[1].flags_after = reasons[i];
        run();
        expect_order(0, 2);
        finish_tail(2);
    }
}

static void test_clock_budget(void)
{
    clock_cost_ms[0] = 5900;
    actions[0] = (action){.text="+PARTIAL: 1\r\n", .kind=TIMEOUT, .duration_ms=1500};
    run();
    expect_order(0, 1);
    CHECK(timeouts[0] == 100 && now_us == 7000000);
    finish_tail(1);
    clock_cost_ms[0] = 6000;
    run();
    CHECK(calls == 0 && clock_calls == 1);
    reset_fake();
    clock_flags_after[0] = FLAG_OTA;
    run();
    CHECK(calls == 0 && clock_calls == 1);
    reset_fake();
    run();
    expect_order(0, NCOMMANDS); /* Not-yet-issued commands retain their cursor. */
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    reset_fake();
    if (!strcmp(argv[1], "full")) test_full();
    else if (!strcmp(argv[1], "errors")) test_errors();
    else if (!strcmp(argv[1], "timeout")) test_timeout();
    else if (!strcmp(argv[1], "invalid")) test_invalid();
    else if (!strcmp(argv[1], "bounds")) test_bounds();
    else if (!strcmp(argv[1], "deadline")) test_deadline();
    else if (!strcmp(argv[1], "readiness")) test_readiness();
    else if (!strcmp(argv[1], "clock_budget")) test_clock_budget();
    else CHECK(false);
    printf("modem_support_diag %s: %u checks, PASS\n", argv[1], checks);
    return 0;
}
