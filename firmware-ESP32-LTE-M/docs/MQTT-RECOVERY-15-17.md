# MQTT service and modem recovery: issues 15–17

## MQTT/TLS service (#15 and #16)

The application enables the vendored MQTT component's bounded service profile:
built-in `mqtts`, MQTT 3.1.1, certificate-authenticated TLS 1.2. The service owns
the TLS context and retains partial RX and TX across short, yielding turns.
DNS is asynchronous; socket readiness checks do not wait for network progress.
Each partial RX packet, admitted TX packet and connection phase has its original
300-second deadline in 0.8.17 (15 seconds in 0.8.16), which positive progress
cannot renew. The three connection
phases are DNS/TCP/TLS, MQTT CONNECT transmission, and CONNACK reception.

Each turn services up to eight ready RX packets within a 10 ms work budget,
then at most one 1024-byte TLS write, and yields outside the SDK mutex. A slow
peer cannot turn a partially received TLS record into a blocking socket read.
The CPU cost of a cryptographic operation or application callback is not
preempted by this budget. The independent application monitor reports the
maximum observed SDK mutex hold and current service/deadline state.

PINGRESP waiting starts at the actual completion of PINGREQ transmission.
Ready input is serviced before timeout decisions, with a finite 200 ms /
64-packet grace at expiry. Other outgoing packets cannot change the PING
response deadline. An unfinished SSL write is retried with the identical buffer
and length; reads resume after it completes or its original TX deadline ends.
This avoids interleaving mbedTLS record state. No new write starts during PING
receive grace. A blocked post-handshake alert (`SSL_read/WANT_WRITE`) terminates
the connection explicitly; renegotiation is disabled for this TLS context.

QoS state and resend timestamps advance only after actual transmission.
TX owns a bounded copy so incoming acknowledgements cannot free its buffer.
Expiration waits for an admitted outbox frame to complete or reach its finite
TX deadline, so a delayed QoS0 completion cannot delete a different id-zero
entry. Normal service reserves control-queue capacity for the entire PING grace
batch, including a response behind 63 QoS1 packets. Reconnect backoff sleeps at
most 10 ms per turn; stopping an already-disconnected client is responsive too.
The application uses `enqueue`; synchronous `publish` is explicitly unsupported
in this opt-in profile. The legacy component path remains available to other
users. Full limits, API semantics, upstream provenance and the two-patch chain
are documented in `components/espressif__mqtt/WEB3PI_PATCH.md`.

## Modem recovery (#17)

The supervisor rechecks backend health after DNS and final AT diagnostics.
A watchdog reset commits before incrementing trip counters, using the lock
order OTA claim -> MQTT application state -> PPP lifecycle state. The MQTT
guard rejects a recovered connection, publication proof, stalled worker,
authentication refusal, OTA activity, or a changed MQTT generation/ACK since
the diagnostic decision began. PPP must still have the same attempt and
observation sequence. Successful commit marks PPP STOPPING; later GOT_IP
callbacks cannot resurrect a DCE already selected for teardown.

All guards perform bounded state operations. AT, DNS, SDK calls and logging
remain outside their critical sections. A separate OTA recovery claim prevents
a new transfer from starting during the committed modem teardown; the claim
is released after teardown. HTTP/Arkiv health accessors run outside these
guards, preserving their existing mutex requirements.

A valid CEREG unregistered-to-registered transition grants one 90-second
opportunity to restore the data path per unhealthy incident. Registered samples,
invalid replies and repeated flaps do not renew it. The normal unhealthy timer
is not reset by registration, and registration never confirms an OTA image.
With failed Internet probes and no backend recovery, escalation is bounded by
the normal 300-second interval plus at most 90 seconds of registration grace,
plus a supervisor tick and the existing diagnostic operation durations.

PPP exposes a nonzero generation only while up. It changes on every new UP
edge, including recovery within the same dial attempt, but not duplicate
GOT_IP notifications. The MQTT owner expedites obsolete transport retries once
per new generation. Authentication-refusal backoff and ordinary idempotent
start requests keep their existing schedule.

## Regression coverage

- `tools/test_mqtt_keepalive.py`: historical issue counterexamples, actual
  parser/outbox/service integration, PING ordering and send-completion origin,
  finite partial-input/output deadlines, cancellation and outbox lifetimes.
- `tools/test_mqtt_transport_nb.py`: actual transport adapter with controlled
  ESP-TLS/lwIP boundaries, including delayed TCP readiness, DNS cancellation,
  original-hostname SNI, WANT retry identity and blocked-alert termination.
- `tools/test_mqtt_abort.py` and `tools/test_mqtt_sdk_adapter.py`: verified
  upstream patch lineage and unchanged stopped-task lifecycle contract.

- `tools/test_modem_recovery.py`: CEREG parsing, one-shot grace, flapping and
  finite expiration.
- `tools/test_modem_recovery_supervisor.py`: sealed pre-fix baseline and actual
  supervisor/commit functions with controlled DNS, AT and event interleavings.
- `tools/test_modem_ppp_events.py`: real extracted PPP lifecycle, including new
  generation, duplicate GOT_IP and loss/recovery ordering.
- `tools/test_mqtt_runtime.py`: complete application owner with controlled SDK
  boundaries, transport/auth retry schedules and reset arbitration.
- `tools/test_fw_ota_policy.py`: competing OTA/recovery claims and the existing
  image confirmation/rollback gates.
- `tools/test_modem_uart_boot_policy.py`: the actual boot-time UART selector
  and OTA confirmation/rollback functions, both configured baud rates, state
  query failures and a selection retained through validation and retries.

