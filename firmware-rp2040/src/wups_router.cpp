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

/* Reset after byte `b` FAILED a check — `b` must be re-evaluated as a SYNC1
 * candidate, never discarded. END2 == SYNC1 == 0xAA: a deframer that lost
 * sync mid-frame scans that frame's tail, takes END2 for SYNC1, moves to
 * SYNC2 and then meets the REAL SYNC1 of the next frame. A blind reset
 * throws it away, the real SYNC2 is skipped in SYNC1 state, the whole next
 * frame is lost and the collision repeats at its end — one frame behind
 * forever (2026-09-07 relay blackout on the ESP32 side; same state machine
 * here). Keeping 0xAA as the candidate re-locks on the very next SYNC2. */
static inline void rx_reset_with(PortRxState& s, uint8_t b)
{
    s.state = (b == WUPS_SYNC1) ? WUPS_S_SYNC2 : WUPS_S_SYNC1;
}

/* Inter-frame silence while mid-frame: a frame is <= 254 B = 2.8 ms at
 * 921600 (USB-CDC delivers a frame within a few ms too), so 50 ms without
 * a byte inside a frame means it will never complete (peer reset, bytes
 * lost). Reset so the next real SYNC pair is matched from SYNC1. */
static const uint32_t WUPS_RX_IDLE_RESET_MS = 50;
static uint32_t port_last_rx_ms[WUPS_PORT_COUNT] = { 0, 0, 0, 0 };

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
    /* Trailer + one inter-frame guard byte (protocol.h WUPS_GUARD_BYTE). A
     * peer still running the pre-2026-09 deframer and stuck one frame behind
     * burns its bogus SYNC2 check on the guard instead of on our real SYNC1
     * and re-locks on this very frame. Matters most for the CH32X, which
     * cannot be updated remotely; harmless for every receiver (a byte that
     * is neither 0xAA nor 0x55 is ignored while hunting for SYNC). */
    uint8_t trailer[5] = { a, b, WUPS_END1, WUPS_END2, WUPS_GUARD_BYTE };

    Stream& s = *port_streams[port];
    s.write(header, 10);
    if (payload_len) s.write((const uint8_t*)payload, payload_len);
    s.write(trailer, sizeof(trailer));

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

    /* Command RESP bridge: a HOST command RESP coming UP from the RPi agent
     * (service start/stop/restart, os.reboot/shutdown) has dst = the REQ's src
     * (RPI), so the unicast rule below would route it back out the RPI port and
     * drop it (out_port == inbound_port). Hand it to the local handler, which
     * republishes it to MQTT cmd/response so the panel confirms the command
     * instead of timing out. */
    if (inbound_port == WUPS_PORT_RPI && (f.flags & WUPS_FLAG_RESP) &&
        f.cls == WUPS_CLASS_HOST)
    {
        wups_on_local_frame(inbound_port, f);
        return;
    }

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
            rx_reset_with(s, b);    /* 0xAA here may be the real SYNC1 */
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
        if (s.len > WUPS_MAX_PAYLOAD) { rx_reset_with(s, b); break; }
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
            rx_reset_with(s, b);
        }
        break;
    case WUPS_S_END1:
        if (b == WUPS_END1) s.state = WUPS_S_END2;
        else                rx_reset_with(s, b);
        break;
    case WUPS_S_END2:
        if (b == WUPS_END2)
        {
            deliver_frame(inbound_port, s);
            rx_reset(s);
        }
        else
        {
            rx_reset_with(s, b);
        }
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
        int budget = 1024; /* must cover a full mains-transition burst from the
                            * CH32X (event+status+diag logs) within one loop
                            * pass — a lagging drain lets the SW ring hit its
                            * full-boundary corruption bug (2026-07-11). Still
                            * bounded so one port can't starve the others. */
        const uint32_t now_ms = millis();
        bool got_bytes = false;
        while (budget-- > 0 && s->available())
        {
            rx_byte(p, port_rx[p], (uint8_t)s->read());
            got_bytes = true;
        }
        if (got_bytes)
        {
            port_last_rx_ms[p] = now_ms;
        }
        else if (port_rx[p].state != WUPS_S_SYNC1 &&
                 (uint32_t)(now_ms - port_last_rx_ms[p]) > WUPS_RX_IDLE_RESET_MS)
        {
            rx_reset(port_rx[p]);   /* stale partial frame — see WUPS_RX_IDLE_RESET_MS */
        }
    }
}
