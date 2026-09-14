# HTTP control mode — specification & self-hosting guide (DOC-HTTP)

Deliverable **DOC-HTTP** (plan §4.17). Companion to the HTTP-1 design note
([`../../web3pi_scope/milestones/M1/evidence/HTTP-1-design-note.md`](../../web3pi_scope/milestones/M1/evidence/HTTP-1-design-note.md))
and the working example HTTP-2. This is the normative description of what the
firmware sends and expects; the reference server lives in
[`../examples/http-control-server/`](../examples/http-control-server/).

> **What this is for.** A worked example for users who want to drive the UPS
> from their **own** infrastructure — no EMQX, no Arkiv. It is one of the
> three runtime-selectable backend modes (ADR-0012): MQTT, Arkiv, **HTTP**.

## 1. Model

The UPS (ESP32-S3) is a pure HTTP **client**. It makes outbound requests only,
so it works behind the 1nce LTE-M carrier-grade NAT exactly like the MQTT and
Arkiv backends — no inbound reachability, no public IP, no VPN needed on the
device side.

```
[ UPS / ESP32-S3 ]  --outbound HTTP(S)-->  [ your server (public IP) ]
   telemetry  : up   in the POST body
   commands   : down in the POST response
   acks       : up   on the next POST
```

Commands are delivered by **polling**: they ride in the HTTP response to the
device's periodic telemetry POST (~30 s cadence). There is no server push and
no long-poll — this keeps the radio idle between posts. Command latency is
therefore up to one cadence interval (~30 s); this is a control channel for
shutdown/reset/beep-class actions, not a real-time link.

The bundled SIM is a **1NCE prepaid pool: 500 MB per SIM or 10 years from
activation, whichever comes first** (top-up is possible via the 1NCE portal,
i.e. by Web3 Pi — not by the end user). That is a **lifetime allowance, not a
monthly quota**, and every poll cycle spends part of it — see §9 for the
measured cost per cycle and how to budget it.

## 2. Endpoint

```
POST  {base}/api/v1/devices/{device_id}/telemetry
```

- `{base}` is the operator-supplied URL (see §6 for how it is configured).
  The device appends the rest of the path.
- `{device_id}` defaults to the SIM **ICCID**; it can be overridden (§6).
- A single endpoint carries both directions: telemetry up in the request,
  commands down in the response, acks up on the following request.

## 3. Authentication

**Both directions are HMAC-SHA256 signed** with the per-device secret, so
neither telemetry nor commands can be forged or injected — independently of the
transport. There is no bearer token; the secret never crosses the wire. **TLS
is therefore optional** (it adds confidentiality only — see §8).

> This supersedes the HTTP-1 design note's "TLS mandatory": the command channel
> is secured by the **response signature** below, not by TLS.

### Request signature (device → server)

| Header | Value |
|---|---|
| `X-W3PUPS-Device` | the `device_id` |
| `X-W3PUPS-Ts` | request time, unix seconds (ASCII decimal) |
| `X-W3PUPS-Nonce` | 8 random bytes, lower-case hex (16 chars) |
| `X-W3PUPS-Sig` | `hex( HMAC_SHA256( secret, Ts ‖ Nonce ‖ raw_body ) )` |

- **Signed input** is the byte concatenation of the ASCII `X-W3PUPS-Ts`
  string, the ASCII `X-W3PUPS-Nonce` string, and the **raw request body bytes**
  — in that order. Signature is lower-case hex.
- The server should reject a stale `Ts` (e.g. ±300 s skew) and a
  recently-seen `Nonce` (replay protection).

### Response signature (server → device)

The server signs its response so the device can trust the commands it returns:

| Response header | Value |
|---|---|
| `X-W3PUPS-Sig` | `hex( HMAC_SHA256( secret, request_Nonce ‖ raw_response_body ) )` |

- Binding to the **request nonce** ties the response to that exact request, so
  a captured-and-replayed old response is rejected.
- The device **verifies this before executing any command**. A response that
  carries commands but is missing/invalid-signed is ignored (fail-closed); a
  response with no commands needs no signature.
- This means a network attacker on a plain-HTTP path **cannot inject
  commands** — they don't have the secret.

### Key — the "HTTP key" shown on the OLED

The key for both signatures is a **dedicated HTTP-mode secret**, separate from
the MQTT/Arkiv per-device secret. The device **generates it itself** on first
use and shows it on the OLED (menu → **HTTP Key**); you can re-roll it from the
same menu (**New key**). So a self-hoster never has to extract a factory secret
— they just read the code off the screen.

- Format: 16 characters of Crockford base32 (≈80 bits), shown as two groups of
  8, e.g. `ABCD EFGH JKLM NPQR`.
- The HMAC key is the **ASCII bytes of the code**, normalised to upper-case with
  any spaces/dashes removed. Type it into the server exactly as shown; case and
  separators don't matter (the reference server normalises it the same way).
