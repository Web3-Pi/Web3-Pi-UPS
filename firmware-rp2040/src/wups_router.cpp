/*
 * RP2040 binary wire-protocol router. See wups_router.h for the
 * overall design. This file implements the per-port byte-by-byte
 * deframer state machine, the routing rules, and the encode helpers.
 */
#include "wups_router.h"
#include <string.h>

/* --- module state --------------------------------------------------- */

static Stream* port_streams[WUPS_PORT_COUNT] = { nullptr, nullptr, nullptr, nullptr };
static uint8_t addr_to_port[256];

static uint8_t tx_seq = 0;

volatile uint32_t Wups_Frames_Rx[WUPS_PORT_COUNT]        = { 0, 0, 0, 0 };
volatile uint32_t Wups_Frames_Tx[WUPS_PORT_COUNT]        = { 0, 0, 0, 0 };
volatile uint32_t Wups_Frames_Forwarded[WUPS_PORT_COUNT] = { 0, 0, 0, 0 };

/* --- per-port deframer --------------------------------------------- */

enum WupsRxState : uint8_t {
    WUPS_S_SYNC1 = 0,
    WUPS_S_SYNC2,
    WUPS_S_DST,
    WUPS_S_SRC,
    WUPS_S_CLASS,
    WUPS_S_OP,
    WUPS_S_FLAGS,
    WUPS_S_SEQ,
    WUPS_S_LEN_L,
    WUPS_S_LEN_H,
    WUPS_S_PAYLOAD,
    WUPS_S_CK_A,
    WUPS_S_CK_B,
    WUPS_S_END1,
    WUPS_S_END2,
};

struct PortRxState {
    WupsRxState state = WUPS_S_SYNC1;
    uint8_t  dst, src, cls, op, flags, seq;
    uint16_t len;
    uint16_t pidx;
    uint8_t  payload[WUPS_MAX_PAYLOAD];
    uint8_t  rx_ck_a;
    uint8_t  exp_a, exp_b;
};

static PortRxState port_rx[WUPS_PORT_COUNT];

static inline void rx_step(PortRxState& s, uint8_t b)
{
    s.exp_a = (uint8_t)(s.exp_a + b);
    s.exp_b = (uint8_t)(s.exp_b + s.exp_a);
}

static inline void rx_reset(PortRxState& s) { s.state = WUPS_S_SYNC1; }

/* --- encode / emit -------------------------------------------------- */

void wups_send_with_src(uint8_t port, uint8_t dst, uint8_t src,
                        uint8_t cls, uint8_t op, uint8_t flags, uint8_t seq,
                        const void* payload, uint16_t payload_len)
{
    if (port == WUPS_PORT_NONE || port >= WUPS_PORT_COUNT) return;
    if (!port_streams[port])                              return;
    if (payload_len > WUPS_MAX_PAYLOAD)                   return;

    uint8_t header[10];
    header[0] = WUPS_SYNC1;
    header[1] = WUPS_SYNC2;
    header[2] = dst;
    header[3] = src;
    header[4] = cls;
    header[5] = op;
    header[6] = flags;
    header[7] = seq;
    header[8] = (uint8_t)(payload_len & 0xFFu);
    header[9] = (uint8_t)((payload_len >> 8) & 0xFFu);

    /* Fletcher-8 over DST..LEN_H..payload. SYNC and end marker excluded. */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 10; ++i) { a = (uint8_t)(a + header[i]); b = (uint8_t)(b + a); }
    const uint8_t* p = (const uint8_t*)payload;
    for (uint16_t i = 0; i < payload_len; ++i) { a = (uint8_t)(a + p[i]); b = (uint8_t)(b + a); }
    uint8_t trailer[4] = { a, b, WUPS_END1, WUPS_END2 };

    Stream& s = *port_streams[port];
    s.write(header, 10);
    if (payload_len) s.write((const uint8_t*)payload, payload_len);
    s.write(trailer, 4);

    Wups_Frames_Tx[port]++;
}

void wups_send_seq(uint8_t port, uint8_t dst, uint8_t cls, uint8_t op,
                   uint8_t flags, uint8_t seq,
                   const void* payload, uint16_t payload_len)
{
    wups_send_with_src(port, dst, WUPS_ADDR_RP2040, cls, op, flags, seq,
                       payload, payload_len);
}

uint8_t wups_send(uint8_t port, uint8_t dst, uint8_t cls, uint8_t op,
                  uint8_t flags, const void* payload, uint16_t payload_len)
{
    uint8_t seq = tx_seq++;
    wups_send_with_src(port, dst, WUPS_ADDR_RP2040, cls, op, flags, seq,
                       payload, payload_len);
    return seq;
}

/* --- routing -------------------------------------------------------- */

static void emit_forward(uint8_t out_port, const WupsFrame& f)
{
    if (out_port == WUPS_PORT_NONE || out_port >= WUPS_PORT_COUNT) return;
    if (!port_streams[out_port]) return;
    wups_send_with_src(out_port, f.dst, f.src, f.cls, f.op, f.flags, f.seq,
                       f.payload, f.len);
    Wups_Frames_Forwarded[out_port]++;
}

