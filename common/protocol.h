/*
 * Web3 Pi UPS — wire protocol v1
 *
 * Shared header for all four firmware images:
 *   - CH32X035        (firmware-ch32x)
 *   - RP2040          (firmware-rp2040, hub/router)
 *   - ESP32-S3        (firmware on M.2 LTE-M module)
 *   - RPi 5 host      (Web3-Pi-UPS-Service, mirrored in src/proto.rs)
 *
 * Wire format (UBX-style binary):
 *
 *   +------+------+-----+-----+-------+----+-------+-----+-------+--------+----+----+------+------+
 *   | 0xAA | 0x55 | DST | SRC | CLASS | OP | FLAGS | SEQ | LEN_L | LEN_H  |    payload     | CK_A |
 *   |      |      |     |     |       |    |       |     |       |        |   LEN bytes    | CK_B |
 *   +------+------+-----+-----+-------+----+-------+-----+-------+--------+----+----+------+------+
 *                                                                                          | 0x55 | 0xAA |
 *                                                                                          +------+------+
 *
 *   - SYNC bytes (0xAA 0x55) and end marker (0x55 0xAA) are NOT covered by the checksum.
 *   - Fletcher-8 (CK_A, CK_B) is computed over: DST, SRC, CLASS, OP, FLAGS, SEQ, LEN_L, LEN_H, payload[0..LEN-1].
 *   - LEN is little-endian u16. Max payload = WUPS_MAX_PAYLOAD bytes.
 *   - Total frame size = 14 + LEN bytes.
 *
 * All multi-byte fields are little-endian. All payload structs use packed attribute;
 * ALWAYS memcpy to/from the payload buffer (Cortex-M0/M0+ does not support unaligned
 * loads).
 */

#ifndef WEB3PI_UPS_PROTOCOL_H
#define WEB3PI_UPS_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WUPS_PROTO_VERSION       1u

#define WUPS_SYNC1               0xAAu
#define WUPS_SYNC2               0x55u
#define WUPS_END1                0x55u
#define WUPS_END2                0xAAu

#define WUPS_HEADER_BYTES        10u   /* SYNC1 SYNC2 DST SRC CLASS OP FLAGS SEQ LEN_L LEN_H */
#define WUPS_TRAILER_BYTES       4u    /* CK_A CK_B END1 END2 */
#define WUPS_FRAMING_BYTES       (WUPS_HEADER_BYTES + WUPS_TRAILER_BYTES)  /* 14 */

#define WUPS_MAX_PAYLOAD         240u
#define WUPS_MAX_FRAME           (WUPS_FRAMING_BYTES + WUPS_MAX_PAYLOAD)   /* 254 */

/* ---- Addresses ----------------------------------------------------------- */

typedef enum {
    WUPS_ADDR_NULL      = 0x00,
    WUPS_ADDR_RPI       = 0x01,
    WUPS_ADDR_RP2040    = 0x02,
    WUPS_ADDR_CH32X     = 0x03,
    WUPS_ADDR_ESP32     = 0x04,
    WUPS_ADDR_INTERNAL  = 0x05,  /* multicast to all MCUs, NOT to RPi */
    WUPS_ADDR_BROADCAST = 0xFF,  /* multicast to all nodes including RPi */
} wups_addr_t;

/* ---- Flags --------------------------------------------------------------- */

#define WUPS_FLAG_REQ            (1u << 0)  /* request, expects RESP */
#define WUPS_FLAG_RESP           (1u << 1)  /* response, SEQ echoes the REQ */
#define WUPS_FLAG_EVENT          (1u << 2)  /* unsolicited (periodic status OR change event) */
#define WUPS_FLAG_NEED_ACK       (1u << 7)  /* reserved for later — no implementation yet */

/* ---- Classes ------------------------------------------------------------- */

typedef enum {
    WUPS_CLASS_SYSTEM = 0x01,
    WUPS_CLASS_POWER  = 0x02,
    WUPS_CLASS_NET    = 0x03,
    WUPS_CLASS_HOST   = 0x04,
    WUPS_CLASS_UI     = 0x05,
} wups_class_t;

/* ---- Ops ----------------------------------------------------------------- */

/* Class 0x01 SYSTEM (every node implements) */
typedef enum {
    WUPS_OP_SYS_PING         = 0x01,
    WUPS_OP_SYS_HELLO        = 0x02,
    WUPS_OP_SYS_STATUS_QUERY = 0x03,
    WUPS_OP_SYS_LOG          = 0x04,
} wups_op_system_t;