- Stored in the device's writable `nvs` (`w3http/secret`); survives reboots and
  mode switches; cleared by a factory reset (a new one is generated next time).
- Re-rolling it on the OLED invalidates the old code immediately — update your
  server's `--secret` to match.

Server responses (what the device does with each status):

| Status | Device behaviour |
|---|---|
| `2xx` | accepted: the carried `acks` / `rejected` / `resp_dropped` are dropped from the device, then the returned commands are applied (subject to the receiver limits in §5). Counts as "uplink alive" for the modem watchdog even when the body is over-size and dropped (§4). |
| `3xx` | **not followed** (`disable_auto_redirect`, since `esp32:0.8.10`): nothing applied, nothing dropped. Device log: `redirect N not followed — point the device at the final URL`. Configure the final URL (§6). |
| `401` | signature/identity rejected; nothing dropped, retried next cadence. The reference server sends no `WWW-Authenticate`, so `esp_http_client` fails before the body is read and the device log shows `POST failed: ESP_ERR_NOT_SUPPORTED (HTTP 401, url=…)` followed by `401 — signature/identity rejected`. The *reason* (bad signature / bad or stale timestamp / replayed nonce) is printed only on the **server** console — the device never sees the 401 body. |
| other `4xx`/`5xx` | nothing dropped, retried next cadence. Device log: `server answered N — nothing dropped, retried next cadence`. No backoff escalation. |
| timeout / no response | per-socket timeout 20 s (`HTTP_TIMEOUT_MS`); device log `POST failed: <esp_err> (no response, url=…)`; nothing dropped, retried next cadence. |

Carried `acks`, `rejected` and `resp_dropped` are removed from the device only
after a `2xx`; a failed POST keeps them for the next poll (bench-verified:
server port blocked for 58 s → `POST failed: ESP_ERR_HTTP_CONNECT (no
response)` → the next POST carried the same 8 acks).

**Uplink watchdog (modem supervisor, `esp32:0.8.7`+).** "Uplink alive" above
refers to the modem supervisor in `firmware-ESP32-LTE-M/main/modem.c`: in HTTP
mode the uplink counts as healthy while the last `2xx` is ≤ 90 s old
(`HTTP_UPLINK_FRESH_SECS`); only a `2xx` refreshes it — a `3xx`, `401`, other
non-2xx or a timeout does not. Once it has been unhealthy for 300 s
(`UPLINK_DEAD_SECS`, i.e. ≈ 6.5 min after the last `2xx`, quantised to the
30 s supervise tick) the firmware first probes the internet (DNS to 1.1.1.1 /
8.8.8.8). If the probes answer, the server is the problem (down, wrong URL,
wrong key) and the firmware **holds** — no modem reset; after 1800 s in that
state (`UPLINK_HOLD_ALERT_S`, ≈ 31 min after the last `2xx`) it raises the
OLED alert `! MODEM / NO UPLINK / no uplink` with the buzzer. Only when the
probes fail too is the cellular link torn down and re-established (escalating
to a modem power-cycle on repeated trips). Both the hold and the alert clear on
the next `2xx`.

## 4. Request body (device → server)

`Content-Type: application/json`. Sub-objects appear only when that telemetry
class has a **fresh** snapshot (observed within ~90 s); otherwise they are
omitted rather than sent stale.

Body as sent by `esp32:0.8.9` with a CH32X that emits `power.status` v2 (the
production case). The `power` object is from a real unit on 15 V USB-C mains
with a full battery and the Pi rail up; the remaining objects are illustrative.
Since `esp32:0.8.10` the body may additionally carry the optional `rejected`
and `resp_dropped` fields next to `acks` — see "Command feedback" at the end
of this section:

```json
{
  "ts": 1780047458,
  "fw_ver": "esp32:0.8.9",
  "uptime_s": 137,
  "power": {
    "version": 2, "flags": 31, "charge_state": 3,
    "vbus_in_mv": 14709, "pd_in_mv": 15000, "pd_in_ma": 1750,
    "vbus_out_mv": 5051, "vout_set_mv": 5000, "vout_read_mv": 5046,
    "iout_limit_ma": 5010, "pd_out_mv": 5000, "pd_out_ma": 3000,
    "vbat_mv": 7897, "ibat_ma": 0,
    "vsys_mv": 7900, "iin_ma": 1200,
    "temp_lm_dc": 350, "temp_mp_dc": 440, "temp_dc": 440,
    "faults": 0, "uptime_s": 86272
  },
  "host": {
    "eth_state": 2, "cpu_temp_dc": 451,
    "mem_pct": 40, "disk_pct": 55, "load_x100": 120, "uptime_s": 99999
  },
  "net": {
    "state": 4, "rssi_dbm": -71, "rsrp_dbm": -98, "rsrq_db": -11,
    "bytes_tx": 12345, "bytes_rx": 67890
  },
  "acks": ["c-0042"]
}
```

