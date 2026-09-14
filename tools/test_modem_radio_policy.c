#include "modem_radio_policy.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

typedef struct {
    unsigned network, preference, functionality;
    const char *bands;
    unsigned calls;
    const char *trace[64];
    unsigned fail_call; /* one-based, 0 disables */
    unsigned fail_kind; /* 0 transport, 1 ERROR, 2 unterminated, 3 empty */
    unsigned override_call;
    const char *override_response;
    bool fail_after_mutation;
    bool fail_all_off;
} fake_modem;

static fake_modem modem(bool desired)
{
    fake_modem result = {0};
    result.network = desired ? 38 : 2;
    result.preference = desired ? 1 : 3;
    result.functionality = 1;
    result.bands = desired ? "3,20" : "1,2,3,4,5,8,12,13,14,18,19,20,25,26,28,66";
    return result;
}

static bool fault(fake_modem *m, char *response, size_t capacity)
{
    if (m->fail_kind == 1) snprintf(response, capacity, "\r\n+CME ERROR: 3\r\n");
    else if (m->fail_kind == 2) memset(response, 'X', capacity);
    else if (m->fail_kind == 3) response[0] = '\0';
    return m->fail_kind != 0;
}

static bool fake_at(void *context, const char *command, char *response,
                    size_t capacity, unsigned timeout)
{
    fake_modem *m = context;
    CHECK(capacity == MODEM_RADIO_RESPONSE_CAPACITY);
    CHECK(timeout == (strstr(command, "AT+CFUN") == command ? 10000u : 3000u));
    CHECK(m->calls < sizeof(m->trace) / sizeof(m->trace[0]));
    if (m->calls >= sizeof(m->trace) / sizeof(m->trace[0])) return false;
    m->trace[m->calls++] = command;
    if (m->fail_all_off && !strcmp(command, "AT+CFUN=4")) return false;
    bool failing = m->fail_call == m->calls;
    if (failing && !m->fail_after_mutation) return fault(m, response, capacity);
    if (m->override_call == m->calls) {
        CHECK(strlen(m->override_response) < capacity);
        snprintf(response, capacity, "%s", m->override_response);
        return true;
    }
    if (!strcmp(command, "AT+CNMP?"))
        snprintf(response, capacity, "AT+CNMP?\r\n+CEREG: 2\r\n+CNMP: %u\r\nOK\r\n", m->network);
    else if (!strcmp(command, "AT+CMNB?"))
        snprintf(response, capacity, "\r\n+CMNB: %u\r\nOK\r\n", m->preference);
    else if (!strcmp(command, "AT+CBANDCFG?"))
        snprintf(response, capacity, "+CBANDCFG: \"CAT-M\",%s\r\n+CBANDCFG: \"NB-IOT\",1,2,3,8,20\r\nOK\r\n", m->bands);
    else if (!strcmp(command, "AT+CFUN?"))
        snprintf(response, capacity, "+CFUN: %u\r\nOK\r\n", m->functionality);
    else {
        if (!strcmp(command, "AT+CFUN=4")) m->functionality = 4;
        else if (!strcmp(command, "AT+CFUN=1")) m->functionality = 1;
        else if (!strcmp(command, "AT+CNMP=38")) m->network = 38;
        else if (!strcmp(command, "AT+CMNB=1")) m->preference = 1;
        else if (!strcmp(command, "AT+CBANDCFG=\"CAT-M\",3,20")) m->bands = "3,20";
        else CHECK(false); /* No broader RAT/band fallback is permitted. */
        snprintf(response, capacity, "\r\nOK\r\n");
    }
    return failing ? fault(m, response, capacity) : true;
}

static void trace_is(fake_modem *m, const char *const *expected, size_t count)
{
    CHECK(m->calls == count);
    for (size_t i = 0; i < count && i < m->calls; ++i)
        CHECK(!strcmp(m->trace[i], expected[i]));
}

static bool saw_enable(const fake_modem *m)
{
    for (unsigned i = 0; i < m->calls; ++i)
        if (!strcmp(m->trace[i], "AT+CFUN=1")) return true;
    return false;
}

static void ended_off(const fake_modem *m)
{
    CHECK(m->calls > 0);
    CHECK(!strcmp(m->trace[m->calls - 1], "AT+CFUN=4"));
    CHECK(m->functionality == 4);
}

