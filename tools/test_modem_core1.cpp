#include "cxx_include/esp_modem_dte.hpp"
#include "esp_modem_config.h"
#include "sdkconfig.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

static unsigned checks, failures;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)
#define MODEM_TAG "modem"
#define MODEM_UART_DIAG 0

struct task_request {
    TaskFunction_t function;
    void *argument;
    TaskHandle_t handle;
    uint32_t stack;
    unsigned priority;
    int affinity;
    char name[24];
};
static task_request last_task;
static unsigned task_calls, pinned_calls, task_deletes, installs, driver_deletes;
static int current_core, task_affinity, install_core, active_port = -1;
static bool fail_create, fail_install;
static uart_config_t actual_uart_config;
static int actual_rx, actual_tx, actual_queue;
static unsigned handle_tokens[64], next_handle;
static QueueHandle_t event_queue = &handle_tokens[63];
static char last_log[512];

void mock_log(const char *, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = std::vsnprintf(last_log, sizeof(last_log), format, arguments);
    va_end(arguments);
    CHECK(count >= 0 && static_cast<size_t>(count) < sizeof(last_log));
}
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
static BaseType_t create(TaskFunction_t function, const char *name, uint32_t stack,
                         void *argument, UBaseType_t priority, TaskHandle_t *handle, int affinity)
{
    ++task_calls;
    last_task = { function, argument, nullptr, stack, priority, affinity, "" };
    std::snprintf(last_task.name, sizeof(last_task.name), "%s", name);
    if (fail_create) return pdFALSE;
    CHECK(next_handle < 63);
    last_task.handle = &handle_tokens[next_handle++];
    if (handle) *handle = last_task.handle;
    return pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                      void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    return create(function, name, stack, argument, priority, handle, tskNO_AFFINITY);
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char *name, uint32_t stack,
                                  void *argument, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core)
{
    ++pinned_calls;
    return create(function, name, stack, argument, priority, handle, core);
}
void vTaskDelete(TaskHandle_t handle)
{
    CHECK(handle == nullptr || handle == last_task.handle);
    ++task_deletes;
}
BaseType_t xTaskGetCoreID(TaskHandle_t) { return task_affinity; }
BaseType_t xPortGetCoreID() { return current_core; }
UBaseType_t uxTaskPriorityGet(TaskHandle_t) { return last_task.priority; }
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t) { return pdFALSE; }
BaseType_t xQueueReset(QueueHandle_t queue) { CHECK(queue == event_queue); return pdPASS; }

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    CHECK(port == 1 || port == 2);
    actual_uart_config = *config;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t, int tx, int rx, int rts, int cts)
{
    CHECK(tx == 2 && rx == 4 && rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
    return ESP_OK;
}
esp_err_t uart_set_hw_flow_ctrl(uart_port_t, int, int) { CHECK(false); return ESP_FAIL; }
esp_err_t uart_set_sw_flow_ctrl(uart_port_t, bool, int, int) { CHECK(false); return ESP_FAIL; }
esp_err_t uart_driver_install(uart_port_t port, int rx, int tx, int count, QueueHandle_t *queue, int flags)
{
    CHECK(active_port == -1 && queue != nullptr && flags == 0);
    if (fail_install) return ESP_FAIL;
    ++installs;
    active_port = port; install_core = current_core;
    actual_rx = rx; actual_tx = tx; actual_queue = count;
    *queue = event_queue;
    return ESP_OK;
}
esp_err_t uart_driver_delete(uart_port_t port)
{
    CHECK(port == active_port);
    ++driver_deletes;
    active_port = -1;
    return ESP_OK;
}
esp_err_t uart_set_rx_timeout(uart_port_t port, int value)
{
    CHECK(port == active_port && value == 1); return ESP_OK;
}
esp_err_t uart_set_rx_full_threshold(uart_port_t port, int value)
{
    CHECK(port == active_port && value == 64); return ESP_OK;
}
esp_err_t uart_flush_input(uart_port_t) { return ESP_OK; }
esp_err_t uart_get_buffered_data_len(uart_port_t, size_t *length) { *length = 0; return ESP_OK; }
int uart_read_bytes(uart_port_t, void *, uint32_t, uint32_t) { return 0; }
int uart_write_bytes(uart_port_t port, const void *, size_t length)
{
    CHECK(port == active_port); return static_cast<int>(length);
}
static void ppp_supervisor_task(void *) {}
#include "modem_startup.inc"

static void startup(void)
{
    unsigned before = task_calls;
    modem_at_pass_through_start();
    CHECK(task_calls == before + 1);
    CHECK(last_task.function == ppp_supervisor_task && last_task.argument == nullptr);
    CHECK(!std::strcmp(last_task.name, "ppp_sup"));
    CHECK(last_task.stack == 8192 && last_task.priority == 5);
    CHECK(last_task.affinity == (CONFIG_WUPS_MODEM_CORE1 ? 1 : tskNO_AFFINITY));
}

static void lifecycle(int port, int tx_size, unsigned cycles)
{
    esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    CHECK(config.task_priority == 5 && config.task_stack_size == 4096 && config.dte_buffer_size == 512);
    CHECK(config.uart_config.rx_buffer_size == 4096 && config.uart_config.tx_buffer_size == 512);
    config.uart_config.port_num = port;
    config.uart_config.baud_rate = 230400;
    config.uart_config.tx_io_num = 2; config.uart_config.rx_io_num = 4;
    config.uart_config.tx_buffer_size = tx_size;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        current_core = CONFIG_WUPS_MODEM_CORE1 && port == 1 ? 1 : static_cast<int>(cycle % 2);
        unsigned before_tasks = task_calls, before_installs = installs;
        unsigned before_deletes = driver_deletes, before_task_deletes = task_deletes;
        auto terminal = esp_modem::create_uart_terminal(&config);
        CHECK(terminal != nullptr && task_calls == before_tasks + 1 && installs == before_installs + 1);
        CHECK(last_task.stack == 4096 && last_task.priority == 5 && last_task.argument != nullptr);
        CHECK(!std::strcmp(last_task.name, "uart_task"));
        CHECK(last_task.affinity == (CONFIG_WUPS_MODEM_CORE1 && port == 1 ? 1 : tskNO_AFFINITY));
        CHECK(install_core == current_core && active_port == port);
        CHECK(actual_rx == 4096 && actual_tx == tx_size && actual_queue == 30);
        CHECK(actual_uart_config.baud_rate == 230400 && actual_uart_config.flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
        CHECK(actual_uart_config.data_bits == UART_DATA_8_BITS && actual_uart_config.stop_bits == UART_STOP_BITS_1);
        uint8_t payload[] = {1, 2, 3};
        CHECK(terminal->write(payload, sizeof(payload)) == 3);
        terminal.reset();
        CHECK(active_port == -1 && driver_deletes == before_deletes + 1);
        CHECK(task_deletes == before_task_deletes + 1);
    }
    unsigned before_deletes = driver_deletes;
    fail_create = true;
    CHECK(esp_modem::create_uart_terminal(&config) == nullptr);
    CHECK(active_port == -1 && driver_deletes == before_deletes + 1);
    fail_create = false;
    unsigned before_tasks = task_calls;
    fail_install = true;
    CHECK(esp_modem::create_uart_terminal(&config) == nullptr);
    CHECK(active_port == -1 && task_calls == before_tasks);
    fail_install = false;
}

int main()
{
    startup();
    for (int tx : {512, 0}) {
        lifecycle(1, tx, 3); /* Each recreation must retain the selected profile. */
        lifecycle(2, tx, 2); /* The overlay must not pin another UART. */
    }
    CHECK((pinned_calls > 0) == static_cast<bool>(CONFIG_WUPS_MODEM_CORE1));
    std::printf("modem_core1 profile=%d: %u checks, %u failures\n", CONFIG_WUPS_MODEM_CORE1, checks, failures);
    return failures ? 1 : 0;
}