/* Class 0x02 POWER (CH32X) */
typedef enum {
    WUPS_OP_PWR_STATUS        = 0x01,
    WUPS_OP_PWR_ENABLE        = 0x02,
    WUPS_OP_PWR_DISABLE       = 0x03,
    WUPS_OP_PWR_CYCLE         = 0x04,
    WUPS_OP_PWR_RESET         = 0x05,
    WUPS_OP_PWR_EVENT         = 0x10,
} wups_op_power_t;

/* Class 0x03 NET (ESP32) */
typedef enum {
    WUPS_OP_NET_STATUS        = 0x01,
    WUPS_OP_NET_PUBLISH       = 0x02,
    WUPS_OP_NET_DOWNLINK      = 0x10,
    WUPS_OP_NET_TIME_SYNC     = 0x20,
} wups_op_net_t;

/* Class 0x04 HOST (RPi) */
typedef enum {
    WUPS_OP_HOST_STATUS          = 0x01,
    WUPS_OP_HOST_SHUTDOWN        = 0x02,
    WUPS_OP_HOST_RESET           = 0x03,
    WUPS_OP_HOST_SERVICE_RESTART = 0x04,
    WUPS_OP_HOST_EVENT           = 0x10,
} wups_op_host_t;

/* Class 0x05 UI (RP2040) */
typedef enum {
    WUPS_OP_UI_BUTTON_EVENT   = 0x01,
    WUPS_OP_UI_SET_SCREEN     = 0x02,
    WUPS_OP_UI_BEEP           = 0x03,
    WUPS_OP_UI_DISPLAY_MSG    = 0x04,
} wups_op_ui_t;

/* ---- Capability bitmap (used in system.hello) ---------------------------- */

#define WUPS_CAP_SYSTEM   (1u << WUPS_CLASS_SYSTEM)
#define WUPS_CAP_POWER    (1u << WUPS_CLASS_POWER)
#define WUPS_CAP_NET      (1u << WUPS_CLASS_NET)
#define WUPS_CAP_HOST     (1u << WUPS_CLASS_HOST)
#define WUPS_CAP_UI       (1u << WUPS_CLASS_UI)

/* ---- Payload structs ----------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#  define WUPS_PACKED __attribute__((packed))
#else
#  define WUPS_PACKED
#endif

/* system.ping (REQ empty; RESP carries uptime+fw_version) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  reserved;
    uint16_t fw_version;     /* (major << 8) | minor */
    uint32_t uptime_ms;
} wups_sys_pong_v1_t;

/* system.hello (BCAST on boot) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  proto_version;  /* = WUPS_PROTO_VERSION */
    uint8_t  node_addr;
    uint8_t  reserved;
    uint16_t fw_version;
    uint16_t caps_classes;   /* bitmap, bit N = supports class N */
    uint32_t build_id;       /* git short hash or build epoch */
} wups_sys_hello_v1_t;

/* system.log (EVENT, ASCII text follows; len = LEN of frame minus header) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  level;          /* 0=trace 1=debug 2=info 3=warn 4=error */
    uint8_t  text_len;       /* bytes of text following */
    uint8_t  reserved;
    /* text[text_len] follows immediately */
} wups_sys_log_v1_hdr_t;

/* power.status — emitted by CH32X to RP2040 every 1 s, and on demand */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  charge_state;   /* 0=idle 1=charging 2=charged 3=fault */
    uint16_t vbus_in_mV;
    uint16_t vbus_out_mV;
    int16_t  ibus_out_mA;
    uint16_t vbat_mV;
    int16_t  ibat_mA;
    int16_t  temp_dC;        /* deci-Celsius (e.g. 253 = 25.3 C) */
    uint16_t pd_contract_mV;
    uint16_t pd_contract_mA;
    uint16_t faults;         /* bitmap, see WUPS_PWR_FAULT_* below */
} wups_power_status_v1_t;

#define WUPS_PWR_FAULT_OVP        (1u << 0)
#define WUPS_PWR_FAULT_OCP        (1u << 1)
#define WUPS_PWR_FAULT_OTP        (1u << 2)
#define WUPS_PWR_FAULT_PD_NEG     (1u << 3)

/* power.cycle */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  reserved;
    uint16_t off_ms;
} wups_power_cycle_v1_t;