static void test_terminal_parser(void)
{
    static const struct {
        const char *text;
        modem_radio_response_status_t wanted;
    } cases[] = {
        {"", MODEM_RADIO_RESPONSE_WAIT},
        {"\r\n", MODEM_RADIO_RESPONSE_WAIT},
        {"O", MODEM_RADIO_RESPONSE_WAIT},
        {"OK", MODEM_RADIO_RESPONSE_WAIT},
        {"OK\r", MODEM_RADIO_RESPONSE_WAIT},
        {"OK\r\n", MODEM_RADIO_RESPONSE_OK},
        {"\r\n \tOK \r\n", MODEM_RADIO_RESPONSE_OK},
        {"AT+CNMP?\r\n+CNMP: 38\r\nOK\r\n", MODEM_RADIO_RESPONSE_OK},
        {"+CEREG: 2\r\nOK\r\n+CEREG: 5\r\n", MODEM_RADIO_RESPONSE_OK},
        {"+TEXT: NOT OK\r\n", MODEM_RADIO_RESPONSE_WAIT},
        {"BROKEN_OK\r\n", MODEM_RADIO_RESPONSE_WAIT},
        {"+TEXT: ERROR inside data\r\nOK\r\n", MODEM_RADIO_RESPONSE_OK},
        {"ERROR\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {" +CME ERROR: SIM busy\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"+CMS ERROR: 500\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"NO CARRIER\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"NO ANSWER\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"BUSY\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"OK\r\nERROR\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"ERROR\r\nOK\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"OK\r\nOK\r\n", MODEM_RADIO_RESPONSE_ERROR},
        {"OK\r\n\x01", MODEM_RADIO_RESPONSE_ERROR},
        {"OK\r\n\xff", MODEM_RADIO_RESPONSE_ERROR},
        {"+CME ERROR: 1", MODEM_RADIO_RESPONSE_WAIT},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        CHECK(modem_radio_response_status(cases[i].text, strlen(cases[i].text)) == cases[i].wanted);
    CHECK(modem_radio_response_status(NULL, 0) == MODEM_RADIO_RESPONSE_WAIT);
    CHECK(modem_radio_response_status(NULL, 1) == MODEM_RADIO_RESPONSE_ERROR);
    const char nul[] = "OK\r\n\0+CME ERROR: 1\r\n";
    CHECK(modem_radio_response_status(nul, sizeof(nul) - 1) == MODEM_RADIO_RESPONSE_ERROR);
    char large[MODEM_RADIO_RESPONSE_CAPACITY + 1];
    memset(large, ' ', sizeof(large));
    memcpy(large, "OK\n", 3);
    CHECK(modem_radio_response_status(large, MODEM_RADIO_RESPONSE_CAPACITY - 1) == MODEM_RADIO_RESPONSE_OK);
    CHECK(modem_radio_response_status(large, MODEM_RADIO_RESPONSE_CAPACITY) == MODEM_RADIO_RESPONSE_ERROR);
    CHECK(modem_radio_response_status(large, sizeof(large)) == MODEM_RADIO_RESPONSE_ERROR);
    /* Actual DTE callbacks receive cumulative prefixes, not new-only chunks. */
    const char *full = "AT+CBANDCFG?\r\n+CBANDCFG: \"CAT-M\",3,20\r\n+CBANDCFG: \"NB-IOT\",1,8\r\nOK\r\n";
    size_t length = strlen(full);
    for (size_t n = 0; n < length; ++n)
        CHECK(modem_radio_response_status(full, n) == MODEM_RADIO_RESPONSE_WAIT);
    CHECK(modem_radio_response_status(full, length) == MODEM_RADIO_RESPONSE_OK);
    CHECK(modem_radio_response_status(full, length) == MODEM_RADIO_RESPONSE_OK);
}

static void test_order_and_preservation(void)
{
    const char *const changed[] = {
        "AT+CNMP?", "AT+CMNB?", "AT+CBANDCFG?", "AT+CFUN=4", "AT+CFUN?",
        "AT+CNMP=38", "AT+CMNB=1", "AT+CBANDCFG=\"CAT-M\",3,20",
        "AT+CNMP?", "AT+CMNB?", "AT+CBANDCFG?"
    };
    fake_modem m = modem(false);
    CHECK(modem_radio_prepare(fake_at, &m));
    trace_is(&m, changed, sizeof(changed) / sizeof(changed[0]));
    CHECK(m.functionality == 4 && !saw_enable(&m));
    /* Caller programs the existing APN while RF remains disabled. */
    m.calls = 0;
    CHECK(modem_radio_resume(fake_at, &m));
    const char *const resumed[] = {
        "AT+CNMP?", "AT+CMNB?", "AT+CBANDCFG?", "AT+CFUN?", "AT+CFUN=1", "AT+CFUN?"
    };
    trace_is(&m, resumed, sizeof(resumed) / sizeof(resumed[0]));
    CHECK(m.functionality == 1);
    for (unsigned dce = 0; dce < 3; ++dce) {
        m.calls = 0;
        CHECK(modem_radio_prepare(fake_at, &m));
        CHECK(modem_radio_resume(fake_at, &m));
        const char *const unchanged[] = {
            "AT+CNMP?", "AT+CMNB?", "AT+CBANDCFG?",
            "AT+CNMP?", "AT+CMNB?", "AT+CBANDCFG?", "AT+CFUN?"
        };
        trace_is(&m, unchanged, sizeof(unchanged) / sizeof(unchanged[0]));
        CHECK(m.functionality == 1 && !saw_enable(&m));
    }
    CHECK(!modem_radio_prepare(NULL, NULL));
    CHECK(!modem_radio_resume(NULL, NULL));
}

static void test_every_failure_boundary(void)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        for (unsigned step = 1; step <= 11; ++step) {
            fake_modem m = modem(false);
            m.fail_call = step;
            m.fail_kind = kind;
            CHECK(!modem_radio_prepare(fake_at, &m));
            CHECK(m.calls == step + 1);
            CHECK(!saw_enable(&m));
            ended_off(&m);
        }
        for (unsigned step = 1; step <= 6; ++step) {
            fake_modem m = modem(true);
            m.functionality = 4;
            m.fail_call = step;
            m.fail_kind = kind;
            CHECK(!modem_radio_resume(fake_at, &m));
            CHECK(m.calls == step + 1);
            if (step <= 4) CHECK(!saw_enable(&m));
            ended_off(&m);
        }
    }
    /* Setter may take effect despite a lost response. Failure still attempts RF-off. */
    fake_modem uncertain = modem(true);
    uncertain.functionality = 4;
    uncertain.fail_call = 5;
    uncertain.fail_after_mutation = true;
    CHECK(!modem_radio_resume(fake_at, &uncertain));
    ended_off(&uncertain);
    fake_modem offline_failure = modem(false);
    offline_failure.fail_all_off = true;
    CHECK(!modem_radio_prepare(fake_at, &offline_failure));
    CHECK(offline_failure.calls == 5 && !saw_enable(&offline_failure));
    /* No internal retry/fallback even when the best-effort RF-off also fails. */
}

