#include "modem_awake_policy.h"

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

#define COUNT(items) (sizeof(items) / sizeof((items)[0]))
#define TRACE_CAPACITY 32u

typedef enum {
    FAULT_TRANSPORT, FAULT_ERROR, FAULT_UNTERMINATED, FAULT_EMPTY,
    FAULT_CONTROL, FAULT_CONFLICT, FAULT_INCOMPLETE, FAULT_EMBEDDED_NUL,
    FAULT_COUNT
} fault_kind;

typedef struct {
    unsigned csclk, psm, edrx, functionality;
    unsigned active_psm, active_edrx;
    unsigned calls;
    char trace[TRACE_CAPACITY][48];
    unsigned fail_call, override_call, ignore_write_call;
    fault_kind fail_kind;
    const char *override_response;
    bool fail_after_mutation, fail_all_off;
} fake_modem;

static fake_modem modem(unsigned enabled)
{
    fake_modem result = {0};
    result.csclk = enabled & 1u;
    result.psm = (enabled >> 1) & 1u;
    result.edrx = (enabled >> 2) & 1u;
    result.functionality = 1;
    return result;
}

static bool fault(fake_modem *m, char *response, size_t capacity)
{
    switch (m->fail_kind) {
    case FAULT_TRANSPORT: return false;
    case FAULT_ERROR:
        snprintf(response, capacity, "\r\n+CME ERROR: 3\r\n");
        return true;
    case FAULT_UNTERMINATED:
        memset(response, 'X', capacity);
        return true;
    case FAULT_EMPTY:
        response[0] = '\0';
        return true;
    case FAULT_CONTROL:
        snprintf(response, capacity, "\r\nOK\r\n\x1b");
        return true;
    case FAULT_CONFLICT:
        snprintf(response, capacity, "\r\nOK\r\nERROR\r\n");
        return true;
    case FAULT_INCOMPLETE:
        snprintf(response, capacity, "\r\nOK\r");
        return true;
    case FAULT_EMBEDDED_NUL: {
        const char corrupted[] = "\r\nOK\r\n\0ERROR\r\n";
        CHECK(sizeof(corrupted) <= capacity);
        memcpy(response, corrupted, sizeof(corrupted));
        /* The adapter contract rejects embedded NULs before this interface. */
        return false;
    }
    case FAULT_COUNT: break;
    }
    CHECK(false);
    return false;
}

static bool permitted(const char *command)
{
    static const char *const commands[] = {
        "AT+CSCLK?", "AT+CPSMS?", "AT+CEDRX?", "AT+CFUN?", "AT+CFUN=4",
        "AT+CSCLK=0", "AT+CPSMS=0", "AT+CEDRXS=0,4,\"0000\"",
        "AT+CPSMRDP", "AT+CEDRXRDP"
    };
    for (size_t i = 0; i < COUNT(commands); ++i)
        if (!strcmp(command, commands[i])) return true;
    return false;
}

