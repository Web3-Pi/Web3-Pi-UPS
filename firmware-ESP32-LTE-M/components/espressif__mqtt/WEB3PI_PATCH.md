# Local esp-mqtt 1.0.0 resend/abort fix

This project-local component contains the complete build inputs of Espressif
`espressif/mqtt` 1.0.0, with one narrow change to `mqtt_client.c`. It is selected by
`main/idf_component.yml` using `override_path: ../components/espressif__mqtt`.
Shared managed components and the installed ESP-IDF are unchanged.

Upstream: <https://github.com/espressif/esp-mqtt>, Apache-2.0; the original
`LICENSE`, README, component manifest, public and private headers are retained.
Registry component hash:
`ffdad5659706b4dc14bc63f8eb73ef765efa015bf7e9adf71c813d52a2dc9342`.
The original SHA256 of every retained file is in `UPSTREAM_SHA256.json`.
Examples, documentation assets and registry-generated checksum files are omitted;
all inputs referenced by the upstream component CMakeLists are included.

Original `mqtt_client.c` SHA256:
`4b24720b34c2bd44b0857a5251f5392663225c618595229540b35f1529663a9a`.
Patched `mqtt_client.c` SHA256:
`1a120957d6f8078a0cd27f4febac54389c5dce7f025069cad493e945d005f361`.
Unchanged `lib/include/mqtt_client_priv.h` SHA256:
`ee8f464f6cbf77a83468126bf22d91833b9c2b8985ba5860381acb99a655c565`.
The project CMake checks these last two files for the stopped-task adapter.

## Change

`0001-stop-after-resend-abort.patch` records the complete source modification:

- A failed QUEUED, TRANSMITTED or ACKNOWLEDGED/PUBREL resend exits the current
  CONNECTED switch arm immediately. The existing task epilogue releases the API
  lock and skips transport polling because the connection has been aborted.
- Failure to construct a retransmitted PUBREL aborts the connection once, just
  like failure to write it. The separate receive-path PUBREL error is unchanged:
  its existing caller already aborts.

This prevents another outbox send, PING, or refresh from using an already closed
transport and prevents duplicate DISCONNECTED callbacks from that fallthrough.
It preserves outbox entries for retry and leaves success-path QoS bookkeeping,
packet IDs, public/private SDK interfaces and task lifecycle unchanged.

The fix does not remove the initial configured 15-second transport write timeout
or solve a radio/network outage. No global transport or invalid-descriptor guard
was added: the demonstrated invalid-descriptor call is prevented at its MQTT
caller. Other unproven callers are outside this patch.

## Verification

From the source root, run `python3 tools/test_mqtt_abort.py` (default host
ASan+UBSan), or select `MQTT_TEST_SANITIZERS=undefined` / an empty value for UBSan /
plain C. The runner reconstructs and SHA-verifies the original source by reversing
the patch. It compiles verbatim SDK functions, the entire CONNECTED switch arm and
the actual unlock/poll epilogue, and links the unchanged SDK `mqtt_outbox.c`.
Transport, clock, RTOS and packet construction are controlled test boundaries.

Both MQTT 3.1.1 and MQTT 5 compilation paths are exercised. Timeout, negative
write return, PUBREL construction failure, QoS 0/1/2, another pending PUBREL,
overdue keepalive/refresh, outbox preservation and successful retry are covered.
The original must fail the regression checks; the patched source must pass.
`tools/test_mqtt_sdk_adapter.py` also verifies that lifecycle excerpts remain
byte-identical to the reviewed upstream fixture.

Local plain C and UBSan results: original 112 expected failed assertions per
protocol build; patched 755/761 checks with zero failures. Firmware build,
Linux ASan execution and hardware qualification are recorded at release level.

When changing this dependency, review this patch and the adapter together. Do not
remove the local override or accept a new source hash solely to make a build pass.
