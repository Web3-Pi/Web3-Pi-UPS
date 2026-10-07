# ESP32 0.8.18: backend-outage clock survives modem resets (issue #20)

## Problem

The uplink watchdog in `supervise_uplink()` (`main/modem.c`) kept its
"last healthy uplink" timestamp in a local variable, armed on every entry.
Every watchdog trip (AT+CFUN=1,1 reset or PWRKEY power-cycle) ends the
supervision session, so a continuous backend outage in which Internet probes
fail intermittently restarted the clock on every PPP recovery. The
`NO UPLINK` hold alert (`UPLINK_HOLD_ALERT_S`, 1800 s after the last healthy
uplink) was therefore never reached; the only alarm came from the separate
four-failure escalation, which classified the outage as `NO NETWORK` even
though PPP came up every time. Reported with hardware evidence in
[issue #20](https://github.com/Web3-Pi/Web3-Pi-UPS/issues/20).

## Change

- A boot-lifetime backend-outage clock (`s_uplink_last_healthy_s`) now holds
  the last second the active backend's uplink was demonstrably healthy. The
  `NO UPLINK` hold-alert deadline runs on it. The clock is refreshed by a
  healthy uplink and frozen during an OTA transfer, exactly like the
  per-session timer.
- A supervision session that starts mid-escalation (watchdog trips pending,
  or the zero-trip hold alert active) inherits the clock. A session with no
  escalation pending (first PPP of the boot, or PPP restored after a genuine
  network outage) starts a fresh clock, mirroring the existing GOT_IP
  bookkeeping. Routine backend redeploys therefore still do not beep.
- The per-session timer is unchanged and still paces the reset ladder: a
  fresh PPP gets `UPLINK_DEAD_SECS` (300 s) to prove itself before the next
  trip.
- A watchdog trip no longer downgrades an established `NO UPLINK`
  classification to `NO NETWORK`. The reset ladder and the four-failure
  escalation keep running; only a healthy uplink clears the alert.
- Supervisor logs now report both the per-session dead time and the total
  backend outage.

Behaviour is otherwise unchanged. In the all-probes-fail case (no DNS
answer in any session) the firmware still resets the modem and reports
`NO NETWORK` after four failures, which remains the honest classification
when the Internet is unreachable.

## Verification

`tools/test_modem_recovery_supervisor.py` runs the actual `supervise_uplink()`
against synthetic clocks and probes, in the sealed pre-#17 baseline and the
current code. New multi-session scenarios replay the issue #20 sequence
(trip, trip, hold, trip, recovery):

| Scenario | Baseline | Fixed |
|---|---|---|
| Backend dead, probes fail twice then answer | no alert 2434 s after the last healthy uplink | `NO UPLINK` 1814 s after the last healthy uplink, across two resets |
| Probe timeout after the alert | banner flips to `NO NETWORK` | `NO UPLINK` kept, trip #3 still power-cycles |
| Backend returns | escalation cleared | escalation cleared, clock refreshed |
| Fresh session, no escalation pending | — | clock re-armed, no alert |
| Hold alert survives a genuine PPP re-dial | — | clock inherited, banner refreshed every tick |

The existing #17 scenarios are unchanged and still pass. The host harness
exercises control flow with synthetic timing; it is not a substitute for the
reviewer's hardware reproduction (HTTP endpoint down, probes answering then
failing, PPP reconnecting). Release images are built as for 0.8.17 with
`-DPROJECT_VER=0.8.18-1nce` / `0.8.18-sensor`.