static bool fake_at(void *context, const char *command, char *response,
                    size_t capacity, unsigned timeout)
{
    fake_modem *m = context;
    CHECK(capacity == MODEM_RADIO_RESPONSE_CAPACITY);
    CHECK(timeout == (!strncmp(command, "AT+CFUN", 7) ? 10000u : 3000u));
    CHECK(permitted(command)); /* No NB setters, masks, RF-on, resets or fallback. */
    CHECK(m->calls < TRACE_CAPACITY);
    if (m->calls >= TRACE_CAPACITY) return false;
    CHECK(strlen(command) < sizeof(m->trace[0]));
    snprintf(m->trace[m->calls++], sizeof(m->trace[0]), "%s", command);
    if (m->fail_all_off && !strcmp(command, "AT+CFUN=4")) return false;
    bool failing = m->fail_call == m->calls;
    if (failing && !m->fail_after_mutation) return fault(m, response, capacity);
    if (m->override_call == m->calls) {
        CHECK(m->override_response != NULL);
        CHECK(strlen(m->override_response) < capacity);
        snprintf(response, capacity, "%s", m->override_response);
        return true;
    }
    if (!strcmp(command, "AT+CSCLK?"))
        snprintf(response, capacity, "AT+CSCLK?\r\n+CEREG: 5\r\n+CSCLK: %u\r\nOK\r\n", m->csclk);
    else if (!strcmp(command, "AT+CPSMS?"))
        snprintf(response, capacity, "+CPSMS: %u,,,\"00100001\",\"00000101\"\r\nOK\r\n", m->psm);
    else if (!strcmp(command, "AT+CEDRX?"))
        snprintf(response, capacity, "+CEDRX: 2,1,15,15\r\n+CEDRX: 3,%u,5,7\r\nOK\r\n", m->edrx);
    else if (!strcmp(command, "AT+CFUN?"))
        snprintf(response, capacity, "+CFUN: %u\r\nOK\r\n", m->functionality);
    else if (!strcmp(command, "AT+CPSMRDP"))
        snprintf(response, capacity, "+CPSMRDP: %u,32,3600,12,3600,0\r\nOK\r\n", m->active_psm);
    else if (!strcmp(command, "AT+CEDRXRDP"))
        snprintf(response, capacity, "+CEDRXRDP: %u\r\nOK\r\n", m->active_edrx);
    else {
        if (m->ignore_write_call != m->calls) {
            if (!strcmp(command, "AT+CFUN=4")) m->functionality = 4;
            else if (!strcmp(command, "AT+CSCLK=0")) m->csclk = 0;
            else if (!strcmp(command, "AT+CPSMS=0")) m->psm = 0;
            else if (!strcmp(command, "AT+CEDRXS=0,4,\"0000\"")) m->edrx = 0;
            else CHECK(false);
        }
        snprintf(response, capacity, "\r\nOK\r\n");
    }
    return failing ? fault(m, response, capacity) : true;
}

static void trace_is(const fake_modem *m, const char *const *expected, size_t count)
{
    CHECK(m->calls == count);
    for (size_t i = 0; i < count && i < m->calls; ++i)
        CHECK(!strcmp(m->trace[i], expected[i]));
}

static void ended_off(const fake_modem *m)
{
    CHECK(m->calls > 0);
    if (m->calls) CHECK(!strcmp(m->trace[m->calls - 1], "AT+CFUN=4"));
    CHECK(m->functionality == 4);
}

static void reads_only(const fake_modem *m)
{
    for (unsigned i = 0; i < m->calls; ++i) {
        CHECK(strchr(m->trace[i], '=') == NULL);
        CHECK(strcmp(m->trace[i], "AT+CFUN?") != 0);
    }
}

static void test_all_configurations(void)
{
    static const char *const config[] = {"AT+CSCLK?", "AT+CPSMS?", "AT+CEDRX?"};
    for (unsigned enabled = 0; enabled < 8; ++enabled) {
        fake_modem m = modem(enabled);
        const char *expected[16];
        size_t count = 0;
        for (size_t i = 0; i < COUNT(config); ++i) expected[count++] = config[i];
        if (enabled & 6u) {
            expected[count++] = "AT+CFUN=4";
            expected[count++] = "AT+CFUN?";
        }
        if (enabled & 1u) expected[count++] = "AT+CSCLK=0";
        if (enabled & 2u) expected[count++] = "AT+CPSMS=0";
        if (enabled & 4u) expected[count++] = "AT+CEDRXS=0,4,\"0000\"";
        if (enabled)
            for (size_t i = 0; i < COUNT(config); ++i) expected[count++] = config[i];
        CHECK(modem_awake_prepare(fake_at, &m));
        trace_is(&m, expected, count);
        CHECK(m.csclk == 0 && m.psm == 0 && m.edrx == 0);
        CHECK(m.functionality == (enabled & 6u ? 4u : 1u));
        /* AUTO_SAVE readbacks must avoid writes on each subsequent DCE setup. */
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            m.calls = 0;
            CHECK(modem_awake_prepare(fake_at, &m));
            trace_is(&m, config, COUNT(config));
            reads_only(&m);
        }
        m.calls = 0;
        CHECK(modem_awake_verify_config(fake_at, &m));
        trace_is(&m, config, COUNT(config));
        reads_only(&m);
    }
    CHECK(!modem_awake_prepare(NULL, NULL));
    CHECK(!modem_awake_verify_config(NULL, NULL));
    CHECK(!modem_awake_verify_active(NULL, NULL));
}

