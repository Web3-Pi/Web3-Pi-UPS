#ifndef TRANSPORT_DIAG_H
#define TRANSPORT_DIAG_H

/* Sparse, task-context report (normally every 30 s). Counters reset after
 * each snapshot; wrappers never log, allocate, delay or change transport.
 * RX peaks are sampled lower bounds; zero samples means unavailable, not
 * zero occupancy. Call wall time includes blocking and
 * preemption, not CPU busy time. UART event counts cover consumed events,
 * not all ISR events; UART2 has no event queue in the current application.
 * No-op when CONFIG_WUPS_PERF_DIAG is disabled. */
void transport_diag_log(void);

#endif
