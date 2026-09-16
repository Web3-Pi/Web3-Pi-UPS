#!/usr/bin/env python3
"""Compile enabled production transport wrappers against controlled SDK boundaries.

Checks behavior through public wrapper results and emitted window aggregates.
No driver, UART, FreeRTOS scheduler, PPP network, serial port or device is used.
MQTT_TEST_SANITIZERS defaults to address,undefined; overrides are explicit.
"""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


HEADERS = {
    "sdkconfig.h": "#pragma once\n#define CONFIG_WUPS_PERF_DIAG 1\n",
    "freertos/FreeRTOS.h": """#pragma once
#include <stdint.h>
#include <pthread.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void mock_lock(portMUX_TYPE *);
void mock_unlock(portMUX_TYPE *);
#define portENTER_CRITICAL(lock) mock_lock(lock)
#define portEXIT_CRITICAL(lock) mock_unlock(lock)
int xPortGetCoreID(void);
""",
    "freertos/task.h": '#pragma once\n#include "freertos/FreeRTOS.h"\n',
    "freertos/queue.h": "#pragma once\ntypedef void *QueueHandle_t;\n",
    "esp_log.h": """#pragma once
void mock_log(const char *, const char *, ...);
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
""",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "driver/uart.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
typedef int esp_err_t;
typedef int uart_port_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define UART_NUM_1 1
#define UART_NUM_2 2
typedef enum { UART_DATA, UART_BREAK, UART_BUFFER_FULL, UART_FIFO_OVF,
               UART_FRAME_ERR, UART_PARITY_ERR } uart_event_type_t;
typedef struct { uart_event_type_t type; size_t size; } uart_event_t;
""",
    "netif/ppp/pppos.h": """#pragma once
#include <stdint.h>
typedef int8_t err_t;
typedef uint8_t u8_t;
typedef struct { int marker; } ppp_pcb;
#define ERR_OK 0
""",
}


def main():
    root = Path(__file__).resolve().parents[1]
    firmware = root / "firmware-ESP32-LTE-M/main"
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("modem_transport_diag sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    for name in ("transport_diag.c", "transport_diag.h"):
        print(name + " SHA256=" + hashlib.sha256((firmware / name).read_bytes()).hexdigest(), flush=True)
    print("Scope: actual enabled wrappers; SDK/clock/locks simulated; no device accessed.", flush=True)
    with tempfile.TemporaryDirectory(prefix="modem-transport-diag-") as directory:
        temporary = Path(directory)
        for name, content in HEADERS.items():
            header = temporary / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(content)
        binary = temporary / "test_modem_transport_diag"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
            "-I", str(temporary), "-I", str(firmware),
            str(root / "tools/test_modem_transport_diag.c"),
            str(firmware / "transport_diag.c"), "-o", str(binary)]
        if sanitizers:
            command += ["-fsanitize=" + sanitizers, "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=30)
        for case in ("write", "read", "occupancy", "events", "generation", "lifecycle",
                     "snapshot", "concurrent", "ppp"):
            subprocess.run([str(binary), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
