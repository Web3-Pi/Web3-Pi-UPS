#!/usr/bin/env python3
"""Run the production B-UART overlay and SDK constructors against host boundaries.

The real CMake generator, generated UART terminal, unchanged vendor UART driver
resource, actual default config and application's task-creation function run.
Only FreeRTOS/UART/exception boundaries are mocked. No device/serial/network.
MQTT_TEST_SANITIZERS defaults to address,undefined; overrides are explicit.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from test_modem_radio_adapter import function


HEADERS = {
    "freertos/FreeRTOS.h": """#pragma once
#include <cstddef>
#include <cstdint>
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
using TaskHandle_t = void *;
using TaskFunction_t = void (*)(void *);
using QueueHandle_t = void *;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define tskNO_AFFINITY -1
#define BIT0 1
#define BIT1 2
#define BIT2 4
""",
    "freertos/task.h": """#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t);
void vTaskDelete(TaskHandle_t);
BaseType_t xTaskGetCoreID(TaskHandle_t);
BaseType_t xPortGetCoreID();
UBaseType_t uxTaskPriorityGet(TaskHandle_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
""",
    "freertos/semphr.h": '#pragma once\n#include "freertos/FreeRTOS.h"\n',
    "esp_log.h": """#pragma once
void mock_log(const char *, const char *, ...);
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
""",
    "esp_idf_version.h": """#pragma once
#define ESP_IDF_VERSION_MAJOR 6
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(6,0,2)
""",
    "driver/uart.h": """#pragma once
#include <cstddef>
#include <cstdint>
#include "freertos/FreeRTOS.h"
using esp_err_t = int;
using uart_port_t = int;
using uart_word_length_t = int;
using uart_stop_bits_t = int;
using uart_parity_t = int;
using uart_sclk_t = int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_IDF_VERSION_MAJOR 6
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2
#define UART_NUM_MAX 3
#define UART_DATA_8_BITS 3
#define UART_STOP_BITS_1 1
#define UART_PARITY_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_HW_FLOWCTRL_CTS_RTS 3
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_PIN_NO_CHANGE -1
#define UART_FIFO_LEN 128
struct uart_config_t { int baud_rate, data_bits, stop_bits, parity, flow_ctrl, source_clk; };
enum uart_event_type_t { UART_DATA, UART_FIFO_OVF, UART_BUFFER_FULL, UART_BREAK, UART_PARITY_ERR, UART_FRAME_ERR };
struct uart_event_t { uart_event_type_t type; size_t size; };
esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t, int, int, int, int);
esp_err_t uart_set_hw_flow_ctrl(uart_port_t, int, int);
esp_err_t uart_set_sw_flow_ctrl(uart_port_t, bool, int, int);
esp_err_t uart_driver_install(uart_port_t, int, int, int, QueueHandle_t *, int);
esp_err_t uart_driver_delete(uart_port_t);
esp_err_t uart_set_rx_timeout(uart_port_t, int);
esp_err_t uart_set_rx_full_threshold(uart_port_t, int);
esp_err_t uart_flush_input(uart_port_t);
esp_err_t uart_get_buffered_data_len(uart_port_t, size_t *);
int uart_read_bytes(uart_port_t, void *, uint32_t, uint32_t);
int uart_write_bytes(uart_port_t, const void *, size_t);
""",
    "exception_stub.hpp": """#pragma once
#include <stdexcept>
#define ESP_MODEM_THROW_IF_FALSE(value, message) do { if (!(value)) throw std::runtime_error(message); } while (0)
#define ESP_MODEM_THROW_IF_ERROR(value, message) ESP_MODEM_THROW_IF_FALSE((value) == 0, message)
#define TRY_CATCH_RET_NULL(...) try { __VA_ARGS__ } catch (...) { return nullptr; }
""",
    "cxx_include/esp_modem_dte.hpp": """#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include "freertos/task.h"
