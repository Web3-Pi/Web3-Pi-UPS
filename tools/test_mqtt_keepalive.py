#!/usr/bin/env python3
"""Reproduce the pinned legacy PING bug, then exercise the bounded MQTT service.

The tests use the actual MQTT parser, encoder, outbox and selected client code.
Clock, transport, event callbacks and RTOS boundaries are deterministic fakes;
these are scheduling regressions, not a TLS/ESP32 runtime emulator.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from mqtt_sdk_sources import reconstruct
from test_mqtt_abort import extract_function

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "firmware-ESP32-LTE-M/components/espressif__mqtt"


def legacy_excerpts(source):
    names = ("esp_mqtt_handle_transport_read_error", "has_timed_out", "esp_mqtt_write",
             "esp_mqtt_abort_connection", "esp_mqtt_client_ping", "process_keepalive",
             "remove_initiator_message", "deliver_publish", "mqtt_message_receive",
             "mqtt_process_receive", "mqtt_resend_queued", "mqtt_resend_pubrel")
    bounded = "static esp_err_t mqtt_bounded_service(" in source
    if bounded:
        names = ("mqtt_client_read",) + names + ("mqtt_bounded_write", "mqtt_bounded_tx_step",
                 "mqtt_bounded_schedule_outbox", "mqtt_bounded_keepalive", "mqtt_bounded_service",
                 "mqtt_service_observe_end", "mqtt_delete_expired_messages")
    pieces = [extract_function(source, name) for name in names]
    task = extract_function(source, "esp_mqtt_task")
    start = task.index("        case MQTT_STATE_CONNECTED:")
    end = task.index("        case MQTT_STATE_WAIT_RECONNECT:", start)
    connected = task[start:end]
    start = task.index("        MQTT_API_UNLOCK(client);\n        if (", end)
    if bounded:
        start = task.rfind("        if (client->config->bounded_service) mqtt_service_observe_end(client);", end, start)
        assert start >= 0
    end = task.index("\n\n    }", start)
    epilogue = task[start:end]
    observation = ("atomic_store(&client->service_observation.slice_started_ms, platform_tick_get_ms());\n"
                   if bounded else "")
    pieces.append("static void run_connected_iteration(esp_mqtt_client_handle_t client)\n{\n"
                  "outbox_tick_t msg_tick = 0;\nMQTT_API_LOCK(client);\n" + observation +
                  "do { switch (client->state) {\n"
                  + connected + "}\n" + epilogue + "\n} while (0);\n}\n")
    if bounded:
        wait_start = task.index("        case MQTT_STATE_WAIT_RECONNECT:")
        wait_end = task.index("        default:", wait_start)
        pieces.append("static void run_wait_reconnect_iteration(esp_mqtt_client_handle_t client)\n{\n"
                      "MQTT_API_LOCK(client);\ndo { switch (client->state) {\n" +
                      task[wait_start:wait_end] + "} } while (0);\n"
                      "if (lock_depth) MQTT_API_UNLOCK(client);\n}\n")
    return "\n".join(pieces)


def headers(build):
    (build / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n"
        "#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_TIMEOUT 0x107\n"
        "#define ESP_ERR_NO_MEM 0x101\n")
    (build / "sdkconfig.h").write_text("#define CONFIG_MQTT_MSG_ID_INCREMENTAL 1\n")
    (build / "mqtt_client.h").write_text("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n"
        "#include \"esp_err.h\"\n"
        "typedef enum { MQTT_PROTOCOL_V_3_1=3, MQTT_PROTOCOL_V_3_1_1=4, MQTT_PROTOCOL_V_5=5 } esp_mqtt_protocol_ver_t;\n"
        "typedef struct { const char *filter; int qos; } esp_mqtt_topic_t;\n")
    (build / "platform.h").write_text("#pragma once\n#include <stdlib.h>\n"
        "static inline int platform_random(int max) { return max > 1 ? 1 : 0; }\n")
    (build / "esp_heap_caps.h").write_text("#pragma once\n#include <stdlib.h>\n"
        "#define MALLOC_CAP_DEFAULT 0\n#define heap_caps_malloc(n,caps) malloc(n)\n")
    (build / "esp_log.h").write_text("#pragma once\n#define ESP_LOGD(tag,...) ((void)(tag))\n")
    (build / "support.h").write_text("#include <stdlib.h>\n#include <stdio.h>\n#include <stdint.h>\n"
        "#include <stddef.h>\n#include <inttypes.h>\n#include <sys/queue.h>\n"
        "#define ESP_MEM_CHECK(tag,value,action) do { if (!(value)) { action; } } while (0)\n"
        "#ifndef STAILQ_FOREACH_SAFE\n#define STAILQ_FOREACH_SAFE(var,head,field,temp) "
        "for ((var)=STAILQ_FIRST(head); (var) && ((temp)=STAILQ_NEXT(var,field),1); (var)=(temp))\n#endif\n")


def compile_run(build, name, c_file, extra=()):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    binary = build / name
    command = compiler + ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
        "-Wno-sign-compare", "-g", "-O1", "-fno-omit-frame-pointer",
        "-include", str(build / "support.h"), "-I", str(build), "-I", str(SDK / "lib/include"),
        str(c_file), str(SDK / "lib/mqtt_msg.c"), str(SDK / "lib/mqtt_outbox.c"), *extra,
        "-o", str(binary)]
    if sanitizers:
        command += ["-fsanitize=" + sanitizers]
    subprocess.run(command, check=True, timeout=30)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not result.stderr, result.stderr
    print(result.stdout, end="", flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix="mqtt-keepalive-") as directory:
        build = Path(directory)
        _, legacy, current = reconstruct(build)
        headers(build)
        (build / "sdk_keepalive_excerpts.inc").write_text(legacy_excerpts(legacy))
        compile_run(build, "legacy", ROOT / "tools/test_mqtt_keepalive_baseline.c")
        (build / "sdk_keepalive_excerpts.inc").write_text(legacy_excerpts(current))
        compile_run(build, "bounded", ROOT / "tools/test_mqtt_keepalive.c",
                    [str(SDK / "lib/mqtt_service.c")])


if __name__ == "__main__":
    main()
