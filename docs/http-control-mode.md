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
no long-poll — this keeps the radio idle between posts and is cheap on the
500 MB/month plan. Command latency is therefore up to one cadence interval
(~30 s); this is a control channel for shutdown/reset/beep-class actions, not a
real-time link.

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

Server responses:

| Status | Meaning (device behaviour) |
|---|---|
| `2xx` | accepted; device applies returned commands, treats carried acks as delivered |
| `401` | signature/identity rejected; device logs and retries next cadence |
| other `4xx`/`5xx`/timeout | device logs and retries next cadence (no backoff escalation in the example) |

## 4. Request body (device → server)

`Content-Type: application/json`. Sub-objects appear only when that telemetry
class has a **fresh** snapshot (observed within ~90 s); otherwise they are
omitted rather than sent stale.

Body as sent by `esp32:0.8.9` with a CH32X that emits `power.status` v2 (the
production case). The `power` object is from a real unit on 15 V USB-C mains
with a full battery and the Pi rail up; the remaining objects are illustrative:

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
`wups_net_status_v1_t`. Units are as named: `*_mv` millivolts, `*_ma`
milliamps (signed where noted), `*_dc` deci-Celsius (253 = 25.3 °C), `*_pct`
percent, `load_x100` = 1-min load × 100, `*_s` seconds.

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

`acks` lists the `id`s of commands the device applied since its previous POST
(empty array if none). This is the only command-confirmation channel.

> **Ack semantics in the example.** The device acks a command once it has
> dispatched the corresponding WUPS frame onto the in-board bus (RP2040). A
> richer "executed/failed" report (e.g. waiting for the RP2040/RPi cmd
> response) is a documented extension point, not implemented in the worked
> example.

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
  it already applied (it remembers a small ring of recent ids).

### Command surface

The `cmd` names map 1:1 onto WUPS frames (identical to what the web panel emits
— `Web3-Pi-UPS-Panel/apps/api/src/lib/commands.ts`), so the ESP32 introduces
no new control semantics: it just translates JSON → the existing frame.

| `cmd` | `args` | WUPS class.op | Target | Notes |
|---|---|---|---|---|
| `ui.beep` | `freq_hz`, `dur_ms` (both optional, 0 ⇒ firmware default) | UI 0x05 / 0x03 | RP2040 | best end-to-end "it landed" proof |
| `ui.display_msg` | `text` (≤64 B), `line` | UI 0x05 / 0x04 | RP2040 | shows text on the OLED |
| `host.shutdown` | `delay_s` (default 5) | HOST 0x04 / 0x02 | RPi (via RP2040) | reason = remote_cmd |
| `host.reset` | `delay_s` (default 5) | HOST 0x04 / 0x03 | RPi (via RP2040) | reboot |
| `power.cycle` | `off_ms` (default 1500, ≤60000) | POWER 0x02 / 0x04 | CH32X (via RP2040) | power-cycle the output |

Unknown `cmd` names are ignored (logged). `host.service_restart` (whitelisted
unit restart) is supported by the protocol but intentionally not wired into the
worked example.

## 6. Configuring the endpoint (no re-flash)

You do **not** re-flash the ESP32 to point a fielded unit at your server (the
ESP32's USB may not even be accessible in the enclosure). The endpoint is set
at runtime from the **Raspberry Pi
host**, which sends a `net.config` frame (NET op `0x21`, see `protocol.h`) down
the existing serial link. The RP2040 routes RPi→ESP32 frames unchanged (no
RP2040 firmware change), and the ESP32 persists the value in its writable `nvs`
partition (namespace `w3http`, keys `url` / `devid`). It survives reboots and
backend-mode switches (cleared only by a factory reset).

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
   the next poll and acked on the one after.

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

## 9. Data budget

At ~30 s cadence the telemetry POST is a few hundred bytes of JSON plus
TLS/HTTP overhead; commands are small and infrequent. This is comparable to the
MQTT uplink and fits the 500 MB/month plan with margin. The cadence can be
widened under inactivity if needed (firmware constant `HTTP_PERIOD_MS`).
