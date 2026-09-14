/* SDK-boundary harness; production callbacks are included, never rewritten. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

typedef int esp_event_base_t;
typedef struct { int identity; } esp_netif_t;
typedef struct { uint32_t addr; } ip4_t;
typedef struct { ip4_t ip, gw, netmask; } ip_info_t;
typedef struct { esp_netif_t *esp_netif; ip_info_t ip_info; bool ip_changed; } ip_event_got_ip_t;
typedef struct { struct { union { ip4_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
typedef uint32_t EventBits_t;
typedef uint32_t TickType_t;
typedef EventBits_t *EventGroupHandle_t;

#define ESP_OK 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_DNS_BACKUP 1
#define IP_EVENT 1
#define NETIF_PPP_STATUS 2
#define IP_EVENT_PPP_GOT_IP 6
#define IP_EVENT_PPP_LOST_IP 7
#define NETIF_PPP_ERRORNONE 0
#define NETIF_PPP_ERRORPARAM 1
#define NETIF_PPP_ERROROPEN 2
#define NETIF_PPP_ERRORDEVICE 3
#define NETIF_PPP_ERRORALLOC 4
#define NETIF_PPP_ERRORUSER 5
#define NETIF_PPP_ERRORCONNECT 6
#define NETIF_PPP_ERRORAUTHFAIL 7
#define NETIF_PPP_ERRORPROTOCOL 8
#define NETIF_PPP_ERRORPEERDEAD 9
#define NETIF_PPP_ERRORIDLETIMEOUT 10
#define NETIF_PPP_ERRORCONNECTTIME 11
#define NETIF_PPP_ERRORLOOPBACK 12
#define NETIF_PP_PHASE_OFFSET 0x100
#define NETIF_PPP_CONNECT_FAILED 0x200
#define EVT_GOT_IP 1u
#define EVT_LOST_IP 2u
#define EVT_PPP_FAIL 4u
#define EVT_MQTT_DOWN 8u
#define EVT_PPP_CHANGED (EVT_GOT_IP | EVT_LOST_IP | EVT_PPP_FAIL)
#define MODEM_TAG "modem"
#define IPSTR "%u"
#define IP2STR(ip) ((unsigned)(ip)->addr)
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

static esp_netif_t own_netif = {1};
static esp_netif_t *s_ppp_netif = &own_netif;
static EventBits_t host_bits;
static EventGroupHandle_t s_modem_evt = &host_bits;
static unsigned checks;
#ifndef PPP_LEGACY_BASELINE
static unsigned host_lock_depth;
#endif

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 2; } } while (0)

static void host_log(const char *tag, const char *format, ...)
{
#ifndef PPP_LEGACY_BASELINE
    assert(host_lock_depth == 0);
#endif
    (void)tag;
    char text[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
}
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)

static int esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *dns)
{
#ifndef PPP_LEGACY_BASELINE
    assert(host_lock_depth == 0);
#endif
    (void)type;
    assert(netif == s_ppp_netif);
    memset(dns, 0, sizeof(*dns));
    return ESP_OK;
}
static EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
#ifndef PPP_LEGACY_BASELINE
    assert(host_lock_depth == 0);
#endif
    return *group |= bits;
}

#ifdef PPP_LEGACY_BASELINE
static volatile bool s_ppp_up;
#include "test_modem_ppp_events_baseline.c"

int main(void)
{
    ip_event_got_ip_t event = {.esp_netif = s_ppp_netif};
    esp_netif_t *source = s_ppp_netif;
    /* Old stop notification, then the supervisor's next-attempt clear. */
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORUSER, &source);
    host_bits = 0;
    /* Timer expires during registration at 120s; GOT arrives 14.610s later. */
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_LOST_IP, &event);
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_GOT_IP, &event);
    CHECK(s_ppp_up);
    CHECK(host_bits & EVT_GOT_IP);
    if (host_bits & EVT_LOST_IP) {
        puts("BASELINE_REPRODUCED: fresh GOT leaves older LOST latched; expected regression failure");
        return 1;
    }
    return 0;
}
#else
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static void host_enter(portMUX_TYPE *lock)
{
    assert(host_lock_depth == 0);
    assert(pthread_mutex_lock(lock) == 0);
    host_lock_depth++;
}
static void host_exit(portMUX_TYPE *lock)
{
    assert(host_lock_depth == 1);
    host_lock_depth--;
    assert(pthread_mutex_unlock(lock) == 0);
}
#define portENTER_CRITICAL(lock) host_enter(lock)
#define portEXIT_CRITICAL(lock) host_exit(lock)

