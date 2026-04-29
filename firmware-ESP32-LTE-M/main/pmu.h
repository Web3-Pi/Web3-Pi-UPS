#pragma once

#include "esp_err.h"

/*
 * Minimal AXP2101 PMU driver for the LilyGo T-SIM7080G-S3 board.
 *
 * Only the rails we need on this firmware:
 *   DC3   — SIM7080G modem main supply (3.0 V)
 *   BLDO1 — level shifter between ESP32-S3 and SIM7080G (3.3 V)
 *           CRITICAL: never disable, otherwise UART to modem stops working.
 *
 * Plus disabling the TS-pin ADC measurement so charging can run on a board
 * without an NTC thermistor.
 *
 * Other rails (DC1=ESP32 core, ALDOx=camera, BLDO2=GPS) are left at their
 * boot defaults — we don't need them for the LTE-M card use case.
 */

esp_err_t pmu_init(void);
