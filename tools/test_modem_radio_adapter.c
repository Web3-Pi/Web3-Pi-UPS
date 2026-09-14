#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem_radio_policy.h"
#include "modem_signal.h"

typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL = -1, ESP_ERR_NOT_FINISHED = 0x10c, ESP_ERR_TIMEOUT = 0x107 };
#define MODEM_TAG "modem-host"
static void *s_dce = (void *)(uintptr_t)1;
static bool inside_sdk;
static char logs[8192];
static size_t log_length;
static unsigned sdk_calls, callbacks;
static const char *expected_command;
static const uint8_t *reply_bytes;
static size_t reply_lengths[8];
static esp_err_t callback_results[8];
static unsigned reply_count;
static esp_err_t transport_result;

static void capture_log(const char *tag, const char *format, ...)
{
    (void)tag; assert(!inside_sdk); /* logging must occur after command returns */
    va_list arguments; va_start(arguments, format);
    int n = vsnprintf(logs + log_length, sizeof(logs) - log_length, format, arguments);
    va_end(arguments);
    assert(n >= 0 && (size_t)n + 2 < sizeof(logs) - log_length);
    log_length += (size_t)n; logs[log_length++] = '\n'; logs[log_length] = '\0';
}
#define ESP_LOGI(...) capture_log(__VA_ARGS__)
static const char *esp_err_to_name(esp_err_t error)
{
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_FAIL) return "ESP_FAIL";
    if (error == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
    return "ESP_ERR_NOT_FINISHED";
}
static void modem_diag_clock_log(const char *event) { assert(event && !inside_sdk); }
static esp_err_t esp_modem_command(void *dce, const char *command,
                                  esp_err_t (*callback)(uint8_t *, size_t), unsigned timeout);

/* Generated directly from modem.c at test time: no copied implementation. */
#include "modem_radio_adapter.inc"

