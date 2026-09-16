#include "modem_uart_baud.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, scenarios;
static const char *scenario;
#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d [%s]: %s\n", __func__, __LINE__, scenario, #condition); \
        failures++; \
    } \
} while (0)
#define COUNT(items) (sizeof(items) / sizeof((items)[0]))

typedef enum {
    IO_NONE, IO_INSTALL, IO_CONFIG, IO_PIN, IO_BAUD, IO_FLUSH,
    IO_WRITE, IO_SHORT_WRITE, IO_WAIT, IO_READ
} io_failure;

typedef struct {
    int host_baud, modem_baud, original_baud;
    bool installed, data_mode, reject_write, ignore_write, lose_write_ok;
    bool corrupt_changed_readback, fail_delete;
    bool saw_escape, escape_valid;
    unsigned installs, deletes, writes, baud_writes, escapes, operations;
    unsigned read_fragment, response_delay_ms;
    int64_t now, ready_at, rx_ready_at, last_tx_at, escaped_at;
    int64_t changed_at, first_new_rate_command_at;
    unsigned clock_step_us, minimum_switch_guard_ms;
    io_failure fail_io;
    const char *corrupt_command;
    const unsigned char *corrupt_reply;
    size_t corrupt_length;
    const char *supported;
    char command[128];
    size_t command_length;
    unsigned char rx[2048];
    size_t rx_length, rx_offset;
} peripheral;

static peripheral sim;
static const int test_rates[] = {115200, 230400, 921600};
static const int test_targets[] = {115200, 230400};
static const modem_uart_baud_config_t config = { .port = UART_NUM_1, .tx_gpio = 17, .rx_gpio = 18 };
static void bounded(void)
{
    if (++sim.operations > 20000 || sim.now > INT64_C(180000000)) {
        fprintf(stderr, "Nonterminating UART operation [%s], time=%lld calls=%u\n",
                scenario, (long long)sim.now, sim.operations);
        exit(2);
    }
}

static void reset(const char *name, int baud)
{
    scenario = name;
    scenarios++;
    memset(&sim, 0, sizeof(sim));
    sim.modem_baud = sim.original_baud = baud;
    sim.read_fragment = sizeof(sim.rx);
    sim.response_delay_ms = 5;
    sim.last_tx_at = -INT64_C(10000000);
    sim.first_new_rate_command_at = -1;
    sim.supported = "\r\n+IPR: (),(300,600,1200,2400,4800,9600,19200,38400,57600,115200,230400,921600)\r\nOK\r\n";
}

static void cleaned(void)
{
    CHECK(!sim.installed);
    CHECK(sim.installs == sim.deletes);
    CHECK(sim.now < INT64_C(180000000));
}

static void queue_bytes(const void *bytes, size_t length)
{
    CHECK(length <= sizeof(sim.rx));
    if (length > sizeof(sim.rx)) abort();
    memcpy(sim.rx, bytes, length);
    sim.rx_offset = 0;
    sim.rx_length = length;
    sim.rx_ready_at = sim.now + sim.response_delay_ms * 1000;
}

static void queue(const char *text) { queue_bytes(text, strlen(text)); }