#include "esp_modem_config.h"
#include "exception_stub.hpp"
namespace esp_modem {
enum class terminal_error { BUFFER_OVERFLOW, UNEXPECTED_CONTROL_FLOW, CHECKSUM_ERROR };
class SignalGroup {
public:
    void set(size_t) {}
    void wait_any(size_t, uint32_t) {}
    bool is_any(size_t) { return false; }
};
class Terminal {
public:
    virtual ~Terminal() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual int write(uint8_t *, size_t) = 0;
    virtual int read(uint8_t *, size_t) = 0;
    virtual void set_read_cb(std::function<bool(uint8_t *, size_t)>) = 0;
    std::function<bool(uint8_t *, size_t)> on_read;
    std::function<void(terminal_error)> on_error;
};
std::unique_ptr<Terminal> create_uart_terminal(const esp_modem_dte_config *);
}
""",
}


def generate(cmake_file, component, output, harness, success=True):
    harness.write_text('include("' + str(cmake_file) + '")\n'
                       'wups_generate_modem_uart("' + str(component) + '" "' + str(output) + '")\n')
    result = subprocess.run(["cmake", "-P", str(harness)], capture_output=True, text=True, timeout=30)
    if (result.returncode == 0) != success:
        raise AssertionError("Unexpected generator result:\n" + result.stdout + result.stderr)
    if not success and output.exists():
        raise AssertionError("Rejected input still produced an overlay")
    return result.stderr + result.stdout


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M"
    component = firmware / "managed_components/espressif__esp_modem"
    generator = firmware / "cmake/wups_modem_core1.cmake"
    source = (firmware / "main/modem.c").read_text()
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_core1 sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    for file in (generator, firmware / "main/modem.c", component / "src/esp_modem_uart.cpp",
                 component / "src/esp_modem_term_uart.cpp", component / "include/esp_modem_config.h"):
        print(str(file.relative_to(root)) + " SHA256=" + hashlib.sha256(file.read_bytes()).hexdigest(), flush=True)
    print("Scope: real generator, vendor constructors/defaults, app task creation; SDK simulated.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-core1-") as directory:
        temporary = Path(directory)
        for name, content in HEADERS.items():
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(content)
        generated = temporary / "esp_modem_uart.cpp"
        harness = temporary / "generate.cmake"
        generate(generator, component, generated, harness)
        first = generated.read_bytes()
        generate(generator, component, generated, harness)
        assert generated.read_bytes() == first, "Overlay must be deterministic"
        print("generated UART SHA256=" + hashlib.sha256(first).hexdigest(), flush=True)
        # Real generator must refuse upstream drift before emitting any source.
        fixture = temporary / "upstream"
        for relative in ("idf_component.yml", "src/esp_modem_uart.cpp", "src/esp_modem_term_uart.cpp"):
            copied = fixture / relative
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(component / relative, copied)
        for relative in ("src/esp_modem_uart.cpp", "src/esp_modem_term_uart.cpp", "idf_component.yml"):
            altered = fixture / relative
            pristine = altered.read_bytes()
            altered.write_bytes(pristine.replace(b"version: 1.4.2", b"version: 1.4.3")
                                if relative.endswith("yml") else pristine + b"\n/* unexpected drift */\n")
            diagnostic = generate(generator, fixture, temporary / "rejected.cpp", harness, False)
            assert "hash" in diagnostic.lower() or "requires exactly esp_modem" in diagnostic, diagnostic
            altered.write_bytes(pristine)
        print("generator guards PASS: version and both upstream hashes; deterministic output", flush=True)
        startup = function(source, "void modem_at_pass_through_start(")
        (temporary / "modem_startup.inc").write_text(startup)
        for profile in (0, 1):
            (temporary / "sdkconfig.h").write_text(
                f"#pragma once\n#define CONFIG_WUPS_MODEM_CORE1 {profile}\n#define CONFIG_WUPS_PERF_DIAG 1\n")
            binary = temporary / ("test_modem_core1_" + str(profile))
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
                "-Wno-unused-parameter", "-include", str(temporary / "sdkconfig.h"),
                "-I", str(temporary), "-I", str(component / "include"),
                "-I", str(component / "private_include"),
                str(root / "tools/test_modem_core1.cpp"), str(generated),
                str(component / "src/esp_modem_term_uart.cpp"), "-o", str(binary)]
            if sanitizers:
                command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
