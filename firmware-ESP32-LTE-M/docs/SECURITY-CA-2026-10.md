# PKI remediation — ESP32 0.8.20

The firmware uses reviewed project-owned Mozilla CA inputs, blocks eight
compromised public keys before the ESP-IDF certificate callback, and explicitly
enforces certificate validity dates. MQTT, HTTPS, Arkiv and OTA use the same
trust wrapper. Radio, power control, automatic ICCID/APN selection, provisioning
and partition layout remain unchanged.

Version 0.8.19 is superseded: hardware acceptance discovered that its inherited
SDK configuration disabled `MBEDTLS_HAVE_TIME_DATE`. Real handshakes accepted an
expired leaf, a not-yet-valid leaf and an expired intermediate. In 0.8.20 the
flag is enabled in defaults and enforced by CMake and the binary/config audit.
The modem already synchronizes the clock before starting TLS clients.

See [trust inputs and update procedure](../certs/README.md) and the
[on-device TLS fixture](../../tools/tls_target_fixture/README.md).

## Build

With ESP-IDF v6.0.2 activated, from the firmware directory:

```sh
idf.py -B build-release-auto -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-release-auto/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
  -DPROJECT_VER=0.8.20 -DWUPS_FIXED_APN= build
python ../tools/check_tls_trust.py --build-dir build-release-auto
```

An empty `WUPS_FIXED_APN` selects the existing ICCID policy: five legacy SIM
entries use `iot.1nce.net`; other valid ICCIDs use `sensor.net`. It does not probe
arbitrary operators or alternate APNs on retry. Preserve intentional hardware
settings when migrating an existing generated sdkconfig, and explicitly enable
`CONFIG_MBEDTLS_HAVE_TIME_DATE=y`; stale generated settings are rejected.

The public checkout lacks the production Arkiv WS token. Use the deployment's
existing gitignored header for an operational image; never commit that header
or per-device provisioning. Upload only the app binary for OTA, not a merged
flash image, partition table or bootloader. A release must exclude the optional
network acceptance component and any disposable test trust anchors.

## Acceptance

- Match ESP32-S3 / 4 MB flash, the two-slot OTA layout, APN and installed image.
- Retain a verified recovery image and physical USB recovery access.
- Run actual mbedTLS certificate tests on the target. The fixture covers eight
  complete TLS handshakes and sixteen cross-issuer blocked-key chain checks,
  including a valid baseline without the guard for each blocked-key case.
- Check MQTT authentication, fresh telemetry, HTTPS and Arkiv RPC/WSS over LTE.
- Exercise real HTTPS OTA, pending-image confirmation and the automatic
  ten-minute rollback on missing healthy uplink. A direct USB app write with
  existing valid metadata does not prove these OTA transitions.
- Verify the final release bytes/version on the device, provisioning preservation
  and recovery of normal connectivity before expanding to other devices.

Fixture roots exist only in the isolated, network-free test app. The sixteen
blocked-key checks use real X.509 path verification, not TLS proof of possession
with compromised private keys. The optional network component makes read-only
requests and does not change the device backend mode or write to the chain.