enum delivery { DELIVER_NONE, DELIVER_GOT, DELIVER_LOST, DELIVER_USER };
static int64_t host_now_us;
static struct { int64_t at_us; enum delivery event; } scheduled[8];
static size_t scheduled_count, scheduled_next;
static enum delivery after_wait;
static unsigned waits;
static void deliver(enum delivery event);

static int64_t esp_timer_get_time(void) { return host_now_us; }
static EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    assert(host_lock_depth == 0);
    EventBits_t before = *group;
    *group &= ~bits;
    return before;
}
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                      int clear, int all, TickType_t timeout)
{
    assert(host_lock_depth == 0 && !all && timeout > 0);
    assert(++waits < 100);
    const int64_t deadline = host_now_us + (int64_t)timeout * 1000;
    while (!(*group & bits) && scheduled_next < scheduled_count &&
           scheduled[scheduled_next].at_us <= deadline) {
        host_now_us = scheduled[scheduled_next].at_us;
        enum delivery event = scheduled[scheduled_next++].event;
        deliver(event);
    }
    EventBits_t result = *group;
    if (!(result & bits)) host_now_us = deadline;
    if (clear) *group &= ~bits;
    /* Another callback can run after the kernel saved the wake bits but
     * before the supervisor inspects them. Its newer state must win. */
    if (after_wait != DELIVER_NONE) {
        enum delivery event = after_wait;
        after_wait = DELIVER_NONE;
        deliver(event);
    }
    return result;
}

#include "modem_ppp_events.inc"

static void deliver(enum delivery event)
{
    ip_event_got_ip_t ip = {.esp_netif = s_ppp_netif};
    esp_netif_t *netif = s_ppp_netif;
    if (event == DELIVER_GOT) on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_GOT_IP, &ip);
    if (event == DELIVER_LOST) on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_LOST_IP, &ip);
    if (event == DELIVER_USER) on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORUSER, &netif);
}
static void reset(void)
{
    host_now_us = 0;
    scheduled_count = scheduled_next = 0;
    after_wait = DELIVER_NONE;
    waits = 0;
    host_bits = 0;
    s_ppp_netif = &own_netif;
    ppp_events_begin_stop();
    ppp_events_begin_attempt();
}
static void dial(void)
{
    reset();
    ppp_events_start_dial();
}
static void at_ms(int64_t at, enum delivery event)
{
    assert(scheduled_count < sizeof(scheduled) / sizeof(scheduled[0]));
    scheduled[scheduled_count].at_us = at * 1000;
    scheduled[scheduled_count++].event = event;
}
static void terminal(int32_t id)
{
    esp_netif_t *netif = s_ppp_netif;
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, id, &netif);
}

static int test_delayed_loss(void)
{
    /* Long registration: stop at t=0, retry prep at110s, old LOST at120s.
     * These are observations from the persistent netif, not new PPP loss. */
    dial();
    deliver(DELIVER_GOT);
    ppp_events_begin_stop();
    deliver(DELIVER_USER);
    host_now_us = 110000000;
    ppp_events_begin_attempt();
    host_now_us = 120000000;
    deliver(DELIVER_LOST);
    CHECK(ppp_events_snapshot().phase == PPP_PREPARING);
    CHECK(!modem_ppp_is_up());
    host_now_us = 130000000;
    ppp_events_start_dial();
    at_ms(134610, DELIVER_GOT);
    CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
    CHECK(host_now_us == 134610000);
    CHECK(!ppp_events_take_loss());
    CHECK(modem_ppp_is_up());

    /* Same timer can expire during the new dial, not just registration. */
    dial();
    host_now_us = 110000000;
    at_ms(120000, DELIVER_LOST);
    at_ms(134610, DELIVER_GOT);
    CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
    CHECK(host_now_us == 134610000);
    CHECK(ppp_events_snapshot().had_ip);
    CHECK(!ppp_events_take_loss());
    /* Stale notifications deliberately retained/coalesced cannot override UP. */
    host_bits = EVT_GOT_IP | EVT_LOST_IP | EVT_PPP_FAIL;
    CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
    CHECK(!ppp_events_take_loss());
    return 0;
}

