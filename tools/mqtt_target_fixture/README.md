# ESP32 MQTT / TLS loopback qualification

This standalone ESP-IDF 6.0.2 application exercises the repository's actual
opt-in MQTT service, ESP-TLS, mbedTLS and lwIP on an ESP32-S3. A second task is a
minimal TLS 1.2 MQTT broker at `127.0.0.1:18884`. No modem, Wi-Fi, internet,
provisioning, NVS or real MQTT credentials are used. The self-signed certificate
and private key in `main/` are disposable, public test fixtures, unsuitable for
any deployed service. Certificate verification remains enabled. The test sets
its own wall clock to September 2026 so the fixture certificate is valid.

The eight cases cover:

- PUBLISH and PINGRESP combined into one TLS application record, including PUBACK.
- A complete TLS record delivered in small ciphertext fragments.
- Positive ciphertext trickle exceeding the aggregate RX deadline.
- A TLS record prefix followed by silence.
- A missing PINGRESP with a finite response deadline.
- A delayed PINGRESP inside the response interval.
- Real TCP/TLS transmit backpressure with an aggregate operation deadline.
- Stop/cancellation while transmit progress remains blocked.

After the handshake, a custom **server** BIO splits real encrypted record bytes;
the client TLS and network stack are unmodified. Small TCP windows make the
backpressure cases reproducible. Host regression tests separately cover exact
packet ordering, the grace packet limit, outbox expiry and ownership.

Build from this directory with the installed ESP-IDF 6.0.2 environment:

```sh
source /path/to/esp-idf-v6.0.2/export.sh
IDF_COMPONENT_MANAGER=0 idf.py set-target esp32s3 build
```

All component inputs are local, so the component manager is unnecessary. The
app uses the repository vendor component via a relative CMake path. Its flash
header is configured for 4 MB to match the UPS hardware profile.

**Do not flash this fixture's generated partition table or bootloader onto a
provisioned UPS.** Preserve the original flash/partition data and install only
`build/web3pi_mqtt_loopback_test.bin` into an explicitly selected test slot. On
the tested UPS layout, OTA0 begins at `0x10000` and has size `0x180000`; verify the
actual device layout and binary size before using those values. Restore the
full firmware after qualification. Building this fixture never flashes a device.

The application runs once after boot and emits `CASE_RESULT` for every case,
followed by `FIXTURE_RESULT cases=8 failures=0 result=PASS`. Failure tests require
the intended delay and preconditions, so an immediate unrelated disconnect
does not count as a successful timeout test. Results include observed maximum
SDK mutex hold time, cancellation time, and independent observer-call duration.
Every case checks heap integrity after client destruction and reports free and
minimum free heap; TLS caches and task cleanup mean exact baseline equality is
not required.
These measurements cover the controlled loopback workload, not LTE performance
or a universal execution-time bound. The 10 ms service slice is a scheduling
budget; an individual cryptographic operation is not preempted at that limit.
The controlled fixture requires observed maximum lock hold at most 1000 ms,
independent observer calls at most 1000 microseconds, and stop completion below
500 ms. It does not establish a worst-case bound for arbitrary certificates or
workloads. A build or host test pass is not a device
qualification result.
