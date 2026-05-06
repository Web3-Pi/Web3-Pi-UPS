# Web3 Pi UPS — wire protocol v1

This document describes the binary protocol used between the four firmware
images of the UPS:

- **RPi 5** host service (`Web3-Pi-UPS-Service`, Rust)
- **RP2040** UI / hub firmware (`firmware-rp2040`)
- **CH32X035** power-path firmware (`firmware-ch32x`)
- **ESP32-S3** LTE-M / Arkiv firmware (`firmware-ESP32-LTE-M`)

The canonical machine-readable spec is [`protocol.h`](protocol.h). This file
is the human companion: structure, rationale, routing rules, examples.

## Topology

```
   +-------+   USB-CDC    +---------+   UART0    +--------+
   | RPi 5 | <----------> | RP2040  | <--------> | CH32X  |
   +-------+              |  (hub)  |            +--------+
                          |         |   UART1
                          |         | <-------> +---------+
                          +---------+           | ESP32-S3 |
                                                +---------+
```

RP2040 is the only node connected to all others. It acts as a router for
frames whose `DST` is not itself or a broadcast address.

## Frame format

```
+-----+-----+-----+-----+-------+----+-------+-----+-------+-------+
|0xAA |0x55 | DST | SRC | CLASS | OP | FLAGS | SEQ | LEN_L | LEN_H |
| 1B  | 1B  | 1B  | 1B  |  1B   | 1B |  1B   | 1B  |  1B   |  1B   |
+-----+-----+-----+-----+-------+----+-------+-----+-------+-------+
| payload  ...  LEN bytes (0..240)                                |
+-----------------------------------------------------------------+
| CK_A  | CK_B  | 0x55  | 0xAA  |
|  1B   |  1B   |  1B   |  1B   |
+-------+-------+-------+-------+
```

- **SYNC**: `0xAA 0x55`. Receiver scans for these to start frame parsing.
- **DST / SRC**: see address table below.
- **CLASS / OP**: dispatch keys (e.g. `power.status` = `0x02 0x01`).
- **FLAGS** bitmap:
  - bit 0 `REQ` — request expecting a RESP.
  - bit 1 `RESP` — response. `SEQ` echoes the originating REQ's `SEQ`.
  - bit 2 `EVENT` — unsolicited (periodic status or change event).
  - bit 7 `NEED_ACK` — reserved, no implementation in v1.
- **SEQ**: u8 incrementing counter, scoped per (`SRC`, `DST`). Wraps at 256.
  RESP must echo the REQ's SEQ for correlation.
- **LEN**: u16 little-endian payload length. `0 ≤ LEN ≤ 240`.
- **PAYLOAD**: `LEN` bytes. First byte is conventionally a `version` field
  (matches the v1 suffix on every struct in `protocol.h`).
- **CK_A / CK_B**: Fletcher-8 over `DST..LEN_H..payload[LEN-1]`. Same algo
  as u-blox UBX. SYNC and end marker are NOT covered.
- **End marker**: `0x55 0xAA`. Aids resynchronization after corruption.

Total frame size = `14 + LEN` bytes. Max frame = 254 bytes.

## Endianness and alignment

- All multi-byte fields are **little-endian**.
- All four targets are little-endian, so direct struct overlay is byte-correct.
- However, payload buffers are not guaranteed to be aligned, and CH32X
  (Cortex-M0) and RP2040 (Cortex-M0+) **fault on unaligned word access**.
  Always copy structs in/out of the payload buffer with `memcpy`. All structs
  in `protocol.h` are `__attribute__((packed))` as belt-and-suspenders.

## Addresses

| Value | Name      | Meaning                                                  |
|-------|-----------|----------------------------------------------------------|
| 0x00  | NULL      | Unused. Frames with DST=NULL must be dropped.            |
| 0x01  | RPI       | Raspberry Pi 5 (host service over USB-CDC).              |
| 0x02  | RP2040    | UI / hub controller.                                     |
| 0x03  | CH32X     | Power path / PD output controller.                       |
| 0x04  | ESP32     | M.2 LTE-M card (or current devkit T-SIM7080G-S3).        |
| 0x05  | INTERNAL  | Multicast to all MCUs **but not** RPi.                   |
| 0xFF  | BROADCAST | Multicast to all nodes including RPi.                    |

