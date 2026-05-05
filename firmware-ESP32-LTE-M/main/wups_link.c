#include "wups_link.h"
#include "wups_proto.h"
#include "mqtt.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "wups_link"

/* --- pin map ------------------------------------------------------------ */
/*
 * Confirmed with the LilyGo T-SIM7080G-S3 pinout (see docs/info.md and the
 * board image). All four pads sit on the same left header, contiguous-ish
 * after skipping GPIO3 (modem RI / strapping) and GPIO46 (strapping):
 *
 *   GPIO16 — CTS  (input,  ESP32  ← RP2040 RTS = GPIO23)
 *   GPIO17 — TX   (output, ESP32  → RP2040 RX  = GPIO21)
 *   GPIO18 — RX   (input,  ESP32  ← RP2040 TX  = GPIO20)
 *   GPIO8  — RTS  (output, ESP32  → RP2040 CTS = GPIO22)
 *
 * GPIO16 happens to be the default U0CTS — irrelevant here, we route via
 * the IO MUX to UART2.
 */
#define WUPS_UART_NUM        UART_NUM_2
#define WUPS_UART_TX_PIN     17
#define WUPS_UART_RX_PIN     18
#define WUPS_UART_CTS_PIN    16
#define WUPS_UART_RTS_PIN     8
#define WUPS_UART_BAUD       921600

/* RX driver buffer. Comfortably larger than one full frame (254 B) — gives
 * us slack across the ~50 ms RX-task tick. TX buffer = 0 means writes are
 * synchronous (fine: largest frame is 2.8 ms at 921600). */
#define WUPS_UART_RX_BUFSIZE 1024
#define WUPS_UART_TX_BUFSIZE 0

/* --- module state ------------------------------------------------------- */

static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_tx_seq;

/* Diagnostic counters — surfaced via wups_link_log_stats(). */
static volatile uint32_t s_frames_tx = 0;
static volatile uint32_t s_frames_rx = 0;
static volatile uint32_t s_bytes_tx  = 0;
static volatile uint32_t s_bytes_rx  = 0;
static volatile uint32_t s_rx_resync = 0;  /* SYNC2 mismatch — out-of-frame bytes seen */

typedef enum {
    WUPS_RX_SYNC1 = 0,
    WUPS_RX_SYNC2,
    WUPS_RX_DST,
    WUPS_RX_SRC,
    WUPS_RX_CLASS,
    WUPS_RX_OP,
    WUPS_RX_FLAGS,
    WUPS_RX_SEQ,
    WUPS_RX_LEN_L,
    WUPS_RX_LEN_H,
    WUPS_RX_PAYLOAD,
    WUPS_RX_CK_A,
    WUPS_RX_CK_B,
    WUPS_RX_END1,
    WUPS_RX_END2,
} wups_rx_state_t;

static struct {
    wups_rx_state_t state;
    uint8_t  dst, src, cls, op, flags, seq;
    uint16_t len;
    uint16_t pidx;
    uint8_t  payload[WUPS_MAX_PAYLOAD];
    uint8_t  rx_ck_a;
    uint8_t  exp_a, exp_b;
} s_rx;

static inline void rx_reset(void) { s_rx.state = WUPS_RX_SYNC1; }
static inline void rx_step(uint8_t b)
{
    s_rx.exp_a = (uint8_t)(s_rx.exp_a + b);
    s_rx.exp_b = (uint8_t)(s_rx.exp_b + s_rx.exp_a);
}

/* --- TX ---------------------------------------------------------------- */

static void send_frame_full(uint8_t dst, uint8_t src, uint8_t cls, uint8_t op,
                            uint8_t flags, uint8_t seq,
                            const void *payload, uint16_t payload_len)
{
    if (payload_len > WUPS_MAX_PAYLOAD) return;

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

    /* Fletcher-8 over DST..LEN_H..payload (sync and end marker excluded). */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 10; ++i) { a = (uint8_t)(a + header[i]); b = (uint8_t)(b + a); }
    const uint8_t *p = (const uint8_t *)payload;
    for (uint16_t i = 0; i < payload_len; ++i) { a = (uint8_t)(a + p[i]); b = (uint8_t)(b + a); }
    uint8_t trailer[4] = { a, b, WUPS_END1, WUPS_END2 };

    /* Serialize at frame granularity so concurrent senders (RX task,
     * MQTT data callback, hello at boot) don't interleave. */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(WUPS_UART_NUM, (const char *)header, 10);
    if (payload_len) uart_write_bytes(WUPS_UART_NUM, (const char *)payload, payload_len);
    uart_write_bytes(WUPS_UART_NUM, (const char *)trailer, 4);
    xSemaphoreGive(s_tx_mutex);

    s_frames_tx++;
    s_bytes_tx += (uint32_t)(14 + payload_len);
}

