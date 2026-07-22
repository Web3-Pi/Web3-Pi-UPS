#pragma once

/*
 * HTTP-2 (§4.18a) — HTTP control-mode backend (worked example).
 *
 * A third backend alongside MQTT and Arkiv (ADR-0012). When the device boots
 * in HTTP mode it talks to a user-hosted endpoint, fully independent of EMQX
 * and Arkiv. Design: web3pi_scope/milestones/M1/evidence/HTTP-1-design-note.md.
 *
 * Direction of travel (HTTP-1 §"Connectivity model"):
 *   - The device is a pure HTTP *client* — it only makes outbound requests
 *     (works behind 1nce CGNAT, same posture as MQTT/Arkiv).
 *   - Telemetry goes UP in the request body of a periodic signed POST.
 *   - Commands come DOWN in the JSON response body (polling on the telemetry
 *     POST — no server push, no long-poll; cheap on the 500 MB LTE-M plan).
 *   - Command acks ride UP on the *next* POST.
 *
 * The ESP32 stays a dumb pipe: it serialises the telemetry aggregate it
 * already snoops off the RP2040 link into JSON, and translates a JSON `cmd`
 * into the corresponding WUPS frame routed back to the RP2040 (exactly the
 * frames the panel emits for MQTT — see Web3-Pi-UPS-Panel commands.ts).
 *
 * Auth: both directions are HMAC-SHA256 signed with the per-device 32-byte
 * secret (identity_secret_raw). The request signs (Ts || Nonce || raw_body);
 * the server signs (request_Nonce || raw_response_body) and the device verifies
 * it before executing any command — so commands can't be injected even over
 * plain HTTP. TLS is therefore optional (confidentiality only); https:// URLs
 * are validated against the bundled CA store.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the HTTP backend task (idempotent). Call once PPP + SNTP are up and
 * the active backend mode is HTTP. The task self-paces; it logs and retries
 * if the endpoint is unset or unreachable. */
void http_backend_start(void);

/* Snoop a telemetry-bound WUPS frame on its way through the RP2040 link so
 * the next POST carries an up-to-date power/host/net snapshot. `frame` is the
 * whole inner WUPS frame (SYNC..END); only power.status / host.status /
 * net.status are cached, others are ignored. Mirrors arkiv_tlm_observe_frame.
 * Safe to call from the wups_link RX task context. */
void http_backend_observe_telemetry_frame(const uint8_t *frame, uint16_t frame_len);

/* esp_timer second of the last successful (2xx) telemetry POST exchange;
 * 0 = none yet this boot. Uplink-health source for the modem's post-PPP
 * watchdog in HTTP mode. Lock-free single-word read, safe from any task. */
uint32_t http_backend_last_success_s(void);

/* True when an endpoint base URL is configured (NVS `w3http/url` or the
 * compile-time HTTP_ENDPOINT_BASE) — i.e. the backend can actually POST.
 * The modem's uplink watchdog treats an unconfigured endpoint as "nothing
 * to supervise" (same posture as unclaimed Arkiv), so a device awaiting its
 * guided VPS setup is never reset-looped on a healthy network. */
bool http_backend_is_configured(void);

#ifdef __cplusplus
}
#endif
