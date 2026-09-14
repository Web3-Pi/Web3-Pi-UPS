# ESP32 0.8.15: SINR telemetry

This release adds SIM7080G SINR measurements to the existing LTE-M telemetry.
It targets the production W3P MODEM V1 M.2 card (ESP32-S3FH4R2, 4 MB flash).

- `AT+CPSI?` RSSNR is converted using `SINR = 2 * RSSNR - 20`.
- The parser requires the complete documented 14-field LTE response. Missing,
  malformed and truncated responses do not create a SINR measurement.
- `net.status` v3 appends SINR to the unchanged v2 prefix. Signed `-128` means
  unavailable; `0 dB` remains a valid measurement. MQTT and Arkiv carry the
  extended frame; HTTP includes `sinr_db` when a reading is available.
- The matching panel change combines SINR, RSRQ, RSRP and RSSI. Installing
  firmware alone does not deploy that panel change.

Two fixed-APN OTA variants are prepared:

| APN | Image version | Runtime identity |
| --- | --- | --- |
| `iot.1nce.net` | `0.8.15-1nce` | `esp32:0.8.15-1nce` |
| `sensor.net` | `0.8.15-sensor` | `esp32:0.8.15-sensor` |

These variants use their selected APN regardless of SIM ICCID, including the
initial DCE configuration, PDP context and reconnect attempts. Builds without
`WUPS_FIXED_APN` keep the existing automatic ICCID-based selection.

Build with ESP-IDF 6.0.2 and the existing production configuration:

```sh
./tools/idf -B build-1nce -DPROJECT_VER=0.8.15-1nce -DWUPS_FIXED_APN=iot.1nce.net build
./tools/idf -B build-sensor -DPROJECT_VER=0.8.15-sensor -DWUPS_FIXED_APN=sensor.net build
```

The distributed `*-ota.bin` files are application images for the existing
two-slot OTA layout. Per-device provisioning is not part of these images.
Bootloader rollback support stays enabled. LTE-M-only mode, B3/B20 and the
existing modem-awake, MQTT and OTA recovery behavior are retained.

Parser, protocol layout, HTTP projection, MQTT routing and APN selection are
checked with host tests. Image format, embedded versions and OTA partition
fit are checked after building both variants. These checks do not replace a
hardware boot/registration test; no device is flashed as part of packaging.
