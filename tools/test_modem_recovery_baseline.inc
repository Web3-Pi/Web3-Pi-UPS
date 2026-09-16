/* Sealed #17 baseline, Web3-Pi-UPS@64453e2e964ee6d258c9d2c0a6f830db128e4625.
 * Original supervisor/health/snapshot functions, byte-preserved. */
static void modem_network_snapshot(const char *reason)
{
    char reply[MODEM_RADIO_RESPONSE_CAPACITY];
    ESP_LOGI(MODEM_TAG, "network snapshot: %s", reason);
    (void)radio_at(NULL, "AT+CEREG?", reply, sizeof(reply), 3000);
    (void)radio_at(NULL, "AT+COPS?", reply, sizeof(reply), 3000);
    (void)radio_at(NULL, "AT+CPSI?", reply, sizeof(reply), 3000);
}

static void uplink_health(bool *up, bool *wd_ok, bool *ota_ok)
{
    switch (backend_mode_get()) {
    case WUPS_BACKEND_MODE_MQTT:
        /* CONNECT alone is not evidence that an uplink publication reached
         * the broker. The owner also invalidates proof during OTA transfer. */
        *up = mqtt_publication_proof_fresh();
        *wd_ok = *up;
        *ota_ok = *up;
        if (!*up && mqtt_auth_refused()) {
            /* Broker reachable, TLS fine, CONNACK refused our credentials:
             * the unit is merely unclaimed (or its secret was rotated) and
             * the network is provably healthy — a modem reset cannot help.
             * Same posture as unclaimed Arkiv below; without it factory
             * units beeped "NO UPLINK" and reset the modem forever while
             * simply waiting for their first claim (2026-08-21). */
            *wd_ok = true;
        }
        break;
    case WUPS_BACKEND_MODE_HTTP: {
        if (!http_backend_is_configured()) {
            /* Endpoint not set yet (guided VPS setup / net.config pending) —
             * same "nothing to supervise" posture as unclaimed Arkiv below,
             * or the watchdog would reset a healthy modem forever. */
            *up = false;
            *wd_ok = true;
            *ota_ok = true;     /* legacy: UART-flashed update must confirm */
            break;
        }
        uint32_t last = http_backend_last_success_s();
        *up = last != 0 && (now_s() - last) <= HTTP_UPLINK_FRESH_SECS;
        *wd_ok = *up;
        *ota_ok = *up;
        break;
    }
    case WUPS_BACKEND_MODE_ARKIV:
        if (cmdauth_arkiv_claim_state() != ARKIV_CLAIMED) {
            *up = false;
            *wd_ok = true;      /* unclaimed: no uplink expected, never trip */
            *ota_ok = true;     /* legacy: UART-flashed update must confirm */
        } else {
            /* RPC round-trips prove the uplink (telemetry every 30 s, cmd
             * poll every 5 s while the WS is down). The WS subscription is
             * only the low-latency cmd push — a refused/404 WS (e.g. a
             * placeholder-token CI build; field incident 2026-08-16: the
             * old `*wd_ok = ws_subscribed` put the unit in a permanent
             * CFUN/PWRKEY reset cycle with a "NO UPLINK" banner while
             * writes were landing on-chain) must never trip the modem
             * watchdog on its own. */
            uint32_t last = arkiv_rpc_last_success_s();
            bool rpc_fresh =
                last != 0 && (now_s() - last) <= ARKIV_UPLINK_FRESH_SECS;
            *up = rpc_fresh || arkiv_ws_subscribed();
            *wd_ok = *up;
            *ota_ok = *up;
        }
        break;
    default:
        *up = false;
        *wd_ok = true;          /* unknown mode — never trip */
        *ota_ok = true;
        break;
    }
}