**Legacy v1 shape** — no `version` key, the eight original keys (`ibus_out_ma`
and `ibat_ma` are signed). It appears only for units whose CH32X still emits
`power.status` v1:

```json
"power": {
  "charge_state": 1,
  "vbus_in_mv": 5012, "vbus_out_mv": 5050, "ibus_out_ma": 1840,
  "vbat_mv": 7920, "ibat_ma": -200, "temp_dc": 253, "faults": 0
}
```

> **Since `esp32:0.8.9`.** Earlier firmware decoded every `power.status`
> payload through the v1 struct, so a v2-emitting CH32X produced the 8 legacy
> keys with scrambled values (`temp_dc` was really `vout_set_mv`, `faults` was
> `pd_out_mv`, `charge_state` was `flags`) — GitHub issue #7. Servers should
> dispatch on `power.version` to tell the two shapes apart.

Field origins map directly onto the WUPS telemetry structs in
[`../common/protocol.h`](../common/protocol.h). The device dispatches on the
wire version byte (offset 0 of the payload): `power.version == 2` ↔
`wups_power_status_v2_t` (40 B); no `version` key ↔ `wups_power_status_v1_t`
(20 B); any other version byte, or a payload shorter than its struct → the
`power` object is omitted and the device logs one warning per boot. `host.*` ← `wups_host_status_v1_t`, `net.*` ←
`wups_net_status_v1_t` (`net.bytes_tx` / `net.bytes_rx` are the modem's
cumulative PPP byte counters, refreshed every ~60 s, so consecutive POSTs may
repeat a sample — §9 uses them to measure the on-wire cost). Units are as named: `*_mv` millivolts, `*_ma`
milliamps (signed where noted), `*_dc` deci-Celsius (253 = 25.3 °C), `*_pct`
percent, `load_x100` = 1-min load × 100, `*_s` seconds.

`net.status` v3 additionally supplies optional `net.sinr_db` in dB (`-20..30`,
including `0`). It is omitted for v1/v2 telemetry or an unavailable/invalid
measurement; the binary `-128` sentinel is never emitted as a JSON reading.
The v1-prefix network fields above keep their existing names and meaning.

`power` v2 keys (since `esp32:0.8.9`), in emission order:

| key | unit | wire field | note |
|---|---|---|---|
| `version` | — | `version` | always `2` for this shape |
| `flags` | bitmask | `flags` | see legend below |
| `charge_state` | enum | `charge_state` | see legend below |
| `vbus_in_mv` | mV | `vbus_in_mV` | input rail after the ideal-diode OR of USB-C and barrel |
| `pd_in_mv` | mV | `pd_in_mV` | negotiated INPUT PD contract (HUSB238); `0` = no contract |
| `pd_in_ma` | mA | `pd_in_mA` | negotiated INPUT PD contract; `0` = no contract |
| `vbus_out_mv` | mV | `vbus_out_mV` | independent ADC measurement of the Pi rail |
| `vout_set_mv` | mV | `vout_set_mV` | TPS55289 commanded output voltage |
| `vout_read_mv` | mV | `vout_read_mV` | TPS55289 output voltage readback |
| `iout_limit_ma` | mA | `iout_limit_mA` | TPS55289 current **limit** — not a load-current measurement |
| `pd_out_mv` | mV | `pd_out_mV` | OUTPUT PD contract to the Pi; `0` = rail off |
| `pd_out_ma` | mA | `pd_out_mA` | OUTPUT PD contract to the Pi; `0` = rail off |
| `vbat_mv` | mV | `vbat_mV` | battery voltage (authoritative ADC) |
| `ibat_ma` | mA, signed | `ichg_mA` | MP2762A **charge** current; `0` on discharge (not measured) |
| `vsys_mv` | mV | `vsys_mV` | MP2762A VSYS rail (feeds the TPS55289) |
| `iin_ma` | mA | `iin_mA` | MP2762A charger input current |
| `temp_lm_dc` | 0.1 °C, signed | `temp_lm_dC` | LM75B board temperature |
| `temp_mp_dc` | 0.1 °C, signed, or `null` | `temp_mp_dC` | MP2762A junction temperature; `null` when the charger is unpowered (wire sentinel −32768) |
| `temp_dc` | 0.1 °C, signed | derived | aggregate: `max(temp_mp_dc, temp_lm_dc)`, or `temp_lm_dc` when `temp_mp_dc` is `null` — same rule as the OLED, the host service and the Workbench |
| `faults` | bitmask | `faults` | see legend below |
| `uptime_s` | s | `uptime_s` | CH32X uptime (restart detection) — distinct from the top-level ESP32 `uptime_s` |

Legends:

