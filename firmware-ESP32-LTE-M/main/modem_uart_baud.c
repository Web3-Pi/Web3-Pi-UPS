#include "modem_uart_baud.h"
#include "modem_radio_policy.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REPLY_SIZE MODEM_RADIO_RESPONSE_CAPACITY
#define AT_TIMEOUT_MS 1500u
#define PROBE_AT_MS 300u
#define ESCAPE_GUARD_MS 1100u

static const char *TAG = "MODEM_UART";
/* 921600 is detection-only: return units from the previous experiment. */
static const int rates[] = {115200, 230400, 921600};

typedef struct { const char *data; size_t length; } span_t;

static void trim(span_t *text)
{
    while (text->length && (text->data[0] == ' ' || text->data[0] == '\t' ||
                           text->data[0] == '\r')) {
        ++text->data;
        --text->length;
    }
    while (text->length && (text->data[text->length - 1] == ' ' ||
                           text->data[text->length - 1] == '\t' ||
                           text->data[text->length - 1] == '\r'))
        --text->length;
}

static bool take_integer(span_t *text, int *value)
{
    trim(text);
    if (!text->length || text->data[0] < '0' || text->data[0] > '9') return false;
    int result = 0;
    do {
        int digit = text->data[0] - '0';
        if (result > (INT_MAX - digit) / 10) return false;
        result = result * 10 + digit;
        ++text->data;
        --text->length;
    } while (text->length && text->data[0] >= '0' && text->data[0] <= '9');
    trim(text);
    *value = result;
    return true;
}

static bool ipr_value(const char *response, size_t length, span_t *value)
{
    if (modem_radio_response_status(response, length) != MODEM_RADIO_RESPONSE_OK)
        return false;
    bool found = false, terminal = false;
    span_t remaining = {response, length};
    while (remaining.length) {
        const char *end = memchr(remaining.data, '\n', remaining.length);
        if (!end) break;
        span_t line = {remaining.data, (size_t)(end - remaining.data)};
        remaining.length -= line.length + 1;
        remaining.data = end + 1;
        trim(&line);
        if (line.length == 2 && !memcmp(line.data, "OK", 2)) terminal = true;
        if (line.length < 4 || memcmp(line.data, "+IPR", 4)) continue;
        line.data += 4;
        line.length -= 4;
        /* A different URC such as +IPROTHER is not an IPR result. */
        if (line.length && line.data[0] != ':' && line.data[0] != ' ' &&
            line.data[0] != '\t') continue;
        trim(&line);
        if (terminal || found || !line.length || line.data[0] != ':') return false;
        ++line.data;
        --line.length;
        trim(&line);
        *value = line;
        found = true;
    }
    return found;
}

bool modem_uart_baud_parse_ipr(const char *response, size_t length, int *baud)
{
    span_t value;
    int parsed;
    if (!baud || !ipr_value(response, length, &value) ||
        !take_integer(&value, &parsed) || value.length) return false;
    *baud = parsed;
    return true;
}

bool modem_uart_baud_supports_ipr(const char *response, size_t length, int target)
{
    span_t value;
    if (target <= 0 || !ipr_value(response, length, &value)) return false;
    bool supported = false;
    do {
        if (!value.length || value.data[0] != '(') return false;
        ++value.data;
        --value.length;
        trim(&value);
        if (value.length && value.data[0] == ')') {
            ++value.data;
            --value.length;
            trim(&value);
        } else {
            for (;;) {
                int rate;
                if (!take_integer(&value, &rate)) return false;
                if (rate == target) supported = true;
                if (!value.length) return false;
                char delimiter = value.data[0];
                ++value.data;
                --value.length;
                trim(&value);
                if (delimiter == ')') break;
                if (delimiter != ',') return false;
            }
        }
        if (!value.length) return supported;
        if (value.data[0] != ',') return false;
        ++value.data;
        --value.length;
        trim(&value);
    } while (value.length);
    return false; /* Trailing comma. */
}