static int test_order_and_errors(void)
{
    for (int id = NETIF_PPP_ERRORPARAM; id <= NETIF_PPP_ERRORLOOPBACK; ++id) {
        dial();
        terminal(id);
        CHECK(ppp_events_snapshot().phase == PPP_DOWN);
        CHECK(ppp_events_snapshot().error == id);
        deliver(DELIVER_GOT);
        CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
        CHECK(modem_ppp_is_up());
        CHECK(!ppp_events_take_loss());

        dial();
        deliver(DELIVER_GOT);
        terminal(id);
        CHECK(!modem_ppp_is_up());
        CHECK(ppp_events_wait_for_link(60000) == PPP_DOWN);
        CHECK(ppp_events_snapshot().phase == PPP_STOPPING);
        /* The decision to tear down was atomic; late GOT cannot revive it. */
        deliver(DELIVER_GOT);
        CHECK(!modem_ppp_is_up());
    }
    dial();
    terminal(NETIF_PPP_ERRORAUTHFAIL);
    deliver(DELIVER_LOST);
    CHECK(ppp_events_wait_for_link(60000) == PPP_DOWN);

    dial();
    deliver(DELIVER_LOST);
    deliver(DELIVER_GOT);
    CHECK(!ppp_events_take_loss());
    deliver(DELIVER_LOST);
    CHECK(!modem_ppp_is_up());
    CHECK(ppp_events_take_loss());
    CHECK(!ppp_events_take_loss()); /* exactly one recovery decision */
    return 0;
}

static int test_expected_stop(void)
{
    dial();
    deliver(DELIVER_GOT);
    ppp_events_begin_stop(); /* before AT reset / COMMAND / DCE destroy */
    host_bits = 0;
    deliver(DELIVER_USER);
    deliver(DELIVER_LOST);
    deliver(DELIVER_GOT);
    CHECK(ppp_events_snapshot().phase == PPP_STOPPING);
    CHECK(!modem_ppp_is_up());
    CHECK(host_bits == 0);
    uint32_t previous = ppp_events_snapshot().attempt;
    ppp_events_begin_attempt();
    CHECK(ppp_events_snapshot().attempt == previous + 1);
    deliver(DELIVER_USER); /* queued old teardown while preparing new DCE */
    deliver(DELIVER_GOT);
    CHECK(ppp_events_snapshot().phase == PPP_PREPARING);
    ppp_events_start_dial();
    CHECK(!ppp_events_snapshot().had_ip);
    deliver(DELIVER_USER); /* unexpected termination during actual dialing */
    CHECK(ppp_events_wait_for_link(60000) == PPP_DOWN);
    return 0;
}

