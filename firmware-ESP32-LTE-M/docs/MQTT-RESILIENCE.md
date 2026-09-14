# MQTT resilience and verification

This implementation addresses issues [#10](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/10),
[#11](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/11),
[#12](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/12) and
[#13](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/13). Host checks do not establish
the cause of field LTE disconnects. Hardware fault injection and deployment have
not been performed as part of this work.

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
exit path already purges its outbox; queued application messages remain available.
These decisions use actual SDK results, not request admission.

## Publication proof and recovery

An existing, recent `net.status` frame on the normal telemetry topic supplies a
selected QoS 1 probe; no new heartbeat topic or payload format is introduced.
There is one logical probe in flight. The first is due after CONNECT or OTA resume;
subsequent probes are scheduled no more often than every 120 seconds. A 60 second
deadline starts at the planned due time, including scheduler/admission delay.
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

## Validation and remaining hardware gates

Portable queue, health and packet-guard tests exercise production modules. The OTA
policy harness extracts the production confirmation/rollback functions and stubs
flash and scheduling calls; it checks deadline ordering, failures, active/short
transfers, explicit recovery reasons and confirmation/rollback interleavings.
These tests do not emulate the whole ESP-IDF runtime.

Host regressions also cover power decoding (#7), Arkiv balance (#8), Arkiv command
sweep/ACK bookkeeping (#9), the HTTP command model and the deframer model. The
Python models are supporting regressions, not executions of all firmware paths.
Native macOS plain C and UBSan runs passed. The native ASan runtime hangs even
for a trivial smoke test, so the full MQTT/OTA suite was additionally run in an
isolated Linux container with GCC 14.2, ASan and UBSan: it passed. The complete
runtime harness held its SDK boundary for 15 real seconds while 5,089 host
responsiveness checks passed; its UART/SDK boundaries remain stubs, not hardware.
The original #12 harness also reproduced the old UAF under Linux ASan; the new
initialization/runtime tests passed there. Existing #7–9, HTTP and deframer
regressions passed on both hosts. GCC host coverage required explicit uint64_t
formatting in the OLED helper and a test-fixture indentation correction; these
do not change the ESP32 feature behavior.

The new runners default to ASan+UBSan, fail explicitly and impose a timeout; no
sanitizer failure silently falls back to a weaker mode. Run from the repository
root, for example:

```sh
MQTT_TEST_SANITIZERS=undefined python3 tools/test_mqtt_health.py
MQTT_TEST_SANITIZERS=undefined python3 tools/test_fw_ota_policy.py
```

Before a release or field pilot, complete and record:

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
- Old/new SIM cold and warm starts, registration/PPP/MQTT/first-publication times,
  followed by a separately authorized pilot with physical recovery and a control
  device. Host success is not evidence that the LTE field flaps are fixed.

APN selection is outside this patch. The reported old-SIM registration improvement
from `apn9920.1` should be preserved and measured in a separately prepared variant
with consistent DCE/CGDCONT APN. Do not apply its fixed `iot.1nce.net` globally to
the base fleet or lose support for `sensor.net`.
