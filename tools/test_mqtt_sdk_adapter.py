#!/usr/bin/env python3
"""Exercise the actual stopped-task adapter with pinned SDK lifecycle excerpts.

The lifecycle fixture must match the vendored, pinned SDK build inputs, including
the reviewed resend/abort and opt-in bounded-service patch series.
MQTT_TEST_SANITIZERS=undefined selects UBSan; an empty value selects plain C.
"""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile

from mqtt_sdk_sources import verify_current

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware-ESP32-LTE-M/main"
HOST = ROOT / "tools/mqtt_sdk_adapter_host"
SDK = ROOT / "firmware-ESP32-LTE-M/components/espressif__mqtt"
START_SHA = "a061f3b31aff4ee0ac8cfc8281057547cfca01eebbfe4705b6e69842b4894a91"
CLEANUP_SHA = "32d9c4d7037a11ed4cfa086e15275ea0e51ee3deb44a174e640760e9a5778026"


def excerpts(source):
    start = source.index("esp_err_t esp_mqtt_client_start(")
    end = source.index("\n}", start) + 2
    start_fn = source[start:end] + "\n"
    start = source.index("    esp_transport_close(client->transport);\n"
                         "    outbox_delete_all_items(client->outbox);")
    end = source.index("\n}", start)
    cleanup = source[start:end] + "\n"
    return start_fn, cleanup


def verify_source():
    fixture = (HOST / "sdk_lifecycle_1_0_0.c").read_text()
    start_fn, cleanup = excerpts(fixture)
    assert hashlib.sha256(start_fn.encode()).hexdigest() == START_SHA
    assert hashlib.sha256(cleanup.encode()).hexdigest() == CLEANUP_SHA
    upstream, patched = verify_current(SDK)
    pins = {**upstream, **patched}
    cmake = (MAIN / "CMakeLists.txt").read_text()
    for path in ("mqtt_client.c", "lib/include/mqtt_client_priv.h"):
        expected = pins[path]
        assert f"{path}|{expected}" in cmake, "Build must enforce the reviewed SDK pins"
    source = (SDK / "mqtt_client.c").read_text()
    assert excerpts(source) == (start_fn, cleanup)
    task = source[source.index("static void esp_mqtt_task(void *pv)\n{"):]
    entry = task[task.index("    client->run = true;"):task.index("    while (client->run)")]
    assert entry == ("    client->run = true;\n\n"
                     "    client->state = MQTT_STATE_INIT;\n"
                     "    xEventGroupClearBits(client->status_bits, STOPPED_BIT);\n")


def main():
    verify_source()
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined")
    sanitizer_flags = [f"-fsanitize={sanitizers}"] if sanitizers else []
    with tempfile.TemporaryDirectory(prefix="mqtt-sdk-adapter-") as build:
        for core_selection in (0, 1):
            binary = Path(build) / f"test_adapter_{core_selection}"
            subprocess.run(
                compiler + [
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                    "-fno-omit-frame-pointer", f"-DMQTT_CORE_SELECTION_ENABLED={core_selection}",
                    "-I", str(HOST), "-I", str(MAIN),
                    str(MAIN / "mqtt_sdk_adapter.c"),
                    str(HOST / "sdk_lifecycle_1_0_0.c"), str(HOST / "test_adapter.c"),
                    "-o", str(binary),
                ] + sanitizer_flags, check=True, timeout=30,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
