# MQTT resilience and verification

This implementation addresses issues [#10](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/10),
[#11](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/11),
[#12](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/12) and
[#13](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/13). Host checks do not establish
the cause of field LTE disconnects. The sealed old-SIM 0.8.14 image has passed
local boot/restart observations, and the user reported installing it on the
Bemowo device. Physical fault injection and prolonged field qualification remain
outstanding. See [0.8.14 integration and validation](RELEASE-0.8.14.md) for the
changes and evidence attached to the integrated source.

## Ownership and admission

UART, modem supervision and OTA producers copy messages into a bounded
application queue. One MQTT owner performs SDK initialization, subscription,
enqueue, reconnect and revive calls. The SDK callback copies complete downlink
messages into a separate command queue; a separate command task authenticates and
executes them. A third task observes health once per second, independently of a
blocked SDK call. No application state lock spans SDK/network I/O or command
execution.

`mqtt_runtime_init()` runs before UART producers, with retry by the main task if
allocation fails. Created tasks wait behind a commit barrier; partial startup
deletes those waiting tasks and the command queue. Getters return an unavailable
snapshot until commit. OTA transitions are latched even before runtime readiness.
Initialization has one caller. HTTP/Arkiv modes do not create this MQTT runtime.

The SDK client stays private until event registration succeeds. Registration
failure destroys only that private object. Once published, the client and topic
strings live until reboot; failed task startup retries the same registered client.
Recovery does not destroy/recreate a client still used by the SDK.

`mqtt_publish_raw()`, `mqtt_publish_snapshot()` and `mqtt_publish_critical()` return:

| Result | Meaning |
|---|---|
| `0` | Copied into the application queue |
| `-1` | Unavailable or invalid request |
| `-2` | Application queue/receipt capacity exhausted |
| `-3` | Topic or payload too large |

Application acceptance, SDK outbox admission, broker PUBACK and application
ingestion are separate events. The return value is **not** an MQTT packet ID.
Tracked submissions expose one of eight receipts for SDK admission, not delivery;
polling a terminal receipt or explicitly forgetting it releases the receipt slot.

Only known snapshots may coalesce, using their explicit key. FIFO is preserved
within each priority class even when its head is waiting for a retry; the other
class may continue. Opaque `net.publish`
traffic preserves bytes, QoS, retain and FIFO; empty retained payloads stay empty.
QoS 1 enters the SDK outbox once. After successful SDK admission, retransmission
belongs to the SDK, so a transport error cannot add another application copy.
MQTT QoS 1 still permits delivery duplicates; exactly-once application ingestion
is not promised. Failed admission retries are paced and bounded by the item's
deadline. Counters expose full, expired and replaced entries.

## Limits and resource budget

| Resource | Current limit |
|---|---|
| Application queue | 32 entries; 8 reserved for critical messages |
| Topic / payload | 63 topic bytes plus NUL / 256 payload bytes |
| Application lifetime | FIFO: 1 hour; known snapshots: 120 seconds |
| SDK outbox | 32 KiB total; ordinary admission stops at 24 KiB |
| SDK outbox lifetime | 1 hour, independently of application queue expiry |
| Downlink queue | 4 complete commands; 512 data bytes and up to 200 topic bytes each |
| SDK admission receipts | 8 concurrent receipts |
| New task stacks | Owner 6,144 B; command executor 8,192 B; monitor 3,072 B |
| Existing SDK task stack | 12,288 B |

The three new task stacks total 17 KiB. Queue metadata, downlink topic storage,
SDK outbox, TLS allocations and other task stacks are additional memory. Critical
reserve also applies to admission into the SDK outbox; it does not reorder entries
already in that outbox or guarantee delivery during an unlimited outage. OTA
progress is a replaceable snapshot; started/verifying/terminal events and command
responses use critical admission.

The owner ordinarily polls every 100 ms and also wakes for work. Retries start at
10 seconds; repeated authentication/connect failures use 30–120 second backoff.
Twenty rejected reconnect requests lead to a guarded revive attempt on the same
client. The private adapter requires the pinned SDK's DISCONNECTED + STOPPED
state, after transport/outbox cleanup. It clears the stopped bit before starting
and restores it only after task creation failure. This avoids overlapping a new
task with old cleanup; repeated rejections alone are not proof of task exit.
CMake verifies exact SDK source/header SHA-256 before using this adapter. The SDK
task exit path already purges its outbox; queued application messages remain available.
These decisions use actual SDK results, not request admission.

The project-local esp-mqtt 1.0.0 component also fixes retransmission error
fallthrough. After a failed QUEUED/TRANSMITTED/PUBREL resend it ends that session's
CONNECTED handling, avoiding another send or poll on the already closed
transport. Ordinary disconnection preserves the outbox for retry. The patch
does not shorten the initial transport write timeout or change the stopped-task
adapter's lifecycle contract. See the
[adapter documentation](../../docs/mqtt-sdk-adapter.md) and
[component provenance](../components/espressif__mqtt/WEB3PI_PATCH.md).

## Publication proof and recovery

An existing, recent `net.status` frame on the normal telemetry topic supplies a
selected QoS 1 probe; no new heartbeat topic or payload format is introduced.
There is one logical probe in flight. The first is due after CONNECT or OTA resume.
Periodic probes use a 120-second schedule, with a 60-second deadline including
scheduling/admission. The deadline starts at the planned due time, so a delayed
probe can begin less than 120 seconds before the next periodic probe.
`now >= deadline` is expired. A frame older than 90 seconds is not used as a probe.
The monitor detects a missed admission even if the owner never begins the probe.

Only a fresh, correlated PUBACK establishes broker publication proof. CONNECT and
PINGRESP do not establish it. Initial grace suppresses a premature failure verdict
but does not manufacture proof. Missing admission, missing ACK and a worker stuck
inside an SDK call have separate diagnostics. A busy worker is reported stalled
after 30 seconds without real progress; idle time is not a stall. UART age is
reported separately and is not evidence that Orange, the broker or the producer
has necessarily failed. Broker proof does not establish panel/database ingestion
or fresh RP2040 data.

Correlation uses a logical generation/sequence token and a separate packet guard.
The guard requires pinned **esp-mqtt 1.0.0**, incremental packet IDs
(`CONFIG_MQTT_MSG_ID_INCREMENTAL=y`), clean sessions and one SDK owner. Every
ID-generating enqueue/subscribe attempt, including a failed attempt, consumes a
conservative allocation budget. At 60,000 attempts, allocation pauses until the
SDK outbox is empty and a socket boundary followed by CONNECT renews the epoch.
Reconnect alone never clears the budget while old outbox entries remain. Early
ACKs are bounded and matched after enqueue returns; ambiguous or stale ACKs cannot
establish proof. SDK upgrades must revalidate this contract before changing the pin.

A degraded connected session can request a paced MQTT disconnect/reconnect, at
most once per 120 seconds. This does not delete a blocked task, destroy a live
client or erase the outbox. A permanently blocked SDK may remain blocked; the
monitor exposes that condition. Connected MQTT without proof and a stalled worker
are held out of the modem reset ladder. Existing disconnected-uplink DNS/PPP
recovery remains, with its existing limitations: DNS success or failure does not
identify the incident's root cause. This change adds no automatic CPU restart;
do not assume TWDT is configured to recover every task stall.

## OTA confirmation

Every transfer start and finish immediately invalidates MQTT proof, including a
failed transfer shorter than the monitor's sampling interval. Transfer pause is
not proof; resuming requires a new probe. Automatic MQTT image confirmation
requires current publication proof and must begin **before 600 seconds from
boot**. Reconnect and grace do not renew that window. HTTP/Arkiv retain their
existing caller-selected health criteria.

Confirmation and rollback are serialized without holding a critical section over
flash operations. The proof check and validation claim are serialized with OTA
start/finish. An active transfer prevents rollback. Two explicit recovery reasons
remain distinct from PUBACK: an accepted authorized next update and a deliberate
physical transfer may confirm the current image before overwriting its rollback
slot. If confirmation fails or remains pending, that overwrite is refused.

ESP32 image downloads allow five attempts with 5/15/30/60-second backoff. URL,
expected size/SHA and target slot remain fixed through the operation. Resume
uses the successfully written checkpoint within the current boot; HTTP status,
Content-Length and Content-Range are checked before writing. An incompatible
Range response causes a bounded restart from zero. Flash, certificate, image
validation and final SHA failures are terminal. The final flash SHA check
precedes selection of the new boot partition.

The local ESP-IDF v6.0.2 HTTPS OTA component returns header transport errors to
the caller rather than spinning on timeout. See its
[provenance and patch](../components/esp_https_ota/PROVENANCE.md). The application's
20-minute overall and 90-second no-progress limits are checked between SDK
operations; they cannot asynchronously interrupt one internally blocking read.
A sufficiently slow continuous response can delay the next check, but an expired
deadline prevents commit once the call returns. Resume across a device reboot
and compressed/chunked firmware responses are unsupported.

## LTE and PPP integration

The integrated modem policy requires LTE-M on B3/B20 and verifies UART sleep,
PSM and LTE-M eDRX are disabled. Full registration/operator/cell replies and
bounded APN/PSM/eDRX diagnostics make network gaps easier to correlate. These
settings do not identify the cause of the Bemowo outages; no observed log has
established NB-IoT switching or interference as their cause.

PPP event bits wake the supervisor; an ordered state machine records the latest
observation. Fresh GOT_IP supersedes an earlier LOST_IP, while a later real loss
is consumed once. Intentional teardown is recorded before calling the SDK.
This corrects the reproduced case where an old 120-second lost-IP timer caused
the supervisor to tear down a newly established connection. It does not resolve
every possible cause of lost network data.

APN selection restores both existing fleet groups: the five known original
SIM ICCIDs use `iot.1nce.net`; the others use `sensor.net`. The same selected value
is used for the DCE's cached PDP configuration and `AT+CGDCONT`, without retry
rotation. The earlier sealed old-SIM image used a fixed `iot.1nce.net`; its bench
results alone do not validate the integrated selection path on both SIM groups.

## Validation and remaining hardware gates

Portable queue, health and packet-guard tests exercise production modules. The OTA
policy harness extracts the production confirmation/rollback functions and stubs
flash and scheduling calls; it checks deadline ordering, failures, active/short
transfers, explicit recovery reasons and confirmation/rollback interleavings.
These tests do not emulate the whole ESP-IDF runtime.

Host regressions also cover power decoding (#7), Arkiv balance (#8), Arkiv command
sweep/ACK bookkeeping (#9), the HTTP command model and the deframer model. The
Python models are supporting regressions, not executions of all firmware paths.
During the original MQTT implementation, native macOS plain C and UBSan runs passed. The native ASan runtime hangs even
for a trivial smoke test, so the full MQTT/OTA suite was additionally run in an
isolated Linux container with GCC 14.2, ASan and UBSan: it passed. The complete
runtime harness held its SDK boundary for 15 real seconds while 5,089 host
responsiveness checks passed; its UART/SDK boundaries remain stubs, not hardware.
The original #12 harness also reproduced the old UAF under Linux ASan; the new
initialization/runtime tests passed there. Existing #7–9, HTTP and deframer
regressions passed on both hosts. GCC host coverage required explicit uint64_t
formatting in the OLED helper and a test-fixture indentation correction; these
do not change the ESP32 feature behavior.

The sealed 0.8.14 release additionally passed 20 MQTT/modem/OTA host suites with
ASan+UBSan and three protocol checks. These include the actual PPP callbacks,
the original and patched MQTT resend paths, OTA header transport errors,
download retry/resume and HTTP response validation. Local ESP32 observations
covered a 300-second post-install run and two 180-second startup/restart runs,
with LTE-M/B20, PPP/MQTT publication proof and fresh RP2040 frames. They did not
exercise intentional LTE interruption during HTTPS OTA. The integrated source
and APN selection have their own validation record in
[RELEASE-0.8.14.md](RELEASE-0.8.14.md).

The new runners default to ASan+UBSan, fail explicitly and impose a timeout; no
sanitizer failure silently falls back to a weaker mode. Run from the repository
root, for example:

```sh
MQTT_TEST_SANITIZERS=undefined python3 tools/test_mqtt_health.py
MQTT_TEST_SANITIZERS=undefined python3 tools/test_fw_ota_policy.py
```

Remaining hardware qualification requires recording:

- Fault injection with delayed/black-holed connect, writes and PUBACK while
  PINGRESP continues; producer stalls; every queue full; reconnect and packet-ID
  epoch renewal; stale/early ACKs; and runtime allocation/start failures.
- UART and PPP responsiveness under blocked SDK calls, including RP2040 relay OTA
  ACKs arriving behind telemetry. Measure the proposed MQTT-path budgets: submit
  within 5 ms, complete local-frame dispatch within 100 ms and PPP-loss observation
  within 1 second. Existing AT operations require separate measurement.
- Both MCU OTA paths, rollback timing and slot protection, authentication/replay
  behavior, retained messages, LWT, backend switching and recovery from overload.
- Heap/minimum heap, task stack high-water marks, CPU load and actual watchdog
  configuration under sustained load. Validate probe/backoff data cost on the SIM.
- Old/new SIM cold and warm starts through the integrated APN selection,
  registration/PPP/MQTT/first-publication times, and prolonged comparison with a
  control device and physical recovery available. The current Bemowo observation
  must distinguish firmware installation time from earlier sessions. Host and
  short bench success are not evidence that the LTE field flaps are fixed.