### Routing rules (RP2040 hub)

For an incoming frame on port `P`:

1. If `DST == self` (RP2040): consume.
2. If `DST == 0xFF` (BROADCAST): consume **and** retransmit on every other
   port (USB-CDC, UART0, UART1) except `P`.
3. If `DST == 0x05` (INTERNAL): consume **and** retransmit on every MCU
   port except `P`. Never forward to USB-CDC.
4. Otherwise: forward on the single port mapped to `DST` (table below).
   If port unknown, drop and emit `system.log` with level=warn.

| Address | Port               |
|---------|--------------------|
| RPI     | USB-CDC (Serial)   |
| CH32X   | UART0 (GPIO16/17)  |
| ESP32   | UART1 (GPIO4/5)    |

Loop prevention: a hub never retransmits a broadcast/internal back on the
port it arrived from. With a star topology this rule is sufficient (no
cycles possible).

## Versioning

- `WUPS_PROTO_VERSION = 1` is the global protocol version. Every node sends
  its `proto_version` in `system.hello` on boot. A mismatch is logged and the
  remote is treated as offline.
- Each payload struct has a `version u8` first byte. New fields are appended
  to a new versioned struct (`_v2_t`). Receivers may accept v1 with reduced
  data while a rolling upgrade is in progress.
- Capabilities are advertised in `system.hello.caps_classes` (u16 bitmap).
  Bit `N` set means the sender implements class `N`.

## Class / Op catalogue

### Class 0x01 SYSTEM (every node)

| Op   | Name           | Direction          | Payload struct                |
|------|----------------|--------------------|--------------------------------|
| 0x01 | ping           | REQ ↔, RESP ←      | empty / `wups_sys_pong_v1_t`  |
| 0x02 | hello          | EVENT (broadcast)  | `wups_sys_hello_v1_t`         |
| 0x03 | status_query   | REQ                | empty                          |
| 0x04 | log            | EVENT              | `wups_sys_log_v1_hdr_t` + ASCII text |

### Class 0x02 POWER (CH32X)

| Op   | Name           | Direction              | Payload struct              |
|------|----------------|------------------------|------------------------------|
| 0x01 | status         | EVENT (CH32X→RP2040 @1Hz), RESP on query | `wups_power_status_v1_t` |
| 0x02 | enable         | REQ                    | empty                        |
| 0x03 | disable        | REQ                    | empty                        |
| 0x04 | cycle          | REQ                    | `wups_power_cycle_v1_t`     |
| 0x05 | reset          | REQ                    | empty                        |
| 0x10 | event          | EVENT (broadcast)      | `wups_power_event_v1_t`     |

CH32X emits `power.status` autonomously every 1 s, **unicast to RP2040**.
RP2040 caches the latest sample and includes the data in its own
`host.aggregate` frames sent to RPi.

### Class 0x03 NET (ESP32)

| Op   | Name           | Direction                       | Payload struct               |
|------|----------------|----------------------------------|-------------------------------|
| 0x01 | status         | EVENT (ESP32→RP2040), RESP       | `wups_net_status_v1_t`       |
| 0x02 | publish        | REQ → ESP32                      | `wups_net_publish_v1_hdr_t`  |
| 0x10 | downlink       | EVENT → destination              | `wups_net_downlink_v1_hdr_t` |
| 0x20 | time_sync      | EVENT (INTERNAL broadcast)       | `wups_net_time_sync_v1_t`    |

ESP32 is a dumb pipe: it never inspects payloads of `net.publish` (the
caller composes whatever bytes go to MQTT). It forwards `net.downlink` to
whichever address the cloud directed (typically `RPI`).

### Class 0x04 HOST (RPi)

| Op   | Name             | Direction         | Payload struct                          |
|------|------------------|-------------------|------------------------------------------|
| 0x01 | status           | EVENT (RPi→RP2040 @5s), RESP | `wups_host_status_v1_t`     |
| 0x02 | shutdown         | REQ               | `wups_host_shutdown_v1_t`                |
| 0x03 | reset            | REQ               | `wups_host_shutdown_v1_t`                |
| 0x04 | service_restart  | REQ               | `wups_host_service_restart_v1_hdr_t` + unit name |
| 0x10 | event            | EVENT (broadcast) | `wups_host_event_v1_t`                   |