static void rejects_resume(unsigned step, const char *response)
{
    fake_modem m = modem(true);
    m.override_call = step;
    m.override_response = response;
    CHECK(!modem_radio_resume(fake_at, &m));
    CHECK(!saw_enable(&m));
    ended_off(&m);
}

static void test_strict_settings_parsers(void)
{
    static const char *bad_network[] = {
        "+CNMP: 138\r\nOK\r\n", "+CNMP: 2\r\nOK\r\n",
        "+CNMP: 38junk\r\nOK\r\n", "+CNMP: 38,1\r\nOK\r\n",
        "+CNMP: +38\r\nOK\r\n", "+CNMP: -38\r\nOK\r\n",
        "+CNMP: 42949672960\r\nOK\r\n", "+CNMP:\r\nOK\r\n",
        "+CNMP 38\r\nOK\r\n", "+CNMPX: 38\r\nOK\r\n",
        "+CNMP: 38\r\n+CNMP: 38\r\nOK\r\n",
        "+CNMP: 38\r\n+CNMP: 2\r\nOK\r\n",
        "+CNMP: 38\r\nOK\r\n+CNMP: 2\r\n",
        "OK\r\n+CNMP: 38\r\n", "AT+CNMP?\r\nOK\r\n",
        "+CNMP: 38\r\n", "+CNMP: 38\r\nOK",
    };
    for (size_t i = 0; i < sizeof(bad_network) / sizeof(bad_network[0]); ++i)
        rejects_resume(1, bad_network[i]);
    static const char *bad_preference[] = {
        "+CMNB: 3\r\nOK\r\n", "+CMNB: 2\r\nOK\r\n",
        "+CMNB: 11\r\nOK\r\n", "+CMNB: 1,3\r\nOK\r\n",
        "+CMNB: 1\r\n+CMNB: 1\r\nOK\r\n",
        "+CMNB: 1\r\nOK\r\n+CMNB: 3\r\n",
    };
    for (size_t i = 0; i < sizeof(bad_preference) / sizeof(bad_preference[0]); ++i)
        rejects_resume(2, bad_preference[i]);
    static const char *bad_bands[] = {
        "+CBANDCFG: \"CAT-M\",3\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,8\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,3\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,,20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3 20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20junk\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,0\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,256\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20,42949672960\r\nOK\r\n",
        "+CBANDCFG: CAT-M,3,20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\"3,20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20\r\n+CBANDCFG: \"CAT-M\",3,20\r\nOK\r\n",
        "+CBANDCFG: \"CAT-M\",3,20\r\nOK\r\n+CBANDCFG: \"CAT-M\",8\r\n",
        "+CBANDCFG: \"NB-IOT\",3,20\r\nOK\r\n",
        "+CBANDCFG: \"UNKNOWN\",3,20\r\nOK\r\n",
        "OK\r\n+CBANDCFG: \"CAT-M\",3,20\r\n",
    };
    for (size_t i = 0; i < sizeof(bad_bands) / sizeof(bad_bands[0]); ++i)
        rejects_resume(3, bad_bands[i]);
    rejects_resume(4, "+CFUN: 0\r\nOK\r\n");
    rejects_resume(4, "+CFUN: 7\r\nOK\r\n");
    rejects_resume(4, "+CFUN: 1\r\n+CFUN: 4\r\nOK\r\n");
}

