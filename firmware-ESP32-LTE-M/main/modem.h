#pragma once

#include "esp_err.h"

/*
 * SIM7080G modem control: power-on sequence + UART setup + USB-CDC
 * pass-through bridge for hands-on AT command exploration.
 *
 * Pin map on the LilyGo T-SIM7080G-S3 board (from the official examples'
 * utilities.h, verified against the schematic):
 *
 *   GPIO41 — PWRKEY (drives modem PWRKEY low through inverting transistor:
 *                    GPIO HIGH = PWRKEY pressed, GPIO LOW = released)
 *   GPIO42 — DTR    (modem flow control / wake — unused initially)
 *   GPIO3  — RI     (ring indicator / wake-up signal — input, unused initially)
 *   GPIO4  — UART RX (ESP32 RX ← Modem TX)
 *   GPIO5  — UART TX (ESP32 TX → Modem RX)
 *
 * Required power rails (provided by pmu.c): DC3 = 3.0 V, BLDO1 = 3.3 V.
 * Make sure pmu_init() has run before calling modem_*().
 */

esp_err_t modem_init(void);

/* Pulse PWRKEY for ~1 second to power the modem on. After return, give the
 * modem ~5 seconds to finish booting before sending AT commands. */
esp_err_t modem_power_on(void);

/* Spawn two FreeRTOS tasks that bridge USB-CDC stdio ↔ modem UART:
 *   - bytes typed on the host console go to the modem
 *   - bytes from the modem go to the console (and the serial monitor's log)
 * After this returns, AT commands can be sent interactively. */
void modem_at_pass_through_start(void);
