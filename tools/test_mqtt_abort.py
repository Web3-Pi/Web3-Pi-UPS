#!/usr/bin/env python3
"""Run fault injection against actual esp-mqtt 1.0.0 code before/after our patch.

The original is reconstructed by reversing the complete checked-in patch series
and verified against its upstream SHA256. We compile verbatim resend, abort, write, ping,
keepalive functions, the whole CONNECTED switch arm and its real unlock/poll
epilogue; the complete, unchanged SDK mqtt_outbox.c is linked too. No manually
copied implementation serves as the oracle. Baseline failure is mandatory.

MQTT_TEST_SANITIZERS defaults to address,undefined; empty selects plain C.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

from mqtt_sdk_sources import reconstruct

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "firmware-ESP32-LTE-M/components/espressif__mqtt"


def extract_function(source, name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    assert match, name
    brace = source.index("{", match.start())
    depth = 0
    for position in range(brace, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():position + 1] + "\n"
    raise AssertionError(name)


def excerpts(source):
    names = ("has_timed_out", "esp_mqtt_write", "esp_mqtt_abort_connection",
             "esp_mqtt_client_ping", "process_keepalive", "mqtt_resend_queued",
             "mqtt_resend_pubrel")
    pieces = [extract_function(source, name) for name in names]
    task = extract_function(source, "esp_mqtt_task")
    start = task.index("        case MQTT_STATE_CONNECTED:")
    end = task.index("        case MQTT_STATE_WAIT_RECONNECT:", start)
    connected = task[start:end]
    start = task.index("        MQTT_API_UNLOCK(client);\n        if (", end)
    end = task.index("\n\n    }", start)
    epilogue = task[start:end]
    pieces.append("static void run_connected_iteration(esp_mqtt_client_handle_t client)\n"
                  "{\n    outbox_tick_t msg_tick = 0;\n    MQTT_API_LOCK(client);\n"
                  "    do { switch (client->state) {\n" + connected +
                  "    }\n" + epilogue + "\n    } while (0);\n}\n")
    return "\n".join(pieces)


def write_headers(build):
    (build / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n"
                                    "#define ESP_OK 0\n#define ESP_FAIL -1\n"
                                    "#define ESP_ERR_TIMEOUT 0x107\n")
    (build / "sdkconfig.h").write_text("/* Host defaults; no ESP_PLATFORM. */\n")
    (build / "esp_heap_caps.h").write_text("#pragma once\n#include <stdlib.h>\n"
        "#define MALLOC_CAP_DEFAULT 0\n#define heap_caps_malloc(n, caps) malloc(n)\n")
    (build / "esp_log.h").write_text("#pragma once\n"
        "#define ESP_LOGD(tag, ...) ((void)(tag))\n")
    (build / "host_support.h").write_text("#include <stdint.h>\n#include <stddef.h>\n#include <inttypes.h>\n"
        "#define ESP_MEM_CHECK(tag, value, action) do { if (!(value)) { action; } } while (0)\n"
        "#include <sys/queue.h>\n"
        "#ifndef STAILQ_FOREACH_SAFE\n"
        "#define STAILQ_FOREACH_SAFE(var, head, field, temp) "
        "for ((var) = STAILQ_FIRST(head); (var) && ((temp) = STAILQ_NEXT(var, field), 1); (var) = (temp))\n"
        "#endif\n")


def verify_sources(build):
    baseline, _, patched = reconstruct(build, SDK)
    return baseline, patched


def main():
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    flags = [f"-fsanitize={sanitizers}"] if sanitizers else []
    with tempfile.TemporaryDirectory(prefix="mqtt-abort-") as directory:
        build = Path(directory)
        write_headers(build)
        baseline, patched = verify_sources(build)
        for mqtt5 in (0, 1):
            for name, source in (("baseline", baseline), ("fixed", patched)):
                (build / "sdk_abort_excerpts.inc").write_text(excerpts(source))
                binary = build / f"{name}-mqtt5-{mqtt5}"
                subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-g", "-O1", "-fno-omit-frame-pointer", "-include", str(build / "host_support.h"),
                    "-I", str(build), "-I", str(SDK / "lib/include"),
                    str(ROOT / "tools/test_mqtt_abort.c"), str(SDK / "lib/mqtt_outbox.c"),
                    str(SDK / "lib/mqtt_service.c"),
                    "-o", str(binary)] + (["-DMQTT_PROTOCOL_5"] if mqtt5 else []) +
                    (["-DWUPS_CURRENT_SDK"] if name == "fixed" else []) + flags,
                    check=True, timeout=30)
                result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
                if name == "baseline":
                    assert result.returncode == 1, result.stdout + result.stderr
                    for path in ("QUEUED-write", "TRANSMITTED-write", "PUBREL-write", "PUBREL-build"):
                        assert f"FAIL {path}:" in result.stderr, result.stdout + result.stderr
                    assert "FAIL success:" not in result.stderr, result.stderr
                    assert "runtime error:" not in result.stderr and "Sanitizer" not in result.stderr, result.stderr
                    print(f"EXPECTED BASELINE FAILURE MQTT5={mqtt5}: {result.stdout.strip()}")
                else:
                    assert result.returncode == 0, result.stdout + result.stderr
                    assert not result.stderr, result.stderr
                    print(f"FIXED PASS MQTT5={mqtt5}: {result.stdout.strip()}")


if __name__ == "__main__":
    main()