static teardown_action_t supervise_uplink(void)
{
    uint32_t last_healthy_s  = now_s();  /* watchdog timer starts at GOT_IP */
    uint32_t last_clear_s    = now_s();  /* GOT_IP path just sent a clear    */
    uint32_t last_radio_snapshot_s = now_s();
    uint32_t last_radio_poll_s = now_s();
    bool radio_snapshot_pending = false;
    int8_t last_rsrp = 0, last_rsrq = 0, last_sinr = WUPS_NET_SINR_UNKNOWN;
    uint32_t last_probe_ok_s = 0;        /* internet-probe OK verdict cache  */

    for (;;) {
        /* MQTT pacing belongs to its independent owner; this task only
         * supervises PPP and wakes early to observe a reported MQTT drop. */
        EventBits_t bits = xEventGroupWaitBits(s_modem_evt,
                                               EVT_PPP_CHANGED | EVT_MQTT_DOWN,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(PPP_SUPERVISE_TICK_MS));
        if (ppp_events_take_loss()) {
            ESP_LOGW(MODEM_TAG, "PPP loss: last radio snapshot %us ago; see preceding AT replies",
                     (unsigned)(now_s() - last_radio_snapshot_s));
            ESP_LOGW(MODEM_TAG, "PPP link lost — tearing down DCE");
            return TEARDOWN_NORMAL;
        }
        if (bits & EVT_MQTT_DOWN) {
            /* Early wake only — the MQTT owner performs recovery. */
            radio_snapshot_pending = true;
        }
        uint32_t now = now_s();

        /* Fast idempotent request only. SDK start/retry/reconnect/revive and
         * their backoff execute in the MQTT owner, never in the PPP task. */
        if (backend_mode_get() == WUPS_BACKEND_MODE_MQTT && s_iccid_known) {
            (void)mqtt_client_start();
        }

        /* OTA-1 — while a firmware download runs, the uplink is deliberately
         * busy (MQTT may go quiet under the TLS transfer) and a watchdog
         * teardown would kill it. Freeze the watchdog timer, count no trips,
         * escalate nothing; genuine PPP loss (the EVT bits above) still ends
         * the session and fails the download on its own. */
        if (fw_ota_in_progress()) {
            last_healthy_s = now_s();
            continue;
        }

        bool uplink_up = false, wd_healthy = false, ota_proof = false;
        uplink_health(&uplink_up, &wd_healthy, &ota_proof);
        if (wd_healthy) {
            last_healthy_s = now;
            /* OTA-1 rollback — first demonstrably healthy uplink marks a
             * pending-verify OTA image valid (one-shot, no-op otherwise).
             * Gated on ota_proof, NOT wd_ok: an auth-refused unit must not
             * confirm an image on network reachability alone. */
            if (ota_proof) {
                fw_ota_mark_uplink_healthy();
            }
            if (s_uplink_trips != 0 || s_alert_active) {
                /* Deferred recovery bookkeeping (see the GOT_IP branch):
                 * only a demonstrably healthy uplink ends an escalation.
                 * Also covers the hold-alert (backend outage) case, which
                 * raises the alert with zero trips. */
                ESP_LOGI(MODEM_TAG, "uplink healthy again — resetting trip escalation");
                s_uplink_trips = 0;
                s_fails_since_ok = 0;
                s_fail_stage = MODEM_FAIL_NONE;
                if (s_alert_active) {
                    s_alert_active = false;
                    modem_ui_alert_clear();
                    s_alert_clear_pending = ALERT_CLEAR_RESENDS;
                    last_clear_s = now;
                    ESP_LOGI(MODEM_TAG, "uplink recovered — clearing OLED/buzzer alert");
                }
            }
        } else if (s_alert_active && s_fail_stage == MODEM_FAIL_UPLINK) {
            /* Hold alert survived a PPP re-dial (last_healthy_s re-armed, so
             * the hold branch won't re-assert for another UPLINK_DEAD_SECS):
             * keep refreshing it here, or the RP2040's 5-min non-refresh
             * auto-clear would drop the banner mid-outage. */
            modem_ui_alert(modem_fail_msg(MODEM_FAIL_UPLINK));
        }

        /* Signal-quality poll — CMUX sessions only (the DATA fallback has no
         * AT channel while PPP runs). RSRP/RSRQ stay 0 and SINR stays -128
         * (unknown) unless AT+CPSI? supplies a valid measurement. */
        int8_t rssi = s_ns_last_rssi, rsrp = last_rsrp, rsrq = last_rsrq;
        int8_t sinr = last_sinr;
        if (s_cmux_active && now - last_radio_poll_s >= 30) {
            last_radio_poll_s = now;
            int csq = 99, ber = 99;
            if (esp_modem_get_signal_quality(s_dce, &csq, &ber) == ESP_OK) {
                rssi = csq_to_dbm(csq);
            }
            bool support_due = false;
            /* Existing CPSI cadence (30 s); COPS/CEREG once per minute,
             * or on MQTT drop, rate limited to one snapshot per 30 s.
             * No AT is sent on a live plain-DATA PPP channel. */
            if (now - last_radio_snapshot_s >= 60 ||
                (radio_snapshot_pending && now - last_radio_snapshot_s >= 30)) {
                char reply[MODEM_RADIO_RESPONSE_CAPACITY];
                ESP_LOGI(MODEM_TAG, "network snapshot: %s",
                         radio_snapshot_pending ? "MQTT disconnected" : "periodic");
                (void)radio_at(NULL, "AT+CEREG?", reply, sizeof(reply), 3000);
                (void)radio_at(NULL, "AT+COPS?", reply, sizeof(reply), 3000);
                last_radio_snapshot_s = now;
                support_due = true;
                radio_snapshot_pending = false;
            }
            rsrp = rsrq = 0;
            sinr = WUPS_NET_SINR_UNKNOWN;
            poll_cpsi(&rsrp, &rsrq, &sinr);
            last_rsrp = rsrp;
            last_rsrq = rsrq;
            last_sinr = sinr;
            /* Collect the radio snapshot first. Optional APN/PSM/eDRX reads
             * have a bounded batch and yield to OTA/PPP loss per command.
             * Failures are diagnostic only, never a recovery trigger. */
            if (support_due) modem_support_snapshot(true);
        }
        /* Events can arrive while a bounded AT read is in progress. Do not
         * run stale health/probe/reset decisions after optional diagnostics. */
        if (ppp_events_take_loss()) {
            ESP_LOGW(MODEM_TAG, "PPP loss during diagnostics — yielding to recovery");
            return TEARDOWN_NORMAL;
        }
        if (fw_ota_in_progress()) {
            last_healthy_s = now_s();
            continue;
        }
        now = now_s();
        uint8_t state = uplink_up ? NET_STATE_MQTT_UP : NET_STATE_PPP_UP;
        int rssi_delta = (int)rssi - (int)s_ns_last_rssi;
        if (state != s_ns_last_state ||
            abs(rssi_delta) > NET_STATUS_RSSI_DELTA_DB ||
            now - s_ns_last_emit_s >= NET_STATUS_EMIT_PERIOD_S) {
            /* Also anchors the DATA fallback, which has no live AT channel. */
            modem_diag_clock_log("net.status");
            emit_net_status(state, rssi, rsrp, rsrq, sinr);
        }

        /* Belt-and-braces alert-clear re-sends: the single clear sent at
         * GOT_IP rides an unACKed UART frame, so repeat it a few times once
         * the uplink is demonstrably healthy. The RP2040 ignores redundant
         * clears. */
        if (s_alert_clear_pending > 0 && wd_healthy &&
            now - last_clear_s >= ALERT_CLEAR_RESEND_S) {
            modem_ui_alert_clear();
            s_alert_clear_pending--;
            last_clear_s = now;
        }

        /* Preserve the existing disconnected-uplink recovery policy: a DNS
         * response holds escalation; failed probes permit the established
         * reset ladder. Neither result identifies the operator/backend root
         * cause. Connected/degraded MQTT and a stalled worker are held below. */
        if (now - last_healthy_s >= UPLINK_DEAD_SECS) {
            /* A live MQTT session with stale publication proof, or an SDK
             * worker stuck in its own call, is handled by the independent
             * MQTT monitor. A modem reset cannot safely unstick that task.
             * Keep reporting the outage without counting it as healthy. */
            mqtt_health_snapshot_t mqtt_health = {0};
            bool mqtt_owned_fault = false;
            if (backend_mode_get() == WUPS_BACKEND_MODE_MQTT) {
                mqtt_get_health(&mqtt_health);
                mqtt_owned_fault = mqtt_health.connected || mqtt_health.worker_stalled;
            }
            /* The hold must never cover a client that cannot even attempt
             * unless its owner is itself stalled (handled separately above).
             * Otherwise preserve the previous missing-client escalation. */
            bool client_missing = (backend_mode_get() == WUPS_BACKEND_MODE_MQTT &&
                                   !mqtt_sdk_is_started());
            bool inet_ok = (now - last_probe_ok_s < INET_PROBE_CACHE_S);
            if (!mqtt_owned_fault && !inet_ok && !client_missing && inet_probe()) {
                last_probe_ok_s = now;
                inet_ok = true;
                ESP_LOGW(MODEM_TAG,
                         "uplink dead %us but internet probes answer — "
                         "holding modem recovery (cause not yet established)",
                         (unsigned)(now - last_healthy_s));
            }
            if (mqtt_owned_fault || (inet_ok && !client_missing)) {
                /* Long hold (wedged client / marathon backend outage):
                 * surface the alert WITHOUT resetting the modem, so the
                 * unit is never silent forever. Re-asserted every tick —
                 * the RP2040 auto-clears a non-refreshed alert after 5 min
                 * and does not re-beep on a repeated identical text. */
                if (now - last_healthy_s >= UPLINK_HOLD_ALERT_S) {
                    if (!s_alert_active) {
                        ESP_LOGE(MODEM_TAG,
                                 "uplink dead %us (%s) — raising 'NO UPLINK' "
                                 "alert without modem reset",
                                 (unsigned)(now - last_healthy_s),
                                 mqtt_owned_fault ? "MQTT owner/proof degraded"
                                                  : "internet probes answer");
                    }
                    s_fail_stage = MODEM_FAIL_UPLINK;
                    s_alert_active = true;
                    modem_ui_alert(modem_fail_msg(s_fail_stage));
                }
                continue;
            }
            if (ppp_events_take_loss())
                return TEARDOWN_NORMAL;
            if (fw_ota_in_progress()) {
                last_healthy_s = now_s();
                continue;
            }
            if (s_cmux_active) modem_network_snapshot("uplink recovery");
            /* The final AT snapshot can overlap a new OTA or a real loss. */
            if (ppp_events_take_loss())
                return TEARDOWN_NORMAL;
            if (fw_ota_in_progress()) {
                last_healthy_s = now_s();
                continue;
            }
            now = now_s();
            s_uplink_trips++;
            s_fail_stage = MODEM_FAIL_NET;
            s_fails_since_ok++;
            ESP_LOGE(MODEM_TAG,
                     "uplink DEAD for %us with PPP up (zombie PDP?) — trip #%d, "
                     "%s",
                     (unsigned)(now - last_healthy_s), s_uplink_trips,
                     s_uplink_trips >= 2 ? "escalating to PWRKEY power-cycle"
                                         : "resetting module (AT+CFUN=1,1)");
            return s_uplink_trips >= 2 ? TEARDOWN_PWRCYCLE
                                       : TEARDOWN_MODULE_RESET;
        }
    }
}