static int test_payloads(void)
{
    esp_netif_t foreign = {2};
    esp_netif_t *foreign_pointer = &foreign;
    ip_event_got_ip_t other_ip = {.esp_netif = &foreign};
    ip_event_got_ip_t own_ip = {.esp_netif = s_ppp_netif};
    dial();
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_GOT_IP, &other_ip);
    CHECK(!modem_ppp_is_up());
    deliver(DELIVER_GOT);
    uint32_t sequence = ppp_events_snapshot().sequence;
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_LOST_IP, &other_ip);
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_LOST_IP, NULL);
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_GOT_IP, NULL);
    on_ip_event(NULL, NETIF_PPP_STATUS, IP_EVENT_PPP_LOST_IP, &own_ip);
    on_ip_event(NULL, IP_EVENT, 9999, &own_ip);
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORUSER, &foreign_pointer);
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORUSER, NULL);
    on_netif_ppp_status(NULL, IP_EVENT, NETIF_PPP_ERRORUSER, &s_ppp_netif);
    terminal(NETIF_PPP_ERRORNONE);
    for (int phase = NETIF_PP_PHASE_OFFSET; phase < NETIF_PP_PHASE_OFFSET + 13; ++phase)
        terminal(phase);
    terminal(NETIF_PPP_CONNECT_FAILED); /* SDK6.0.2 malformed source is not trusted */
    terminal(-1);
    terminal(13);
    CHECK(modem_ppp_is_up());
    CHECK(ppp_events_snapshot().sequence == sequence);
    s_ppp_netif = NULL;
    on_ip_event(NULL, IP_EVENT, IP_EVENT_PPP_LOST_IP, &own_ip);
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORUSER, &foreign_pointer);
    s_ppp_netif = &own_netif;
    CHECK(ppp_events_snapshot().sequence == sequence);

    /* PPP status copies a pointer, not the pointed-to structure. memcpy
     * also handles a byte-aligned event buffer without an alignment cast. */
    unsigned char bytes[sizeof(s_ppp_netif) + 1];
    memcpy(bytes + 1, &s_ppp_netif, sizeof(s_ppp_netif));
    on_netif_ppp_status(NULL, NETIF_PPP_STATUS, NETIF_PPP_ERRORPEERDEAD, bytes + 1);
    CHECK(!modem_ppp_is_up());
    CHECK(ppp_events_take_loss());
    return 0;
}

static int test_wake_races_and_diagnostics(void)
{
    dial();
    at_ms(1, DELIVER_GOT);
    after_wait = DELIVER_LOST; /* kernel returned GOT; newer LOSS wins */
    CHECK(ppp_events_wait_for_link(60000) == PPP_DOWN);
    CHECK(!modem_ppp_is_up());

    dial();
    at_ms(1, DELIVER_USER);
    after_wait = DELIVER_GOT; /* kernel returned FAIL; newer GOT wins */
    CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
    CHECK(!ppp_events_take_loss());

    /* A diagnostic command can run while the event task reports a loss.
     * Even if the wake was consumed, the actual post-diagnostic helper must
     * observe it; test both orders before the atomic recovery decision. */
    host_now_us += 3000000;
    deliver(DELIVER_LOST);
    xEventGroupClearBits(s_modem_evt, EVT_PPP_CHANGED);
    CHECK(!modem_ppp_is_up());
    deliver(DELIVER_GOT);
    CHECK(!ppp_events_take_loss());
    host_now_us += 3000000;
    deliver(DELIVER_LOST);
    xEventGroupClearBits(s_modem_evt, EVT_PPP_CHANGED);
    CHECK(ppp_events_take_loss());
    CHECK(!modem_ppp_is_up());
    return 0;
}

static int test_timeout(void)
{
    dial();
    at_ms(1, DELIVER_LOST);
    at_ms(59000, DELIVER_LOST);
    CHECK(ppp_events_wait_for_link(60000) == PPP_STARTING);
    CHECK(host_now_us == 60000000);
    CHECK(ppp_events_snapshot().phase == PPP_STOPPING);
    deliver(DELIVER_GOT);
    CHECK(!modem_ppp_is_up());
    dial();
    at_ms(59999, DELIVER_GOT);
    CHECK(ppp_events_wait_for_link(60000) == PPP_UP);
    CHECK(host_now_us == 59999000);
    return 0;
}

int main(void)
{
    if (test_delayed_loss() || test_order_and_errors() || test_expected_stop() ||
        test_payloads() || test_wake_races_and_diagnostics() || test_timeout()) return 2;
    printf("modem_ppp_events PASS: %u checks; virtual120s, real extracted callbacks/state decisions\n", checks);
    return 0;
}
#endif
