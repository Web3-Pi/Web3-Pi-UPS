# HTTP control-mode reference server

A tiny, dependency-free worked example for driving a Web3 Pi UPS from **your
own** infrastructure over HTTP — no EMQX, no Arkiv. This is the HTTP-2
deliverable of plan §4.18a; the interface it speaks is defined in
[`HTTP-1-design-note.md`](../../../web3pi_scope/milestones/M1/evidence/HTTP-1-design-note.md)
and implemented on the device in
[`firmware-ESP32-LTE-M/main/http_backend.c`](../../firmware-ESP32-LTE-M/main/http_backend.c).

## How it works

The UPS (ESP32-S3) is a pure HTTP **client**. On a fixed cadence (~30 s) it
makes one signed `POST` carrying telemetry; your server's **response** to that
POST carries any queued commands; the device executes them and **acks** them on
the next POST. Nothing connects *to* the device, so it works behind the 1nce
LTE-M carrier NAT exactly like the MQTT and Arkiv backends.

```
[ UPS / ESP32-S3 ] --outbound HTTPS--> [ your server ]
   telemetry up in the POST body
   commands down in the POST response
   acks up on the next POST
```

```
POST /api/v1/devices/{device_id}/telemetry
X-W3PUPS-Device: {device_id}
X-W3PUPS-Ts:     {unix_seconds}
X-W3PUPS-Nonce:  {random_hex}
X-W3PUPS-Sig:    hex(HMAC_SHA256(secret, Ts || Nonce || raw_body))
Content-Type:    application/json

{ "ts":…, "fw_ver":…, "uptime_s":…, "power":{…}, "host":{…}, "net":{…}, "acks":[…] }
```

Response:

```json
{ "commands": [ { "id": "c-0007", "cmd": "ui.beep", "args": { "freq_hz": 2000, "dur_ms": 200 } } ] }
```

**Both directions are authenticated** with **HMAC-SHA256**, keyed by the
device's **HTTP key** — a short code the device generates and shows on its OLED
(menu → HTTP Key), separate from the MQTT/Arkiv secret and re-rollable on the
device:

- **Request**: signed over `Ts || Nonce || raw_body`. The timestamp gives a
  ±300 s replay window and the nonce is remembered to reject exact replays.
- **Response**: the server signs `request_Nonce || raw_response_body` and
  returns it in the response `X-W3PUPS-Sig` header. The device verifies this
  before executing any command, so **commands can't be injected even over
  plain HTTP**.

Because of this, **TLS is optional** — it only adds confidentiality (hiding
telemetry contents on the wire). The simplest deployment is "run this on a VPS
and point the device at `http://<ip>:8080`". Add a domain + TLS later if you
want privacy (see below).

## Files

| File | What it is |
|---|---|
| [`server.py`](server.py) | The reference server. Verifies signatures, prints telemetry, lets you enqueue commands from the terminal, returns + clears them via acks. |
| [`send_config.py`](send_config.py) | Run on the **RPi host**: points a fielded device at your server by sending a `net.config` frame over the serial link (no ESP32 re-flash). Needs pyserial (`sudo apt install -y python3-serial`). |
| [`test_client.py`](test_client.py) | Simulates a device POST (correct signature) so you can smoke-test the server — or a public TLS deployment — without hardware. |

## Quick start (no hardware)

```bash
# 1. pick a key (with real hardware this is the "HTTP Key" code shown on the
#    device OLED; for a dry run any string works as long as both sides match)
SECRET="ABCD EFGH JKLM NPQR"

# 2. start the server
python3 server.py --secret "$SECRET" --device-id TESTDEV --port 8080

# 3. in another terminal, pretend to be the device
python3 test_client.py --secret "$SECRET" --device-id TESTDEV --url http://127.0.0.1:8080

# 4. back in the server terminal, type a command at the prompt:
> beep 2000 200
# the next POST from the device (or test_client) returns it; the one after,
# carrying acks, clears it.
```

## Supported commands