static void test_every_prepare_failure(void)
{
    for (unsigned enabled = 0; enabled < 8; ++enabled) {
        fake_modem success = modem(enabled);
        CHECK(modem_awake_prepare(fake_at, &success));
        for (unsigned kind = 0; kind < FAULT_COUNT; ++kind) {
            for (unsigned step = 1; step <= success.calls; ++step) {
                fake_modem m = modem(enabled);
                m.fail_call = step;
                m.fail_kind = (fault_kind)kind;
                CHECK(!modem_awake_prepare(fake_at, &m));
                CHECK(m.calls == step + 1);
                for (unsigned i = 0; i < step && i < m.calls; ++i)
                    CHECK(!strcmp(m.trace[i], success.trace[i]));
                ended_off(&m);
            }
        }
    }
    /* The fail-safe itself can fail. Stop; never recurse, retry or enable RF. */
    fake_modem m = modem(7);
    m.fail_all_off = true;
    CHECK(!modem_awake_prepare(fake_at, &m));
    CHECK(m.calls == 5);
    CHECK(!strcmp(m.trace[3], "AT+CFUN=4"));
    CHECK(!strcmp(m.trace[4], "AT+CFUN=4"));
    CHECK(m.functionality == 1);
    m = modem(0);
    m.fail_call = 1;
    m.fail_all_off = true;
    CHECK(!modem_awake_prepare(fake_at, &m));
    CHECK(m.calls == 2);
}

static void test_uncertain_mutations(void)
{
    /* RF-off + three setters are the mutations in the full change sequence. */
    static const unsigned changed_steps[] = {4, 6, 7, 8};
    for (size_t i = 0; i < COUNT(changed_steps); ++i) {
        for (unsigned kind = 0; kind < FAULT_COUNT; ++kind) {
            fake_modem m = modem(7);
            m.fail_call = changed_steps[i];
            m.fail_kind = (fault_kind)kind;
            m.fail_after_mutation = true;
            CHECK(!modem_awake_prepare(fake_at, &m));
            CHECK(m.calls == changed_steps[i] + 1);
            ended_off(&m);
            if (changed_steps[i] >= 6) CHECK(m.csclk == 0);
            if (changed_steps[i] >= 7) CHECK(m.psm == 0);
            if (changed_steps[i] >= 8) CHECK(m.edrx == 0);
        }
        fake_modem m = modem(7);
        m.ignore_write_call = changed_steps[i];
        CHECK(!modem_awake_prepare(fake_at, &m));
        ended_off(&m);
        if (changed_steps[i] == 4) CHECK(m.calls == 6);
        /* Readback, never the setter's OK alone, confirms changed configuration. */
        else CHECK(m.calls >= 10 && m.calls <= 12);
    }
    fake_modem csclk_only = modem(1);
    csclk_only.ignore_write_call = 4;
    CHECK(!modem_awake_prepare(fake_at, &csclk_only));
    ended_off(&csclk_only);
}