static void accept_command(void)
{
    char *command = sim.command;
    command[sim.command_length] = '\0';
    if (!sim.command_length) return;
    if (sim.host_baud != sim.modem_baud || sim.now < sim.ready_at) return;
    if (sim.saw_escape) {
        if (sim.escape_valid && sim.now - sim.escaped_at >= INT64_C(1000000))
            sim.data_mode = false;
        sim.saw_escape = false;
    }
    if (sim.data_mode) return;
    if (sim.changed_at && sim.first_new_rate_command_at < 0) {
        sim.first_new_rate_command_at = sim.now;
        if (sim.now - sim.changed_at < (int64_t)sim.minimum_switch_guard_ms * 1000)
            return;
    }
    if (sim.corrupt_command && !strcmp(command, sim.corrupt_command)) {
        queue_bytes(sim.corrupt_reply, sim.corrupt_length);
        return;
    }
    if (!strcmp(command, "AT")) {
        queue("\r\nOK\r\n");
    } else if (!strcmp(command, "AT+CGMR")) {
        queue("\r\nRevision:1951B14SIM7080\r\nOK\r\n");
    } else if (!strcmp(command, "AT+CMUX=?")) {
        queue("\r\n+CMUX: (0),(0),(1-6),(1-1509),(1-255),(0-100),(2-255),(1-255)\r\nOK\r\n");
    } else if (!strcmp(command, "AT+IPR?")) {
        char reply[96];
        int reported = sim.corrupt_changed_readback && sim.modem_baud != sim.original_baud
                     ? 0 : sim.modem_baud;
        snprintf(reply, sizeof(reply), "AT+IPR?\r\n+IPR: %d\r\nOK\r\n", reported);
        queue(reply);
    } else if (!strcmp(command, "AT+IPR=?")) {
        queue(sim.supported);
    } else if (!strncmp(command, "AT+IPR=", 7)) {
        int baud = atoi(command + 7);
        CHECK(baud == 115200 || baud == 230400 || baud == 921600);
        sim.baud_writes++;
        if (sim.reject_write) {
            queue("\r\nERROR\r\n");
        } else {
            if (!sim.ignore_write) {
                sim.modem_baud = baud;
                sim.changed_at = sim.now;
            }
            if (!sim.lose_write_ok) queue("\r\nOK\r\n");
        }
    } else {
        /* Baud preparation must not save/reset, alter SIM/APN/radio or dial. */
        CHECK(false);
        queue("\r\nERROR\r\n");
    }
}

void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }

int64_t esp_timer_get_time(void)
{
    bounded();
    sim.now += sim.clock_step_us;
    return sim.now;
}

void vTaskDelay(TickType_t ticks)
{
    bounded();
    CHECK(ticks < 60000);
    sim.now += (int64_t)ticks * 1000;
}

esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int event_size,
                              void *queue_handle, int flags)
{
    bounded();
    CHECK(port == config.port);
    CHECK(!sim.installed);
    CHECK(rx >= 128);
    CHECK(tx == 0 && event_size == 0 && queue_handle == NULL && flags == 0);
    if (sim.fail_io == IO_INSTALL) return ESP_FAIL;
    sim.installed = true;
    sim.installs++;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    sim.installed = false;
    sim.deletes++;
    return sim.fail_delete ? ESP_FAIL : ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *settings)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    CHECK(settings->data_bits == UART_DATA_8_BITS);
    CHECK(settings->parity == UART_PARITY_DISABLE);
    CHECK(settings->stop_bits == UART_STOP_BITS_1);
    CHECK(settings->flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
    if (sim.fail_io == IO_CONFIG) return ESP_FAIL;
    sim.host_baud = settings->baud_rate;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    CHECK(tx == config.tx_gpio && rx == config.rx_gpio);
    CHECK(rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
    return sim.fail_io == IO_PIN ? ESP_FAIL : ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t port, uint32_t baud)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    CHECK(baud == 115200 || baud == 230400 || baud == 921600);
    if (sim.fail_io == IO_BAUD) return ESP_FAIL;
    sim.host_baud = (int)baud;
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t port)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    if (sim.fail_io == IO_FLUSH) return ESP_FAIL;
    sim.rx_offset = sim.rx_length = 0;
    return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *buffer, size_t length)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    sim.writes++;
    if (sim.fail_io == IO_WRITE) return -1;
    if (sim.fail_io == IO_SHORT_WRITE) return length ? (int)length - 1 : 0;
    const char *bytes = buffer;
    if (length == 3 && !memcmp(bytes, "+++", 3)) {
        sim.escapes++;
        CHECK(sim.now - sim.last_tx_at >= INT64_C(1000000));
        sim.escape_valid = sim.host_baud == sim.modem_baud && sim.now >= sim.ready_at &&
                           sim.now - sim.last_tx_at >= INT64_C(1000000);
        sim.saw_escape = true;
        sim.escaped_at = sim.now;
        sim.command_length = 0;
    } else {
        if (sim.saw_escape) CHECK(sim.now - sim.escaped_at >= INT64_C(1000000));
        for (size_t i = 0; i < length; i++) {
            if (bytes[i] == '\r' || bytes[i] == '\n') {
                accept_command();
                sim.command_length = 0;
            } else {
                CHECK(sim.command_length + 1 < sizeof(sim.command));
                if (sim.command_length + 1 >= sizeof(sim.command)) abort();
                sim.command[sim.command_length++] = bytes[i];
            }
        }
    }
    sim.last_tx_at = sim.now;
    return (int)length;
}

esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    CHECK(ticks > 0 && ticks <= 5000);
    sim.now += 1000; /* Transmission itself consumes measurable wire time. */
    return sim.fail_io == IO_WAIT ? ESP_FAIL : ESP_OK;
}

int uart_read_bytes(uart_port_t port, void *buffer, uint32_t capacity, TickType_t ticks)
{
    bounded();
    CHECK(port == config.port && sim.installed);
    CHECK(capacity > 0);
    CHECK(ticks <= 5000);
    if (sim.fail_io == IO_READ) {
        sim.now += (int64_t)ticks * 1000;
        return -1;
    }
    int64_t deadline = sim.now + (int64_t)ticks * 1000;
    if (sim.rx_offset == sim.rx_length || sim.rx_ready_at > deadline) {
        sim.now = deadline;
        return 0;
    }
    if (sim.now < sim.rx_ready_at) sim.now = sim.rx_ready_at;
    size_t count = sim.rx_length - sim.rx_offset;
    if (count > capacity) count = capacity;
    if (count > sim.read_fragment) count = sim.read_fragment;
    memcpy(buffer, sim.rx + sim.rx_offset, count);
    sim.rx_offset += count;
    sim.now += 1000;
    return (int)count;
}

static void corrupt(const char *command, const void *response, size_t length)
{
    sim.corrupt_command = command;
    sim.corrupt_reply = response;
    sim.corrupt_length = length;
}

static void test_discovery(void)
{
    static const struct {
        const char *name;
        bool fragmented, data_mode;
        uint32_t ready_ms, window_ms;
    } cases[] = {
        {"probe at each supported baud", false, false, 0, 1500},
        {"fragmented probe reply", true, false, 0, 1500},
        {"escape retained data session", false, true, 0, 1500},
        {"cold delayed readiness", false, false, 12000, 30000},
        {"cold delayed data session", false, true, 12000, 30000}
    };
    for (size_t rate = 0; rate < COUNT(test_rates); ++rate) {
        int baud = test_rates[rate];
        for (size_t i = 0; i < COUNT(cases); ++i) {
            reset(cases[i].name, baud);
            if (cases[i].fragmented) sim.read_fragment = 1;
            sim.data_mode = cases[i].data_mode;
            sim.ready_at = (int64_t)cases[i].ready_ms * 1000;
            CHECK(modem_uart_baud_probe(&config, cases[i].window_ms));
            CHECK(sim.modem_baud == baud && sim.baud_writes == 0);
            CHECK(sim.now >= sim.ready_at && !sim.data_mode);
            if (cases[i].data_mode) CHECK(sim.escapes > 0);
            cleaned();
        }
    }
    reset("unreachable modem times out", 9600);
    CHECK(!modem_uart_baud_probe(&config, 1500));
    CHECK(sim.baud_writes == 0 && sim.now < INT64_C(12000000));
    cleaned();
}

