/*
 * Web3 Pi UPS — LTE-M firmware (ESP32-S3 + SimCom SIM7080G).
 *
 * Stage 1 — modem brought up at AT level:
 *   1. PMU init: AXP2101 over I²C → DC3 (3.0 V), BLDO1 (3.3 V), TS-pin off.
 *   2. Modem power-on: pulse PWRKEY (GPIO41), bring up UART1 at 115200 on 4/5.
 *   3. AT pass-through bridge: USB-CDC ⇄ UART1 for hands-on AT exploration.
 *
 * Coming next:
 *   4. PPP via esp_modem (gives us esp_netif + lwIP).
 *   5. MQTT (esp-mqtt) over PPP.
 *   6. Arkiv on the same TCP/IP stack.
 *
 * Hardware: LilyGo T-SIM7080G-S3 dev board. See docs/info.md for pinout,
 * power domains, and other details cribbed from the board datasheet.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "identity.h"
#include "cmdauth.h"
#include "cmdauth_arkiv.h"
#include "arkiv_rpc.h"
#include "arkiv_claim.h"
#include "arkiv_writer.h"
#include "arkiv_tlm.h"
#include "arkiv_ws.h"
#include "backend_mode.h"
#include "fw_ota.h"
#include "modem.h"
#include "pmu.h"
#include "wups_link.h"
#include "arkiv_crypto_selftest.h"

static const char *TAG = "app";

#define HEARTBEAT_PERIOD_MS  5000
#define MODEM_BOOT_DELAY_MS  8000

static void log_boot_banner(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "===== Web3 Pi UPS — LTE-M firmware (stage 1) =====");
    ESP_LOGI(TAG, "target=%s cores=%d rev=v%u.%u",
             CONFIG_IDF_TARGET,
             info.cores,
             (unsigned)(info.revision / 100),
             (unsigned)(info.revision % 100));
    ESP_LOGI(TAG, "features=%s%s%s",
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (info.features & CHIP_FEATURE_BT) ? "BT " : "",
             (info.features & CHIP_FEATURE_BLE) ? "BLE " : "");
    ESP_LOGI(TAG, "flash=%" PRIu32 " MB %s",
             flash_size / (1024U * 1024U),
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "(embedded)" : "(external)");
    ESP_LOGI(TAG, "mac=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "idf=%s", esp_get_idf_version());
}

void app_main(void)
{
    ESP_LOGI(TAG, "app_main entered");
    log_boot_banner();

    /* OTA-1 — log which OTA slot we run from (visible proof on serial that
     * an update actually switched slots) and latch pending-verify state for
     * the rollback machinery (see fw_ota.h). */
    fw_ota_boot_log();

#if ARKIV_CRYPTO_SELFTEST
    /* ADR-0013: confirm the on-device crypto path (mbedtls + HW AES) is
     * byte-identical to the host/JS golden vectors before trusting it to seal
     * telemetry onto the immutable ledger. */
    (void)arkiv_crypto_selftest();
