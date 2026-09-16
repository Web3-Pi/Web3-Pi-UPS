#ifndef MODEM_UART_BAUD_H
#define MODEM_UART_BAUD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/uart.h"

typedef struct {
    uart_port_t port;
    int tx_gpio;
    int rx_gpio;
} modem_uart_baud_config_t;

/* Own a temporary UART driver; call only before esp_modem/CMUX/PPP owns it.
 * Probe 115200/230400, plus 921600 to return from the prior experiment.
 * DATA escapes use two 1.1 s guards
 * per rate initially, repeated halfway for windows >= 10 s. No power cycling and
 * no IPR writes. A silent probe does not prove the modem is powered off: raw
 * AT cannot recover an existing CMUX session. */
bool modem_uart_baud_probe(const modem_uart_baud_config_t *config,
                           uint32_t window_ms);

/* Detect, inspect capabilities, migrate if needed and verify the target.
 * Supported targets: 115200 (manual restore) and 230400. IPR is AUTO_SAVE.
 * On migration failure,
 * try to restore the previously detected explicit rate, then return false.
 * The caller MUST NOT start esp_modem/CMUX/PPP after false. */
bool modem_uart_baud_prepare(const modem_uart_baud_config_t *config,
                             int target_baud);

/* Strict pure parsers for complete, successful AT responses (host-testable).
 * IPR support accepts only parenthesized comma-separated integer lists;
 * empty groups are allowed (SIMCom reports an empty autobaud group). */
bool modem_uart_baud_parse_ipr(const char *response, size_t length, int *baud);
bool modem_uart_baud_supports_ipr(const char *response, size_t length, int target);

#endif
