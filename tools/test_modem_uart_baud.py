#!/usr/bin/env python3
"""Run the production UART baud migration against a simulated modem and clock.

The peripheral double accepts AT commands only at its actual baud and models
guarded data-mode escape, fragmented replies, delayed boot and transport errors.
No serial port, modem, network or firmware flash is used. This does not replace
hardware validation of the SIM7080 UART and persistent AT+IPR behavior.
MQTT_TEST_SANITIZERS defaults to address,undefined; overrides are explicit.
"""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


HEADERS = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY UINT32_MAX
""",
    "freertos/task.h": """
#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
""",
    "esp_timer.h": """
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
    "esp_log.h": """
#pragma once
void mock_log(const char *tag, const char *format, ...);
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGD(...) mock_log(__VA_ARGS__)
#define ESP_LOGV(...) mock_log(__VA_ARGS__)
""",
    "driver/uart.h": """
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
#define UART_NUM_1 1
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    int source_clk;
} uart_config_t;
esp_err_t uart_driver_install(uart_port_t, int, int, int, void *, int);
esp_err_t uart_driver_delete(uart_port_t);
esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t, int, int, int, int);
esp_err_t uart_set_baudrate(uart_port_t, uint32_t);
esp_err_t uart_flush_input(uart_port_t);
int uart_write_bytes(uart_port_t, const void *, size_t);
esp_err_t uart_wait_tx_done(uart_port_t, TickType_t);
int uart_read_bytes(uart_port_t, void *, uint32_t, TickType_t);
""",
}


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_uart_baud sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    for name in ("modem_uart_baud.c", "modem_uart_baud.h", "modem_radio_policy.c"):
        print(name + " SHA256=" + hashlib.sha256((firmware / name).read_bytes()).hexdigest(), flush=True)
    print("Scope: production module; UART, modem and time simulated; no hardware accessed.", flush=True)
    print("Sources: 115200/230400/921600; targets: 115200/230400; all migrations/no-ops with faults.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-uart-baud-") as directory:
        temporary = Path(directory)
        for name, content in HEADERS.items():
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(content)
        binary = temporary / "test_modem_uart_baud"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_uart_baud.c"),
            str(firmware / "modem_uart_baud.c"),
            str(firmware / "modem_radio_policy.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=45)
        subprocess.run([str(binary)], check=True, timeout=45)


if __name__ == "__main__":
    main()
