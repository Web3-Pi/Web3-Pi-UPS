# Web3-Pi esp-mqtt 1.0.0 patches

This project-local component is selected with `override_path` in the main
component. The installed ESP-IDF and shared managed components are unchanged.
Espressif esp-mqtt is Apache-2.0 licensed; upstream inputs retain their original
license. `UPSTREAM_SHA256.json` pins every original retained build input.
`PATCHED_SHA256.json` pins every changed or added build input. The complete patch
chain reverses exactly to upstream, including the original private/public API.

1. `0001-stop-after-resend-abort.patch`: stop the connected iteration after a
   failed resend, preserving the outbox and emitting a single disconnect.
2. `0002-bounded-mqtt-service.patch`: opt-in cooperative service for issues
   #15 and #16. Legacy mode retains synchronous public publish semantics and its
   original connected branch. Both modes now check failed QoS2 response writes.

## Supported bounded profile

Set `network.bounded_service = true` for the built-in `mqtts` URI with MQTT 3.1.1
and certificate-based TLS 1.2. Custom transports, MQTT 5, secure-element/DS/ECDSA
peripheral and PSK configurations are rejected rather than silently degraded.
Certificate validation, common name, SNI, CA bundle/store, ALPN, interface,
software client certificate/key, cipher list and TCP keepalive remain configured.
The configured broker name remains the TLS verification and SNI name even though
asynchronous DNS gives the TCP connect path a numeric address.

Only the SDK service task enters the bounded TLS context. Public `enqueue()`,
subscribe and unsubscribe admit owned outbox packets; TRANSMITTED and its
retransmission timestamp advance only after the full packet is actually accepted
by TLS. QoS0 stored outbox entries are deleted only then. While an outbox frame is
staged, expiration waits until completion/abort (at most its original operation
deadline), so shared QoS0 packet ID zero cannot resolve to the next unsent item. Synchronous `publish()`
is rejected with `-1` / `errno=ENOTSUP` before admission in this opt-in mode; use
`enqueue()`. Changing config/URI while the bounded client runs is rejected; stop
before reconfiguration so TLS/DNS cannot retain freed configuration pointers.
Stop/disconnect promptly abort rather than promising graceful MQTT
DISCONNECT delivery; the broker may publish the LWT. The original stopped-task
lifecycle contract is preserved, with bounded transport/queue cleanup before its
unchanged close/outbox-delete/DISCONNECTED/STOPPED/task-delete suffix.

## Budgets and protocol state

- Each service turn attempts at most eight complete RX packets within a 10 ms
  CPU budget, then at most one 1024-byte TLS write. A 10 ms RTOS yield occurs
  outside the API mutex, even when the socket remains permanently ready. These
  are network-wait and work-quantity limits, not a hard CPU preemption guarantee:
  certificate crypto and application callbacks may exceed 10 ms. Callbacks must
  remain nonblocking. The measured maximum SDK lock hold is independently
  observable and must be checked on the target.
  The unlocked WAIT_RECONNECT event wait is also capped at 10 ms (at least one
  RTOS tick), because stop changes the run flag without setting RECONNECT_BIT.
  Cancellation therefore does not sleep through the reconnect backoff.
- `network.timeout_ms` is a monotonic, non-renewable deadline per RX packet
  (starting with first ciphertext readiness), per queued TX packet (including
  queue residence), and per connection phase: DNS+TCP+TLS, CONNECT send, CONNACK.
  At 15000 ms, connection establishment has at most three such phase budgets,
  plus CPU scheduling overhead. Progress never resets an operation deadline.
- Partial TLS and MQTT input persists between slices. A complete incoming packet
  is assembled up to a hard 16 KiB limit before callback delivery, avoiding lost
  streaming continuation. Invalid Remaining Length encodings and oversize input
  fail the connection before allocation/delivery. TX copies are capped at 32 KiB
  and 68 frames; 512 bytes are reserved against bulk admission for ACK/PING
  control frames. Normal RX/bulk admission stops at two staged frames; the
  remaining frame slots are reserved for the 64-packet PING grace, so full
  normal control traffic cannot prevent reading a buffered PINGRESP.
  Outbox storage is independent and keeps its existing limit.
- A partial SSL write keeps the same buffer and length for every WANT_READ or
  WANT_WRITE retry. No SSL read runs while that write is suspended. The original
  TX operation deadline bounds this local backpressure interval; producers and
  cancellation continue to run. Renegotiation is disabled on this connection's
  own TLS configuration. An SSL read returning WANT_WRITE means a blocked
  post-handshake alert in this supported profile: the connection fails closed,
  and no application write is attempted afterward. This explicit recovery avoids
  interleaving/duplicating records; it is not a general TLS transport replacement.
- PING response time begins only when the complete PINGREQ finishes sending and
  lasts keepalive/2. Before timeout, the service drains already available input.
  At expiration it gets one bounded 200 ms / 64-packet receive grace. Unrelated
  traffic never renews that grace. A previously suspended TLS write must first
  complete or reach its original TX deadline (at most the network timeout); this
  is local TX backpressure, not a claim that a buffered PINGRESP never arrived.
  No new TLS write starts during grace. Worst-case peer failure detection is
  response deadline + remaining in-flight TX budget + 200 ms + scheduling/CPU
  overhead; TX deadline expiry is a separate transport recovery path.

## ESP-IDF v6.0.2 boundaries

`mqtt_transport_nb.c` uses public ESP-TLS and mbedTLS APIs. DNS runs through a
nonblocking lwIP callback whose job is reference-counted independently of the
client, including cancellation before DNS completes. ESP-TLS async TCP connect
still calls select; its per-call timeout is therefore a positive 1 ms (zero
means infinite). Its CONNECTING fd_sets are modified by select and not rearmed
in this IDF version. The scoped adapter independently polls that socket, checks
SO_ERROR, and invokes the already-ready CONNECTING step with non_block temporarily
false to skip that select. Socket O_NONBLOCK remains set: IDF only changes socket
flags from INIT. The adapter restores the configuration immediately afterward.

`esp_mqtt_client_get_service_status()` reads atomic observations without acquiring
the SDK mutex: slice start/end, maximum SDK lock hold, last complete RX/partial TX
progress, RX/TX remaining budget, queue occupancy, deadline failures and current
operation. Fields are individual observations, not a transactionally coherent
snapshot; timestamps are monotonic milliseconds modulo 2^32. This permits an
independent monitor to observe a stalled operation without joining its lock.

## Verification

`tools/mqtt_sdk_sources.py` verifies both manifests and reverses 0002 then 0001,
checking the intermediate historical client and every upstream input exactly.
`test_mqtt_abort.py` retains mandatory original failures and checks the current
legacy branch. `test_mqtt_sdk_adapter.py` verifies the original stopped-task
lifecycle against its fixture. `test_mqtt_keepalive.py` runs the issue's mandatory
historical counterexamples and current bounded loop with actual MQTT parser,
encoder, outbox and service queue. `test_mqtt_transport_nb.py` compiles the actual
adapter against controlled public-API boundaries, including delayed TCP connect,
DNS cancellation, SNI, WANT retry identity and the blocked-alert terminal path.
Host tests are deterministic boundary checks, not a claim of physical TLS/LTE
qualification. ESP32 builds and hardware/fault-injection results are recorded in
the release report. Never change source pins simply to make a build pass.