static void test_whitespace_urcs_and_reordered_bands(void)
{
    static const struct { unsigned step; const char *reply; } valid[] = {
        {1, "AT+CNMP?\r\n  +CNMP \t: \t38  \r\n+CEREG: 5\r\nOK\r\n"},
        {2, "  +CMNB : 1\t\r\n+CEREG: 2\r\nOK\r\n"},
        {3, "AT+CBANDCFG?\r\n+CBANDCFG: \"NB-IOT\",1,8,28\r\n+CEREG: 2\r\n  +CBANDCFG : \"CAT-M\" , 20 , 3  \r\nOK\r\n"},
        {3, "+CBANDCFG: \"CAT-M\",3,20\nOK\n+CEREG: 5\n"},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        fake_modem m = modem(true);
        m.override_call = valid[i].step;
        m.override_response = valid[i].reply;
        CHECK(modem_radio_prepare(fake_at, &m));
        CHECK(m.calls == 3 && m.functionality == 1 && !saw_enable(&m));
    }
}

static void test_prepare_readback_and_external_retry(void)
{
    /* Changed but still wrong settings never reach resume. */
    static const struct { unsigned step; const char *reply; } bad[] = {
        {5, "+CFUN: 1\r\nOK\r\n"},
        {9, "+CNMP: 2\r\nOK\r\n"},
        {10, "+CMNB: 3\r\nOK\r\n"},
        {11, "+CBANDCFG: \"CAT-M\",3,20,8\r\nOK\r\n"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fake_modem m = modem(false);
        m.override_call = bad[i].step;
        m.override_response = bad[i].reply;
        CHECK(!modem_radio_prepare(fake_at, &m));
        CHECK(!saw_enable(&m));
        ended_off(&m);
    }
    fake_modem m = modem(false);
    m.fail_call = 7;
    CHECK(!modem_radio_prepare(fake_at, &m));
    ended_off(&m);
    m.calls = 0;
    m.fail_call = 0;
    CHECK(modem_radio_prepare(fake_at, &m)); /* Supervisor-controlled later retry. */
    CHECK(m.functionality == 4 && !saw_enable(&m));
    m.calls = 0;
    m.preference = 3; /* A reset/external configuration changed policy before resume. */
    CHECK(!modem_radio_resume(fake_at, &m));
    ended_off(&m);
    CHECK(!saw_enable(&m));
    fake_modem invalid = modem(false);
    invalid.override_call = 1;
    invalid.override_response = "+CNMP: broken\r\nOK\r\n";
    CHECK(!modem_radio_prepare(fake_at, &invalid));
    CHECK(invalid.calls == 2); /* Malformed readback is an error, not permission to continue. */
    ended_off(&invalid);
    fake_modem wrong_rf = modem(true);
    wrong_rf.functionality = 4;
    wrong_rf.override_call = 6;
    wrong_rf.override_response = "+CFUN: 4\r\nOK\r\n";
    CHECK(!modem_radio_resume(fake_at, &wrong_rf));
    ended_off(&wrong_rf);
}

int main(void)
{
    test_terminal_parser();
    test_order_and_preservation();
    test_every_failure_boundary();
    test_strict_settings_parsers();
    test_whitespace_urcs_and_reordered_bands();
    test_prepare_readback_and_external_retry();
    printf("modem_radio_policy: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
