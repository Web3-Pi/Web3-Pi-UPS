#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "mqtt_health.h"
#include "modem_recovery.h"
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 3
#include "definitions.inc"

#define ESP_OK 0
#define MODEM_TAG "modem"
#define MODEM_RADIO_RESPONSE_CAPACITY 512
#define WUPS_NET_SINR_UNKNOWN (-128)
#define EVT_PPP_CHANGED 7u
#define EVT_MQTT_DOWN 8u
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(tag, ...) log_ignore(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_ignore(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_ignore(__VA_ARGS__)

typedef unsigned EventBits_t;
enum { WUPS_BACKEND_MODE_MQTT, WUPS_BACKEND_MODE_HTTP, WUPS_BACKEND_MODE_ARKIV };
enum { MODEM_FAIL_NONE, MODEM_FAIL_NET, MODEM_FAIL_UPLINK, ARKIV_CLAIMED };
enum scenario {
    DEAD_LINK,
    REGISTRATION_RETURNS,
    MQTT_RETURNS_IN_PROBE,
    MQTT_RETURNS_IN_FINAL_SNAPSHOT,
    MQTT_RETURNS_IN_PERIODIC_DIAGNOSTICS,
    OTA_STARTS_IN_PROBE,
    PPP_LOST_IN_PROBE,
    INTERNET_ANSWERS,
    MQTT_CONNECTED_WITHOUT_PROOF,
    SECOND_DEAD_TRIP,
    REGISTRATION_GRACE_EXPIRES,
    MQTT_CONNECTED_IN_FINAL_SNAPSHOT,
    WORKER_STALLED_IN_FINAL_SNAPSHOT,
    MQTT_PROOF_AT_COMMIT,
    MQTT_REPLACED_DURING_PROBE,
    PPP_REPLACED_DURING_PROBE,
    AUTH_REFUSED_IN_FINAL_SNAPSHOT,
    OTA_IN_FINAL_SNAPSHOT,
    PPP_LOST_IN_FINAL_SNAPSHOT,
    HTTP_DEAD,
    HTTP_RETURNS_IN_FINAL_SNAPSHOT,
    ARKIV_DEAD,
    ARKIV_RETURNS_IN_FINAL_SNAPSHOT,
};

static int s_modem_evt;
static void *s_dce;
static bool s_cmux_active, s_iccid_known, s_alert_active;
static int s_uplink_trips, s_fails_since_ok, s_fail_stage, s_alert_clear_pending;
static int8_t s_ns_last_rssi;
static uint8_t s_ns_last_state;
static uint32_t s_ns_last_emit_s;

static enum scenario current_scenario;
static uint32_t simulated_seconds, registration_seen_s;
static unsigned probes, health_reads;
static bool ota_active, ppp_loss_pending, registration_present, mqtt_restored;
static mqtt_health_t health;
static bool auth_refused;
static unsigned horizon;
static int active_backend;
static bool backend_restored;
#ifdef FIXED
static int lock_depth;
#endif
static jmp_buf simulation_end;
static int observed_action;

static void log_ignore(const char *format, ...)
{
#ifdef FIXED
    assert(lock_depth == 0);
#endif
    (void)format;
}
static uint32_t now_s(void) { return simulated_seconds; }
static uint64_t now_ms(void) { return (uint64_t)simulated_seconds * 1000; }

static EventBits_t xEventGroupWaitBits(int group, unsigned bits,
                                      int clear, int all, unsigned ticks)
{
    (void)group; (void)bits; (void)clear; (void)all;
    simulated_seconds += ticks / 1000;
    if (simulated_seconds > horizon) longjmp(simulation_end, 1);
    return 0;
}

static bool ppp_events_take_loss(void)
{
    bool lost = ppp_loss_pending;
    ppp_loss_pending = false;
    return lost;
}
static int backend_mode_get(void) { return active_backend; }
static int mqtt_client_start(void) { return ESP_OK; }
static bool mqtt_sdk_is_started(void) { return true; }
static bool fw_ota_in_progress(void) { return ota_active; }
static void fw_ota_mark_uplink_healthy(void) {}
static void modem_ui_alert_clear(void) {}
static void modem_ui_alert(const char *text) { (void)text; }
static const char *modem_fail_msg(int stage) { (void)stage; return "synthetic"; }
static void modem_diag_clock_log(const char *event) { (void)event; }
static bool mqtt_auth_refused(void) { return auth_refused; }
static bool http_backend_is_configured(void)
{
#ifdef FIXED
    assert(lock_depth == 0); /* Real accessors may take task mutexes. */
#endif
    return true;
}
static uint32_t http_backend_last_success_s(void) { return backend_restored ? now_s() : 0; }
static int cmdauth_arkiv_claim_state(void) { return ARKIV_CLAIMED; }
static uint32_t arkiv_rpc_last_success_s(void) { return backend_restored ? now_s() : 0; }
static bool arkiv_ws_subscribed(void)
{
#ifdef FIXED
    assert(lock_depth == 0); /* Real accessor takes s_mu. */
#endif
    return false;
}

static void mqtt_get_health(mqtt_health_snapshot_t *out)
{
    ++health_reads;
    mqtt_health_poll(&health, now_ms(), out);
}
static bool mqtt_publication_proof_fresh(void)
{
    mqtt_health_snapshot_t out;
    mqtt_get_health(&out);
    return out.proof_fresh;
}

/* Real proof state machine: establish generation, admit probe and accept
 * its matching ACK. Packet transport and callback scheduling are synthetic. */
static void restore_mqtt(void)
{
    if (mqtt_restored) return;
    mqtt_restored = true;
    mqtt_health_on_connected(&health, now_ms());
    mqtt_health_token_t token;
    assert(mqtt_health_begin_probe(&health, now_ms(), &token));
    assert(mqtt_health_probe_admitted(&health, token, now_ms()));
    assert(mqtt_health_probe_ack(&health, token, now_ms()));
}

#ifdef FIXED
static modem_recovery_t s_uplink_recovery;
static int lock_stack[4], lock_count;
static int s_claim_mux = 1, s_lock = 2;
static atomic_bool s_ready = true;
static bool s_ota_active, s_validation_busy, s_modem_recovery_busy;
#define s_health health
#define s_in_progress ota_active
static void host_enter(portMUX_TYPE *lock)
{
    if (*lock == 1 && current_scenario == MQTT_PROOF_AT_COMMIT) restore_mqtt();
    assert(*lock > lock_depth);
    lock_stack[lock_count++] = lock_depth;
    lock_depth = *lock;
}
static void host_exit(portMUX_TYPE *lock)
{
    assert(lock_depth == *lock);
    lock_depth = lock_stack[--lock_count];
    /* Non-nested PPP snapshots are supplied by the host; commit always
     * holds OTA -> MQTT -> PPP, so every return must unwind in that order. */
}
#define portENTER_CRITICAL(lock) host_enter(lock)
#define portEXIT_CRITICAL(lock) host_exit(lock)
static ppp_event_state_t ppp_events_snapshot(void) { return s_ppp_state; }
#include "guards.inc"
#endif

static bool inet_probe(void)
{
#ifdef FIXED
    assert(lock_depth == 0);
#endif
    ++probes;
    simulated_seconds += INET_PROBE_TIMEOUT_MS / 1000;
    if (current_scenario == MQTT_RETURNS_IN_PROBE) restore_mqtt();
    if (current_scenario == MQTT_REPLACED_DURING_PROBE && probes == 1) {
        restore_mqtt();
        mqtt_health_on_disconnected(&health, now_ms());
    }
    if (current_scenario == PPP_REPLACED_DURING_PROBE && probes == 1)
        s_ppp_state.sequence++;
    if (current_scenario == OTA_STARTS_IN_PROBE) ota_active = true;
    if (current_scenario == PPP_LOST_IN_PROBE) ppp_loss_pending = true;
    if (current_scenario == INTERNET_ANSWERS) return true;
    simulated_seconds += INET_PROBE_TIMEOUT_MS / 1000;
    return false;
}

static int esp_modem_get_signal_quality(void *dce, int *csq, int *ber)
{
    (void)dce; *csq = 20; *ber = 0; return ESP_OK;
}
static int8_t csq_to_dbm(int csq) { return (int8_t)(-113 + 2 * csq); }

static bool radio_at(void *ctx, const char *command, char *reply,
                     size_t size, unsigned timeout_ms)
{
#ifdef FIXED
    assert(lock_depth == 0);
#endif
    (void)ctx; (void)timeout_ms;
    if (current_scenario == MQTT_RETURNS_IN_FINAL_SNAPSHOT && probes) {
        ++simulated_seconds;
        restore_mqtt();
    }
    if (current_scenario == MQTT_CONNECTED_IN_FINAL_SNAPSHOT && probes && !mqtt_restored) {
        mqtt_restored = true;
        mqtt_health_on_connected(&health, now_ms());
    }
    if (current_scenario == WORKER_STALLED_IN_FINAL_SNAPSHOT && probes && !mqtt_restored) {
        mqtt_restored = true;
        mqtt_health_worker_begin(&health, now_ms());
        simulated_seconds += 31;
    }
    if (current_scenario == AUTH_REFUSED_IN_FINAL_SNAPSHOT && probes) auth_refused = true;
    if (current_scenario == OTA_IN_FINAL_SNAPSHOT && probes) ota_active = true;
    if (current_scenario == PPP_LOST_IN_FINAL_SNAPSHOT && probes) ppp_loss_pending = true;
    if ((current_scenario == HTTP_RETURNS_IN_FINAL_SNAPSHOT ||
         current_scenario == ARKIV_RETURNS_IN_FINAL_SNAPSHOT) && probes)
        backend_restored = true;
    if (current_scenario == MQTT_RETURNS_IN_PERIODIC_DIAGNOSTICS &&
        simulated_seconds >= 1300 && !probes) restore_mqtt();
    if (strcmp(command, "AT+CEREG?") == 0) {
        if ((current_scenario == REGISTRATION_RETURNS ||
             current_scenario == REGISTRATION_GRACE_EXPIRES) &&
            simulated_seconds >= 1300 && !registration_present) {
            registration_present = true;
            registration_seen_s = simulated_seconds;
        }
        snprintf(reply, size, "+CEREG: 0,%d\r\nOK\r\n",
                 registration_present ? 5 : 0);
    } else if (strcmp(command, "AT+CPSI?") == 0) {
        snprintf(reply, size, "%s\r\nOK\r\n", registration_present
                 ? "+CPSI: LTE CAT-M1,Online,001-01,0x0001,12345,1,EUTRAN-BAND20,6200,3,3,-15,-98,-70,10"
                 : "+CPSI: NO SERVICE,Online");
    } else {
        snprintf(reply, size, "OK\r\n");
    }
    return true;
}
static void poll_cpsi(int8_t *rsrp, int8_t *rsrq, int8_t *sinr)
{
    char reply[MODEM_RADIO_RESPONSE_CAPACITY];
    (void)radio_at(NULL, "AT+CPSI?", reply, sizeof(reply), 3000);
    *rsrp = registration_present ? -98 : 0;
    *rsrq = registration_present ? -15 : 0;
    *sinr = registration_present ? 0 : WUPS_NET_SINR_UNKNOWN;
}
static void modem_support_snapshot(bool periodic) { (void)periodic; }
static void emit_net_status(uint8_t state, int8_t rssi,
                            int8_t rsrp, int8_t rsrq, int8_t sinr)
{
    (void)rsrp; (void)rsrq; (void)sinr;
    s_ns_last_state = state;
    s_ns_last_rssi = rssi;
    s_ns_last_emit_s = now_s();
}

#ifdef BASELINE
#include "test_modem_recovery_baseline.inc"
#else
#include "supervisor.inc"
#endif

static void run_case(enum scenario scenario, const char *name,
                     int expected_action, int expected_trips,
                     bool expected_connected, bool expected_proof)
{
    current_scenario = scenario;
    simulated_seconds = 1000;
    registration_seen_s = 0;
    registration_present = scenario != REGISTRATION_RETURNS && scenario != REGISTRATION_GRACE_EXPIRES;
    probes = health_reads = 0;
    ota_active = ppp_loss_pending = mqtt_restored = false;
    auth_refused = false;
    backend_restored = false;
    active_backend = (scenario == HTTP_DEAD || scenario == HTTP_RETURNS_IN_FINAL_SNAPSHOT)
        ? WUPS_BACKEND_MODE_HTTP :
        (scenario == ARKIV_DEAD || scenario == ARKIV_RETURNS_IN_FINAL_SNAPSHOT)
        ? WUPS_BACKEND_MODE_ARKIV : WUPS_BACKEND_MODE_MQTT;
    horizon = scenario == REGISTRATION_GRACE_EXPIRES ? 1500 : 1335;
    s_ppp_state = (ppp_event_state_t){.phase = PPP_UP, .attempt = 1, .sequence = 1, .generation = 1};
#ifdef FIXED
    s_ota_active = s_validation_busy = s_modem_recovery_busy = false;
    lock_depth = lock_count = 0;
#endif
    s_cmux_active = s_iccid_known = true;
    s_alert_active = false;
    s_uplink_trips = scenario == SECOND_DEAD_TRIP ? 1 : 0;
    s_fails_since_ok = s_alert_clear_pending = 0;
    s_fail_stage = MODEM_FAIL_NONE;
    s_ns_last_rssi = 0;
    s_ns_last_state = NET_STATE_PPP_UP;
    s_ns_last_emit_s = 1000;
    assert(mqtt_health_init(&health, now_ms(), NULL));
    if (scenario == MQTT_CONNECTED_WITHOUT_PROOF)
        mqtt_health_on_connected(&health, now_ms());
    observed_action = -1; /* simulation horizon, no supervisor teardown */
    if (setjmp(simulation_end) == 0) observed_action = supervise_uplink();
    mqtt_health_snapshot_t final_health;
    mqtt_health_poll(&health, now_ms(), &final_health);
    assert(observed_action == expected_action);
    assert(s_uplink_trips == expected_trips);
    assert(final_health.connected == expected_connected);
    assert(final_health.proof_fresh == expected_proof);
    if (scenario == REGISTRATION_RETURNS && observed_action != -1)
        assert(registration_seen_s == 1300 && now_s() - registration_seen_s == 8);
#ifdef FIXED
    assert(lock_depth == 0);
    if (scenario == REGISTRATION_GRACE_EXPIRES) {
        assert(registration_seen_s == 1300);
        assert(now_s() >= 1390 && now_s() < 1450);
    }
    if (observed_action == TEARDOWN_MODULE_RESET || observed_action == TEARDOWN_PWRCYCLE) {
        assert(s_modem_recovery_busy && s_ppp_state.phase == PPP_STOPPING);
        assert(!fw_ota_try_modem_recovery(modem_recovery_commit_backend, NULL));
    } else assert(!s_modem_recovery_busy);
#endif
    printf("PASS %-38s t=%us action=%s trips=%d connected=%d proof=%d probes=%u",
           name, now_s(), observed_action == -1 ? "NONE" :
           observed_action == TEARDOWN_MODULE_RESET ? "MODULE_RESET" :
           observed_action == TEARDOWN_PWRCYCLE ? "PWRKEY" : "PPP_LOSS",
           s_uplink_trips, final_health.connected, final_health.proof_fresh, probes);
    if (registration_seen_s) printf(" registration_age=%us", now_s() - registration_seen_s);
    printf("\n");
}

int main(void)
{
    (void)s_ppp_event_lock;
    run_case(DEAD_LINK, "persistent dead link control", TEARDOWN_MODULE_RESET, 1, false, false);
#ifdef BASELINE
    run_case(REGISTRATION_RETURNS, "LTE recovered 8s before reset", TEARDOWN_MODULE_RESET, 1, false, false);
    run_case(MQTT_RETURNS_IN_PROBE, "MQTT/PUBACK during DNS probes", TEARDOWN_MODULE_RESET, 1, true, true);
    run_case(MQTT_RETURNS_IN_FINAL_SNAPSHOT, "MQTT/PUBACK during final AT snapshot", TEARDOWN_MODULE_RESET, 1, true, true);
#else
    run_case(REGISTRATION_RETURNS, "LTE gets bounded recovery grace", -1, 0, false, false);
    run_case(REGISTRATION_GRACE_EXPIRES, "dead link after bounded LTE grace", TEARDOWN_MODULE_RESET, 1, false, false);
    run_case(MQTT_RETURNS_IN_PROBE, "MQTT/PUBACK during DNS probes", -1, 0, true, true);
    run_case(MQTT_RETURNS_IN_FINAL_SNAPSHOT, "MQTT/PUBACK during final AT snapshot", -1, 0, true, true);
    run_case(MQTT_CONNECTED_IN_FINAL_SNAPSHOT, "CONNECTED during final AT snapshot", -1, 0, true, false);
    run_case(WORKER_STALLED_IN_FINAL_SNAPSHOT, "worker stalls during final AT snapshot", -1, 0, false, false);
    run_case(MQTT_PROOF_AT_COMMIT, "MQTT proof at commit boundary", -1, 0, true, true);
    run_case(MQTT_REPLACED_DURING_PROBE, "changed MQTT generation cancels decision", -1, 0, false, false);
    run_case(PPP_REPLACED_DURING_PROBE, "changed PPP observation cancels decision", -1, 0, false, false);
    run_case(AUTH_REFUSED_IN_FINAL_SNAPSHOT, "auth refusal during final AT snapshot", -1, 0, false, false);
    run_case(OTA_IN_FINAL_SNAPSHOT, "OTA during final AT snapshot", -1, 0, false, false);
    run_case(PPP_LOST_IN_FINAL_SNAPSHOT, "PPP loss during final AT snapshot", TEARDOWN_NORMAL, 0, false, false);
    run_case(HTTP_DEAD, "HTTP dead path still escalates", TEARDOWN_MODULE_RESET, 1, false, false);
    run_case(HTTP_RETURNS_IN_FINAL_SNAPSHOT, "HTTP recovery during final AT snapshot", -1, 0, false, false);
    run_case(ARKIV_DEAD, "Arkiv dead path still escalates", TEARDOWN_MODULE_RESET, 1, false, false);
    run_case(ARKIV_RETURNS_IN_FINAL_SNAPSHOT, "Arkiv recovery during final AT snapshot", -1, 0, false, false);
#endif
    run_case(MQTT_RETURNS_IN_PERIODIC_DIAGNOSTICS, "earlier MQTT recovery control", -1, 0, true, true);
    run_case(OTA_STARTS_IN_PROBE, "OTA overlap control", -1, 0, false, false);
    run_case(PPP_LOST_IN_PROBE, "real PPP loss control", TEARDOWN_NORMAL, 0, false, false);
    run_case(INTERNET_ANSWERS, "reachable Internet control", -1, 0, false, false);
    run_case(MQTT_CONNECTED_WITHOUT_PROOF, "MQTT owner responsibility control", -1, 0, true, false);
    run_case(SECOND_DEAD_TRIP, "persistent second trip control", TEARDOWN_PWRCYCLE, 2, false, false);
#ifdef BASELINE
    puts("10 baseline scenarios reproduced. PASS means unwanted resets were reproduced.");
#else
    puts("Fixed supervisor scenarios PASS: fresh recovery wins; dead paths still escalate.");
#endif
    return 0;
}
