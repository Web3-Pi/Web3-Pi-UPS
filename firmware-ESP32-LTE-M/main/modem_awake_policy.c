#include "modem_awake_policy.h"

#include <limits.h>
#include <string.h>

#define AWAKE_AT_TIMEOUT_MS 3000u
#define AWAKE_CFUN_TIMEOUT_MS 10000u

typedef struct {
    const char *data;
    size_t length;
} span;

typedef struct {
    unsigned slow_clock;
    unsigned psm;
    unsigned edrx;
} awake_config;

typedef enum {
    TAIL_NONE,
    TAIL_PSM_REQUEST,
    TAIL_PSM_ACTIVE,
    TAIL_EDRX_ACTIVE
} tail_kind;

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static span trim(span text)
{
    while (text.length && space(text.data[0])) {
        ++text.data;
        --text.length;
    }
    while (text.length && space(text.data[text.length - 1])) --text.length;
    return text;
}

static bool equals(span text, const char *literal)
{
    size_t n = strlen(literal);
    return text.length == n && !memcmp(text.data, literal, n);
}

static bool next_line(span *remaining, span *line)
{
    const char *end = memchr(remaining->data, '\n', remaining->length);
    if (!end) return false;
    size_t n = (size_t)(end - remaining->data);
    *line = trim((span){remaining->data, n});
    remaining->data += n + 1;
    remaining->length -= n + 1;
    return true;
}

/* 0: unrelated line/URC, 1: named field, -1: malformed named field. */
static int named_value(span line, const char *key, span *value)
{
    size_t n = strlen(key);
    if (line.length < n || memcmp(line.data, key, n)) return 0;
    span rest = {line.data + n, line.length - n};
    if (rest.length && rest.data[0] != ':' && !space(rest.data[0])) return 0;
    rest = trim(rest);
    if (!rest.length || rest.data[0] != ':') return -1;
    ++rest.data;
    --rest.length;
    *value = trim(rest);
    return 1;
}

static bool unrelated_fragment(span remaining, const char *key)
{
    remaining = trim(remaining);
    if (!remaining.length) return true;
    size_t n = strlen(key);
    /* A cut-off copy of the requested field cannot be dismissed as a URC. */
    if (remaining.length < n)
        return memcmp(remaining.data, key, remaining.length) != 0;
    span ignored;
    return named_value(remaining, key, &ignored) == 0;
}

static bool take_unsigned(span *text, unsigned *number)
{
    *text = trim(*text);
    unsigned value = 0;
    size_t n = 0;
    while (n < text->length && text->data[n] >= '0' && text->data[n] <= '9') {
        unsigned digit = (unsigned)(text->data[n] - '0');
        if (value > (UINT_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        ++n;
    }
    if (!n) return false;
    text->data += n;
    text->length -= n;
    *text = trim(*text);
    *number = value;
    return true;
}

static bool take_comma(span *text)
{
    *text = trim(*text);
    if (!text->length || text->data[0] != ',') return false;
    ++text->data;
    --text->length;
    *text = trim(*text);
    return true;
}

static bool binary_string(span text, size_t bits)
{
    if (text.length != bits + 2 || text.data[0] != '"' ||
        text.data[text.length - 1] != '"') return false;
    for (size_t i = 1; i <= bits; ++i)
        if (text.data[i] != '0' && text.data[i] != '1') return false;
    return true;
}

static bool optional_tail(span text, tail_kind kind)
{
    if (kind == TAIL_NONE) return text.length == 0;
    unsigned count = 0;
    unsigned limit = kind == TAIL_PSM_REQUEST ? 4u :
                     kind == TAIL_PSM_ACTIVE ? 5u : 3u;
    while (text.length) {
        if (++count > limit || !take_comma(&text)) return false;
        const char *end = memchr(text.data, ',', text.length);
        size_t n = end ? (size_t)(end - text.data) : text.length;
        span field = trim((span){text.data, n});
        if (field.length) {
            if (kind == TAIL_PSM_ACTIVE) {
                unsigned ignored;
                if (!take_unsigned(&field, &ignored) || field.length) return false;
            } else if (!binary_string(field, kind == TAIL_PSM_REQUEST ? 8u : 4u)) {
                return false;
            }
        }
        text.data += n;
        text.length -= n;
    }
    return true;
}

static bool exchange(modem_radio_at_fn at, void *context, const char *command,
                     unsigned timeout, char response[MODEM_RADIO_RESPONSE_CAPACITY],
                     size_t *length)
{
    memset(response, 0xa5, MODEM_RADIO_RESPONSE_CAPACITY);
    if (!at(context, command, response, MODEM_RADIO_RESPONSE_CAPACITY, timeout))
        return false;
    const char *end = memchr(response, '\0', MODEM_RADIO_RESPONSE_CAPACITY);
    if (!end) return false;
    *length = (size_t)(end - response);
    /* Match the radio collector: a complete terminal OK ends the reply even
     * if an unrelated URC fragment follows in the same UART read. Matching
     * fields still have to be unique and complete, before that terminal. */
    return modem_radio_response_status(response, *length) == MODEM_RADIO_RESPONSE_OK;
}

static bool command_ok(modem_radio_at_fn at, void *context, const char *command,
                       unsigned timeout)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    return exchange(at, context, command, timeout, response, &length);
}

static bool read_mode(modem_radio_at_fn at, void *context, const char *command,
                      const char *key, tail_kind tail, unsigned timeout,
                      unsigned *mode)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    if (!exchange(at, context, command, timeout, response, &length)) return false;
    span remaining = {response, length}, line;
    bool found = false, terminal = false;
    while (next_line(&remaining, &line)) {
        if (equals(line, "OK")) terminal = true;
        span value;
        int match = named_value(line, key, &value);
        if (match < 0) return false;
        if (!match) continue;
        if (terminal || found || !take_unsigned(&value, mode) ||
            !optional_tail(value, tail)) return false;
        found = true;
    }
    return found && unrelated_fragment(remaining, key);
}

