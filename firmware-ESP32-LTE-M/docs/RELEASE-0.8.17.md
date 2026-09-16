# ESP32 0.8.17: LTE-M timing

The default MQTT application profile now uses these explicit settings:

| Setting | Value | Meaning |
|---|---:|---|
| `session.message_retransmit_timeout` | 5000 ms | Retry an unacknowledged QoS publication; does not disconnect at 5 s |
| `network.timeout_ms` | 300000 ms | Deadline for each bounded connection phase or partial RX/TX packet |
| `session.keepalive` | 600 s | The pinned bounded client derives a 300 s PINGRESP deadline |
| Health probe interval/deadline | 300000 ms | Allow delayed publication admission and broker acknowledgement |

The existing PING receive grace remains 200 ms. Socket errors, PPP loss and
protocol errors can still end a connection before the deadline. Connection
phases have separate deadlines and can take more than 300 s in total.
The 30 s worker-stall diagnostic, queue limits, snapshot freshness/coalescing,
and one-hour MQTT outbox expiry remain unchanged. Panel command-response
expiry is a separate backend setting (300 s in the corresponding panel release).

Both fixed-APN artifacts are built from this release using
`sdkconfig.defaults;sdkconfig.release-c1`: CPU 240 MHz, configured modem UART
230400, CPU1 affinity, TX0 and sparse performance diagnostics. Benchmarks remain
disabled. The overlay also retains t300's Newlib and IRAM choices instead of
changing them to ESP-IDF 6.0.2's fresh-config defaults. The first OTA boot keeps
the existing 0.8.16 UART migration safeguard:
an image not yet marked VALID uses 115200 for that boot. Normal clean-build
defaults still leave the optional CPU1/TX0/performance profile disabled.

```sh
tools/idf -DIDF_TARGET=esp32s3 -B build-release-1nce -DSDKCONFIG=sdkconfig.release-1nce \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.release-c1' \
  -DPROJECT_VER=0.8.17-1nce -DWUPS_FIXED_APN=iot.1nce.net build
tools/idf -DIDF_TARGET=esp32s3 -B build-release-sensor -DSDKCONFIG=sdkconfig.release-sensor \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.release-c1' \
  -DPROJECT_VER=0.8.17-sensor -DWUPS_FIXED_APN=sensor.net build
```

Use only the application `.bin` for OTA. Each APN is fixed; there is no automatic
APN fallback. Provisioning remains in the device's existing NVS partition.
The release retains current main's UART/OTA safeguards. Temporary Bemowo
instrumentation and forced-DATA experiment switches are not promoted.

Host regressions verify the actual mqtt.c SDK configuration, 300 s admission
deadline, delayed PUBACK, partial RX/TX and PING deadline boundaries, and the
5 s retransmission boundary with removal on PUBACK. They use controlled clocks
and transports; a successful build/test is not proof that field LTE gaps are gone.
