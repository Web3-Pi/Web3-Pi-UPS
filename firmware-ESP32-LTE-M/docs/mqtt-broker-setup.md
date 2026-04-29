# MQTT Broker Setup — EMQX on Dokploy with Traefik TLS

A step-by-step guide to standing up your own MQTT broker for the Web3 Pi
UPS fleet (or any IoT project), with proper TLS certificates from
Let's Encrypt and three transports: HTTPS dashboard, MQTT-over-TLS for
embedded clients, and WebSocket Secure for browser / web clients.

This document captures everything we ran into during the first end-to-end
bring-up, so the next person doesn't have to discover the same gotchas.

## What you get

After following this guide:

- **`https://<dashboard>.<your-domain>`** — EMQX admin web UI behind LE TLS
- **`mqtts://<broker>.<your-domain>:8883`** — raw MQTT-over-TLS, native and
  efficient, ideal for embedded ESP32 / cellular clients
- **`wss://<wss>.<your-domain>:443/mqtt`** — MQTT-over-WebSocket-Secure on
  port 443, works through any restrictive firewall

All three share the same EMQX container, the same authentication backend,
and the same Let's Encrypt automation managed by Traefik (Dokploy's
default reverse proxy).

## Architecture

```
                                 the internet
                                       │
                                       │ TCP/443 (HTTPS, WSS)
                                       │ TCP/8883 (mqtts)
                                       │ TCP/80 (ACME challenges)
                                       ▼
            ┌────────────────────────────────────────────────────────┐
            │              VPS (e.g. Hetzner CX32)                   │
            │                                                        │
            │  ┌──────────────────────────────────────────────────┐  │
            │  │                    Traefik                       │  │
            │  │   entryPoints: web, websecure, mqtts             │  │
            │  │   certresolvers: letsencrypt (HTTP-01 challenge) │  │
            │  └────────┬───────────────────────┬─────────────────┘  │
            │           │                       │                    │
            │           │ HTTP/HTTPS routers    │ TCP router         │
            │           │ (Host: ...)           │ (HostSNI: ...)     │
            │           ▼                       ▼                    │
            │  ┌──────────────────────────────────────────────────┐  │
            │  │              dokploy-network (bridge)            │  │
            │  └────────────────────────┬─────────────────────────┘  │
            │                           │                             │
            │  ┌────────────────────────┴─────────────────────────┐  │
            │  │                   emqx container                 │  │
            │  │   :18083 (HTTP dashboard)                        │  │
            │  │   :8083  (WS for WSS termination at Traefik)     │  │
            │  │   :1883  (plain MQTT for mqtts termination)      │  │
            │  │   …                                              │  │
            │  └──────────────────────────────────────────────────┘  │
            └────────────────────────────────────────────────────────┘
```

Key idea: **TLS terminates at Traefik**, not at EMQX. Traefik holds the
LE cert and forwards plain MQTT / plain HTTP to EMQX over the internal
Docker network. EMQX's own TLS listeners (with their bundled
self-signed cert) are unreachable from outside and effectively unused.

## Prerequisites

- A VPS with a public IP address. We used Hetzner Cloud CX32 (Ubuntu
  24.04 LTS, ~€5/month). Anything with Docker support works.
