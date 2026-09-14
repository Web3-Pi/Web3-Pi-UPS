#pragma once

#include <stdint.h>
#include "wups_proto.h"

/* SIM7070/SIM7080/SIM7090 AT Command Manual V1.05, CPSI RSSNR table:
 * 0 -> -20 dB, 1 -> -18 dB, ..., 25 -> 30 dB. */
int8_t modem_rssnr_to_sinr(long rssnr);

/* Parse only the LTE +CPSI line's ...,RSRQ,RSRP,RSSI,RSSNR tail.
 * Each valid field updates its output independently; absent/invalid fields
 * leave outputs untouched. Initialize RSRP/RSRQ to 0 and SINR to
 * WUPS_NET_SINR_UNKNOWN before every poll to avoid retaining old readings. */
void modem_signal_parse_cpsi(const char *reply, int8_t *rsrp_out,
                            int8_t *rsrq_out, int8_t *sinr_out);