static esp_err_t esp_modem_command(void *dce, const char *command,
                                  esp_err_t (*callback)(uint8_t *, size_t), unsigned timeout)
{
    assert(dce == s_dce && !inside_sdk && timeout == 3000);
    assert(strcmp(command, expected_command) == 0);
    assert(s_radio_reply.length == 0 && !s_radio_reply.invalid && s_radio_reply.data[0] == '\0');
    ++sdk_calls; inside_sdk = true;
    for (unsigned i = 0; i < reply_count; ++i) {
        esp_err_t result = callback((uint8_t *)reply_bytes, reply_lengths[i]);
        ++callbacks; assert(result == callback_results[i]);
        if (result != ESP_ERR_NOT_FINISHED) assert(i + 1 == reply_count);
    }
    inside_sdk = false; return transport_result;
}
static void plan(const char *command, const void *bytes, size_t length, esp_err_t result)
{
    expected_command = command; reply_bytes = bytes;
    reply_count = bytes ? 1 : 0; reply_lengths[0] = length;
    callback_results[0] = result; transport_result = result;
    if (result == ESP_ERR_NOT_FINISHED) transport_result = ESP_ERR_TIMEOUT;
    sdk_calls = callbacks = 0; log_length = 0; logs[0] = '\0';
}
static unsigned occurrences(const char *haystack, const char *needle)
{
    unsigned result = 0; size_t length = strlen(needle);
    for (const char *p = haystack; (p = strstr(p, needle)); p += length) ++result;
    return result;
}
static void assert_printable_logs(void)
{
    for (size_t i = 0; i < log_length; ++i)
        assert(logs[i] == '\n' || logs[i] == '\t' || ((unsigned char)logs[i] >= 32 && (unsigned char)logs[i] <= 126));
}
static void test_cumulative(void)
{
    char output[512];
    const char cnmp[] = "AT+CNMP?\r\n+CNMP: 38\r\n\r\nOK\r\n";
    plan("AT+CNMP?\r", cnmp, strlen(cnmp), ESP_OK);
    reply_count = 3;
    reply_lengths[0] = strlen("AT+CNMP?\r\n");
    reply_lengths[1] = strlen(cnmp) - 1; /* final CR is not a complete terminal line */
    reply_lengths[2] = strlen(cnmp);
    callback_results[0] = callback_results[1] = ESP_ERR_NOT_FINISHED; callback_results[2] = ESP_OK;
    assert(radio_at(NULL, "AT+CNMP?", output, sizeof(output), 3000));
    assert(strcmp(output, cnmp) == 0 && callbacks == 3 && sdk_calls == 1);
    assert(occurrences(logs, "+CNMP: 38") == 1 && strstr(logs, "complete=1"));

    const char bands[] = "\r\n+CBANDCFG: \"CAT-M\",3,20\r\n+CBANDCFG: \"NB-IOT\",1,3,8,20\r\nOK\r\n";
    plan("AT+CBANDCFG?\r", bands, strlen(bands), ESP_OK);
    reply_count = 3;
    reply_lengths[0] = strlen("\r\n+CBANDCFG: \"CAT-M\",3,20\r\n");
    reply_lengths[1] = strlen(bands) - 3; /* partial terminal O */
    reply_lengths[2] = strlen(bands);
    callback_results[0] = callback_results[1] = ESP_ERR_NOT_FINISHED; callback_results[2] = ESP_OK;
    assert(radio_at(NULL, "AT+CBANDCFG?", output, sizeof(output), 3000));
    assert(strcmp(output, bands) == 0 && callbacks == 3);
    assert(occurrences(logs, "+CBANDCFG: \"CAT-M\",3,20") == 1);
    assert(occurrences(logs, "+CBANDCFG: \"NB-IOT\",1,3,8,20") == 1);
    assert_printable_logs();
    puts("PASS actual AT collector: cumulative CNMP/two-line CBANDCFG retained once; partial OK waits; logs only after SDK return");
}
static void test_failures(void)
{
    char output[512];
    memset(&s_radio_reply, 0, sizeof(s_radio_reply));
    assert(radio_reply_cb(NULL, 0) == ESP_ERR_NOT_FINISHED && !s_radio_reply.invalid);
    assert(radio_reply_cb(NULL, 1) == ESP_FAIL && s_radio_reply.invalid);
    const char error[] = "\r\n+CME ERROR: operation not allowed\r\n";
    plan("AT+COPS?\r", error, strlen(error), ESP_FAIL);
    assert(!radio_at(NULL, "AT+COPS?", output, sizeof(output), 3000) && !output[0]);
    assert(strstr(logs, "+CME ERROR: operation not allowed") && strstr(logs, "complete=0"));
    assert_printable_logs();
    const char incomplete[] = "\r\n+COPS: 0,0,\"TEST\",7\r\nO";
    plan("AT+COPS?\r", incomplete, strlen(incomplete), ESP_ERR_NOT_FINISHED);
    assert(!radio_at(NULL, "AT+COPS?", output, sizeof(output), 3000) && !output[0]);
    assert(strstr(logs, "ESP_ERR_TIMEOUT") && strstr(logs, "+COPS:"));
    plan("AT+COPS?\r", NULL, 0, ESP_ERR_TIMEOUT);
    assert(!radio_at(NULL, "AT+COPS?", output, sizeof(output), 3000) && !output[0] && callbacks == 0);

    const uint8_t forbidden[] = {0, 1, 27, 127, 128};
    for (size_t i = 0; i < sizeof(forbidden); ++i) {
        uint8_t binary[] = {'X', forbidden[i], '\r', '\n', 'O', 'K', '\r', '\n'};
        plan("AT+CPSI?\r", binary, sizeof(binary), ESP_FAIL);
        assert(!radio_at(NULL, "AT+CPSI?", output, sizeof(output), 3000) && !output[0]);
        assert_printable_logs();
    }
    char oversized[512]; memset(oversized, 'A', sizeof(oversized));
    plan("AT+CPSI?\r", oversized, sizeof(oversized), ESP_FAIL);
    assert(!radio_at(NULL, "AT+CPSI?", output, sizeof(output), 3000) && !output[0]);
    assert(strstr(logs, "INVALID/OVERSIZE") && !strstr(logs, "AAAA"));
    puts("PASS actual AT adapter: readable modem errors retained; partial/silent timeout and binary/oversized replies fail closed without log injection");
}
static void test_capacity(void)
{
    char maximum[512], output[512]; memset(maximum, 'A', 505);
    memcpy(maximum + 505, "\r\nOK\r\n", 7);
    plan("AT+CPSI?\r", maximum, 511, ESP_OK);
    assert(radio_at(NULL, "AT+CPSI?", output, sizeof(output), 3000));
    assert(memcmp(output, maximum, sizeof(output)) == 0);
    struct { uint8_t before; char bytes[511]; uint8_t after; } small;
    memset(&small, 0x5a, sizeof(small));
    plan("AT+CPSI?\r", maximum, 511, ESP_OK);
    assert(!radio_at(NULL, "AT+CPSI?", small.bytes, sizeof(small.bytes), 3000));
    assert(!small.bytes[0] && small.before == 0x5a && small.after == 0x5a);
    plan("AT+CPSI?\r", maximum, 511, ESP_OK); transport_result = ESP_ERR_TIMEOUT;
    assert(!radio_at(NULL, "AT+CPSI?", output, sizeof(output), 3000) && !output[0]);
    puts("PASS actual AT adapter: 511-byte boundary, caller-capacity canaries and transport result govern success");
}
static void test_cpsi(void)
{
    const char cpsi[] = "\r\n+URC: 10,20,30,40\r\n+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-110,-900,-700,12\r\n+CEREG: 2,5,\"cafe\",\"123\",7\r\n+URC: 1,2,3,4\r\nOK\r\n";
    plan("AT+CPSI?\r", cpsi, strlen(cpsi), ESP_OK);
    int8_t rsrp = 0, rsrq = 0, sinr = WUPS_NET_SINR_UNKNOWN;
    poll_cpsi(&rsrp, &rsrq, &sinr);
    assert(rsrp == -90 && rsrq == -11 && sinr == 4);
    assert(strstr(logs, "+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-110,-900,-700,12"));
    const char direct[] = "\r\n+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-10,-95,-70,12\r\nOK\r\n";
    plan("AT+CPSI?\r", direct, strlen(direct), ESP_OK); poll_cpsi(&rsrp, &rsrq, &sinr);
    assert(rsrp == -95 && rsrq == -10 && sinr == 4);
    const char no_service[] = "\r\n+CPSI: NO SERVICE\r\n+URC: LTE,1,2,-100,-900,-700,12\r\nOK\r\n";
    plan("AT+CPSI?\r", no_service, strlen(no_service), ESP_OK);
    rsrp = 7; rsrq = 8; sinr = WUPS_NET_SINR_UNKNOWN;
    poll_cpsi(&rsrp, &rsrq, &sinr);
    assert(rsrp == 7 && rsrq == 8 && sinr == WUPS_NET_SINR_UNKNOWN);
    plan("AT+CPSI?\r", NULL, 0, ESP_ERR_TIMEOUT);
    poll_cpsi(&rsrp, &rsrq, &sinr);
    assert(rsrp == 7 && rsrq == 8 && sinr == WUPS_NET_SINR_UNKNOWN);
    puts("PASS actual CPSI poll: RSRP/RSRQ/SINR from CPSI only, comma-bearing URCs ignored, dB/tenths handled, no-service/timeout leave outputs unchanged");
}
int main(void)
{
    test_cumulative(); test_failures(); test_capacity(); test_cpsi();
    puts("ALL MODEM RADIO ADAPTER TESTS PASSED");
    return 0;
}
