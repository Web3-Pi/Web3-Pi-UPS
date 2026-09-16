#!/usr/bin/env python3
"""Compile the real MQTT transport adapter with controlled public IDF APIs.

This tests adapter state/lifetime and the pinned IDF CONNECTING select behavior,
not real cryptography, sockets, lwIP scheduling, or hardware latency. In
particular the TLS WANT model is deliberately adversarial: a read during a
suspended write would flush it, as a TLS 1.2 HelloRequest alert can in mbedTLS.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    vendor = root / "firmware-ESP32-LTE-M/components/espressif__mqtt"
    stubs = root / "tools/test_mqtt_transport_nb_stubs"
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print("mqtt transport nb: sanitizers=" + (sanitizers or "none explicitly"), flush=True)
    common = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-I", str(stubs), "-I", str(vendor / "lib/include"),
    ]
    if sanitizers:
        common += ["-fsanitize=" + sanitizers, "-fno-sanitize-recover=all"]
    with tempfile.TemporaryDirectory(prefix="mqtt-transport-nb-") as directory:
        temporary = Path(directory)
        adapter = temporary / "adapter.o"
        binary = temporary / "test"
        subprocess.run(common + ["-Dcalloc=host_calloc", "-Dfree=host_free", "-c",
                                 str(vendor / "lib/mqtt_transport_nb.c"), "-o", str(adapter)], check=True, timeout=30)
        subprocess.run(common + [str(root / "tools/test_mqtt_transport_nb.c"),
                                 str(adapter), "-o", str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=30)
        rejected = subprocess.run(common + ["-DMBEDTLS_SSL_OUT_CONTENT_LEN=512", "-c",
                                  str(vendor / "lib/mqtt_transport_nb.c"), "-o", str(adapter)],
                                  capture_output=True, text=True, timeout=30)
        if rejected.returncode == 0 or "bounded MQTT write chunk exceeds" not in rejected.stderr:
            raise SystemExit("Expected compile-time rejection of TLS output record < write chunk")
        print("PASS compile guard: TLS output record smaller than service chunk rejected", flush=True)


if __name__ == "__main__":
    main()