void wups_link_log_stats(void)
{
    ESP_LOGI(TAG, "stats: tx=%lu (%lu B) rx=%lu (%lu B) resync=%lu",
             (unsigned long)s_frames_tx, (unsigned long)s_bytes_tx,
             (unsigned long)s_frames_rx, (unsigned long)s_bytes_rx,
             (unsigned long)s_rx_resync);
}

void wups_link_send_seq(uint8_t dst, uint8_t cls, uint8_t op,
                        uint8_t flags, uint8_t seq,
                        const void *payload, uint16_t payload_len)
{
    send_frame_full(dst, WUPS_ADDR_ESP32, cls, op, flags, seq, payload, payload_len);
}

void wups_link_send(uint8_t dst, uint8_t cls, uint8_t op, uint8_t flags,
                    const void *payload, uint16_t payload_len)
{
    /* Race on s_tx_seq across multiple sender threads is benign: SEQ is
     * only used for human-readable correlation between REQ and RESP at
     * higher layers, not for transport correctness. */
    send_frame_full(dst, WUPS_ADDR_ESP32, cls, op, flags, s_tx_seq++,
                    payload, payload_len);
}

/* --- dispatch ---------------------------------------------------------- */

static void handle_net_publish(const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(wups_net_publish_v1_hdr_t)) {
        ESP_LOGW(TAG, "net.publish too short: %u", (unsigned)len);
        return;
    }
    wups_net_publish_v1_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.version != 1) {
        ESP_LOGW(TAG, "net.publish version=%u (expected 1)", hdr.version);
        return;
    }
    size_t need = sizeof(wups_net_publish_v1_hdr_t) + hdr.topic_len + hdr.payload_len;
    if (need > len) {
        ESP_LOGW(TAG, "net.publish length mismatch: header wants %u, frame has %u",
                 (unsigned)need, (unsigned)len);
        return;
    }
    if (hdr.topic_len == 0) {
        ESP_LOGW(TAG, "net.publish has empty topic");
        return;
    }

    /* MQTT topic must be a NUL-terminated C string; the payload is opaque. */
    char topic[201];
    if (hdr.topic_len >= sizeof(topic)) {
        ESP_LOGW(TAG, "net.publish topic too long: %u", hdr.topic_len);
        return;
    }
    memcpy(topic, payload + sizeof(hdr), hdr.topic_len);
    topic[hdr.topic_len] = '\0';
    const uint8_t *mqtt_payload = payload + sizeof(hdr) + hdr.topic_len;

    int rc = mqtt_publish_raw(topic, mqtt_payload, hdr.payload_len,
                              hdr.qos, hdr.retain);
    if (rc < 0) {
        ESP_LOGW(TAG, "mqtt publish %s rc=%d (mqtt not connected?)", topic, rc);
    } else {
        ESP_LOGI(TAG, "mqtt publish %s len=%u qos=%u retain=%u msg_id=%d",
                 topic, hdr.payload_len, hdr.qos, hdr.retain, rc);
    }
}

static void on_local_frame(uint8_t src, uint8_t cls, uint8_t op,
                           uint8_t flags, uint8_t seq,
                           const uint8_t *payload, uint16_t len)
{
    if (cls == WUPS_CLASS_SYSTEM) {
        if (op == WUPS_OP_SYS_PING && (flags & WUPS_FLAG_REQ)) {
            wups_sys_pong_v1_t pong;
            pong.version    = 1;
            pong.reserved   = 0;
            pong.fw_version = (uint16_t)((1u << 8) | 0u); /* 1.0 */
            pong.uptime_ms  = (uint32_t)(esp_timer_get_time() / 1000);
            wups_link_send_seq(src, WUPS_CLASS_SYSTEM, WUPS_OP_SYS_PING,
                               WUPS_FLAG_RESP, seq, &pong, sizeof(pong));
            return;
        }
        /* hello / log / status_query — ignored in v1 */
        return;
    }
    if (cls == WUPS_CLASS_NET) {
        if (op == WUPS_OP_NET_PUBLISH && (flags & WUPS_FLAG_REQ)) {
            handle_net_publish(payload, len);
            return;
        }
        /* status / downlink / time_sync are outbound from us; if we ever
         * receive them, drop silently. */
        return;
    }
    /* power / host / ui — not our concern (ESP32 is dumb pipe). */
}

