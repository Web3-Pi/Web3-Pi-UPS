#pragma once

#include "esp_err.h"

/*
 * SIM7080G modem control: power-on sequence + UART setup + PPP supervisor.
 *
 * Pin map on the W3P MODEM V1 M.2 card (ESP32-S3FH4R2, verified vs schematic):
 *
 *   GPIO1 — PWRKEY (drives modem PWRKEY low through inverting transistor Q501:
 *                   GPIO HIGH = PWRKEY pressed, GPIO LOW = released)
 *   GPIO2 — UART TX (ESP32 TX → Modem RX, via TXB0108 level translator)
 *   GPIO4 — UART RX (ESP32 RX ← Modem TX, via TXB0108)
 *   GPIO5 — DTR    (routed, not driven in SW)
 *   GPIO6 — RI     (routed, not driven in SW)
 *
 * No PMU on this card (modem VBAT = always-on PP3800_SYS buck); pmu_init() is
 * a no-op. NB: the modem's VBAT is independent of the ESP, so it stays powered
 * across ESP resets — see modem_ensure_on().
 */

esp_err_t modem_init(void);

/* Pulse PWRKEY for ~1 second to power the modem on. After return, give the
 * modem ~7 seconds to finish booting before sending AT commands. */
esp_err_t modem_power_on(void);

/* Ensure the modem is powered on WITHOUT toggling an already-running one.
 * The modem keeps its own VBAT across ESP resets, so a blind PWRKEY pulse can
 * switch a healthy modem OFF. This probes AT on a temporary raw UART first and
 * only pulses PWRKEY (then waits for boot) if the modem is silent. */
void modem_ensure_on(void);

/* Spawn two FreeRTOS tasks that bridge USB-CDC stdio ↔ modem UART:
 *   - bytes typed on the host console go to the modem
 *   - bytes from the modem go to the console (and the serial monitor's log)
 * After this returns, AT commands can be sent interactively. */
void modem_at_pass_through_start(void);
