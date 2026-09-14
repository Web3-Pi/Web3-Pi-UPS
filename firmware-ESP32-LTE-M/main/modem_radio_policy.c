#include "modem_radio_policy.h"

#include <limits.h>
#include <string.h>

#define RADIO_AT_TIMEOUT_MS 3000u
#define RADIO_CFUN_TIMEOUT_MS 10000u

typedef struct {
    const char *data;
    size_t length;
} text_span;

typedef enum {
    SETTINGS_INVALID,
    SETTINGS_MISMATCH,
    SETTINGS_MATCH
} settings_result;

static bool horizontal_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static text_span trim(text_span text)
{
    while (text.length && horizontal_space(text.data[0])) {
        text.data++;
        text.length--;
    }
    while (text.length && horizontal_space(text.data[text.length - 1]))
        text.length--;
    return text;
}

static bool equals(text_span text, const char *literal)
{
    size_t length = strlen(literal);
    return text.length == length && !memcmp(text.data, literal, length);
}

static bool starts_with(text_span text, const char *literal)
{
    size_t length = strlen(literal);
    return text.length >= length && !memcmp(text.data, literal, length);
}

/* Only LF completes a line. A trailing CR/partial OK is still in flight. */
static bool next_line(text_span *remaining, text_span *line)
{
    const char *end = memchr(remaining->data, '\n', remaining->length);
    if (!end) return false;
    size_t length = (size_t)(end - remaining->data);
    *line = trim((text_span){remaining->data, length});
    remaining->data += length + 1;
    remaining->length -= length + 1;
    return true;
}

modem_radio_response_status_t modem_radio_response_status(const char *response,
                                                         size_t length)
{
    if ((!response && length) || length >= MODEM_RADIO_RESPONSE_CAPACITY)
        return MODEM_RADIO_RESPONSE_ERROR;
    if (!length) return MODEM_RADIO_RESPONSE_WAIT;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)response[i];
        if ((c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c > 0x7e)
            return MODEM_RADIO_RESPONSE_ERROR;
    }
    bool ok = false;
    text_span remaining = {response, length}, line;
    while (next_line(&remaining, &line)) {
        if (equals(line, "ERROR") || starts_with(line, "+CME ERROR:") ||
            starts_with(line, "+CMS ERROR:") || equals(line, "NO CARRIER") ||
            equals(line, "NO ANSWER") || equals(line, "BUSY"))
            return MODEM_RADIO_RESPONSE_ERROR;
        if (equals(line, "OK")) {
            if (ok) return MODEM_RADIO_RESPONSE_ERROR;
            ok = true;
        }
    }
    return ok ? MODEM_RADIO_RESPONSE_OK : MODEM_RADIO_RESPONSE_WAIT;
}

static bool exchange(modem_radio_at_fn at, void *context, const char *command,
                     unsigned timeout_ms, char response[MODEM_RADIO_RESPONSE_CAPACITY],
                     size_t *length)
{
    /* A callback which forgets its NUL terminator must not pass accidentally
     * because a previous command left zeros in the scratch buffer. */
    memset(response, 0xa5, MODEM_RADIO_RESPONSE_CAPACITY);
    if (!at(context, command, response, MODEM_RADIO_RESPONSE_CAPACITY, timeout_ms))
        return false;
    const char *end = memchr(response, '\0', MODEM_RADIO_RESPONSE_CAPACITY);
    if (!end) return false;
    *length = (size_t)(end - response);
    return modem_radio_response_status(response, *length) == MODEM_RADIO_RESPONSE_OK;
}

static bool command_ok(modem_radio_at_fn at, void *context, const char *command,
                       unsigned timeout_ms)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    return exchange(at, context, command, timeout_ms, response, &length);
}

/* Return 0 for another line, 1 for a matching key, -1 for a malformed key. */
static int named_value(text_span line, const char *key, text_span *value)
{
    size_t key_length = strlen(key);
    if (!starts_with(line, key)) return 0;
    text_span rest = {line.data + key_length, line.length - key_length};
    if (rest.length && rest.data[0] != ':' && !horizontal_space(rest.data[0]))
        return 0; /* A different URC key, e.g. +CNMPOTHER. */
    rest = trim(rest);
    if (!rest.length || rest.data[0] != ':') return -1;
    rest.data++;
    rest.length--;
    *value = trim(rest);
    return 1;
}

