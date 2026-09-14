# ESP32 0.8.14: integrated MQTT, modem and OTA fixes

This source integrates the MQTT work for issues [#10](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/10), [#11](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/11), [#12](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/12) and [#13](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/13), followed by the modem and OTA fixes developed during the September 2026 investigation. The HTTP power.status, OLED balance and Arkiv command/ACK fixes for #7–9 were already on main and remain included. No RP2040, CH32 or wire-format changes are introduced by this integration.

## MQTT

Publications are copied into a bounded application queue. One owner calls the SDK; separate tasks execute commands and observe health. A message enters the SDK outbox once, and the SDK owns its retransmissions. Initialization keeps the client private until registration succeeds; a published client lives until reboot. Fresh, correlated QoS 1 PUBACK evidence is separate from CONNECT, PINGRESP, queue admission and UART age, and gates automatic MQTT OTA confirmation.

[MQTT-RESILIENCE.md](MQTT-RESILIENCE.md) describes the queue limits, ownership, probe correlation, rollback policy and remaining hardware acceptance tests. MQTT QoS 1 can still deliver duplicates. A blocked SDK call is diagnosed without forcibly deleting a task that may own SDK resources.

The local esp-mqtt 1.0.0 patch stops processing the connected-state branch immediately after a retransmission abort. It avoids additional PUBREL/PING work and polling an already closed descriptor while retaining the outbox for retry. See the [patch and provenance](../components/espressif__mqtt/WEB3PI_PATCH.md) and [stopped-task adapter](../../docs/mqtt-sdk-adapter.md).

## Modem

- Require LTE-M only, with B3 (1800 MHz) and B20 (800 MHz). The firmware verifies the radio configuration; failure is not silently accepted as permission to use NB-IoT or other bands.
- Explicitly disable UART sleep (`CSCLK=0`), PSM (`CPSMS=0`) and LTE-M eDRX. Read before setting, avoid unnecessary persistent writes, and verify active network sleep parameters after registration. DTR is held low and PWRKEY is released according to the board wiring.
- Log complete CEREG/COPS/CPSI responses and bounded 1NCE support diagnostics for APN, PSM and eDRX. Diagnostics identify unsynchronized time instead of pretending uptime is UTC, and avoid interfering with active OTA or a non-CMUX data channel.
- Track PPP lifecycle state and event ordering explicitly. A newer GOT_IP supersedes earlier loss/failure evidence; an actual later loss starts recovery. Interface identity and intentional teardown are checked. This fixes a reproduced case where an old lost-IP timer caused a newly established connection to be torn down.
- Select the APN from the existing fleet mapping: the five original cards use `iot.1nce.net`; other cards use `sensor.net`. Update the DCE's cached PDP context before programming the attach APN and entering PPP. Keep the selected APN on retries; registration timeouts no longer rotate it. Cards outside these two known profiles need an explicit configuration/code review.

The historical 0.8.14 field candidate fixed `iot.1nce.net` at build time. The integrated source restores fleet selection and adds an APN regression test; it is a different build from that historical image. The firmware does not infer that NB-IoT, Orange infrastructure or nearby airport interference caused the reported outages.

## ESP32 HTTPS OTA

Up to five download attempts share the same authorized URL, expected size/SHA and target slot, with 5/15/30/60-second backoff. Successful writes provide an in-memory resume offset. Before writing, the HTTP policy checks status, Content-Length and exact Content-Range. A server ignoring or rejecting Range causes a bounded restart from zero. Chunked or compressed firmware responses are rejected.

Transport failures and selected transient HTTP statuses can retry. Flash, image, certificate and final SHA failures are terminal. The complete image is read back and hashed before selecting it for boot. The update claim stays held throughout retries. Logging records attempt, offset, HTTP, errno and TLS details without exposing the URL.

The local [esp_https_ota patch](../components/esp_https_ota/PROVENANCE.md) returns image-header timeout/short-read errors instead of looping, preserving the distinction between transport and local failures. Both local SDK components carry licenses, upstream hashes and reversible patches; CMake checks the reviewed inputs. The installed ESP-IDF and managed dependency sources are not patched.

The transfer has a 20-minute overall limit and a 90-second no-progress limit, checked between SDK calls. These checks do not asynchronously interrupt a blocking SDK read; a slow stream can postpone the next check, which then prevents committing an expired transfer. Resume state does not survive reboot. RP2040 OTA relay behavior is unchanged.

**The improved downloader takes effect only after this firmware is installed.** Downloading it using an older image still uses that older image's OTA implementation.

## Verification and its limits

The integrated source passed a clean ESP-IDF 6.0.2 build for ESP32-S3, using the public example WebSocket token. The 911,824-byte image fits the 1,572,864-byte OTA slot with 42% free. It was not flashed during integration.

All **21 MQTT/modem/OTA host suites passed with Linux ASan+UBSan**, including 2,130 checks of the complete production APN bring-up flow for old/new SIMs, SDK cache/AT ordering, retries and failure gates. The MQTT runtime remained responsive during a real 15-second blocked SDK boundary. Three protocol suites and the two separate Arkiv balance/command-ACK suites also passed. The Arkiv runners are not counted as sanitizer coverage.

The regression commands are in [firmware-ci.yml](../../.github/workflows/firmware-ci.yml); the Arkiv tests run after the ESP32 build resolves cJSON. GitHub CI additionally builds RP2040 and CH32. Its result is recorded on the integration pull request.

The earlier fixed-APN 0.8.14 candidate passed a clean ESP-IDF 6.0.2 build, 20 MQTT/modem/OTA host suites with ASan/UBSan, and three protocol suites. It also passed local old-SIM startup/restart observations (300 s, 180 s and 180 s) with LTE-M B20, fresh MQTT proof and advancing RP2040 telemetry. Flash readback verified the new image and preserved the previous valid rollback image. The user subsequently reported installing that candidate on remote device ...9920.

Those observations do not constitute a hardware fault-injection test of SDK blocking, a broker that answers PING but withholds PUBACK, or an HTTPS OTA interrupted over real LTE. Host tests cover those controlled software boundaries. The generalized APN integration requires its own hardware validation, and the long-term Bemowo result remains under observation. No issue is automatically closed by this integration.

A public CI build uses the checked-in example WebSocket token. A production build must supply its existing private `main/arkiv_ws_token.h` locally; do not commit it or package it with source archives. See the [firmware README](../README.md) for build instructions.