- `flags` bits: 0 `DC_IN_EN` (input path enabled), 1 `VBUS_OUT_EN` (Pi rail
  enabled), 2 `BATT_PRESENT`, 3 `POWER_GOOD` (charger ACOK, mains good),
  4 `USB_C_ATTACH` (HUSB238 PD contract present). Bits 5-7 are currently
  unused (0).
  The example's `31` = all five set.
- `charge_state`: 0 not charging, 1 pre-charge (trickle), 2 fast charge,
  3 charge done. Faults are never signalled here — see `faults`.
- `faults` (identical layout in v1 and v2): low byte = MP2762A REG14H fault
  register; bit 8 = TPS55289 SCP, bit 9 = OCP, bit 10 = OVP. `0` = no fault.
- `temp_dc`: the higher of the two sensors; falls back to `temp_lm_dc` when
  the MP2762A is unpowered (`temp_mp_dc` is `null`, e.g. battery-only
  operation).
- There is **no `ibus_out_ma` in v2** — the board has no load-current
  measurement. `iout_limit_ma` is the converter's current *limit*; `ibat_ma`
  is the *charge* current and reads `0` while discharging.

### Command feedback: `acks`, `rejected`, `resp_dropped`

`acks` lists the `id`s of commands the device **dispatched** since its previous
successful POST (empty array if none).

**Ack semantics (normative).** An ack means the ESP32 has verified the
response signature, translated the command into its WUPS frame and dispatched
that frame onto the in-board bus (`net.downlink` to the RP2040, which routes it
on to the RP2040 itself, the Raspberry Pi host service or the CH32X). An ack
does **not** confirm execution: the ESP32 does not wait for the RP2040 / host /
CH32X command RESP, so a frame that is dropped downstream (e.g. `host.*` while
the Pi service is not running) is still acked. Execution is verified **out of
band** — physically or from later telemetry — see §5 "Verifying that a command
took effect". Using the cmd RESP frames on the bus for an "executed / failed"
report is a documented future extension point, not implemented.

Acks are carried until a POST gets a `2xx`; a failed POST keeps them for the
next one (§3). The pending list holds 8 ids (`ACK_PENDING_MAX`) — exactly the
per-response cap, so since `0.8.10` it cannot overflow: a `2xx` drains every
ack the body carried *before* the response is applied, and applying a response
adds at most 8 (§5). `ack_add()` keeps a defensive oldest-evict for that
impossible case (the server would re-send the command and the device would
re-ack it from its dedup ring without re-executing it).

`rejected` (since `esp32:0.8.10`) — present only when non-empty — lists
commands the device **refused** since its previous successful POST:

```json
"rejected": [ { "id": "c-0044", "reason": "unsupported_cmd" } ]
```

| `reason` | Meaning |
|---|---|
| `bad_id` | `id` missing, not a JSON string, empty, or longer than 47 chars. The first 47 chars are echoed when present; an over-long id is **never** truncated into a different id. (Before `0.8.10` ids were cut to 31 chars, so a 36-char UUID was re-executed every cadence.) |
| `unsupported_cmd` | `cmd` is not one of the five names in §5. (Before `0.8.10` unknown names were only logged.) |
| `bad_args` | `cmd` is not a JSON string, or the payload could not be encoded. |

At most 4 entries are carried per POST (`REJ_MAX`; the oldest are dropped),
deduplicated by id — e.g. every entry without an id collapses into a single
`{"id":"","reason":"bad_id"}` report carrying the last reason seen.
**Rejected ids are never acked** — the server must drop them from its queue,
or they are rejected again on every poll.

`resp_dropped` (since `esp32:0.8.10`) — present only after a `2xx` response
whose body exceeded 2047 B (`HTTP_RESP_MAX` − 1). The body was counted, dropped
**whole** (no commands applied, one warning logged) and is reported once:

```json
"resp_dropped": { "bytes": 2310, "max": 2047 }
```

Such a `2xx` still counts as "uplink alive" for the modem watchdog. Before
`0.8.10` an over-size body was silently truncated, failed to parse, produced no
acks and made the server re-send it forever. Treat `resp_dropped` as a bug on
the server side (see §5 "Server-side requirements").

Like `acks`, both fields are cleared from the device only once the POST
carrying them receives a `2xx`.

A body with all three feedback fields (`esp32:0.8.10`; the telemetry objects
are elided here — their content is exactly as in the first example):

```json
{
  "ts": 1780047488,
  "fw_ver": "esp32:0.8.10",
  "uptime_s": 167,
  "power": { "version": 2, "…": "…" },
  "host":  { "…": "…" },
  "net":   { "…": "…" },
  "acks": ["c-0042", "c-0043"],
  "rejected": [
    { "id": "c-0044", "reason": "unsupported_cmd" },
    { "id": "0f8e7d6c-5b4a-4c3d-9e2f-1a0b9c8d7e6f-extra-long", "reason": "bad_id" }
  ],
  "resp_dropped": { "bytes": 2310, "max": 2047 }
}
```