These map 1:1 onto the WUPS command frames the firmware already understands
(identical to what the web panel emits — see
`Web3-Pi-UPS-Panel/apps/api/src/lib/commands.ts`):

| Prompt | `cmd` / `args` | Effect |
|---|---|---|
| `beep [freq_hz] [dur_ms]` | `ui.beep` | Buzzer on the RP2040 — the easiest end-to-end proof a command landed. |
| `msg <text>` | `ui.display_msg {text}` | Show text on the OLED. |
| `shutdown [delay_s]` | `host.shutdown {delay_s}` | Shut down the Raspberry Pi (via the RPi agent). |
| `reset [delay_s]` | `host.reset {delay_s}` | Reboot the Raspberry Pi. |
| `powercycle [off_ms]` | `power.cycle {off_ms}` | Power-cycle the output (via CH32X). |

Commands are **idempotent by `id`**: if the device already applied an id (but
the server hadn't yet seen the ack), it re-acks without re-executing.

## Pointing a real device at your server

1. **Get the HTTP key**: on the device, OLED menu → **HTTP Key**. Note the code
   (e.g. `ABCD EFGH JKLM NPQR`); start the server with it as `--secret`.
   (Re-roll anytime with **New key** — then update the server.)
2. **Set the URL** from the RPi (the device chooses its backend on the OLED
   menu: MQTT / Arkiv / **HTTP**; the URL can be set in any mode beforehand):

```bash
# on the Raspberry Pi attached to the UPS
sudo apt install -y python3-serial   # pyserial
python3 send_config.py --url http://<your-vps-ip>:8080   # or https://… if you added TLS
# the serial port is auto-detected (USB descriptor "Web3_Pi_UPS");
# `--list` shows candidates, `--port` overrides.
```

   **Note — the port is held by the `w3p-ups` service.** It owns the serial
   link, so stop it for the moment you run this, then start it again:
   ```bash
   sudo systemctl stop w3p-ups
   python3 send_config.py --url http://<your-vps-ip>:8080
   sudo systemctl start w3p-ups
   ```
   (Long-term this belongs as a `w3p-ups set-http-url` service subcommand — see
   the HTTP-2 evidence note.)

3. **Switch to HTTP mode** on the OLED (menu → Mode → HTTP). The device reboots
   into HTTP mode and starts posting within ~30 s.

The device appends `/api/v1/devices/{device_id}/telemetry` itself.
`device_id` defaults to the SIM ICCID; override it with
`send_config.py --device-id …` if your server keys devices differently.

For a bench/dev build you can instead bake a default into the firmware via
`HTTP_ENDPOINT_BASE` in `main/endpoints.h` — but the runtime `net.config`
value always wins.

## Optional: TLS

`server.py` speaks plain HTTP and that is a fine, secure-against-forgery setup
(both directions are HMAC-signed). Add TLS only if you want **confidentiality**
— i.e. to stop an on-path observer from *reading* your telemetry/commands.
Terminate it with any reverse proxy, e.g. Caddy (automatic Let's Encrypt):

```
ups.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

…or nginx/Traefik with a Let's Encrypt cert. Keep the
`/api/v1/devices/{device_id}/telemetry` path intact, then point the device at
the `https://` URL.

## Security notes (read before self-hosting)

- **Keep the HTTP key secret.** It authenticates *both* directions — anyone with
  it can forge telemetry and forge signed commands. It's shown on the OLED on
  request and re-rollable there; it's separate from the MQTT/Arkiv secret, so
  leaking it doesn't affect those backends.
- **Clock**: the device timestamps requests from network time (SNTP). If your
  server clock is wildly off, valid requests look stale — keep NTP running.
- **Nonce store** here is in-memory and per-process: a restart forgets recent
  nonces (a brief replay window). A production server should persist them (or a
  short-TTL store like Redis) and scope them per device.
- **One device, one secret** in this stub. A multi-device server looks the
  secret up by the `{device_id}` in the path / `X-W3PUPS-Device` header.
- This is an **example**, not hardened infrastructure. It has no auth on the
  command prompt, no persistence, and no rate limiting.
