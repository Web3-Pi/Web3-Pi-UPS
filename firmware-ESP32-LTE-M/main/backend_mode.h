#pragma once

/*
 * ADR-0012 — runtime backend-mode switching. One firmware binary holds
 * MQTT, Arkiv and HTTP code paths; exactly one is selected at boot from
 * NVS (`w3mode/cur_mode`, default MQTT) and the rest of the firmware
 * gates its subsystems on `backend_mode_get()`.
 *
 * Switching is initiated only from the OLED 2-button menu in Phase 1
 * (the panel reflects but does not control). On switch:
 *   1. Emit a "pre-switch hint" event over the CURRENTLY active channel
 *      (MQTT publish or w3pups-event entity). This is the panel's
 *      authoritative signal — guaranteed delivery on the still-live
 *      channel, regardless of what the target backend is or whether
 *      it's reachable.
 *   2. Persist the new mode in NVS.
 *   3. esp_restart() — clean network-stack init for the new mode.
 *      Power path is hardware and unaffected (~30 s observability gap
 *      on telemetry only).
 *
 * Factory reset (also OLED-gated) erases the whole `nvs` partition
 * (per-device secret, Arkiv owner binding, mode flag, …), then reboots
 * back to first-boot behaviour with MQTT default.
 */

#include "esp_err.h"
#include "../../common/protocol.h"  /* wups_backend_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Read the persisted mode from NVS (or default MQTT if absent). Must be
 * called once early in app_main(), AFTER nvs_flash_init(). */
esp_err_t backend_mode_init(void);

/* Currently active mode (one of WUPS_BACKEND_MODE_MQTT/ARKIV/HTTP).
 * Stable for the lifetime of the boot — switches require a reboot. */
wups_backend_mode_t backend_mode_get(void);

/* Initiate a mode switch:
 *   1. Emit a pre-switch hint over the currently active channel.
 *   2. Write `new_mode` to NVS.
 *   3. esp_restart() (this function does NOT return on success).
 *
 * Returns ESP_ERR_INVALID_ARG for an unknown mode code, ESP_OK is never
 * actually observed because of the reboot. NVS write failures are
 * logged but the reboot still happens — the boot path will fall back
 * to MQTT (default) and announce that.
 *
 * Authority: this MUST only be called from the OLED+2-button gate (or
 * equivalent owner-authority trust anchor in Phase 2). See ADR-0012 §3. */
esp_err_t backend_mode_request_switch(wups_backend_mode_t new_mode);

/* Erase the entire writable `nvs` partition (mode flag, Arkiv owner
 * binding, freshness counters, ...) and reboot. The provisioning-only
 * `prov` partition (per-device key/secret) is left intact — factory
 * reset returns the device to its first-boot, ready-to-claim state,
 * not to un-provisioned. Same authority anchor as the mode switch. */
void backend_mode_factory_reset(void);

/* Helper for the post-switch confirm — called once at startup AFTER the
 * mode-specific subsystems have come up, if NVS indicates we just
 * rebooted from a mode switch. Best-effort; not delivered for HTTP. */
void backend_mode_emit_post_switch_confirm(void);

/* Short human label for logs / serial output. Stable across versions. */
const char *backend_mode_name(wups_backend_mode_t mode);

#ifdef __cplusplus
}
#endif
