# ESP32-S3 PKI acceptance fixture

This isolated ESP-IDF 6.0.2 app links the production `tls_trust.c` unchanged
and runs real mbedTLS 4 on an ESP32-S3. It uses no SDK mocks, modem, network,
device identity or provisioning. Disposable test roots replace the bundle
only inside this isolated app; they never enter the release firmware.

`generate.py` creates fresh P-256 test CA/server keys and certificates:

- Eight complete TLS 1.2 handshakes through bounded memory BIO queues:
  valid and alternate-issuer chains, wrong hostname, unknown CA, expired
  leaf, not-yet-valid leaf, expired intermediate, and invalid signature.
- Sixteen actual X.509 verification paths using the eight production
  blocked SPKIs, each signed by two different trusted test issuers. Each
  certificate must validate with the original SDK callback and fail with
  the production guard. No private key for a compromised SPKI is needed
  or used. These sixteen cases test real chain validation, not complete
  TLS handshakes/proof of possession with the blocked keys.

The clock is fixed at 2026-10-26 12:00 UTC. Negative cases must fail; the
fixture never treats a timeout, failed setup or broken positive control
as a successful certificate rejection. mbedTLS 4 maps a callback abort to
`MBEDTLS_ERR_X509_FATAL_ERROR` with an undefined all-ones verify result;
name/date checks instead retain their specific verification flags.

Run `generate.py` with Python `cryptography`, then build from this directory
using the production generated SDK configuration and ESP-IDF tool wrapper:

```sh
python generate.py
mkdir -p build
cp ../../firmware-ESP32-LTE-M/build-release-auto-0.8.20/sdkconfig build/sdkconfig
../../firmware-ESP32-LTE-M/tools/idf -B build -D SDKCONFIG=build/sdkconfig \
  -D IDF_TARGET=esp32s3 build
```

Use a backed-up, explicitly selected test device. Flash only the app at
the verified active slot address; do not write a bootloader, partition
table, otadata, runtime NVS or provisioning. Restore normal firmware after
the fixture. `TLS_FIXTURE COMPLETE failures=0` is the required result.

For a historical regression, clear `CONFIG_MBEDTLS_HAVE_TIME_DATE` in the
fixture's generated sdkconfig and use a separate build directory. This
reproduced acceptance of expired/future certificates on the test ESP32;
the current production CMake/build audit deliberately rejects that setting.

## Optional network acceptance component

`network_component` is an explicitly opt-in additional component for the
normal firmware, not for this isolated app. It starts an independent task
via the linker's `app_main` wrapper, waits for PPP and a synchronized clock,
then performs a HEAD request to the panel, calls the production Arkiv
`eth_blockNumber` helper and reads `eth_blockNumber` over production WSS.
It does not change backend mode, submit a transaction, write provisioning,
alter TLS verification or install test trust anchors.

Select both `main` and `network_component` in `COMPONENTS`, and point
`EXTRA_COMPONENT_DIRS` to this directory's `network_component` subdirectory.
Use an explicit `-accept` version. A successful run logs
`ACCEPTANCE COMPLETE PASS`. Release builds must exclude this component;
check the compile database and final binary before distribution.