static void test_cfun_readback(void)
{
    static const char *const invalid[] = {
        "+CFUN: 0\nOK\n", "+CFUN: 1\nOK\n", "+CFUN: 3\nOK\n",
        "+CFUN: 5\nOK\n", "+CFUN: 4,0\nOK\n", "+CFUN: 4x\nOK\n",
        "+CFUN: -4\nOK\n", "+CFUN: 4294967296\nOK\n",
        "+CFUN: 4\n+CFUN: 4\nOK\n", "OK\n+CFUN: 4\n",
        "+CFUN: 4\nOK\n+CFUN: 4\n", "+CFUN 4\nOK\n",
        "+CFUNX: 4\nOK\n", "+CFUN: 4\nOK\r",
    };
    for (size_t i = 0; i < COUNT(invalid); ++i) {
        fake_modem m = modem(7);
        m.override_call = 5;
        m.override_response = invalid[i];
        CHECK(!modem_awake_prepare(fake_at, &m));
        CHECK(m.calls == 6);
        ended_off(&m);
        CHECK(m.csclk == 1 && m.psm == 1 && m.edrx == 1);
    }
    fake_modem m = modem(7);
    m.override_call = 5;
    m.override_response = "+CFUNX: 1\n \t+CFUN \t: 4 \r\nOK\r\n+CEREG:";
    CHECK(modem_awake_prepare(fake_at, &m));
    CHECK(m.calls == 11);
    CHECK(m.csclk == 0 && m.psm == 0 && m.edrx == 0 && m.functionality == 4);
}

static void config_response(unsigned step, const char *response, bool accepted)
{
    fake_modem m = modem(0);
    m.override_call = step;
    m.override_response = response;
    CHECK(modem_awake_verify_config(fake_at, &m) == accepted);
    reads_only(&m);
    CHECK(m.functionality == 1);
    m = modem(0);
    m.override_call = step;
    m.override_response = response;
    CHECK(modem_awake_prepare(fake_at, &m) == accepted);
    if (accepted) {
        CHECK(m.calls == 3);
        reads_only(&m);
        CHECK(m.functionality == 1);
    } else {
        ended_off(&m);
        CHECK(m.csclk == 0 && m.psm == 0 && m.edrx == 0);
    }
}