(The second rejected entry shows a 54-char id echoed as its first 47 chars.)

## 5. Response body (server → device)

```json
{
  "commands": [
    { "id": "c-0043", "cmd": "ui.beep", "args": { "freq_hz": 2000, "dur_ms": 200 } }
  ]
}
```

- `{ "commands": [] }` (or any 2xx with no `commands`) means "nothing queued".
- A response **carrying commands must be signed** (`X-W3PUPS-Sig`, §3); the
  device drops the commands otherwise.
- Commands are **idempotent by `id`**: a server that hasn't seen an ack
  re-sends the command; the device re-acks but does **not** re-execute an `id`
  it already applied. The dedup ring holds the last 16 executed ids
  (`EXEC_RING_MAX`) in RAM — it is lost on an ESP32 reboot.
- The device applies **at most 8 commands per response** and reads **at most
  2047 B** of response body — see "Receiver limits" below (since
  `esp32:0.8.10`).

Each `commands[]` entry is checked in this order (since `esp32:0.8.10`):
`id` invalid → `rejected: bad_id`; id already executed → re-ack, skip; `cmd`
not a string → `bad_args`; unknown name → `unsupported_cmd`; payload encode
failure → `bad_args`; otherwise dispatched → executed + acked (§4).

### Command surface

The `cmd` names map 1:1 onto WUPS frames (identical to what the web panel emits
— `Web3-Pi-UPS-Panel/apps/api/src/lib/commands.ts`), so the ESP32 introduces
no new control semantics: it just translates JSON → the existing frame.

| `cmd` | `args` | WUPS class.op | Target | Notes |
|---|---|---|---|---|
| `ui.beep` | `freq_hz`, `dur_ms` (both optional; clamped to 0..65535 by the ESP32; `0` ⇒ RP2040 default 1500 Hz / 150 ms; the RP2040 clamps `dur_ms` to ≤ 5000) | UI 0x05 / 0x03 | RP2040 | best end-to-end "it landed" proof; silent when the unit is muted in the OLED menu |
| `ui.display_msg` | `text` — accepted up to 64 B by the ESP32; the RP2040 shows the first **40 chars** as 4 rows × 10 cols (`\n` breaks a row, no word wrap, DEL / non-ASCII drawn as `?`). `line` is reserved — send `0`, ignored | UI 0x05 / 0x04 | RP2040 | since `rp2040:1.2.2` rendered as a plain **info notice**: no "MODEM" banner, no alarm, one short chirp when first drawn, 60 s TTL, any button closes it. On `rp2040` ≤ 1.2.1 it rendered as the MODEM alarm banner with the alarm sound |
| `host.shutdown` | `delay_s` (default 5; **currently ignored** by the Raspberry Pi host service, which shuts down immediately) | HOST 0x04 / 0x02 | RPi (via RP2040) | reason = remote_cmd |
| `host.reset` | `delay_s` (default 5; **currently ignored** — the host service reboots immediately) | HOST 0x04 / 0x03 | RPi (via RP2040) | reboot |
| `power.cycle` | `off_ms` (default 1500, clamped to ≤ 60000; accepted but **currently fixed at 1500 ms** by the CH32X firmware) | POWER 0x02 / 0x04 | CH32X (via RP2040) | power-cycle the output |

Unknown `cmd` names are reported as `rejected` with reason `unsupported_cmd`
(since `esp32:0.8.10`; earlier firmware only logged them). The `delay_s` /
`off_ms` gaps are protocol-wide (the same frames from the web panel behave the
same way), not HTTP-specific. `host.service_restart` (whitelisted unit restart)
is supported by the protocol but intentionally not wired into the worked
example.

### Verifying that a command took effect

An ack (§4) only says the frame left the ESP32. Confirm execution out of band:

| `cmd` | How to tell it executed |
|---|---|
| `ui.beep` | audible on the unit (RP2040 buzzer); no telemetry trace. A muted unit stays silent. |
| `ui.display_msg` | visible on the OLED for up to 60 s (info notice, `rp2040:1.2.2`+); no telemetry trace. |
| `host.shutdown` | the `host` object **disappears** from subsequent POSTs within ~2 min (90 s freshness + one 30 s poll): `host.status` is only emitted while the Pi service runs, and a class with no snapshot newer than 90 s (`HTTP_FRESHNESS_MS`) is omitted from the body. |
| `host.reset` | `host.uptime_s` drops to a small value in a later POST — the reliable trace. The `host` object may or may not disappear in between: the last pre-reboot snapshot stays "fresh" for 90 s, so a Pi that is back within that window never drops out (its stale `uptime_s` is repeated until the service resumes). |
| `power.cycle` | `power.flags` bit 1 (`VBUS_OUT_EN`) clears and `power.vbus_out_mv` dips in a later `power` object. `power.status` is produced at a 1 s local cadence but the HTTP body samples it once per 30 s poll, so a 1.5 s dip is usually **missed** — the Pi losing its rail reboots it, so `host.uptime_s` restarting (as for `host.reset`) is the reliable trace. |