/* power.event (BCAST) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  event;
} wups_power_event_v1_t;

#define WUPS_PWR_EVT_MAINS_LOST     1u
#define WUPS_PWR_EVT_MAINS_RESTORED 2u
#define WUPS_PWR_EVT_CHARGE_LOW     3u
#define WUPS_PWR_EVT_CHARGE_FULL    4u
#define WUPS_PWR_EVT_FAULT          5u

/* net.status — ESP32 -> RP2040 (and forwarded to RPi) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  state;          /* 0=off 1=init 2=net_attach 3=ppp_up 4=mqtt_up 5=err */
    int8_t   rssi_dBm;
    int8_t   rsrp_dBm;
    int8_t   rsrq_dB;
    uint8_t  reserved;
    uint16_t errors;
    uint32_t ip_addr;        /* network byte order or 0 */
    uint32_t bytes_tx;
    uint32_t bytes_rx;
} wups_net_status_v1_t;

/* net.publish (REQ -> ESP32). Variable-length tail: topic + payload. */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  qos;            /* 0/1/2 */
    uint8_t  retain;         /* 0/1 */
    uint8_t  topic_len;      /* up to 200 bytes */
    uint16_t payload_len;
    /* topic[topic_len] then payload[payload_len] follow */
} wups_net_publish_v1_hdr_t;

/* net.downlink (EVENT -> destination, e.g. RPi) — same layout as publish */
typedef wups_net_publish_v1_hdr_t wups_net_downlink_v1_hdr_t;

/* net.time_sync (BCAST INTERNAL, ESP32 -> all MCUs) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  reserved;
    uint16_t ms_frac;        /* milliseconds part of the second */
    uint32_t unix_s;
} wups_net_time_sync_v1_t;

/* host.status — RPi -> RP2040 */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  eth_client_state; /* 0=stopped 1=syncing 2=synced 3=error */
    int16_t  cpu_temp_dC;
    uint8_t  mem_used_pct;
    uint8_t  disk_used_pct;
    uint16_t load_avg_x100;  /* 1-min load * 100 */
    uint32_t uptime_s;
} wups_host_status_v1_t;

/* host.shutdown / host.reset */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  reason;         /* 1=low_battery 2=remote_cmd 3=user 4=fault */
    uint16_t delay_s;
} wups_host_shutdown_v1_t;

/* host.service_restart REQ — header followed by ASCII unit name (no NUL).
 * The host (RPi agent) enforces a whitelist; non-whitelisted units are rejected
 * with a system.log EVENT and the RESP carries a non-zero result. */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  unit_len;       /* bytes of unit name following */
    uint16_t reserved;
    /* unit[unit_len] follows */
} wups_host_service_restart_v1_hdr_t;

/* host.event (BCAST) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  event;
} wups_host_event_v1_t;

#define WUPS_HOST_EVT_SHUTDOWN_IMMINENT 1u
#define WUPS_HOST_EVT_LOW_DISK          2u
#define WUPS_HOST_EVT_ETH_SYNCED        3u
#define WUPS_HOST_EVT_ETH_LOST          4u

/* ui.button_event (BCAST) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  button;         /* 0=left 1=right */
    uint8_t  action;         /* 0=press 1=release 2=long */
    uint8_t  reserved;
} wups_ui_button_event_v1_t;

/* ui.set_screen */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  screen;
} wups_ui_set_screen_v1_t;

/* ui.beep */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  reserved;
    uint16_t freq_hz;
    uint16_t dur_ms;
} wups_ui_beep_v1_t;

/* ui.display_msg (variable text follows) */
typedef struct WUPS_PACKED {
    uint8_t  version;        /* = 1 */
    uint8_t  line;
    uint8_t  text_len;
    uint8_t  reserved;
    /* text[text_len] follows */
} wups_ui_display_msg_v1_hdr_t;

/* ---- Fletcher-8 (UBX-compatible) ---------------------------------------- */

/*
 * Fletcher-8 over a contiguous byte range. Use exactly the same algorithm UBX
 * uses for ck_a/ck_b. Computed over the bytes: DST..LEN_H..payload[LEN-1].
 *
 * Inline so the compiler emits trivial code on every target.
 */
static inline void wups_fletcher8(const uint8_t *buf, size_t len,
                                  uint8_t *ck_a, uint8_t *ck_b) {
    uint8_t a = 0, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (uint8_t)(a + buf[i]);
        b = (uint8_t)(b + a);
    }
    *ck_a = a;
    *ck_b = b;
}

#ifdef __cplusplus
}
#endif

#endif /* WEB3PI_UPS_PROTOCOL_H */
