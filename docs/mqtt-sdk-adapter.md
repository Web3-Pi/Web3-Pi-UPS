# MQTT stopped-task adapter

The MQTT owner uses `mqtt_sdk_revive_stopped()` to revive a terminated
esp-mqtt task without replacing its client handle. Initial client startup and
retry after an initial task-allocation failure still use the public SDK API.

This adapter is necessary because esp-mqtt 1.0.0 `esp_mqtt_client_start()`
accepts `MQTT_STATE_INIT`, which can describe an existing task, including the
brief interval before fatal-error cleanup completes. Rejected reconnect calls
do not prove that the old task has stopped. Starting another task in that
interval could race with the old task closing the transport and clearing the
outbox.

The adapter holds the SDK's recursive API mutex and requires all three facts:

- The SDK state is `MQTT_STATE_DISCONNECTED`.
- `STOPPED_BIT` is set. In the pinned SDK, the task sets it only after closing
  its transport, deleting its outbox entries and storing `DISCONNECTED`.
- The SDK's `run` flag is false.

After setting `STOPPED_BIT`, the old task only calls `vTaskDelete(NULL)` and
does not access the client again. The adapter clears the bit before launching
the replacement task, so another revive cannot reuse old stopped evidence
while the new task is waiting to be scheduled. If the pinned SDK's task-create
call fails, the adapter restores the stopped bit for a later retry.

The optional `before_restart` hook runs after stopped evidence is established
and before task creation, under the SDK mutex. It lets the owner invalidate
its old connection/probe state before a new CONNECTED callback can arrive.
The hook may take the short application lock; it must not perform SDK calls,
waits or logging. The hook also runs on a failed task-create attempt: the old
socket is nevertheless definitively closed.

The adapter never stops a live task, forces a modem reset, destroys a client
or purges an outbox. Outbox cleanup belongs to the SDK task that has already
terminated. Ordinary reconnects preserve outstanding entries and packet-ID
allocation history.

## Dependency and build checks

Only `main/mqtt_sdk_adapter.c` includes the private MQTT header.
`main/CMakeLists.txt` grants that private include and checks exact SHA-256
hashes for the project-local esp-mqtt 1.0.0 component:

| SDK file | SHA-256 |
| --- | --- |
| `mqtt_client.c` | `1a120957d6f8078a0cd27f4febac54389c5dce7f025069cad493e945d005f361` |
| `lib/include/mqtt_client_priv.h` | `ee8f464f6cbf77a83468126bf22d91833b9c2b8985ba5860381acb99a655c565` |

The component is vendored in
[`firmware-ESP32-LTE-M/components/espressif__mqtt`](../firmware-ESP32-LTE-M/components/espressif__mqtt)
and selected by `main/idf_component.yml` through a local override. Its narrow
resend/abort patch exits the CONNECTED switch arm after a failed retransmission,
preventing another send or poll on the closed transport. It retains the outbox
for retry and does not change the private layout or lifecycle excerpts used by
this adapter. The upstream source hashes, license and reversible patch are
documented in
[`WEB3PI_PATCH.md`](../firmware-ESP32-LTE-M/components/espressif__mqtt/WEB3PI_PATCH.md).

Shared managed components and the installed ESP-IDF are unchanged. A dependency
update intentionally fails the build until the resend patch, task lifecycle,
private layout and tests are reviewed and the pins are updated together.
SDK API locks must remain enabled.

## Verification

`python3 tools/test_mqtt_sdk_adapter.py` compiles the production adapter with
the actual SDK start function and fatal-cleanup tail. Small test-only SDK
excerpts, with their Apache license, permit host CI without component download;
their hashes are checked against the mandatory local component's exact source.
The test covers both pinned and unpinned task creation, a live INIT task,
incomplete cleanup, cleanup completing between scheduler steps, immediate task
execution, repeat revive before task execution, and failed allocation/retry.
Rejected revives must not run the hook.

`python3 tools/test_mqtt_abort.py` separately reconstructs and SHA-verifies the
upstream source by reversing the local patch. It exercises the actual resend
functions, complete CONNECTED switch arm and unlock/poll epilogue with controlled
transport boundaries, including MQTT 3.1.1 and MQTT 5 and outbox preservation.

The default runner uses ASan and UBSan. `MQTT_TEST_SANITIZERS=undefined` selects
UBSan; an empty value selects plain compilation. Host tests exercise lifecycle
orderings and do not replace an ESP32 bench test or heap/stack measurement.
