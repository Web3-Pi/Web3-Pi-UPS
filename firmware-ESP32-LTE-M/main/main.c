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

#include "modem.h"
#include "pmu.h"
#include "wups_link.h"

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

    /* esp_netif and esp_modem need NVS for runtime state (DNS, etc). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_LOGI(TAG, "boot banner printed, calling pmu_init...");

    /* PMU brings up modem rails (DC3 = 3.0 V, BLDO1 = 3.3 V level shifter). */
    ESP_ERROR_CHECK(pmu_init());
    ESP_LOGI(TAG, "pmu_init returned, settling rails...");

    /* Let the rails settle before we reach for PWRKEY. */
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "calling modem_init...");

    /* Configure GPIO + UART, then pulse PWRKEY. */
    ESP_ERROR_CHECK(modem_init());
    ESP_LOGI(TAG, "calling modem_power_on...");
    ESP_ERROR_CHECK(modem_power_on());

    /* Wait for the modem to finish booting before we start watching the UART. */
    ESP_LOGI(TAG, "waiting %d ms for modem boot...", MODEM_BOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_DELAY_MS));

    /* Bring up the binary protocol link to RP2040 (UART2 with HW flow control,
     * MQTT data → net.downlink hook). Done before PPP/MQTT so RP2040 can
     * already start talking to us; net.publish from any caller will simply
     * fail until MQTT connects. */
    ESP_ERROR_CHECK(wups_link_init());

    /* Hand the UART over to the bidirectional bridge. */
    modem_at_pass_through_start();

    /* Keep emitting a heartbeat so the host sees the firmware is still alive
     * even when no AT traffic is happening. */
    uint32_t tick = 0;
    while (true) {
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