void wups_route_frame(uint8_t inbound_port, const WupsFrame& f)
{
    bool to_self     = (f.dst == WUPS_ADDR_RP2040);
    bool is_bcast    = (f.dst == WUPS_ADDR_BROADCAST);
    bool is_internal = (f.dst == WUPS_ADDR_INTERNAL);

    if (to_self || is_bcast || is_internal)
    {
        wups_on_local_frame(inbound_port, f);
    }

    if (is_bcast)
    {
        for (uint8_t p = 1; p < WUPS_PORT_COUNT; ++p)
        {
            if (p != inbound_port) emit_forward(p, f);
        }
        return;
    }

    if (is_internal)
    {
        /* INTERNAL = MCU-only multicast. RPi (port 1) is excluded. */
        for (uint8_t p = 1; p < WUPS_PORT_COUNT; ++p)
        {
            if (p == WUPS_PORT_RPI)   continue;
            if (p == inbound_port)    continue;
            emit_forward(p, f);
        }
        return;
    }

    if (to_self) return;

    uint8_t out_port = addr_to_port[f.dst];
    if (out_port != WUPS_PORT_NONE && out_port != inbound_port)
    {
        emit_forward(out_port, f);
    }
}

/* --- byte-level RX -------------------------------------------------- */

static void deliver_frame(uint8_t inbound_port, const PortRxState& s)
{
    WupsFrame f;
    f.dst   = s.dst;
    f.src   = s.src;
    f.cls   = s.cls;
    f.op    = s.op;
    f.flags = s.flags;
    f.seq   = s.seq;
    f.len   = s.len;
    if (s.len) memcpy(f.payload, s.payload, s.len);
    Wups_Frames_Rx[inbound_port]++;
    wups_route_frame(inbound_port, f);
}

static void rx_byte(uint8_t inbound_port, PortRxState& s, uint8_t b)
{
    switch (s.state)
    {
    case WUPS_S_SYNC1:
        if (b == WUPS_SYNC1) s.state = WUPS_S_SYNC2;
        break;
    case WUPS_S_SYNC2:
        if (b == WUPS_SYNC2)
        {
            s.exp_a = 0;
            s.exp_b = 0;
            s.pidx  = 0;
            s.state = WUPS_S_DST;
        }
        else
        {
            rx_reset(s);
        }
        break;
    case WUPS_S_DST:    s.dst   = b; rx_step(s, b); s.state = WUPS_S_SRC;   break;
    case WUPS_S_SRC:    s.src   = b; rx_step(s, b); s.state = WUPS_S_CLASS; break;
    case WUPS_S_CLASS:  s.cls   = b; rx_step(s, b); s.state = WUPS_S_OP;    break;
    case WUPS_S_OP:     s.op    = b; rx_step(s, b); s.state = WUPS_S_FLAGS; break;
    case WUPS_S_FLAGS:  s.flags = b; rx_step(s, b); s.state = WUPS_S_SEQ;   break;
    case WUPS_S_SEQ:    s.seq   = b; rx_step(s, b); s.state = WUPS_S_LEN_L; break;
    case WUPS_S_LEN_L:
        s.len = b;
        rx_step(s, b);
        s.state = WUPS_S_LEN_H;
        break;
    case WUPS_S_LEN_H:
        s.len |= (uint16_t)((uint16_t)b << 8);
        rx_step(s, b);
        if (s.len > WUPS_MAX_PAYLOAD) { rx_reset(s); break; }
        s.state = (s.len == 0) ? WUPS_S_CK_A : WUPS_S_PAYLOAD;
        break;
    case WUPS_S_PAYLOAD:
        s.payload[s.pidx++] = b;
        rx_step(s, b);
        if (s.pidx >= s.len) s.state = WUPS_S_CK_A;
        break;
    case WUPS_S_CK_A:
        s.rx_ck_a = b;
        s.state = WUPS_S_CK_B;
        break;
    case WUPS_S_CK_B:
        if (s.rx_ck_a == s.exp_a && b == s.exp_b)
        {
            s.state = WUPS_S_END1;
        }
        else
        {
            rx_reset(s);
        }
        break;
    case WUPS_S_END1:
        s.state = (b == WUPS_END1) ? WUPS_S_END2 : WUPS_S_SYNC1;
        break;
    case WUPS_S_END2:
        if (b == WUPS_END2)
        {
            deliver_frame(inbound_port, s);
        }
        rx_reset(s);
        break;
    default:
        rx_reset(s);
        break;
    }
}

/* --- public entry points ------------------------------------------- */

void wups_router_init(Stream* usbcdc, Stream* uart_ch32x, Stream* uart_esp32)
{
    port_streams[WUPS_PORT_NONE]  = nullptr;
    port_streams[WUPS_PORT_RPI]   = usbcdc;
    port_streams[WUPS_PORT_CH32X] = uart_ch32x;
    port_streams[WUPS_PORT_ESP32] = uart_esp32;

    for (int i = 0; i < 256; ++i) addr_to_port[i] = WUPS_PORT_NONE;
    addr_to_port[WUPS_ADDR_RPI]   = WUPS_PORT_RPI;
    addr_to_port[WUPS_ADDR_CH32X] = WUPS_PORT_CH32X;
    addr_to_port[WUPS_ADDR_ESP32] = WUPS_PORT_ESP32;

    for (int p = 0; p < WUPS_PORT_COUNT; ++p) rx_reset(port_rx[p]);
}

void wups_router_drain(void)
{
    for (uint8_t p = 1; p < WUPS_PORT_COUNT; ++p)
    {
        Stream* s = port_streams[p];
        if (!s) continue;
        int budget = 256;  /* don't starve other ports if one floods */
        while (budget-- > 0 && s->available())
        {
            rx_byte(p, port_rx[p], (uint8_t)s->read());
        }
    }
}