static void test_config_parsers(void)
{
    static const struct { unsigned step; const char *response; } valid[] = {
        {1, "\t+CSCLK \t: \t0 \t\r\n \tOK \r\n"},
        {1, "+CSCLKX: 1\n+CSCLK: 0\nOK\n+UNRELATED: 1\n"},
        {1, "+CSCLK:0\r\nOK\r\n+CEREG:"},
        {2, "+CPSMS: 0\r\nOK\r\n"},
        {2, "+CPSMS : 0 , , , , \r\nOK\r\n"},
        {2, "+CPSMS: 0,\"00000000\",\"11111111\",\"10101010\",\"01010101\"\nOK\n"},
        {2, "+CPSMSX: 1\n+CPSMS: 0,,\"00100000\"\nOK\n"},
        {3, "+CEDRX: 3,0,0,0\nOK\n"},
        {3, " \t+CEDRX \t: 3 , 0 , 15 , 15 \r\nOK\r\n"},
        {3, "+CEDRXX: 3,1,0,0\n+CEDRX: 2,ignored for inactive RAT\n+CEDRX: 3,0,5,7\nOK\n"},
        {3, "+CEDRX: 3,0,5,7\n+CEDRX: 2,1,15,15\nOK\n"},
    };
    for (size_t i = 0; i < COUNT(valid); ++i)
        config_response(valid[i].step, valid[i].response, true);
    static const struct { unsigned step; const char *response; } invalid[] = {
        {1, "+CSCLK: 2\nOK\n"}, {1, "+CSCLK: -1\nOK\n"},
        {1, "+CSCLK: +0\nOK\n"}, {1, "+CSCLK: 4294967296\nOK\n"},
        {1, "+CSCLK: 184467440737095516160\nOK\n"},
        {1, "+CSCLK: 0,0\nOK\n"}, {1, "+CSCLK: 0x0\nOK\n"},
        {1, "+CSCLK: \"0\"\nOK\n"}, {1, "+CSCLK: \nOK\n"},
        {1, "+CSCLKX: 0\nOK\n"}, {1, "+CSCLK 0\nOK\n"},
        {1, "+CSCLK\n+CSCLK: 0\nOK\n"}, {1, "+CSCLK: 0\n+CSCLK: 0\nOK\n"},
        {1, "+CSCLK: 0\nOK\n+CSCLK: 0\n"},
        {1, "+CSCLK: 0\nOK\n+CSCLK: 1"},
        {1, "+CSCLK: 0\nOK\n+CSC"},
        {1, "OK\n+CSCLK: 0\n"}, {1, "+CSCLK: 0\nOK"},
        {1, "+CSCLK: 0\rOK\r"}, {1, "+CSCLK: 0\nOK\r"},
        {1, "+CSCLK: 0\nOK\nERROR\n"}, {1, "+CSCLK: 0\nOK\nOK\n"},
        {1, "+CSCLK: 0\nOK\n\x01"}, {1, "+CSCLK: 0\nOK\n\xff"},
        {2, "+CPSMS: 2\nOK\n"}, {2, "+CPSMS: -0\nOK\n"},
        {2, "+CPSMS: 0,,,,,\nOK\n"}, {2, "+CPSMS: 0,00000000\nOK\n"},
        {2, "+CPSMS: 0,\"0000000\"\nOK\n"},
        {2, "+CPSMS: 0,\"000000000\"\nOK\n"},
        {2, "+CPSMS: 0,\"00000002\"\nOK\n"},
        {2, "+CPSMS: 0,\"0000000 \"\nOK\n"},
        {2, "+CPSMS: 0,\"00000000\nOK\n"},
        {2, "+CPSMS: 0,\"00000000\"x\nOK\n"},
        {2, "+CPSMS: 0,\"\"\nOK\n"},
        {2, "+CPSMS: 0\n+CPSMS: 0\nOK\n"},
        {2, "+CPSMS: 0\nOK\n+CPSMS: 1"},
        {2, "+CPSMS: 0\nOK\n+CPSM"},
        {2, "+CPSMS 0\n+CPSMS: 0\nOK\n"},
        {2, "OK\n+CPSMS: 0\n"}, {2, "+CPSMSX: 0\nOK\n"},
        {3, "+CEDRX: 3,2,0,0\nOK\n"}, {3, "+CEDRX: 3,0,16,0\nOK\n"},
        {3, "+CEDRX: 3,0,0,16\nOK\n"}, {3, "+CEDRX: 3,0,-1,0\nOK\n"},
        {3, "+CEDRX: 3,0,4294967296,0\nOK\n"},
        {3, "+CEDRX: 3,0,0\nOK\n"}, {3, "+CEDRX: 3,0,0,0,\nOK\n"},
        {3, "+CEDRX: 3,0,0,0,1\nOK\n"}, {3, "+CEDRX: 3,,0,0\nOK\n"},
        {3, "+CEDRX: 3,0,\"0\",0\nOK\n"},
        {3, "+CEDRX: 2,0,0,0\nOK\n"}, {3, "+CEDRX: 4,0,0,0\nOK\n"},
        {3, "+CEDRX: 3,0,0,0\n+CEDRX: 3,0,0,0\nOK\n"},
        {3, "+CEDRX: 3,0,0,0\nOK\n+CEDRX: 3,1"},
        {3, "+CEDRX: 3,0,0,0\nOK\n+CED"},
        {3, "+CEDRX 3,0,0,0\n+CEDRX: 3,0,0,0\nOK\n"},
        {3, "+CEDRX: garbage\n+CEDRX: 3,0,0,0\nOK\n"},
        {3, "OK\n+CEDRX: 3,0,0,0\n"}, {3, "+CEDRXX: 3,0,0,0\nOK\n"},
        {3, "+CEDRX: 3,0,0,0\n+CME ERROR: 3\n"},
    };
    for (size_t i = 0; i < COUNT(invalid); ++i)
        config_response(invalid[i].step, invalid[i].response, false);
    char longest[MODEM_RADIO_RESPONSE_CAPACITY];
    const char *prefix = "+CSCLK: 0\n+URC: ";
    size_t prefix_length = strlen(prefix);
    memcpy(longest, prefix, prefix_length);
    memset(longest + prefix_length, 'x', sizeof(longest) - prefix_length - 5);
    memcpy(longest + sizeof(longest) - 5, "\nOK\n", 5);
    CHECK(strlen(longest) == MODEM_RADIO_RESPONSE_CAPACITY - 1);
    config_response(1, longest, true);
}

