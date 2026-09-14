# MQTT runtime host boundary tests

`../test_mqtt_runtime.py` compiles the complete current production `mqtt.c`
unchanged, plus the real dispatch queue, health state machine and packet guard.
Each case runs in a fresh process with three pthread-backed FreeRTOS tasks.

The SDK boundary requires every SDK call to come from `mqtt_owner`, outside
the application lock and outside SDK callbacks. The main isolation case holds
the enqueue boundary for **15 real seconds**. Throughout that hold, it checks
that producer, supervisor-style start requests, receipt polling and diagnostics
return within 100 ms on the host. DATA callbacks copy into a bounded command
queue while a separate command handler simulates blocked UART transmission.
It also verifies copied binary data, empty retained messages, queue rejection,
initialization cleanup/retry, pre-initialization OTA state and reconnect backoff
driven by the actual SDK return value.

The `pressure` case controls reported SDK outbox occupancy and injects one
SDK enqueue failure at a time. At 24 KiB, normal traffic waits while critical
traffic can use the reserve; at 32 KiB, both wait. Clearing pressure preserves
retry deadlines and FIFO. Both full (`-2`) and allocation failure (`-1`) leave
messages pending without duplicate SDK admission or same-class overtaking.

The `probe` case publishes a complete cached net.status v2 frame and delivers
PUBACK inside SDK enqueue before it returns. It checks the actual ARMING,
packet-result and health-admission sequence, rejects unrelated and pre-reconnect
ACKs, and requires a new proof after OTA. With the outbox full and then the owner
held at its SDK boundary, the independent monitor reaches admission failure at
the exact virtual 60-second deadline without a producer or diagnostics call
advancing the state first. This case uses virtual time, not another wall-clock
15-second hold.

The 100 ms ceiling is a host regression threshold, not a measured ESP32 latency
guarantee. The UART ACK check is a synthetic next RX-dispatch step; this harness
does not execute the real UART driver or `wups_rx`. Its controlled monotonic
clock accelerates retry deadlines separately from the real SDK hold.

SDK calls are stubs, not a network or TLS emulation. The SDK adapter stub models
its stopped-client contract; a separate adapter test verifies its actual pinned
SDK implementation. Heap/stack metric stubs are fixed synthetic values used for
compilation, not resource measurements. Production client lifetime continues
until reboot; the harness frees it only after all worker threads have joined.

Default sanitizer selection is `address,undefined`. Set `MQTT_TEST_SANITIZERS`
to `undefined` or an empty string explicitly on hosts without working ASan.
`MQTT_TEST_SDK_STALL_MS=0` permits a faster development pass; final regression
evidence should use the default full 15-second hold. No silent fallback occurs.
`MQTT_TEST_RUNTIME_CASES=pressure` selects one case; comma-separated names select
a subset. Omit it to run the complete eight-case suite.

## Optional local Linux runner

A native ASan startup failure can be checked in an isolated Linux container:

```sh
docker build -t wups-mqtt-host:local tools/mqtt_runtime_host
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt=no-new-privileges --tmpfs /tmp:rw,nosuid,nodev,exec \
  --mount type=bind,src="$PWD",dst=/workspace,readonly \
  wups-mqtt-host:local python3 tools/test_mqtt_runtime.py
```

The mount is read-only, there is no runtime network, and compiled tests live in
the disposable executable `/tmp`. Building the image downloads public compiler
packages. The 2026-09-14 Linux GCC 14.2 ASan+UBSan runs passed all eight runtime cases,
including a real 15,000 ms hold and the separately added SDK pressure and probe
cases. This is host evidence, not an ESP32 timing result.
