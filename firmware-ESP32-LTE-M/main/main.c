/*
 * Web3 Pi UPS — LTE-M firmware (ESP32-S3 + SimCom SIM7080G).
 *
 * Stage 0 skeleton: only a boot banner + heartbeat so we have a known-good
 * sign of life on the serial log while we wire up the rest of the system.
 *
 * Planned next stages, in order:
 *   1. PMU init (AXP2101 over I²C) — DC3 (modem), BLDO1 (level shifter), TS-pin off.
 *   2. Modem power-on sequence — pulse PWRKEY (GPIO41), bring up UART1 @ 115200.
 *   3. AT pass-through bridge — USB-CDC ⇄ UART1 for manual command exploration.
 *   4. PPP via esp_modem against SIM7080G; integration with esp_netif.
 *   5. MQTT (esp-mqtt) over PPP.
 *   6. Arkiv integration on the same TCP/IP stack.
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
#include "sdkconfig.h"

static const char *TAG = "app";

#define HEARTBEAT_PERIOD_MS 5000

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

    ESP_LOGI(TAG, "===== Web3 Pi UPS — LTE-M firmware (stage 0) =====");
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
    log_boot_banner();

    uint32_t tick = 0;
    while (true) {
        int64_t uptime_us = esp_timer_get_time();
        uint32_t uptime_s = (uint32_t)(uptime_us / 1000000);
        uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
        uint32_t min_heap = (uint32_t)esp_get_minimum_free_heap_size();

        ESP_LOGI(TAG,
                 "tick=%" PRIu32 " uptime=%" PRIu32 "s free_heap=%" PRIu32 "B min_heap=%" PRIu32 "B",
                 tick, uptime_s, free_heap, min_heap);

        tick++;
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}