static void active_response(unsigned step, const char *response, bool accepted)
{
    fake_modem m = modem(0);
    m.override_call = step;
    m.override_response = response;
    CHECK(modem_awake_verify_active(fake_at, &m) == accepted);
    CHECK(m.calls <= 2);
    reads_only(&m);
    CHECK(m.functionality == 1);
    /* A later invocation is allowed to recover from UNKNOWN without mutation. */
    m.calls = 0;
    m.override_call = 0;
    CHECK(modem_awake_verify_active(fake_at, &m));
    const char *const expected[] = {"AT+CPSMRDP", "AT+CEDRXRDP"};
    trace_is(&m, expected, COUNT(expected));
    reads_only(&m);
}

static void test_active_parsers(void)
{
    static const struct { unsigned step; const char *response; } valid[] = {
        {1, "+CPSMRDP: 0\nOK\n"},
        {1, "\t+CPSMRDP \t: 0 , , , , , \r\nOK\r\n"},
        {1, "+CPSMRDP: 0,1,4294967295,12,3600,0\nOK\n"},
        {1, "+CPSMRDPX: 1\n+CPSMRDP: 0,,12\nOK\n"},
        {2, "+CEDRXRDP: 0\nOK\n"},
        {2, " \t+CEDRXRDP : 0 , , , \r\nOK\r\n"},
        {2, "+CEDRXRDP: 0,\"0000\",\"1111\",\"1010\"\nOK\n"},
        {2, "+CEDRXRDP: 0,\"1111\",\"1110\",\"1101\"\nOK\n"},
        {2, "+CEDRXRDPX: 4\n+CEDRXRDP: 0,,\"1001\"\nOK\n"},
    };
    for (size_t i = 0; i < COUNT(valid); ++i)
        active_response(valid[i].step, valid[i].response, true);
    static const struct { unsigned step; const char *response; } invalid[] = {
        {1, "+CPSMRDP: 1,0,0,0,0,0\nOK\n"}, {1, "+CPSMRDP: 2\nOK\n"},
        {1, "+CPSMRDP: -0\nOK\n"}, {1, "+CPSMRDP: +0\nOK\n"},
        {1, "+CPSMRDP: 0,,,,,,\nOK\n"}, {1, "+CPSMRDP: 0,4294967296\nOK\n"},
        {1, "+CPSMRDP: 0,184467440737095516160\nOK\n"},
        {1, "+CPSMRDP: 0,\"12\"\nOK\n"}, {1, "+CPSMRDP: 0,-1\nOK\n"},
        {1, "+CPSMRDP: 0,+1\nOK\n"}, {1, "+CPSMRDP: 0,1.0\nOK\n"},
        {1, "+CPSMRDP: 0,1x\nOK\n"}, {1, "+CPSMRDP 0\nOK\n"},
        {1, "+CPSMRDP: 0\n+CPSMRDP: 0\nOK\n"}, {1, "OK\n+CPSMRDP: 0\n"},
        {1, "+CPSMRDP: 0\nOK\n+CPSMRDP: 1"},
        {1, "+CPSMRDP: 0\nOK\n+CPSMR"},
        {1, "+CPSMRDPX: 0\nOK\n"}, {1, "+CPSMRDP: 0\nOK\r"},
        {1, "+CPSMRDP: 0\nOK\nOK\n"}, {1, "+CPSMRDP: 0\nOK\n\x7f"},
        {2, "+CEDRXRDP: 4\nOK\n"}, {2, "+CEDRXRDP: 5\nOK\n"},
        {2, "+CEDRXRDP: 1\nOK\n"}, {2, "+CEDRXRDP: -0\nOK\n"},
        {2, "+CEDRXRDP: 0,,,,\nOK\n"}, {2, "+CEDRXRDP: 0,0000\nOK\n"},
        {2, "+CEDRXRDP: 0,\"000\"\nOK\n"},
        {2, "+CEDRXRDP: 0,\"00000\"\nOK\n"},
        {2, "+CEDRXRDP: 0,\"0002\"\nOK\n"},
        {2, "+CEDRXRDP: 0,\"000 \"\nOK\n"},
        {2, "+CEDRXRDP: 0,\"0000\nOK\n"},
        {2, "+CEDRXRDP: 0,\"0000\"x\nOK\n"},
        {2, "+CEDRXRDP: 0,\"\"\nOK\n"},
        {2, "+CEDRXRDP: 0\n+CEDRXRDP: 0\nOK\n"},
        {2, "+CEDRXRDP: 0\nOK\n+CEDRXRDP: 4"},
        {2, "+CEDRXRDP: 0\nOK\n+CEDRXR"},
        {2, "+CEDRXRDP 0\n+CEDRXRDP: 0\nOK\n"},
        {2, "OK\n+CEDRXRDP: 0\n"}, {2, "+CEDRXRDPX: 0\nOK\n"},
        {2, "+CEDRXRDP: 0\nERROR\n"}, {2, "+CEDRXRDP: 0\nOK\nERROR\n"},
        {2, "+CEDRXRDP: 0\nOK\r"}, {2, "+CEDRXRDP: 0\nOK\n\xff"},
    };
    for (size_t i = 0; i < COUNT(invalid); ++i)
        active_response(invalid[i].step, invalid[i].response, false);
}