`host.service_restart` carries a target systemd unit name (without `.service`
suffix). The agent on the RPi enforces a whitelist configured locally; units
not on the whitelist are rejected. A master `allow_service_restart` switch in
the agent's config disables the op entirely.

### Class 0x05 UI (RP2040)

| Op   | Name           | Direction              | Payload struct                  |
|------|----------------|------------------------|----------------------------------|
| 0x01 | button_event   | EVENT (broadcast)      | `wups_ui_button_event_v1_t`     |
| 0x02 | set_screen     | REQ                    | `wups_ui_set_screen_v1_t`       |
| 0x03 | beep           | REQ                    | `wups_ui_beep_v1_t`             |
| 0x04 | display_msg    | REQ                    | `wups_ui_display_msg_v1_hdr_t` + text |

## Example: RPi pings CH32X

RPi composes:

```
SYNC=AA 55  DST=03  SRC=01  CLASS=01  OP=01  FLAGS=01  SEQ=42
LEN=00 00   payload=(empty)   CK_A=??  CK_B=??   END=55 AA
```

Flow:
1. RPi writes 14 bytes to USB-CDC.
2. RP2040 deframes; `DST=0x03` → forwards on UART0.
3. CH32X deframes, dispatches `system.ping` REQ.
4. CH32X composes RESP with `FLAGS=0x02`, `SEQ=42`, payload =
   `wups_sys_pong_v1_t{version:1, fw_version:0x0103, uptime_ms:12345}`.
5. RP2040 sees `DST=0x01` (RPI) → forwards on USB-CDC.
6. RPi correlates by `SEQ=42`.

## Example: CH32X periodic status

Every 1 s CH32X emits unicast to RP2040:

```
DST=02  SRC=03  CLASS=02  OP=01  FLAGS=04  SEQ=auto
LEN=18 00  payload=wups_power_status_v1_t (24 B)
```

RP2040 caches it. RP2040's own `host.status`-equivalent frame to RPi
includes the latest power snapshot via the aggregator (see RP2040 firmware).

## Example: power loss broadcast

CH32X detects mains drop:

```
DST=FF  SRC=03  CLASS=02  OP=10  FLAGS=04  SEQ=auto
LEN=02 00  payload={version:1, event:1 (MAINS_LOST)}
```

RP2040 retransmits to USB-CDC and UART1. Every node reacts:
- RP2040: switches OLED to "ON BATTERY", short beep.
- RPi: starts shutdown timer.
- ESP32: composes a `net.publish` next cycle with low-battery topic.

## Receive state machine (sketch)

```
state = SYNC1
loop:
  byte = uart_read()
  switch state:
    SYNC1: state = (byte == 0xAA) ? SYNC2 : SYNC1
    SYNC2: state = (byte == 0x55) ? HEADER_DST : SYNC1
    HEADER_DST..HEADER_LEN_H: accumulate; after LEN_H known, state = PAYLOAD
    PAYLOAD: accumulate LEN bytes
    CK_A: store; state = CK_B
    CK_B: validate Fletcher-8 over header+payload; on mismatch -> SYNC1
    END1: state = (byte == 0x55) ? END2 : SYNC1
    END2: if byte == 0xAA -> deliver frame; else discard. state = SYNC1
```

If the byte stream loses sync (e.g. UART noise), the receiver returns to
`SYNC1` and resumes. The end marker plus checksum makes a false-positive
deframe extremely unlikely.

## Limits & timeouts

- **Max payload**: 240 B (`WUPS_MAX_PAYLOAD`). Sized to fit in CH32X's
  256 B circular DMA RX buffer with header overhead.
- **Frame timeout**: receiver should reset to `SYNC1` if no byte arrives
  within 100 ms after starting a frame.
- **REQ timeout**: caller waits 1 s for RESP, then retries up to 3× before
  declaring the destination dead.
- **Retransmission**: not implemented in v1. Every link is reliable enough
  (USB-CDC, UART direct). If we observe loss in production, we add it via
  `NEED_ACK` flag.

## Future v2 considerations (not implemented)

- `NEED_ACK` flag with separate `system.ack` op.
- Encryption between RPi and ESP32 only (cloud-bound payloads).
- OTA fragmentation (class 0x06 `update`).
- Capability negotiation (REQ/RESP) replacing the hello-only advertisement.