static bool open_uart(const modem_uart_baud_config_t *config)
{
    if (!config) return false;
    uart_config_t uart_config = {
        .baud_rate = rates[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(config->port, 512, 0, 0, NULL, 0) != ESP_OK)
        return false;
    if (uart_param_config(config->port, &uart_config) != ESP_OK ||
        uart_set_pin(config->port, config->tx_gpio, config->rx_gpio,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(config->port);
        return false;
    }
    return true;
}

static bool exchange(uart_port_t port, const char *command,
                     char response[REPLY_SIZE], unsigned timeout_ms)
{
    size_t length = 0;
    response[0] = '\0';
    size_t command_length = strlen(command);
    if (uart_flush_input(port) != ESP_OK ||
        uart_write_bytes(port, command, command_length) != (int)command_length ||
        uart_write_bytes(port, "\r\n", 2) != 2) return false;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t chunk[64];
        int64_t remaining_ms = (deadline - esp_timer_get_time() + 999) / 1000;
        if (remaining_ms <= 0) break;
        unsigned wait_ms = remaining_ms > 50 ? 50 : (unsigned)remaining_ms;
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        int count = uart_read_bytes(port, chunk, sizeof(chunk), ticks ? ticks : 1);
        if (count < 0) return false;
        if (!count) continue;
        if (length + (size_t)count >= REPLY_SIZE) return false;
        memcpy(response + length, chunk, (size_t)count);
        length += (size_t)count;
        response[length] = '\0';
        modem_radio_response_status_t status =
            modem_radio_response_status(response, length);
        if (status == MODEM_RADIO_RESPONSE_ERROR) {
            /* Never log binary bytes from a wrong baud or a data session. */
            response[0] = '\0';
            return false;
        }
        if (status == MODEM_RADIO_RESPONSE_OK) return true;
    }
    return false;
}

static bool set_host_rate(uart_port_t port, int rate)
{
    return uart_wait_tx_done(port, pdMS_TO_TICKS(500)) == ESP_OK &&
           uart_set_baudrate(port, (uint32_t)rate) == ESP_OK &&
           uart_flush_input(port) == ESP_OK;
}

static bool escape_data(uart_port_t port)
{
    if (uart_wait_tx_done(port, pdMS_TO_TICKS(500)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(ESCAPE_GUARD_MS));
    bool sent = uart_write_bytes(port, "+++", 3) == 3;
    bool drained = uart_wait_tx_done(port, pdMS_TO_TICKS(500)) == ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(ESCAPE_GUARD_MS));
    return sent && drained && uart_flush_input(port) == ESP_OK;
}

/* The finite AT scan budget excludes the explicitly bounded escape guards.
 * Visit all rates in each round, including initial and halfway escapes. */
static int detect_rate(uart_port_t port, uint32_t window_ms)
{
    char response[REPLY_SIZE];
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        if (set_host_rate(port, rates[i]) && escape_data(port) &&
            exchange(port, "AT", response, PROBE_AT_MS)) return rates[i];
    }
    const int64_t start = esp_timer_get_time();
    int64_t deadline = start + (int64_t)window_ms * 1000;
    const int64_t halfway = start + (int64_t)window_ms * 500;
    bool repeat_escape = window_ms >= 10000;
    while (esp_timer_get_time() < deadline) {
        bool escape_round = repeat_escape && esp_timer_get_time() >= halfway;
        if (escape_round) repeat_escape = false;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
            if (!set_host_rate(port, rates[i])) continue;
            if (escape_round) {
                int64_t before = esp_timer_get_time();
                bool escaped = escape_data(port);
                deadline += esp_timer_get_time() - before;
                if (!escaped) continue;
            }
            if (exchange(port, "AT", response, PROBE_AT_MS)) return rates[i];
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

static bool query(uart_port_t port, const char *command, char response[REPLY_SIZE])
{
    bool ok = exchange(port, command, response, AT_TIMEOUT_MS);
    if (ok) ESP_LOGI(TAG, "%s -> %s", command, response);
    else ESP_LOGW(TAG, "%s: no valid terminal OK", command);
    return ok;
}

static bool verify_rate(uart_port_t port, int expected)
{
    char response[REPLY_SIZE];
    int saved = -1;
    return exchange(port, "AT", response, AT_TIMEOUT_MS) &&
           query(port, "AT+IPR?", response) &&
           modem_uart_baud_parse_ipr(response, strlen(response), &saved) &&
           saved == expected;
}

static bool change_rate(uart_port_t port, int target)
{
    char command[32], response[REPLY_SIZE];
    snprintf(command, sizeof(command), "AT+IPR=%d", target);
    /* SIM7080 switches after the old-rate result. If that result is lost,
     * still switch the host and verify: timeout does not mean no mutation. */
    bool ack = exchange(port, command, response, AT_TIMEOUT_MS);
    ESP_LOGI(TAG, "%s: old-rate acknowledgment %s", command, ack ? "OK" : "missing");
    if (uart_wait_tx_done(port, pdMS_TO_TICKS(500)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!set_host_rate(port, target)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    return verify_rate(port, target);
}

static void restore_rate(uart_port_t port, int previous)
{
    int detected = detect_rate(port, 1500);
    bool restored = detected == previous && verify_rate(port, previous);
    if (detected && !restored) restored = change_rate(port, previous);
    if (restored) ESP_LOGW(TAG, "restored explicit UART rate %d", previous);
    else ESP_LOGE(TAG, "could not verify rollback to %d; modem remains blocked", previous);
}

bool modem_uart_baud_probe(const modem_uart_baud_config_t *config,
                           uint32_t window_ms)
{
    if (!open_uart(config)) return false;
    int detected = detect_rate(config->port, window_ms);
    if (detected) ESP_LOGI(TAG, "AT probe answered at %d baud", detected);
    if (uart_driver_delete(config->port) != ESP_OK) return false;
    return detected != 0;
}

bool modem_uart_baud_prepare(const modem_uart_baud_config_t *config,
                             int target_baud)
{
    if ((target_baud != 115200 && target_baud != 230400) || !open_uart(config))
        return false;
    bool ready = false;
    int detected = detect_rate(config->port, 1500);
    char response[REPLY_SIZE];
    int saved = -1;
    if (!detected) {
        ESP_LOGE(TAG, "no AT response at 115200/230400/921600; UART prepare blocked");
        goto done;
    }
    ESP_LOGI(TAG, "detected %d baud, target %d", detected, target_baud);
    /* Complete all four read-only capability checks before any IPR write. */
    bool version_ok = query(config->port, "AT+CGMR", response);
    bool ipr_ok = query(config->port, "AT+IPR?", response) &&
                  modem_uart_baud_parse_ipr(response, strlen(response), &saved);
    bool supported = query(config->port, "AT+IPR=?", response) &&
                     modem_uart_baud_supports_ipr(response, strlen(response), target_baud);
    bool cmux_ok = query(config->port, "AT+CMUX=?", response);
    if (!version_ok || !ipr_ok || !supported || !cmux_ok) {
        ESP_LOGE(TAG, "incomplete UART capabilities or unsupported target; no IPR write");
        goto done;
    }
    if (saved == target_baud) {
        ready = detected == target_baud && verify_rate(config->port, target_baud);
    } else {
        ESP_LOGW(TAG, "changing AUTO_SAVE IPR %d -> %d (active UART %d)",
                 saved, target_baud, detected);
        ready = change_rate(config->port, target_baud);
        if (!ready) restore_rate(config->port, detected);
    }
    if (ready) ESP_LOGI(TAG, "verified UART and saved IPR at %d baud", target_baud);
    else ESP_LOGE(TAG, "UART rate verification failed; PPP must remain stopped");
done:
    if (uart_driver_delete(config->port) != ESP_OK) ready = false;
    return ready;
}