static void test_pure_verification_failures(void)
{
    for (unsigned kind = 0; kind < FAULT_COUNT; ++kind) {
        for (unsigned step = 1; step <= 3; ++step) {
            fake_modem m = modem(0);
            m.fail_call = step;
            m.fail_kind = (fault_kind)kind;
            CHECK(!modem_awake_verify_config(fake_at, &m));
            CHECK(m.calls == step);
            reads_only(&m);
            CHECK(m.functionality == 1);
        }
        for (unsigned step = 1; step <= 2; ++step) {
            fake_modem m = modem(0);
            m.fail_call = step;
            m.fail_kind = (fault_kind)kind;
            CHECK(!modem_awake_verify_active(fake_at, &m));
            CHECK(m.calls == step);
            reads_only(&m);
            CHECK(m.functionality == 1);
            m.calls = 0;
            m.fail_call = 0;
            CHECK(modem_awake_verify_active(fake_at, &m));
            CHECK(m.calls == 2);
            reads_only(&m);
        }
    }
    for (unsigned enabled = 1; enabled < 8; ++enabled) {
        fake_modem m = modem(enabled);
        CHECK(!modem_awake_verify_config(fake_at, &m));
        reads_only(&m);
        CHECK(m.csclk == (enabled & 1u));
        CHECK(m.psm == ((enabled >> 1) & 1u));
        CHECK(m.edrx == ((enabled >> 2) & 1u));
        CHECK(m.functionality == 1);
    }
    fake_modem m = modem(0);
    m.active_psm = 1;
    CHECK(!modem_awake_verify_active(fake_at, &m));
    CHECK(m.calls == 1);
    reads_only(&m);
    m.active_psm = 0;
    m.active_edrx = 4;
    m.calls = 0;
    CHECK(!modem_awake_verify_active(fake_at, &m));
    CHECK(m.calls == 2);
    reads_only(&m);
    m.active_edrx = 0;
    m.calls = 0;
    CHECK(modem_awake_verify_active(fake_at, &m));
    CHECK(m.calls == 2);
    reads_only(&m);
}

int main(void)
{
    test_all_configurations();
    test_every_prepare_failure();
    test_uncertain_mutations();
    test_cfun_readback();
    test_config_parsers();
    test_active_parsers();
    test_pure_verification_failures();
    printf("modem_awake_policy: %u checks, %u failures: %s\n",
           checks, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