Host fault injection verifies control flow and timing policy; it does not
emulate radio, TLS cryptography or ESP32 scheduling. Hardware results must name
the image, profile and exercised scenarios separately.

## Hardware validation, 2026-09-16

The USB-connected ESP32-S3 was tested first with `0.8.16-r17-1nce`, preserving
its existing CPU 240 MHz, UART 230400, core-1 modem profile and fixed 1NCE APN.
The test image completed a concurrent 512 KiB download and upload (HTTP 200),
then two owner-requested PPP teardown/reconnect cycles. Both observed PPP down,
a new PPP generation, MQTT reconnection and fresh publication proof. The time
from request to fresh proof was 44.2 s and 39.5 s, including the supervisor's
request pickup interval. This validates real reconnect plumbing; the precise
DNS/AT/commit races and registration-grace boundaries are host-injected tests.

The standalone `tools/mqtt_target_fixture` then passed all eight scenarios on
the same ESP32-S3 with actual TLS 1.2 encryption/decryption and lwIP sockets:
coalesced PUBLISH/PINGRESP plus PUBACK, fragmented encrypted records, slow
positive ciphertext progress, prefix followed by silence, missing/delayed
PINGRESP, TX backpressure and cancellation during TX backpressure. Certificate
verification was enabled using a disposable fixture certificate. The test's
operation deadline was 3000 ms; incomplete RX and blocked TX reached finite
deadlines while retaining partial state. Both positive-data cases verified
payload integrity and the broker's receipt of PUBACK.

Across these eight cases, stop took 5–12 ms, the independent status read took
at most 41 microseconds, and the maximum SDK lock hold was 601 ms including
handshake cryptography. Heap integrity passed after every case; minimum free
heap was 188352 bytes during the deliberately large queued TX workload. The
fixed network/work budget is not a universal 10 ms or 1 s CPU execution bound:
certificate processing and application callbacks are not preempted by it.
Public enqueue/subscribe still use the SDK mutex; the application producer
queue and independent monitor remain isolated from that mutex.

The target fixture exposed an additional five-second reconnect-state stop
delay, which was fixed and regression-tested before the passing run. An initial
fixture counter mistakenly included CONNACK ciphertext in a later injected
record; the verified run resets that counter at injection start. Earlier failed
runs are retained as evidence and are not included in the passing result.

Final host validation covered 30 MQTT/modem/OTA Python suites under Linux ASan
and UBSan, including historical failures, 11 bounded-service scenario groups,
552 transport-adapter checks and the stopped-task lifecycle. Both core-affinity
profiles also passed their separate native UBSan regression (268 checks each).
These are local validation results; no GitHub CI run or fleet rollout is implied.

With the combined firmware `0.8.16-r1517-1nce`, the real LTE broker connected
and publication proof remained fresh during concurrent HTTPS traffic. MQTT
reported no worker stalls, rejected admissions or deadline failures; the
maximum measured SDK lock hold was 563 ms. The upload completed 524288 bytes
with HTTP 200 in 94.8 s. The download received 206848 bytes with HTTP 200 before
its 120 s benchmark deadline; it did not complete and is not reported as a
throughput-test pass. This single observation does not establish its cause.

The original benchmark intentionally skips recovery cycles after an incomplete
transfer. `CONFIG_WUPS_PERF_RECOVERY_ONLY` provides a separate test image that
runs the same two owner-controlled cycles after a healthy-uplink baseline,
without making recovery qualification depend on HTTPS throughput. It defaults
off and depends on both existing research benchmark options. Normal firmware
keeps all three benchmark options disabled.

The combined recovery-only image `0.8.16-recovery-1nce` completed both cycles:
42.5 s and 38.9 s from request to fresh publication proof, including supervisor
pickup. Each observed PPP down, a new generation (2 then 3), MQTT reconnection
and fresh proof. Together with the first #17-only image this gives four
successful hardware reconnect cycles. The final normal image is
`0.8.16-1nce-240-c1`; the existing APN, UART and CPU/core profile is preserved.
USB tests wrote only OTA metadata and the OTA0 application slot. The original
OTA1 image, provisioning, NVS, bootloader and partition table were preserved.

## Integration into main, 2026-09-16

The integration adds a boot-time UART migration guard after the hardware
qualification above. The modem's persistent `AT+IPR` setting stays at 115200
during an unconfirmed OTA boot, allowing rollback to the earlier 115200-only
`main` firmware. Only a running partition already in OTA state `VALID` at
modem initialization may use the configured 230400 rate. Every other state
or failed query selects 115200 for the entire boot; confirmation does not
promote the rate until a later boot. This also leaves serial-flashed images
without a `VALID` OTA state at 115200.

The guard protects the first migration from a modem initially at 115200. It
does not undo an earlier persistent rate change before the application runs,
and a later manual downgrade to legacy firmware still requires restoring
115200 first, as described in the firmware README.

All 31 MQTT/modem/OTA host suites passed under Linux ASan and UBSan on the
integration tree. The new boot-policy test exercised 178 checks for each
configured baud, including the unchanged 600-second rollback deadline; the
bring-up harness also checked the selected rate at both the AT preparation
and DTE boundaries. A fresh default-APN ESP32 build passed, and both modem
core-affinity profiles passed 268 checks each under native UBSan. These are
host/build checks. The additional guard has not been
flashed or subjected to physical power-failure testing; its first-migration
rollback ordering was independently reviewed against the pinned IDF v6.0.2
bootloader and OTA implementation.

Clean-build defaults retain the research CPU 240 MHz / UART 230400 settings,
with modem CPU1 affinity, TX0 and diagnostics optional. The earlier hardware
qualification used CPU1 and TX0 explicitly. Synthetic traffic and reconnect
benchmarks remain disabled by default.