#endif

    /* esp_netif and esp_modem need NVS for runtime state (DNS, etc). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* ADR-0012 — read the persisted backend mode BEFORE any mode-specific
     * subsystem decides to start. Stays stable for the lifetime of the
     * boot; changes require a reboot via backend_mode_request_switch(). */
    (void)backend_mode_init();
    const wups_backend_mode_t active_mode = backend_mode_get();
    ESP_LOGI(TAG, "active backend mode: %s", backend_mode_name(active_mode));

    /* Load the per-device MQTT secret from the `prov` NVS partition before
     * anything needs the MQTT password (Track 0 / WS-10). Aborts boot if the
     * unit was never provisioned — by design, no fleet-wide fallback. */
    ESP_ERROR_CHECK(identity_init());

    /* WS-9 / ADR-0009: load backend command-signing pubkey + freshness
     * state. NOT fatal if absent — telemetry/identify must still work; a
     * device that isn't WS-9-provisioned simply rejects every command
     * (fail-closed) rather than bricking. */
    if (cmdauth_init() != ESP_OK) {
        ESP_LOGW(TAG, "cmdauth_init failed — all downlink commands will be "
                      "rejected until the device is WS-9 provisioned");
    }

    /* Track 2 / ADR-0011: Paranoic (Arkiv) authority. Also non-fatal — a
     * device with no Arkiv provisioning / no owner bound just can't run
     * Paranoic mode; Default/MQTT is unaffected. Fail-closed. */
    if (cmdauth_arkiv_init() != ESP_OK) {
        ESP_LOGW(TAG, "cmdauth_arkiv_init: Paranoic mode unavailable "
                      "(device not Arkiv-provisioned)");
    }

    /* Track 2 / ADR-0011 P4: device-side writer. Loads ak_dev_priv from
     * prov NVS, derives + LOGS the device's Braga wallet address (the
     * line operators need to fund the wallet with GLM gas). Independent
     * of owner-binding state — telemetry/ACK/event paths gate themselves
     * on owner_bound later. */
    if (arkiv_writer_init() != ESP_OK) {
        ESP_LOGW(TAG, "arkiv_writer_init: Paranoic writer unavailable "
                      "(device key absent)");
    }

    ESP_LOGI(TAG, "boot banner printed, calling pmu_init...");

    /* PMU brings up modem rails (DC3 = 3.0 V, BLDO1 = 3.3 V level shifter). */
    ESP_ERROR_CHECK(pmu_init());
    ESP_LOGI(TAG, "pmu_init returned, settling rails...");

    /* Let the rails settle before we reach for PWRKEY. */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "calling modem_init...");

    /* Configure the PWRKEY GPIO. */
    ESP_ERROR_CHECK(modem_init());

    /* Power the modem on ONLY if it isn't already running — the modem keeps its
     * own VBAT across ESP resets, so a blind PWRKEY pulse would toggle a healthy
     * modem OFF. modem_ensure_on() probes AT first and pulses PWRKEY + waits for
     * boot only when the modem is silent. */
    ESP_LOGI(TAG, "ensuring modem is powered...");
    modem_ensure_on();

    /* Bring up the binary protocol link to RP2040 (UART2 with HW flow control,
     * MQTT data → net.downlink hook). Done before PPP/MQTT so RP2040 can
     * already start talking to us; net.publish from any caller will simply
     * fail until MQTT connects. */
    ESP_ERROR_CHECK(wups_link_init());

    /* Hand the UART over to the bidirectional bridge. */
    modem_at_pass_through_start();

    /* ADR-0012 — Arkiv subsystems only when we're actually in Arkiv mode.
     * Before this gate they ran in every mode (fail-closed when not
     * Arkiv-provisioned) but that wasted heap + a constant poll-task wake
     * loop on every device. Switching modes is a reboot, so this gate
     * stays accurate for the lifetime of the boot. */
    if (active_mode == WUPS_BACKEND_MODE_ARKIV) {
        /* Track 2 / ADR-0011 — Arkiv command poll task. */
        arkiv_poll_start();
        /* Owner-binding driver — shows claim-code, polls w3pups-claim,
         * drives the OLED trust anchor (§10.1/§10.4 path B). */
        arkiv_claim_start();
        /* Periodic Paranoic telemetry emitter (P4 §4.6). */
        arkiv_tlm_start();
    } else {
        ESP_LOGI(TAG, "Arkiv subsystems skipped — not in Arkiv mode");
    }

    /* Track 2 / ADR-0011 — Arkiv WS subscriber (cmd channel). We can't
     * start it before the owner is bound (we need the owner address as
     * the eth_subscribe topic filter), and we can't start before PPP+TLS
     * are up either. The simplest correct gate is to attempt the start
     * lazily from the heartbeat loop: arkiv_ws_start is idempotent, and
     * the WS client's own auto-reconnect handles transient network drops
     * once it's running. Replaces the 5 s HTTP cmd-poll path; the legacy
     * `arkiv_poll` task stays alive at a 5-minute fallback cadence (see
     * `arkiv_rpc.c poll_task` and web3pi_scope/notes/ARKIV-data-usage.md §E). */
    bool ws_armed = false;
    /* `mode_confirm_armed` flips false on the first successful post-switch
     * confirm emit. emit_post_switch_confirm is idempotent (reads NVS,
     * only does work if prev_mode is set, clears it after success), so
     * calling it repeatedly while the channel comes up is safe. */
    bool mode_confirm_armed = true;

    /* Keep emitting a heartbeat so the host sees the firmware is still alive
     * even when no AT traffic is happening. */
    uint32_t tick = 0;
    while (true) {
        if (active_mode == WUPS_BACKEND_MODE_ARKIV && !ws_armed &&
            cmdauth_arkiv_ready() &&
            cmdauth_arkiv_claim_state() == ARKIV_CLAIMED) {
            const uint8_t *owner = cmdauth_arkiv_owner_addr();
            if (owner && arkiv_ws_start(owner) == ESP_OK) {
                ws_armed = true;
            }
        }

        /* ADR-0012 — post-switch confirm. emit_post_switch_confirm is a
         * no-op if NVS prev_mode isn't set (i.e. this isn't a post-switch
         * boot). When it IS set, the helper publishes once via the now-up
         * channel and clears the NVS marker; subsequent calls become
         * no-ops. We keep trying on every heartbeat tick until success. */
        if (mode_confirm_armed) {
            backend_mode_emit_post_switch_confirm();
            /* The helper is idempotent: if prev_mode is unset (or cleared
             * by the successful publish above) further calls do nothing.
             * Disarm to avoid the NVS open on every tick once we're past
             * the first ~minute of boot. */
            if (tick > 12) mode_confirm_armed = false;
        }

        /* OTA-1 rollback safety net: a pending-verify image that never got
         * a healthy uplink rolls itself back after 10 min. No-op otherwise.
         * Lives on the heartbeat (not the modem task) so a wedged modem
         * bring-up can't starve it. */
        fw_ota_rollback_tick();

        /* fw_xfer receiver idle guard: a Workbench push that died mid-
         * transfer must not hold the update slot (and the modem-watchdog
         * freeze) forever — drop the session after 30 s of silence. */
        fw_ota_xfer_tick();

        int64_t uptime_us = esp_timer_get_time();
        uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);
        uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
        uint32_t min_heap = (uint32_t)esp_get_minimum_free_heap_size();

        ESP_LOGI(TAG,
                 "tick=%" PRIu32 " uptime=%" PRIu32 "s free_heap=%" PRIu32 "B min_heap=%" PRIu32 "B",
                 tick, uptime_s, free_heap, min_heap);

        wups_link_log_stats();

        tick++;
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}
