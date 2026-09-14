#!/usr/bin/env python3
"""Compile exact vendor OTA functions, proving the old failure and patched result.

No device or SDK installation is needed. The upstream source is reconstructed
by reversing the reviewed patch and checked against its original SHA-256.
Eight simulated reads bound the baseline spin; firmware has no such test stub.
MQTT_TEST_SANITIZERS defaults to address,undefined, with no silent fallback.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "firmware-ESP32-LTE-M/components/esp_https_ota"
UPSTREAM_SOURCE = "6ea6b29551d1422b46c217795cd5dc06511b0165f70a4ab6417ec9ecdc98aa57"
PATCHED_SOURCE = "c38726feefc22f52a001d7b38c2b8bb675350f59d02b7a827db0129aee2fb6bd"
CASES = {
    "header_eagain": 77, "header_zero": 77, "header_closed": 1,
    "header_short": 1, "header_complete": 0, "header_fragmented": 0,
    "desc_eagain": 77, "desc_zero": 77, "desc_closed": 1,
    "desc_short": 1, "desc_complete": 0, "desc_fragmented": 0,
    "perform_eagain": 77, "perform_zero": 77, "perform_closed": 1,
    "perform_short": 1, "perform_complete": 0, "perform_fragmented": 0,
    "perform_flash": 0, "perform_begin": 0, "perform_verify": 0,
    "body_eagain": 0, "body_zero": 1, "body_closed": 1,
    "body_complete": 0, "body_flash": 0,
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def functions(source):
    names = ("static esp_err_t read_header(",
             "static esp_err_t get_description_from_image(",
             "esp_err_t esp_https_ota_perform(")
    result = []
    for name in names:
        start = source.index(name)
        end = source.index("\n}\n", start) + 3
        result.append(source[start:end])
    return "\n".join(result)


def main():
    patched_pins = json.loads((VENDOR / "PATCHED_SHA256.json").read_text())
    for name, expected in patched_pins.items():
        assert sha(VENDOR / name) == expected, f"Changed vendor input: {name}"
    assert sha(VENDOR / "src/esp_https_ota.c") == PATCHED_SOURCE
    compiler = shlex.split(os.environ.get("CC", "cc"))
    sanitizers = os.environ.get("MQTT_TEST_SANITIZERS", "address,undefined").strip()
    print(f"https_ota_transport: sanitizers={sanitizers or 'none'}", flush=True)
    with tempfile.TemporaryDirectory(prefix="https-ota-transport-") as directory:
        temp = Path(directory)
        upstream = temp / "upstream"
        shutil.copytree(VENDOR, upstream)
        subprocess.run(["patch", "--batch", "-R", "-p1", "-i",
                        str(VENDOR / "0001-return-header-transport-errors.patch")],
                       cwd=upstream, check=True, timeout=30, capture_output=True, text=True)
        upstream_pins = json.loads((VENDOR / "UPSTREAM_SHA256.json").read_text())
        for name, expected in upstream_pins.items():
            assert sha(upstream / name) == expected, f"Not exact upstream input: {name}"
        assert sha(upstream / "src/esp_https_ota.c") == UPSTREAM_SOURCE
        for label, source in (("baseline", upstream), ("patched", VENDOR)):
            build = temp / label
            build.mkdir()
            (build / "ota_functions_under_test.inc").write_text(
                functions((source / "src/esp_https_ota.c").read_text()))
            binary = build / "test_https_ota_transport"
            command = compiler + ["-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                                  "-Werror", "-fno-omit-frame-pointer", "-I", str(build),
                                  str(ROOT / "tools/test_https_ota_transport.c"),
                                  "-o", str(binary)]
            if sanitizers:
                command += [f"-fsanitize={sanitizers}"]
            subprocess.run(command, check=True, timeout=30)
            for case, baseline_return in CASES.items():
                expected = baseline_return if label == "baseline" else 0
                result = subprocess.run([str(binary), case], timeout=30,
                                        capture_output=True, text=True)
                if result.returncode != expected:
                    raise AssertionError(f"{label}/{case}: exit={result.returncode}, "
                                         f"expected={expected}\n{result.stdout}{result.stderr}")
                if result.stderr and expected == 0:
                    raise AssertionError(f"{label}/{case}: unexpected stderr: {result.stderr}")
            print(f"PASS {label}: {len(CASES)} expected outcomes", flush=True)
    print("PASS exact upstream reversal, vendor input hashes, transport propagation, "
          "unchanged flash/verify errors", flush=True)


if __name__ == "__main__":
    main()
