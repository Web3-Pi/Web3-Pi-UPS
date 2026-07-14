#pragma once

/*
 * Compile-time network endpoints — tracked in git; nothing in here is
 * secret. (This replaces the old gitignored main/secrets.h: the retired
 * MASTER_SECRET is no longer part of the firmware — per-device credentials
 * are read at boot from the read-only `prov` NVS partition, see identity.c
 * / ADR-0008 — and the broker URI is public.)
 */

/* MQTT broker URI.
 *  - mqtts://  → MQTT over raw TCP/TLS on 8883 (LE cert via Traefik TCP entryPoint)
 *  - wss://    → MQTT over WebSocket Secure on 443 (LE cert via Traefik HTTP)
 *
 * Both endpoints are LE-protected and routed through Traefik on the same
 * VPS. mqtts:// is preferred for IoT (smaller footprint, no HTTP framing
 * overhead). wss:// is the fallback for restrictive networks where only
 * 443 is reachable. */
#define MQTT_BROKER_URI    "mqtts://broker.w3p.ovh:8883"

/*
 * HTTP-2 (§4.18a) — OPTIONAL compile-time default for the HTTP control-mode
 * backend's endpoint. Leave undefined (or empty) in production: a fielded
 * unit is pointed at the operator's server at runtime from the RPi host
 * (net.config → NVS, see http_cfg.c), so no re-flash is needed. This default
 * is only a convenience for bench/dev — it is used solely when NVS has no
 * `w3http/url` value yet. No trailing slash.
 *
 *   #define HTTP_ENDPOINT_BASE "https://ups.example.com"
 */