- A domain you control. This guide uses `w3p.ovh`; replace with yours.
- DNS access for the domain (we'll need three subdomains).
- An SSH key pair on your local machine.
- A locally-installed [MQTT Explorer](https://mqtt-explorer.com/) for
  manual verification. Optional but useful.

## Step 1 — Prepare the VPS

Standard Ubuntu hardening. Skip if you already have this.

```sh
# As root, after first login:
adduser robert
usermod -aG sudo robert
mkdir -p /home/robert/.ssh
# (paste your public key into /home/robert/.ssh/authorized_keys)
chown -R robert:robert /home/robert/.ssh
chmod 700 /home/robert/.ssh
chmod 600 /home/robert/.ssh/authorized_keys
```

Then disable root login and password auth in `/etc/ssh/sshd_config`:

```
PermitRootLogin no
PasswordAuthentication no
```

`systemctl reload ssh`.

### Firewall

Two firewalls to think about:

1. **Hetzner Cloud Firewall** (if your VPS is on Hetzner Cloud) — manage
   in the Cloud Console under Firewalls. Open inbound:
   - `TCP/22` (SSH)
   - `TCP/80` (HTTP — needed for Let's Encrypt HTTP-01 challenges)
   - `TCP/443` (HTTPS, WSS)
   - `TCP/8883` (MQTT-over-TLS)

2. **`ufw` on the VPS** — *technically* you can also configure ufw, but
   note that **Docker bypasses ufw** by default (it manipulates iptables
   directly through the `DOCKER` chain, which sidesteps ufw rules on the
   `INPUT` chain for any port mapped via `docker run -p`). In our setup
   we removed the ufw rule for 8883 and it still works because Traefik
   maps port 8883 with `-p 8883:8883`, and Docker punches that through
   iptables before ufw sees it. So ufw is mostly cosmetic here. The
   Hetzner Cloud Firewall (or whatever firewall sits in front of the VM)
   is the one that actually matters.

   If you still want ufw + Docker working "properly":
   <https://github.com/chaifeng/ufw-docker>

## Step 2 — Install Dokploy

```sh
curl -sSL https://dokploy.com/install.sh | sh
```

After install, the admin UI is at `http://<VPS_IP>:3000`. First
visit prompts you to create the admin account. Pick a strong password
and 2FA-enable later.

Dokploy is the reason we get a Traefik instance with ACME on autopilot —
it sets that up for us.

## Step 3 — DNS records

Add three A-records pointing to the VPS public IP:

| Subdomain                  | Purpose                                  |
| -------------------------- | ---------------------------------------- |
| `emqx.<your-domain>`       | EMQX admin dashboard (HTTPS)             |
| `mqtt.<your-domain>`       | MQTT over WebSocket Secure (WSS)         |
| `broker.<your-domain>`     | MQTT over raw TCP/TLS (mqtts://)         |

Wait for DNS propagation (`dig +short broker.<your-domain>` should
return your VPS IP).

## Step 4 — Add the `mqtts` entryPoint to Traefik

By default Dokploy's Traefik listens on `:80` (web) and `:443` (websecure)
only. Raw MQTT-over-TLS uses port 8883, so we add a third entryPoint.

### 4a) Edit `traefik.yml`

In Dokploy admin → **Web Server** → look for **"Edit Traefik Configuration"**
or similar, OR SSH to the VPS:

```sh
sudo nano /etc/dokploy/traefik/traefik.yml
```

Add the `mqtts` block to `entryPoints`:

```yaml
entryPoints:
  web:
    address: :80
  websecure:
    address: :443
    http3:
      advertisedPort: 443
    http:
      tls:
        certResolver: letsencrypt
  mqtts:                # ← add this
    address: :8883      # ← add this
```

### 4b) Map port 8883 from the host into the Traefik container

Adding the entryPoint isn't enough on its own — the Traefik *container*
also needs to expose port 8883 to the host network namespace.

In Dokploy admin → **Web Server** → **Additional Port Mappings**
(button labeled like that in v0.20+):

| Target Port | Published Port | Protocol |
| ----------: | -------------: | :------: |
|        8883 |           8883 |    TCP   |

Save. The warning that "the Traefik container will be recreated" is
expected — there will be a few seconds of downtime on the dashboard
and any existing WSS connections.

After Save, verify on the VPS:

```sh
sudo ss -tlnp | grep 8883
# expect: LISTEN  0  4096  0.0.0.0:8883  ...  (with traefik PID/process)
```

If you don't see Traefik listening, the recreation didn't apply — try
`sudo docker restart dokploy-traefik` or redeploy from the Dokploy admin
panel.

### Note: this is *not* configurable from the regular Domains GUI

Dokploy → Domains generates **HTTP** routers for Traefik
(`traefik.http.routers.*`). For MQTT-over-TLS we need a **TCP** router
(`traefik.tcp.routers.*`) which only exists as Docker labels (Step 5
below). Do **not** add `broker.<your-domain>` in the Domains GUI — it
would create an HTTP router that conflicts with the TCP router we're
about to define.

## Step 5 — Deploy EMQX via Dokploy Compose

Create a new project in Dokploy → add a "Compose" service. Paste this
`docker-compose.yml`:

```yaml
services:
  emqx:
    image: emqx/emqx-enterprise:6.2.0
    hostname: node1.emqx.com
    environment:
      EMQX_NODE__NAME: emqx@node1.emqx.com
      EMQX_NODE__COOKIE: "PUT_A_LONG_RANDOM_STRING_HERE"
      # Override the default admin/public credentials so the dashboard
      # isn't exposed with well-known defaults the moment LE issues the cert.
      EMQX_DASHBOARD__DEFAULT_USERNAME: "admin"
      EMQX_DASHBOARD__DEFAULT_PASSWORD: "PUT_A_STRONG_PASSWORD_HERE"
    expose:
      - 18083   # Dashboard (HTTP, behind Traefik HTTPS)
      - 8083    # MQTT-over-WebSocket (HTTP, behind Traefik WSS)
      - 1883    # plain MQTT (behind Traefik TCP/TLS termination)
    # NB: NO `ports:` block — we never expose EMQX directly to the host.
    #     All external access goes through Traefik on the dokploy-network.
    volumes:
      - emqx_data:/opt/emqx/data
      - emqx_log:/opt/emqx/log
    networks:
      dokploy-network:
        aliases:
          - emqx-service
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "/opt/emqx/bin/emqx_ctl", "status"]
      interval: 30s
      timeout: 5s
      retries: 3
    labels:
      # Without this, Dokploy's Traefik provider (exposedByDefault=false)
      # ignores ALL the labels below. Symptom: 404 from Traefik for every
      # router pointed at this container.
      - "traefik.enable=true"

      # === EMQX Dashboard — HTTP router ===
      - "traefik.http.routers.emqx-dashboard.rule=Host(`emqx.w3p.ovh`)"
      - "traefik.http.routers.emqx-dashboard.entrypoints=websecure"
      - "traefik.http.routers.emqx-dashboard.tls.certresolver=letsencrypt"
      - "traefik.http.routers.emqx-dashboard.service=emqx-dashboard"
      - "traefik.http.services.emqx-dashboard.loadbalancer.server.port=18083"

      # === MQTT-over-TLS — TCP router with TLS termination ===
      # Note: HostSNI(...), not Host(...). TCP routers match on the SNI
      # value embedded in the TLS ClientHello, since there's no HTTP
      # Host header to look at.
      - "traefik.tcp.routers.emqx-mqtts.rule=HostSNI(`broker.w3p.ovh`)"
      - "traefik.tcp.routers.emqx-mqtts.entrypoints=mqtts"
      - "traefik.tcp.routers.emqx-mqtts.tls=true"
      - "traefik.tcp.routers.emqx-mqtts.tls.certresolver=letsencrypt"
      - "traefik.tcp.routers.emqx-mqtts.service=emqx-mqtt-svc"
      # Backend port = 1883 (plain MQTT inside the container). Traefik
      # decrypts client traffic with the LE cert and forwards plaintext
      # over the docker network to EMQX.
      - "traefik.tcp.services.emqx-mqtt-svc.loadbalancer.server.port=1883"

      # === MQTT-over-WSS — HTTP router (TLS upgraded to WebSocket) ===
      - "traefik.http.routers.emqx-wss.rule=Host(`mqtt.w3p.ovh`)"
      - "traefik.http.routers.emqx-wss.entrypoints=websecure"
      - "traefik.http.routers.emqx-wss.tls.certresolver=letsencrypt"
      - "traefik.http.routers.emqx-wss.service=emqx-wss-svc"
      - "traefik.http.services.emqx-wss-svc.loadbalancer.server.port=8083"

volumes:
  emqx_data:
  emqx_log:

networks:
  dokploy-network:
    external: true
```

Replace the three `*.w3p.ovh` hostnames with your own subdomains.
Replace the placeholders for `EMQX_NODE__COOKIE` and the dashboard
password.

Click **Deploy**. The first deploy:

1. Pulls the EMQX image (~250 MB).
2. Starts the container.
3. Traefik notices the labels (because `traefik.enable=true`) and
   creates routes.
4. Traefik requests three Let's Encrypt certs (one per hostname) via
   HTTP-01 challenges on port 80. The first request takes ~30 s.
5. Routes go live.

## Step 6 — Verify externally

From any other machine:

### Dashboard
```sh
curl -I https://emqx.w3p.ovh/
# expect: HTTP/2 200
```

### MQTT-over-TLS handshake
```sh
echo | openssl s_client -connect broker.w3p.ovh:8883 \
  -servername broker.w3p.ovh -showcerts 2>&1 \
  | grep -E "subject=|issuer=|verify return"
```

Expected output (issuer changes over time as LE rotates intermediates;
R10/R11/R13 are all fine):

```
verify return:1
verify return:1
verify return:1
subject=CN=broker.w3p.ovh
issuer=C=US, O=Let's Encrypt, CN=R13
```

If you instead see `subject=CN=localhost, issuer=…EMQ RootCA…`, you're
hitting EMQX directly — Traefik isn't routing the connection. See
**Pitfalls** below.

### WSS reachability
```sh
curl -I https://mqtt.w3p.ovh/mqtt
# expect: HTTP/2 200 or HTTP/2 426 (Upgrade Required) — both fine,
#         it means Traefik routes to EMQX which then tries the WS upgrade
```

### MQTT Explorer

Set up two profiles to verify both transports:

**MQTT-over-TLS:**
- Protocol: `mqtts://`
- Host: `broker.w3p.ovh`
- Port: `8883`
- Validate certificate: **ON**
- Username / Password: as configured in EMQX

**WSS:**
- Protocol: `wss://`
- Host: `mqtt.w3p.ovh`
- Port: `443`
- Path: `/mqtt`
- Validate certificate: **ON**
- Username / Password: same

Both should show "Connected" with a green padlock.

## Step 7 — Initial EMQX configuration

Dashboard → first login uses the credentials you set via env var.
Change them via UI as well if you like.

### Add an MQTT user

EMQX → Access Control → Authentication → **Built-in Database** is
enabled by default. Add users you'll authenticate from. We used
`TestUpsUser` for the prototype; for production fleets you'd typically
provision per-device credentials.

### Disable the EMQX-internal SSL/WSS listeners

EMQX comes with `ssl/8883` and `wss/8084` listeners enabled, both using
EMQX's bundled self-signed cert (CN=localhost). Since Traefik handles
all TLS termination in our setup, these listeners are unreachable from
the outside, but it's tidier to disable them outright. EMQX dashboard →
**Cluster → Listeners** → toggle `ssl` and `wss` off.

The `tcp` (1883) and `ws` (8083) listeners must stay **enabled** —
Traefik forwards plain MQTT/WS traffic to those after terminating TLS.

## Pitfalls / lessons learned

### `MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000)` from an embedded client

Two common causes:

1. The broker is presenting EMQX's bundled self-signed cert
   (`CN=localhost`) instead of a real LE cert. Either Traefik isn't
   set up correctly, or you're hitting EMQX's own SSL listener directly
   (port 8883 mapped on the host bypassing Traefik). Run
   `openssl s_client` to check what cert is actually served.

2. The chip's wall clock isn't set, so cert validity period checks
   fail (notBefore is "in the future" from the chip's epoch=0 point of
   view). Solution: SNTP-sync time **before** TLS handshake. Our
   firmware does this in `modem.c::wait_for_time_sync()`.

### `Operation timed out` connecting to port 8883

Packets aren't reaching the VPS at all. The host:

- Hetzner Cloud Firewall blocking 8883 (most common — default is "only
  open what you add")
- Traefik not listening on host:8883 (the Additional Port Mapping wasn't
  saved or the container didn't recreate). Check
  `sudo ss -tlnp | grep 8883` on the VPS.

### `Connection refused` on port 8883

Different signal: packets reach the VPS, but nothing is listening. The
process for resolution:

- Verify the Traefik container exposes 8883:
  `sudo docker port dokploy-traefik` should include `8883/tcp -> 0.0.0.0:8883`
- Verify the entryPoint is in `traefik.yml`:
  `cat /etc/dokploy/traefik/traefik.yml | grep -A1 mqtts:`

### `traefik.enable=true` is mandatory

Dokploy's Traefik provider is configured with `exposedByDefault: false`
(security default — opt-in routing). Without `traefik.enable=true` on
the container, **all** Traefik labels on it are silently ignored, so
none of the routes work. Symptom: every domain returns 404.

When using Dokploy GUI's **Domains** feature, this label is added
automatically. When defining routes via Compose labels (the only way
for TCP routers), you must add it yourself.

### Don't double up on Domains GUI vs Compose labels

A specific gotcha: the Dokploy Domains GUI generates HTTP routers. If
you also have an HTTP router for the same domain in your compose
labels, Traefik may pick either one. Worse, if you accidentally add an
HTTP router for `broker.w3p.ovh` in the Domains GUI while we're trying
to use a TCP router for that domain, the HTTP router "wins" on
:443/HTTPS and you get bad gateway errors.

Recommendation: pick one source. We use compose labels for everything
related to our stack, and leave Dokploy → Domains empty for the
EMQX project.

### Don't expose EMQX ports directly to the host

The `ports: ["8883:8883"]` style mapping in EMQX's compose service
(present in the original Dokploy template) **bypasses Traefik
entirely** — anyone hitting the VPS on 8883 talks to EMQX directly,
including its bundled self-signed cert. Remove the `ports:` block from
EMQX once Traefik is doing TLS termination. Use only `expose:` for
documentation; ports are reachable from other containers on
`dokploy-network` regardless.

### Default credentials change

EMQX ships with `admin` / `public` for the dashboard. Anyone who
guesses your dashboard hostname and tries default creds gets full
access (creating users, reading retained messages, etc.). Change the
default via the `EMQX_DASHBOARD__DEFAULT_USERNAME` and
`EMQX_DASHBOARD__DEFAULT_PASSWORD` env vars in the compose service so
the change is reproducible across container recreations.

### TCP routers match on `HostSNI(...)`, not `Host(...)`

A subtle but important difference. HTTP routers inspect the `Host:`
header in the HTTP request. TCP routers inspect the SNI value in the
TLS ClientHello (because there's no HTTP layer to look at — it's raw
TCP/TLS).

If you mix these up:

- `Host(...)` on a TCP router never matches; Traefik returns nothing
  (client times out or sees connection close)
- `HostSNI(...)` on an HTTP router never matches either

Always: `HostSNI` for `traefik.tcp.routers.*`, `Host` for
`traefik.http.routers.*`.

### Backend port = `1883`, not `8883`

For the TCP-with-TLS-termination pattern, Traefik takes the TLS
connection from the client, decrypts it, and forwards the **plaintext**
MQTT bytes to the backend. EMQX's plaintext MQTT listener is on 1883,
not 8883. If you point the loadbalancer at 8883 instead, Traefik tries
to TLS-handshake again with EMQX's listener (which has its own
self-signed cert), and you get cryptic errors.

```yaml
- "traefik.tcp.services.emqx-mqtt-svc.loadbalancer.server.port=1883"   # ✅
# - "traefik.tcp.services.emqx-mqtt-svc.loadbalancer.server.port=8883" # ❌
```

## Maintenance

### Cert renewal

Traefik handles ACME automatically. Let's Encrypt certs are valid for
90 days; Traefik renews them at ~60 days. Watch:

```sh
sudo docker logs dokploy-traefik 2>&1 | grep -i acme | tail -20
```

You should see periodic renewal events. If a renewal fails (e.g. due
to rate limits or a momentary DNS issue), Traefik retries automatically.

### Backups

`emqx_data` and `emqx_log` are persistent volumes containing:
- Built-in DB users + ACL rules
- Retained messages
- Cluster config

Snapshot regularly:

```sh
sudo docker run --rm \
  -v emqx_data:/source:ro \
  -v $(pwd):/backup \
  alpine tar czf /backup/emqx_data_$(date +%Y%m%d).tgz -C / source
```

Or use Dokploy's built-in backup feature if your version exposes one.

### Version updates

When updating EMQX (e.g. 6.2.0 → 6.3.0):

1. Read EMQX release notes for breaking changes.
2. Backup `emqx_data` first.
3. Bump the `image:` tag in compose.
4. Redeploy in Dokploy.

EMQX major versions are not always rolling-upgrade safe — read the
upgrade guide before bumping major.

## Total cost estimate

Annual operating cost for the setup above (single-broker, fits 100s
of devices comfortably):

| Item                            |        Cost   |
| ------------------------------- | ------------: |
| Hetzner Cloud CCX11 (4 vCPU, 16 GB) | ~€60/year |
| Domain registration (`.ovh`)    | ~€5/year      |
| Let's Encrypt certificates      | €0            |
| Dokploy / EMQX OSS              | €0            |
| **Total**                       | **~€65/year** |

For a 50-unit Web3 Pi UPS deployment, that's roughly €1.30 per device
per year just for broker infrastructure. Plenty of headroom to scale
into the thousands without changing anything.

## References

- Dokploy docs: <https://dokploy.com/docs>
- The discussion that documented the original `mqtts` setup template
  used by Dokploy:
  <https://github.com/Dokploy/dokploy/discussions/3126>
- EMQX docs: <https://docs.emqx.com/en/emqx/v6/>
- Traefik TCP routers: <https://doc.traefik.io/traefik/routing/routers/#tcp-routers>
- Traefik labels reference: <https://doc.traefik.io/traefik/providers/docker/#routing-configuration-with-labels>
- Let's Encrypt rate limits: <https://letsencrypt.org/docs/rate-limits/>
- ufw + Docker interaction: <https://github.com/chaifeng/ufw-docker>
