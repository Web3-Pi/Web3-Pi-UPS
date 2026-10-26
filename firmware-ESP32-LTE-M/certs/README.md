# Reviewed TLS trust inputs

`roots.pem` contains public Mozilla CA certificates downloaded from
https://curl.se/ca/cacert.pem on 2026-10-26 (source dated 2026-10-20), then
parsed and re-encoded as PEM. `manifest.json` records both source and result
SHA-256, certificate count, and the eight distrusted public SPKIs from
[rustls/webpki f3caaa9](https://github.com/rustls/webpki/commit/f3caaa9f2ba94a42dcd31767e367688a416a3d87).
These are public verification keys, never private keys.

The bundle replaces the SDK defaults. Neither `emSign ECC Root CA - G3`
nor `AAA Certificate Services` is included; both are already absent from
this source snapshot. No SDK local/deprecated roots are appended.

`tls_trust.c` checks the complete DER SubjectPublicKeyInfo for every
certificate passed to the verification callback, before delegating to the
ESP-IDF bundle callback. Reissuing/cross-signing a certificate with the same
blocked key cannot evade this comparison. The eight inputs in rustls omit
the outer ASN.1 SEQUENCE; the manifest/header restore and validate that
encoding to match Mbed TLS `pk_raw`. Signature, hostname, expiry
and chain checks are performed by the SDK. `MBEDTLS_HAVE_TIME_DATE` is
explicitly enabled and required by both CMake and the generated-image
audit; the SDK default does not check certificate validity dates. The
modem synchronizes the clock before starting its TLS clients. Roots in the compressed bundle are
also checked against the denylist by the build-input audit.

The SDK callback is exported but not a stable public API. Its source hash
is pinned in `cmake/wups_tls_trust.cmake`; an SDK upgrade must review the
callback contract and rerun the tests before changing that pin.

## Updating

1. Obtain a reviewed CA snapshot from the upstream source over verified TLS.
2. Review additions/removals and any incident-specific intermediate-key
   distrust changes. Do not append roots to an older default bundle.
3. Update the PEM and provenance/hashes in `manifest.json`. Maintain an
   overlap of valid issuers needed by the deployed endpoints during rotation.
4. In the ESP-IDF Python environment run
   `python ../tools/check_tls_trust.py --write-header`, then run the wrapper
   regressions with `python ../tools/test_tls_trust.py` on Linux.
5. Use separate fresh build directories/configs for each APN profile. Run
   `python ../tools/check_tls_trust.py --build-dir BUILD_DIR` after building;
   it compares the generated root subject/key pairs and checks their presence
   and the denylist in the actual firmware image.
6. Validate live endpoints and negative certificate cases on one recoverable
   device, then reboot/reconnect and test another OTA before fleet rollout.

The optional synthetic performance benchmark retains its dedicated GTS
Root R4. Normal MQTT/HTTP/Arkiv/OTA bundle users all use the trust wrapper.
A stale generated `sdkconfig` fails configuration instead of keeping old
roots silently. Regenerate from the reviewed defaults, preserving intentional
hardware/APN/release settings.

## Limits

Host wrapper tests use an SDK callback stub; they verify denylist placement,
all eight keys at multiple depths/flag combinations, error propagation and
fail-closed attachment. They do not establish an ESP32 TLS handshake or
LTE/OTA recovery. `tools/tls_target_fixture` additionally runs real mbedTLS
handshakes and cross-issuer blocked-key verification on the physical ESP32.
Its disposable trust anchors are isolated from production builds.
The public checkout also uses the existing placeholder
Arkiv WS token. Production rollout requires the separately provisioned token
and device verification; do not publish private headers in source artifacts.