static void test_migration(void)
{
    enum { UNCHANGED, SWITCH, FRAGMENTED, LOST_OK, REJECTED, IGNORED, UNSUPPORTED, BAD_READBACK };
    static const struct {
        const char *name;
        bool success;
        unsigned writes;
    } cases[] = {
        {"already target avoids persistent write", true, 0},
        {"migrate every directed baud pair", true, 1},
        {"fragmented migration replies", true, 1},
        {"lost old-rate OK with completed switch", true, 1},
        {"rejected IPR write", false, 1},
        {"ignored IPR write cannot masquerade as success", false, 1},
        {"missing target in supported list", false, 0},
        {"failed post-switch readback restores previous rate", false, 2}
    };
    for (size_t pair = 0; pair < COUNT(test_rates) * COUNT(test_targets); ++pair) {
        int source = test_rates[pair / COUNT(test_targets)];
        int target = test_targets[pair % COUNT(test_targets)];
        for (size_t i = 0; i < COUNT(cases); ++i) {
            if ((source == target) != (i == UNCHANGED)) continue;
            reset(cases[i].name, i == UNCHANGED ? target : source);
            sim.minimum_switch_guard_ms = i == SWITCH ? 200 : 0;
            if (i == FRAGMENTED) sim.read_fragment = 1;
            sim.lose_write_ok = i == LOST_OK;
            sim.reject_write = i == REJECTED;
            sim.ignore_write = i == IGNORED;
            sim.corrupt_changed_readback = i == BAD_READBACK;
            if (i == UNSUPPORTED)
                sim.supported = "\r\n+IPR: (),(9600,19200,38400)\r\nOK\r\n";
            CHECK(modem_uart_baud_prepare(&config, target) == cases[i].success);
            CHECK(sim.modem_baud == (cases[i].success ? target : source));
            CHECK(sim.baud_writes == cases[i].writes);
            if (i == SWITCH)
                CHECK(sim.first_new_rate_command_at - sim.changed_at >= INT64_C(200000));
            cleaned();
        }
    }
}

static void test_corruption(void)
{
    static const unsigned char binary[] = "\r\n+IPR: 115200\r\n\0OK\r\n";
    static const char *const responses[] = {
        "\r\nOK\r", "\r\nNOT OK\r\n", "\r\nERROR\r\n", "\r\nOK\r\nERROR\r\n",
        "\r\n+IPR: 115200\r\nOK\r\n\x1b", "\r\n+IPR: 115200\r\n"
    };
    for (size_t i = 0; i < COUNT(responses); ++i) {
        reset("corrupted/truncated discovery fails closed", 115200);
        corrupt("AT", responses[i], strlen(responses[i]));
        CHECK(!modem_uart_baud_probe(&config, 1500));
        CHECK(sim.baud_writes == 0);
        cleaned();

        reset("corrupted/truncated IPR read fails closed", 115200);
        corrupt("AT+IPR?", responses[i], strlen(responses[i]));
        CHECK(!modem_uart_baud_prepare(&config, 230400));
        CHECK(sim.baud_writes == 0);
        cleaned();
    }
    reset("embedded NUL reply fails closed", 115200);
    corrupt("AT+IPR?", binary, sizeof(binary) - 1);
    CHECK(!modem_uart_baud_prepare(&config, 230400));
    CHECK(sim.baud_writes == 0);
    cleaned();

    unsigned char oversized[1200];
    memset(oversized, 'X', sizeof(oversized));
    memcpy(oversized + sizeof(oversized) - 6, "\r\nOK\r\n", 6);
    reset("oversized reply fails closed", 115200);
    corrupt("AT+IPR?", oversized, sizeof(oversized));
    CHECK(!modem_uart_baud_prepare(&config, 230400));
    CHECK(sim.baud_writes == 0);
    cleaned();

    static const char *const commands[] = {"AT+CGMR", "AT+IPR=?", "AT+CMUX=?"};
    for (size_t i = 0; i < COUNT(commands); ++i) {
        reset("missing capability response blocks persistent write", 115200);
        corrupt(commands[i], "\r\nERROR\r\n", 9);
        CHECK(!modem_uart_baud_prepare(&config, 230400));
        CHECK(sim.baud_writes == 0);
        cleaned();
    }
}