static void deliver_frame(void)
{
    /* Leaf node — accept frames addressed to us, broadcast, or internal
     * multicast; drop everything else (RP2040 router shouldn't be sending
     * us those, but defense in depth). */
    if (s_rx.dst != WUPS_ADDR_ESP32 &&
        s_rx.dst != WUPS_ADDR_BROADCAST &&
        s_rx.dst != WUPS_ADDR_INTERNAL) {
        return;
    }
    s_frames_rx++;
    on_local_frame(s_rx.src, s_rx.cls, s_rx.op, s_rx.flags, s_rx.seq,
                   s_rx.payload, s_rx.len);
}

/* --- RX state machine -------------------------------------------------- */

static void rx_byte(uint8_t b)
{
    switch (s_rx.state) {
    case WUPS_RX_SYNC1:
        if (b == WUPS_SYNC1) s_rx.state = WUPS_RX_SYNC2;
        break;
    case WUPS_RX_SYNC2:
        if (b == WUPS_SYNC2) {
            s_rx.exp_a = 0;
            s_rx.exp_b = 0;
            s_rx.pidx  = 0;
            s_rx.state = WUPS_RX_DST;
        } else {
            s_rx_resync++;
            rx_reset();
        }
        break;
    case WUPS_RX_DST:    s_rx.dst   = b; rx_step(b); s_rx.state = WUPS_RX_SRC;   break;
    case WUPS_RX_SRC:    s_rx.src   = b; rx_step(b); s_rx.state = WUPS_RX_CLASS; break;
    case WUPS_RX_CLASS:  s_rx.cls   = b; rx_step(b); s_rx.state = WUPS_RX_OP;    break;
    case WUPS_RX_OP:     s_rx.op    = b; rx_step(b); s_rx.state = WUPS_RX_FLAGS; break;
    case WUPS_RX_FLAGS:  s_rx.flags = b; rx_step(b); s_rx.state = WUPS_RX_SEQ;   break;
    case WUPS_RX_SEQ:    s_rx.seq   = b; rx_step(b); s_rx.state = WUPS_RX_LEN_L; break;
    case WUPS_RX_LEN_L:
        s_rx.len = b;
        rx_step(b);
        s_rx.state = WUPS_RX_LEN_H;
        break;
    case WUPS_RX_LEN_H:
        s_rx.len |= (uint16_t)((uint16_t)b << 8);
        rx_step(b);
        if (s_rx.len > WUPS_MAX_PAYLOAD) { rx_reset(); break; }
        s_rx.state = (s_rx.len == 0) ? WUPS_RX_CK_A : WUPS_RX_PAYLOAD;
        break;
    case WUPS_RX_PAYLOAD:
        s_rx.payload[s_rx.pidx++] = b;
        rx_step(b);
        if (s_rx.pidx >= s_rx.len) s_rx.state = WUPS_RX_CK_A;
        break;
    case WUPS_RX_CK_A:
        s_rx.rx_ck_a = b;
        s_rx.state = WUPS_RX_CK_B;
        break;
    case WUPS_RX_CK_B:
        if (s_rx.rx_ck_a == s_rx.exp_a && b == s_rx.exp_b) {
            s_rx.state = WUPS_RX_END1;
        } else {
            rx_reset();
        }
        break;
    case WUPS_RX_END1:
        s_rx.state = (b == WUPS_END1) ? WUPS_RX_END2 : WUPS_RX_SYNC1;
        break;
    case WUPS_RX_END2:
        if (b == WUPS_END2) deliver_frame();
        rx_reset();
        break;
    default:
        rx_reset();
        break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    while (1) {
        int n = uart_read_bytes(WUPS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n < 0) {
            ESP_LOGE(TAG, "uart_read_bytes err %d", n);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n > 0) s_bytes_rx += (uint32_t)n;
        for (int i = 0; i < n; ++i) rx_byte(buf[i]);
    }
}

/* --- MQTT inbound → net.downlink -------------------------------------- */

/* Forward an arriving MQTT message to the RPi as a net.downlink event.
 * The frame carries the topic and the raw payload; RPi-side service
 * decides what to do with it. Runs in the MQTT client task context. */
static void on_mqtt_data(const char *topic, size_t topic_len,
                         const void *payload, size_t payload_len)
{
    if (topic_len > 200) {
        ESP_LOGW(TAG, "mqtt downlink: topic too long (%u), dropping",
                 (unsigned)topic_len);
        return;
    }
    size_t total = sizeof(wups_net_downlink_v1_hdr_t) + topic_len + payload_len;
    if (total > WUPS_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "mqtt downlink: %u + %u + hdr exceeds %u, dropping",
                 (unsigned)topic_len, (unsigned)payload_len, WUPS_MAX_PAYLOAD);
        return;
    }

    /* Build the payload in a stack buffer (frame max = 240 B, well within
     * default task stack). */
    uint8_t buf[WUPS_MAX_PAYLOAD];
    wups_net_downlink_v1_hdr_t hdr = {
        .version     = 1,
        .qos         = 0,
        .retain      = 0,
        .topic_len   = (uint8_t)topic_len,
        .payload_len = (uint16_t)payload_len,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), topic, topic_len);
    memcpy(buf + sizeof(hdr) + topic_len, payload, payload_len);

    wups_link_send(WUPS_ADDR_RPI, WUPS_CLASS_NET, WUPS_OP_NET_DOWNLINK,
                   WUPS_FLAG_EVENT, buf, (uint16_t)total);
}