static bool read_edrx(modem_radio_at_fn at, void *context, unsigned *enabled)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    if (!exchange(at, context, "AT+CEDRX?", AWAKE_AT_TIMEOUT_MS, response, &length))
        return false;
    span remaining = {response, length}, line;
    bool found = false, terminal = false;
    while (next_line(&remaining, &line)) {
        if (equals(line, "OK")) terminal = true;
        span value;
        int match = named_value(line, "+CEDRX", &value);
        if (match < 0) return false;
        if (!match) continue;
        unsigned mode, ptw, cycle;
        if (!take_unsigned(&value, &mode)) return false;
        if (mode == 2) continue; /* Inactive NB-IoT configuration is irrelevant. */
        if (mode != 3 || terminal || found ||
            !take_comma(&value) || !take_unsigned(&value, enabled) || *enabled > 1 ||
            !take_comma(&value) || !take_unsigned(&value, &ptw) || ptw > 15 ||
            !take_comma(&value) || !take_unsigned(&value, &cycle) || cycle > 15 ||
            value.length) return false;
        found = true;
    }
    return found && unrelated_fragment(remaining, "+CEDRX");
}

static bool read_config(modem_radio_at_fn at, void *context, awake_config *config)
{
    return read_mode(at, context, "AT+CSCLK?", "+CSCLK", TAIL_NONE,
                     AWAKE_AT_TIMEOUT_MS, &config->slow_clock) &&
           config->slow_clock <= 1 &&
           read_mode(at, context, "AT+CPSMS?", "+CPSMS", TAIL_PSM_REQUEST,
                     AWAKE_AT_TIMEOUT_MS, &config->psm) && config->psm <= 1 &&
           read_edrx(at, context, &config->edrx);
}

static bool all_off(const awake_config *config)
{
    return !config->slow_clock && !config->psm && !config->edrx;
}

static bool fail_closed(modem_radio_at_fn at, void *context)
{
    if (at) (void)command_ok(at, context, "AT+CFUN=4", AWAKE_CFUN_TIMEOUT_MS);
    return false;
}

bool modem_awake_verify_config(modem_radio_at_fn at, void *context)
{
    awake_config config;
    return at && read_config(at, context, &config) && all_off(&config);
}

bool modem_awake_prepare(modem_radio_at_fn at, void *context)
{
    if (!at) return false;
    awake_config config;
    if (!read_config(at, context, &config)) return fail_closed(at, context);
    if (all_off(&config)) return true;
    if (config.psm || config.edrx) {
        unsigned functionality;
        if (!command_ok(at, context, "AT+CFUN=4", AWAKE_CFUN_TIMEOUT_MS) ||
            !read_mode(at, context, "AT+CFUN?", "+CFUN", TAIL_NONE,
                       AWAKE_CFUN_TIMEOUT_MS, &functionality) || functionality != 4)
            return fail_closed(at, context);
    }
    if ((config.slow_clock &&
         !command_ok(at, context, "AT+CSCLK=0", AWAKE_AT_TIMEOUT_MS)) ||
        (config.psm && !command_ok(at, context, "AT+CPSMS=0", AWAKE_AT_TIMEOUT_MS)) ||
        (config.edrx &&
         !command_ok(at, context, "AT+CEDRXS=0,4,\"0000\"", AWAKE_AT_TIMEOUT_MS)) ||
        !modem_awake_verify_config(at, context)) return fail_closed(at, context);
    return true;
}

bool modem_awake_verify_active(modem_radio_at_fn at, void *context)
{
    unsigned psm, edrx;
    return at && read_mode(at, context, "AT+CPSMRDP", "+CPSMRDP", TAIL_PSM_ACTIVE,
                          AWAKE_AT_TIMEOUT_MS, &psm) && psm == 0 &&
           read_mode(at, context, "AT+CEDRXRDP", "+CEDRXRDP", TAIL_EDRX_ACTIVE,
                     AWAKE_AT_TIMEOUT_MS, &edrx) && edrx == 0;
}