static void test_cleanup_and_validation(void)
{
    for (io_failure failure = IO_INSTALL; failure <= IO_READ; ++failure) {
        reset("probe cleans up transport failure", 115200);
        sim.fail_io = failure;
        CHECK(!modem_uart_baud_probe(&config, 1500));
        cleaned();
        reset("prepare cleans up transport failure", 115200);
        sim.fail_io = failure;
        CHECK(!modem_uart_baud_prepare(&config, 230400));
        cleaned();
    }
    reset("invalid API target performs no I/O", 115200);
    CHECK(!modem_uart_baud_prepare(&config, 921600));
    CHECK(!modem_uart_baud_prepare(&config, 460800));
    CHECK(sim.installs == 0 && sim.writes == 0);
    cleaned();
    reset("null config performs no I/O", 115200);
    CHECK(!modem_uart_baud_prepare(NULL, 230400));
    CHECK(!modem_uart_baud_probe(NULL, 1500));
    CHECK(sim.installs == 0 && sim.writes == 0);
    cleaned();

    reset("UART delete error propagates from probe", 115200);
    sim.fail_delete = true;
    CHECK(!modem_uart_baud_probe(&config, 1500));
    CHECK(sim.deletes == 1);
    cleaned();
    reset("UART delete error propagates from prepare", 230400);
    sim.fail_delete = true;
    CHECK(!modem_uart_baud_prepare(&config, 230400));
    CHECK(sim.deletes == 1);
    cleaned();

    reset("deadline crosses between timer reads", 9600);
    sim.clock_step_us = 200000;
    CHECK(!modem_uart_baud_probe(&config, 1500));
    CHECK(sim.now < INT64_C(20000000));
    cleaned();
}

static void test_readback_parsers(void)
{
    reset("IPR current rate parser", 115200);
    int baud = 0;
    const char *valid = "AT+IPR?\r\n+CEREG: 5\r\n+IPR: 115200\r\nOK\r\n";
    CHECK(modem_uart_baud_parse_ipr(valid, strlen(valid), &baud) && baud == 115200);
    static const char *const invalid[] = {
        "\r\n+IPR: 115200junk\r\nOK\r\n", "\r\n+IPR: -115200\r\nOK\r\n",
        "\r\n+IPR: 9999999999999999999999\r\nOK\r\n", "\r\nOK\r\n",
        "\r\n+IPR: 115200\r\n+IPR: 230400\r\nOK\r\n",
        "\r\n+IPR: 115200\r\nOK\r", "\r\n+IPRX: 115200\r\nOK\r\n",
        "\r\nOK\r\n+IPR: 230400\r\n"
    };
    for (size_t i = 0; i < COUNT(invalid); ++i)
        CHECK(!modem_uart_baud_parse_ipr(invalid[i], strlen(invalid[i]), &baud));
    reset("supported rate parser", 115200);
    CHECK(modem_uart_baud_supports_ipr(sim.supported, strlen(sim.supported), 230400));
    static const char *const unsupported[] = {
        "\r\n+IPR: (),(1230400,115200)\r\nOK\r\n",
        "\r\n+IPR: (),(115200,230400junk)\r\nOK\r\n",
        "\r\n+IPR: (),(115200,230400\r\nOK\r\n",
        "\r\n+IPR: (),(115200,230400)\r\nOK\r",
        "\r\n+IPR: (),(115200,230400)\r\nERROR\r\n",
        "\r\nOK\r\n+IPR: (),(115200,230400)\r\n"
    };
    for (size_t i = 0; i < COUNT(unsupported); ++i)
        CHECK(!modem_uart_baud_supports_ipr(unsupported[i], strlen(unsupported[i]), 230400));
}

int main(void)
{
    test_discovery();
    test_migration();
    test_corruption();
    test_cleanup_and_validation();
    test_readback_parsers();
    printf("modem_uart_baud: %u scenarios, %u checks, %u failures\n", scenarios, checks, failures);
    return failures ? 1 : 0;
}