A richer "executed / failed" report driven by the RP2040 / CH32X command RESP
frames on the in-board bus is a documented future extension point, not
implemented.

### Receiver limits (device side)

Defined in one place — the "Receiver limits" block at the top of
[`firmware-ESP32-LTE-M/main/http_backend.c`](../firmware-ESP32-LTE-M/main/http_backend.c)
— and mirrored by the reference server.

| Limit | Value | Behaviour when exceeded | Since |
|---|---|---|---|
| Response body | ≤ 2047 B (`HTTP_RESP_MAX` 2048 incl. NUL) | a larger `2xx` body is counted, dropped **whole** (no commands applied, one warning logged) and reported once as `resp_dropped {bytes, max}`; the `2xx` still counts as uplink-alive. Before `0.8.10`: silently truncated → unparsable → no acks → endless re-send. | `esp32:0.8.10` |
| Commands applied per response | ≤ 8 (`CMDS_PER_RESP_MAX` = `ACK_PENDING_MAX`) | entries beyond the 8th are neither applied nor acked; the server re-sends them next poll (converges 8 per cycle). | `esp32:0.8.10` |
| Pending-ack list | 8 ids (`ACK_PENDING_MAX`) | cannot overflow since `0.8.10` (8 applied per response = 8 pending; a `2xx` drains the carried acks before the response is applied). `ack_add()` keeps a defensive oldest-evict: the server would re-send, the device re-ack from the dedup ring. | — |
| Executed-id dedup ring | last 16 ids (`EXEC_RING_MAX`), RAM only | older ids fall out and would be re-executed if re-sent; lost on an ESP32 reboot. | — |
| Command `id` | JSON string of 1..47 chars (`ACK_ID_MAX` 48 incl. NUL) | missing / not a string / empty / longer → `rejected: bad_id` (first 47 chars echoed when present); never truncated into another id. Before `0.8.10`: cut to 31 chars. | `esp32:0.8.10` |
| `rejected` entries carried | ≤ 4 per POST (`REJ_MAX`), deduplicated by id | oldest dropped. | `esp32:0.8.10` |
| Redirects | none followed (`disable_auto_redirect`) | `3xx` logged, nothing applied, nothing dropped (§3). | `esp32:0.8.10` |
| Per-socket timeout | 20 s (`HTTP_TIMEOUT_MS`) | POST fails, retried next cadence; carried feedback kept. | — |
| Poll period | 30 s (`HTTP_PERIOD_MS`, compile-time); first POST ~12 s after the backend starts (`HTTP_SETTLE_MS`) | — | — |
| Telemetry freshness | 90 s (`HTTP_FRESHNESS_MS`) | a class with no snapshot newer than this is omitted from the body. | — |
| Request body | ≤ 2047 B (`HTTP_BODY_MAX` 2048) | typical v2 body 505–510 B; 578–580 B with 8 acks; ~550 B with `resp_dropped`. | `esp32:0.8.10` (was 1536) |
| `ui.display_msg` text | 64 B accepted by the ESP32; first 40 chars shown by the RP2040 | silently cut. | — |
| `ui.beep` args | `freq_hz`, `dur_ms` clamped to 0..65535 by the ESP32 | RP2040 maps `0` → 1500 Hz / 150 ms and clamps `dur_ms` ≤ 5000. | — |

### Server-side requirements

1. **Re-send until acked.** Keep every command in the response until its `id`
   appears in `acks` (or it expires on your side). The device re-acks an
   already-executed id without re-executing it.
2. **At most 8 commands per response, body ≤ 2047 B.** Entries beyond the 8th
   are neither applied nor acked; a body over 2047 B is dropped whole.
3. **Drop ids reported in `rejected`.** They are never acked; re-sending them
   only repeats the rejection.
4. **Treat `resp_dropped` as a bug on your side** — shrink the response (fewer
   commands per poll, shorter ids / args).
5. **Never mint ids longer than 47 chars** (a 36-char UUID fits). Ids must be
   non-empty JSON strings.
6. **Point the device at the final URL.** Redirects are not followed; a `3xx`
   applies nothing.
7. **Keep the response bytes unmodified end-to-end.** `X-W3PUPS-Sig` covers
   the exact body bytes: a proxy that re-serialises, pretty-prints or otherwise
   rewrites the JSON breaks the signature and the device drops the commands
   (fail-closed, §3).
8. **Expire stale commands.** A `host.shutdown` delivered an hour late (device
   was offline) is worse than none. The reference server drops any command
   not acked within 600 s of being queued — delivered or not (`--cmd-max-age`).

