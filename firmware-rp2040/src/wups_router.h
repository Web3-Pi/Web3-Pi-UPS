/*
 * RP2040 binary wire-protocol router.
 *
 * RP2040 is the hub of the four-node star (RPi, RP2040, CH32X, ESP32).
 * Every other node is connected by a single point-to-point link, so
 * routing is just "look up DST, write on the matching port". We hold
 * one deframer state per port and a tiny address→port table.
 *
 * Port indices are local to this module — they happen to mirror the
 * `WUPS_ADDR_*` values for the corresponding remote node, but the
 * indirection is kept so the protocol address space and the physical
 * port identifier remain conceptually separate.
 */
#ifndef WUPS_ROUTER_H
#define WUPS_ROUTER_H

#include <Arduino.h>
#include "wups_proto.h"

constexpr uint8_t WUPS_PORT_NONE  = 0;
constexpr uint8_t WUPS_PORT_RPI   = 1;  /* USB-CDC (Serial)    */
constexpr uint8_t WUPS_PORT_CH32X = 2;  /* UART0   (Serial1)   */
constexpr uint8_t WUPS_PORT_ESP32 = 3;  /* UART1   (Serial2)   */

constexpr uint8_t WUPS_PORT_COUNT = 4;  /* indices 0..3, 0 unused */

struct WupsFrame {
    uint8_t  dst;
    uint8_t  src;
    uint8_t  cls;
    uint8_t  op;
    uint8_t  flags;
    uint8_t  seq;
    uint16_t len;
    uint8_t  payload[WUPS_MAX_PAYLOAD];
};

/* Wire up the three streams. NULL is allowed for any port that is not
 * physically present yet — frames addressed to a missing port are
 * dropped silently. */
void wups_router_init(Stream* usbcdc, Stream* uart_ch32x, Stream* uart_esp32);

/* Pull bytes from every port and feed each port's deframer. Call once
 * per main loop iteration. */
void wups_router_drain(void);

/* Build and emit a single frame on `port`. SRC is set to RP2040.
 * Returns the SEQ value used (caller may want it for correlation). */
uint8_t wups_send(uint8_t port, uint8_t dst, uint8_t cls, uint8_t op,
                  uint8_t flags, const void* payload, uint16_t payload_len);

/* Same as wups_send but with explicit SEQ — useful for RESPs. */
void wups_send_seq(uint8_t port, uint8_t dst, uint8_t cls, uint8_t op,
                   uint8_t flags, uint8_t seq,
                   const void* payload, uint16_t payload_len);

/* Same as wups_send but with explicit SRC — used by the router itself
 * when forwarding a frame on behalf of another node. Application code
 * should normally use wups_send (which sets SRC=RP2040). */
void wups_send_with_src(uint8_t port, uint8_t dst, uint8_t src,
                        uint8_t cls, uint8_t op, uint8_t flags, uint8_t seq,
                        const void* payload, uint16_t payload_len);

/* Application-defined: invoked when a frame is delivered locally
 * (DST is self, broadcast, or internal). The router has already
 * handled retransmission for broadcast/internal — the callback is
 * for application-level handling only. */
extern void wups_on_local_frame(uint8_t inbound_port, const WupsFrame& f);

/* Diagnostic counters — read via SWD or system.log to confirm
 * the bus is alive without a logic analyzer. */
extern volatile uint32_t Wups_Frames_Rx[WUPS_PORT_COUNT];
extern volatile uint32_t Wups_Frames_Tx[WUPS_PORT_COUNT];
extern volatile uint32_t Wups_Frames_Forwarded[WUPS_PORT_COUNT];

#endif /* WUPS_ROUTER_H */
