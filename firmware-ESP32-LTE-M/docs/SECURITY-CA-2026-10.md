# CA security candidate 0.8.19-ca1

The previous default ESP-IDF CA snapshot could trust compromised emSign
intermediate keys. This candidate uses reviewed project-owned CA inputs,
blocks the eight affected public keys before SDK verification, and validates
the generated bundle and final image in CI. Normal TLS verification remains
enabled. Radio, power control, APN selection, provisioning and partition
layout are unchanged.

See [trust inputs and update procedure](../certs/README.md).

## Reproducible candidate builds

With ESP-IDF v6.0.2 activated, from the firmware directory:

```sh
idf.py -B build-ca-s3-1nce -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-ca-s3-1nce/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.release-c1' \
  -DPROJECT_VER=0.8.19-ca1-1nce -DWUPS_FIXED_APN=iot.1nce.net build
python ../tools/check_tls_trust.py --build-dir build-ca-s3-1nce

idf.py -B build-ca-s3-sensor -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-ca-s3-sensor/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.release-c1' \
  -DPROJECT_VER=0.8.19-ca1-sensor -DWUPS_FIXED_APN=sensor.net build
python ../tools/check_tls_trust.py --build-dir build-ca-s3-sensor
```

Do not flash the wrong APN variant. These are candidate images, not a
fleet-approved release. The public build lacks the production Arkiv WS token.
Keep the existing partition/provisioning data and USB recovery path.

## Device acceptance gate

- Match board ESP32-S3 / 4 MB flash, partition layout, APN and installed image.
- Retain a known recovery image and physical USB recovery access.
- Verify MQTT authentication and fresh telemetry, HTTPS, Arkiv RPC/WSS and
  firmware download with the new trust policy.
- Verify invalid hostname, untrusted/expired certificates and blocked-key
  cases fail; test alternate/cross-signed chains.
- Exercise reconnect, reboot and a subsequent OTA, including rollback.
- Expand to further devices only after the first device passes.

No firmware has been installed by this change alone.