## 6. Configuring the endpoint (no re-flash)

You do **not** re-flash the ESP32 to point a fielded unit at your server (the
ESP32's USB may not even be accessible in the enclosure). The endpoint is set
at runtime from the **Raspberry Pi
host**, which sends a `net.config` frame (NET op `0x21`, see `protocol.h`) down
the existing serial link. The RP2040 routes RPi→ESP32 frames unchanged (no
RP2040 firmware change), and the ESP32 persists the value in its writable `nvs`
partition (namespace `w3http`, keys `url` / `devid`). It survives reboots and
backend-mode switches (cleared only by a factory reset).

Value rules — the device checks only the **length**; it applies no scheme
check and no normalisation, so a value that breaks the other rules is stored
as-is and every POST then fails:

- `url`: ≤ 200 chars (`HTTP_CFG_URL_MAX`; longer is refused, and
  `send_config.py` refuses it locally too). Must start with `http://` or
  `https://`. **No trailing slash** — the device appends
  `/api/v1/devices/{device_id}/telemetry` verbatim, so `https://host/` yields
  `//api/…` (404 on the reference server).
- `device_id`: ≤ 64 chars (`HTTP_CFG_DEVID_MAX`); used verbatim in the path and
  in `X-W3PUPS-Device`.
- `https://` requires a **publicly trusted certificate**: the device verifies
  the chain against the full Mozilla CA bundle **with hostname verification**.
  Self-signed or private-CA certificates are rejected; an IP-literal
  `https://203.0.113.10` needs a certificate carrying that IP in its SAN. Plain
  `http://` needs nothing (commands stay HMAC-authenticated, §3) and costs far
  fewer bytes per poll (§9).

From the Pi (see [`../examples/http-control-server/send_config.py`](../examples/http-control-server/send_config.py)):

```bash
sudo apt install -y python3-serial   # pyserial
python3 send_config.py --url http://<your-vps-ip>:8080
# the port is auto-detected by USB descriptor ("Web3_Pi_UPS"); pass --port to override
# optional: --device-id <id>   (default = ICCID)
# clear an override:  --url ""
```

> **Port contention.** The `w3p-ups` host service normally **owns** the RP2040
> serial port (it reads telemetry / sends commands). Two writers on one CDC
> corrupt framing, so `send_config.py` opens the port exclusively and will tell
> you to stop the service briefly if it can't:
> ```bash
> sudo systemctl stop w3p-ups && python3 send_config.py --url … && sudo systemctl start w3p-ups
> ```
> The cleaner production path is to have the service set this itself (a
> `w3p-ups set-http-url <url>` subcommand routing a `net.config` frame on the
> port it already owns) — see "Open / follow-ups" in the HTTP-2 evidence note.

A compile-time default (`HTTP_ENDPOINT_BASE` in `main/endpoints.h`) is
honoured only when NVS has no `url` yet — convenient for bench/dev. The
runtime `net.config` value always takes precedence.

Switching the device into HTTP mode itself is done from the **OLED menu**
(ADR-0012: MQTT / Arkiv / HTTP), which reboots into the selected backend.

## 7. Deployment recipe

The minimal path is "run the Python program on a VPS and point the device at
its IP" — **no TLS or domain required**:

1. On the device: OLED menu → **HTTP Key** — note the code shown (e.g.
   `ABCD EFGH JKLM NPQR`). Generate a fresh one with **New key** if you like.
2. Stand up the reference server on a host with a public IP, passing that code:
   ```bash
   python3 examples/http-control-server/server.py \
       --secret "ABCD EFGH JKLM NPQR" --device-id <iccid> --port 8080
   ```
3. From the Pi, point the device at it:
   ```bash
   python3 send_config.py --port <rp2040-cdc> --url http://<your-vps-ip>:8080
   ```
4. Switch the device to HTTP mode on the OLED (menu → Mode → HTTP). Within ~30 s
   you should see the first telemetry POST in the server log.
5. Enqueue a command at the server prompt (e.g. `beep`) — it is delivered on
   the next poll and its ack (bus-dispatch, §4) rides on the one after. Confirm
   execution by ear / on the OLED / in later telemetry (§5).

**Optional — add a domain + TLS** for confidentiality (telemetry contents are
otherwise readable on the wire; commands are authenticated either way). Put any
reverse proxy in front and switch the URL to `https://…`, e.g. Caddy
(automatic Let's Encrypt):

```
ups.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

(nginx/Traefik with an LE cert work equally well; keep the
`/api/v1/devices/{device_id}/telemetry` path intact. Then
`send_config.py --url https://ups.example.com`.)

The M3 self-hosting acceptance check (criterion #8) validates that a tester can
stand the server up **from these docs alone**.

## 8. Security caveats for self-hosters

- **Guard the HTTP key.** It is the sole authenticator for *both* directions —
  anyone holding it can forge telemetry and forge signed commands. It's shown on
  the OLED on request; re-roll it (menu → HTTP Key → New key) if it leaks, and
  update your server. It is independent of the MQTT/Arkiv secret, so exposing it
  doesn't compromise those backends.
- **TLS is optional and only adds confidentiality.** Without it, telemetry and
  command contents are readable by an on-path observer, but they **cannot be
  forged or injected** (both directions are HMAC-signed). Add TLS (a reverse
  proxy + domain) if telemetry contents are sensitive on your network, or if
  you want defence-in-depth. The device accepts `http://` and `https://`.
- **Clock**: the device timestamps from network time (SNTP). Keep your server
  clock on NTP or valid requests will look stale.
- **Nonce store**: persist recent nonces per device (the reference stub keeps
  them in memory and forgets them on restart — a brief replay window).
- **One secret per device**: a multi-device server looks the secret up by the
  `{device_id}` path segment / `X-W3PUPS-Device` header.
- The shipped server is an **example**, not hardened infra: no auth on the
  operator command prompt, no persistence, no rate limiting.
- **Bounded responses.** The reference server sends at most 8 commands / 2047 B
  per response and drops any command not acked within 600 s of being queued,
  delivered or not (`--max-commands`, `--cmd-max-age`) — matching the device's
  receiver limits (§5). A server of your own must respect the same limits.

## 9. Data budget

The bundled SIM is a **1NCE prepaid pool: 500 MB per SIM or 10 years from
activation, whichever comes first** — a lifetime allowance, not a monthly
quota (top-up is possible via the 1NCE portal, i.e. by Web3 Pi, not by the end
user). Every poll cycle spends part of it, so the transport and the cadence
decide how long the pool lasts.

**Connection mechanics.** `esp_http_client` is created and torn down for every
POST: each cycle opens a **new TCP connection** and, for `https://`, runs a
**full TLS 1.2 handshake** (no TLS session resumption in use — the client
handle is destroyed after every POST and esp-tls session tickets are not
enabled, `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS`; no keep-alive).
The JSON body itself is ~0.5 KB (§5 "Receiver limits"); connection setup is a
large share of the per-cycle cost, and TLS multiplies it.

| Transport / mode (30 s cadence) | Per cycle | Per day | Per 30 days | 500 MB pool lasts | Basis |
|---|---|---|---|---|---|
| HTTP mode, plain `http://` | **1 629 B** (1 130 up + 498 down) | ≈ 4.7 MB | ≈ 141 MB | **≈ 107 days** | **MEASURED** 2026-09-11 on unit ICCID 8988228066680569990, `esp32:0.8.9`, reference server on a VPS, no commands queued: 78 consecutive POSTs over 39 min, figure = delta of the device's own cumulative PPP counters (`net.bytes_tx` / `net.bytes_rx`) |
| HTTP mode, `https://` | ≈ 5.5–9 KB | ≈ 16–26 MB | ≈ 0.5–0.8 GB | **≈ 3–4 weeks** | **ESTIMATE — not measured**: plain-http figure + Let's Encrypt chain 2.2–3.5 KB per handshake + TLS records + extra TCP segments |
| MQTT mode (for comparison) | — | ≈ 1.66 MB | ≈ 50 MB | ≈ 10 months | measured earlier on the MQTT backend (one persistent TLS session, no per-cycle handshake) |

Cost scales **linearly with the cadence**: a 5 min poll period costs ~10× less
than 30 s (plain http ≈ 0.47 MB/day → the pool lasts ≈ 2.9 years; only at a
cadence of ≥ ~17 min does 500 MB outlast the SIM's 10-year validity).

**Guidance.**

- On the bundled SIM prefer **plain `http://`** with the built-in HMAC signing
  (§3 — TLS adds confidentiality only) and the **widest cadence the use case
  tolerates**.
- Use `https://` only with your own SIM or a topped-up pool.
- Do not plan on more than ~3 months of plain-http polling at the default 30 s
  cadence per 500 MB pool.

**Measuring it yourself.** Every POST carries `net.bytes_tx` / `net.bytes_rx`:
the modem's cumulative PPP byte counters. They are refreshed every ~60 s, so
consecutive POSTs may repeat the same sample; the reference server prints
`Δtx=… Δrx=… B since previous net.status sample` for each *distinct* sample
and skips unchanged or backwards (device rebooted) samples. Divide the delta
by the number of POSTs in the window for the per-cycle figure.

**Future options (not implemented).** The poll period is compile-time today
(`HTTP_PERIOD_MS`); a runtime-configurable period, HTTP keep-alive and TLS
session tickets would each cut the per-cycle cost and are candidate follow-ups,
none of them in the firmware at `esp32:0.8.10`.
