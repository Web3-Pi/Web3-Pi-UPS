#include "modem_support_diag.h"
#include "modem_diag_clock.h"

#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_timer.h"

#if defined(ESP_PLATFORM) && !CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED
#error "Support diagnostics require cumulative responses also over CMUX"
#endif

#define TAG "modem_support"

static const char *const commands[] = {
    "AT+CGDCONT?\r",
    "AT+CPSMS?\r",
    "AT+CPSMRDP\r",
    "AT+CEDRXS?\r",
    "AT+CEDRX?\r",
    "AT+CEDRXRDP\r",
    "AT+CSCLK?\r",
    "AT+CPSMCFG?\r",
    "AT+CPSMCFGEXT?\r",
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

typedef enum {
    REPLY_WAIT,
    REPLY_OK,
    REPLY_ERROR,
    REPLY_INVALID,
} reply_status;

/* esp_modem_command() passes cumulative data and removes its callback under
 * line_lock before returning. Copy, never append, and log outside that lock.
 * This is independent of the smaller, strict radio-policy response buffer. */
static struct {
    char data[MODEM_SUPPORT_DIAG_CAPACITY];
    size_t length;
    size_t observed;
    reply_status status;
    bool overflow;
} reply;
static size_t next_command;

static bool printable(unsigned char c)
{
    return (c >= 0x20 && c <= 0x7e) || c == '\t';
}

static bool line_equals(const char *line, size_t length, const char *text)
{
    size_t n = strlen(text);
    return length == n && !memcmp(line, text, n);
}

static bool line_starts(const char *line, size_t length, const char *text)
{
    size_t n = strlen(text);
    return length >= n && !memcmp(line, text, n);
}

static reply_status classify(const char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (!printable(c) && c != '\r' && c != '\n') return REPLY_INVALID;
    }
    reply_status terminal = REPLY_WAIT;
    size_t start = 0;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != '\n') continue;
        const char *line = data + start;
        size_t n = i - start;
        start = i + 1;
        while (n && (*line == ' ' || *line == '\t' || *line == '\r')) {
            ++line;
            --n;
        }
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r')) --n;
        reply_status found = REPLY_WAIT;
        if (line_equals(line, n, "OK")) found = REPLY_OK;
        else if (line_equals(line, n, "ERROR") ||
                 line_starts(line, n, "+CME ERROR:") ||
                 line_starts(line, n, "+CMS ERROR:") ||
                 line_equals(line, n, "NO CARRIER") ||
                 line_equals(line, n, "NO ANSWER") ||
                 line_equals(line, n, "BUSY")) found = REPLY_ERROR;
        if (found != REPLY_WAIT) {
            if (terminal != REPLY_WAIT) return REPLY_INVALID;
            terminal = found;
        }
    }
    return terminal; /* A partial final line cannot finish a command. */
}

static esp_err_t reply_cb(uint8_t *data, size_t length)
{
    if (!data) {
        if (!length) return ESP_ERR_NOT_FINISHED;
        reply.status = REPLY_INVALID;
        reply.observed = length;
        return ESP_FAIL;
    }
    reply.observed = length;
    reply.overflow = length >= sizeof(reply.data);
    reply.length = reply.overflow ? sizeof(reply.data) - 1 : length;
    memcpy(reply.data, data, reply.length);
    reply.data[reply.length] = '\0';
    reply.status = classify(reply.data, reply.length);
    if (reply.overflow || reply.status == REPLY_INVALID || reply.status == REPLY_ERROR)
        return ESP_FAIL;
    return reply.status == REPLY_OK ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

static const char *result_name(esp_err_t rc)
{
    if (reply.overflow) return "OVERFLOW/TRUNCATED";
    if (reply.status == REPLY_INVALID) return "INVALID";
    if (rc == ESP_OK && reply.status == REPLY_OK) return "OK";
    if (rc == ESP_FAIL && reply.status == REPLY_ERROR) return "ERROR";
    if (rc == ESP_ERR_TIMEOUT) return "INCOMPLETE/TIMEOUT";
    return "INCOMPLETE/TRANSPORT";
}

static void log_reply(const char *command, esp_err_t rc)
{
    int command_length = (int)strlen(command) - 1; /* Omit the wire CR. */
    ESP_LOGI(TAG, "AT reply begin [%.*s] rc=%s status=%s bytes=%u captured=%u",
             command_length, command, esp_err_to_name(rc), result_name(rc),
             (unsigned)reply.observed, (unsigned)reply.length);
    /* Length-based iteration also handles embedded NUL safely. Preserve
     * printable lines even when another line is invalid; never emit binary
     * terminal controls or silently present a truncated reply as complete. */
    for (size_t offset = 0; offset < reply.length;) {
        size_t end = offset;
        bool text = true;
        while (end < reply.length && reply.data[end] != '\r' && reply.data[end] != '\n') {
            if (!printable((unsigned char)reply.data[end])) text = false;
            ++end;
        }
        if (end != offset) {
            if (text) ESP_LOGI(TAG, "AT [%.*s] %.*s", command_length, command,
                               (int)(end - offset), reply.data + offset);
            else ESP_LOGW(TAG, "AT [%.*s] <non-text line omitted; bytes=%u>",
                          command_length, command, (unsigned)(end - offset));
        }
        offset = end;
        while (offset < reply.length &&
               (reply.data[offset] == '\r' || reply.data[offset] == '\n')) ++offset;
    }
    ESP_LOGI(TAG, "AT reply end [%.*s] status=%s", command_length, command, result_name(rc));
}

static unsigned remaining_ms(int64_t started)
{
    int64_t now = esp_timer_get_time();
    if (now < started) return 0;
    int64_t elapsed = now - started;
    int64_t budget = (int64_t)MODEM_SUPPORT_DIAG_BATCH_MS * 1000;
    if (elapsed >= budget) return 0;
    return (unsigned)((budget - elapsed) / 1000);
}

void modem_support_diag_run(esp_modem_dce_t *dce,
                            bool (*ready)(void *), void *context)
{
    if (!dce || !ready) return;
    int64_t started = esp_timer_get_time();
    for (size_t index = next_command; index < COMMAND_COUNT; ++index) {
        if (!ready(context) || !remaining_ms(started)) return;
        const char *command = commands[index];
        /* The clock helper expects an AT command without the wire CR. */
        char command_name[24];
        size_t length = strlen(command) - 1;
        memcpy(command_name, command, length);
        command_name[length] = '\0';
        modem_diag_clock_log(command_name);
        unsigned remaining = remaining_ms(started);
        if (!remaining || !ready(context)) return;
        unsigned timeout = remaining < MODEM_SUPPORT_DIAG_COMMAND_MS
                               ? remaining : MODEM_SUPPORT_DIAG_COMMAND_MS;
        memset(&reply, 0, sizeof(reply));
        /* Advance even on a failed attempt. Otherwise a permanently broken
         * first query would starve all other diagnostics on every call. */
        next_command = (index + 1) % COMMAND_COUNT;
        esp_err_t rc = esp_modem_command(dce, command, reply_cb, timeout);
        log_reply(command, rc);
        if (reply.overflow || reply.status == REPLY_INVALID ||
            !((rc == ESP_OK && reply.status == REPLY_OK) ||
              (rc == ESP_FAIL && reply.status == REPLY_ERROR))) return;
    }
}
