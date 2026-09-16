#!/usr/bin/env python3
"""Exercise 300s deadlines and 5s QoS retry in the actual bounded MQTT client.

The runtime suite separately verifies that mqtt.c passes these settings to the
SDK. This suite uses a simulated clock/transport with the real parser/outbox.
"""
from pathlib import Path
import tempfile
import test_mqtt_keepalive as keepalive


def main():
    with tempfile.TemporaryDirectory(prefix="mqtt-ltem-timing-") as directory:
        build = Path(directory)
        _, _, current = keepalive.reconstruct(build)
        keepalive.headers(build)
        (build / "sdk_keepalive_excerpts.inc").write_text(keepalive.legacy_excerpts(current))
        keepalive.compile_run(build, "ltem", keepalive.ROOT / "tools/test_mqtt_ltem_timing.c",
                              [str(keepalive.SDK / "lib/mqtt_service.c")])


if __name__ == "__main__":
    main()