/* --- hello broadcast --------------------------------------------------- */

static void send_hello_bcast(void)
{
    wups_sys_hello_v1_t h;
    h.version       = 1;
    h.proto_version = WUPS_PROTO_VERSION;
    h.node_addr     = WUPS_ADDR_ESP32;
    h.reserved      = 0;
    h.fw_version    = (uint16_t)((1u << 8) | 0u);
    h.caps_classes  = WUPS_CAP_SYSTEM | WUPS_CAP_NET;
    h.build_id      = 0;
    wups_link_send(WUPS_ADDR_BROADCAST, WUPS_CLASS_SYSTEM,
                   WUPS_OP_SYS_HELLO, WUPS_FLAG_EVENT, &h, sizeof(h));
}

/* --- public init ------------------------------------------------------- */

esp_err_t wups_link_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    rx_reset();

    uart_config_t cfg = {
        .baud_rate           = WUPS_UART_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_CTS_RTS,
        /* Deassert RTS when the RX FIFO has 122 / 128 bytes — leaves a
         * small margin for in-flight bytes after we tell the peer to stop. */
        .rx_flow_ctrl_thresh = 122,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(WUPS_UART_NUM, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(WUPS_UART_NUM, WUPS_UART_TX_PIN, WUPS_UART_RX_PIN,
                       WUPS_UART_RTS_PIN, WUPS_UART_CTS_PIN);
    if (err != ESP_OK) return err;

    err = uart_driver_install(WUPS_UART_NUM, WUPS_UART_RX_BUFSIZE,
                              WUPS_UART_TX_BUFSIZE, 0, NULL, 0);
    if (err != ESP_OK) return err;

    /* 4 KB stack: deframer + occasional mqtt_publish_raw call (which just
     * enqueues to the MQTT task). */
    BaseType_t ok = xTaskCreate(rx_task, "wups_rx", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    /* Hook MQTT inbound → net.downlink. The handler will fire once MQTT
     * has connected and starts receiving messages. */
    mqtt_set_data_handler(on_mqtt_data);

    ESP_LOGI(TAG, "UART2 up: TX=GPIO%d RX=GPIO%d CTS=GPIO%d RTS=GPIO%d @ %d, HW flow ctrl",
             WUPS_UART_TX_PIN, WUPS_UART_RX_PIN, WUPS_UART_CTS_PIN,
             WUPS_UART_RTS_PIN, WUPS_UART_BAUD);

    /* Announce ourselves to anyone listening on the bus (RP2040 hub will
     * receive immediately; RPi sees it after USB-CDC enumerates on its end). */
    send_hello_bcast();
    return ESP_OK;
}