static bool take_unsigned(text_span *text, unsigned *number)
{
    *text = trim(*text);
    unsigned value = 0;
    size_t count = 0;
    while (count < text->length && text->data[count] >= '0' && text->data[count] <= '9') {
        unsigned digit = (unsigned)(text->data[count] - '0');
        if (value > (UINT_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        count++;
    }
    if (!count) return false;
    text->data += count;
    text->length -= count;
    *text = trim(*text);
    *number = value;
    return true;
}

static bool read_scalar(modem_radio_at_fn at, void *context, const char *command,
                        const char *key, unsigned *value)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    unsigned timeout = !strcmp(key, "+CFUN") ? RADIO_CFUN_TIMEOUT_MS : RADIO_AT_TIMEOUT_MS;
    if (!exchange(at, context, command, timeout, response, &length))
        return false;
    text_span remaining = {response, length}, line;
    bool found = false, terminal = false;
    while (next_line(&remaining, &line)) {
        if (equals(line, "OK")) terminal = true;
        text_span text;
        int match = named_value(line, key, &text);
        if (match < 0) return false;
        if (!match) continue;
        if (terminal || found || !take_unsigned(&text, value) || text.length) return false;
        found = true;
    }
    return found;
}

static settings_result read_bands(modem_radio_at_fn at, void *context)
{
    char response[MODEM_RADIO_RESPONSE_CAPACITY];
    size_t length;
    if (!exchange(at, context, "AT+CBANDCFG?", RADIO_AT_TIMEOUT_MS, response, &length))
        return SETTINGS_INVALID;
    text_span remaining = {response, length}, line;
    bool found = false, matches = false, terminal = false;
    while (next_line(&remaining, &line)) {
        if (equals(line, "OK")) terminal = true;
        text_span text;
        int match = named_value(line, "+CBANDCFG", &text);
        if (match < 0) return SETTINGS_INVALID;
        if (!match) continue;
        if (starts_with(text, "\"NB-IOT\"")) continue; /* Inactive RAT's bands are irrelevant. */
        if (!starts_with(text, "\"CAT-M\"") || found || terminal) return SETTINGS_INVALID;
        found = true;
        text.data += 7;
        text.length -= 7;
        text = trim(text);
        if (!text.length || text.data[0] != ',') return SETTINGS_INVALID;
        text.data++;
        text.length--;
        unsigned count = 0;
        unsigned char seen[32] = {0};
        for (;;) {
            unsigned band;
            if (!take_unsigned(&text, &band) || !band || band > 255)
                return SETTINGS_INVALID;
            unsigned char mask = (unsigned char)(1u << (band % 8));
            if (seen[band / 8] & mask) return SETTINGS_INVALID;
            seen[band / 8] |= mask;
            count++;
            if (!text.length) break;
            if (text.data[0] != ',') return SETTINGS_INVALID;
            text.data++;
            text.length--;
        }
        matches = count == 2 && (seen[3 / 8] & (1u << (3 % 8))) &&
                  (seen[20 / 8] & (1u << (20 % 8)));
    }
    if (!found) return SETTINGS_INVALID;
    return matches ? SETTINGS_MATCH : SETTINGS_MISMATCH;
}

static settings_result read_settings(modem_radio_at_fn at, void *context)
{
    unsigned network, preference;
    if (!read_scalar(at, context, "AT+CNMP?", "+CNMP", &network) ||
        !read_scalar(at, context, "AT+CMNB?", "+CMNB", &preference))
        return SETTINGS_INVALID;
    settings_result bands = read_bands(at, context);
    if (bands == SETTINGS_INVALID) return SETTINGS_INVALID;
    return network == 38 && preference == 1 && bands == SETTINGS_MATCH
               ? SETTINGS_MATCH : SETTINGS_MISMATCH;
}

static bool fail_closed(modem_radio_at_fn at, void *context)
{
    if (at) (void)command_ok(at, context, "AT+CFUN=4", RADIO_CFUN_TIMEOUT_MS);
    return false;
}

bool modem_radio_prepare(modem_radio_at_fn at, void *context)
{
    if (!at) return false;
    settings_result initial = read_settings(at, context);
    if (initial == SETTINGS_MATCH) return true;
    if (initial == SETTINGS_INVALID) return fail_closed(at, context);
    unsigned functionality;
    if (!command_ok(at, context, "AT+CFUN=4", RADIO_CFUN_TIMEOUT_MS) ||
        !read_scalar(at, context, "AT+CFUN?", "+CFUN", &functionality) || functionality != 4 ||
        !command_ok(at, context, "AT+CNMP=38", RADIO_AT_TIMEOUT_MS) ||
        !command_ok(at, context, "AT+CMNB=1", RADIO_AT_TIMEOUT_MS) ||
        !command_ok(at, context, "AT+CBANDCFG=\"CAT-M\",3,20", RADIO_AT_TIMEOUT_MS) ||
        read_settings(at, context) != SETTINGS_MATCH)
        return fail_closed(at, context);
    return true;
}

bool modem_radio_resume(modem_radio_at_fn at, void *context)
{
    if (!at) return false;
    if (read_settings(at, context) != SETTINGS_MATCH) return fail_closed(at, context);
    unsigned functionality;
    if (!read_scalar(at, context, "AT+CFUN?", "+CFUN", &functionality))
        return fail_closed(at, context);
    if (functionality == 1) return true;
    if (functionality != 4 ||
        !command_ok(at, context, "AT+CFUN=1", RADIO_CFUN_TIMEOUT_MS) ||
        !read_scalar(at, context, "AT+CFUN?", "+CFUN", &functionality) || functionality != 1)
        return fail_closed(at, context);
    return true;
}
