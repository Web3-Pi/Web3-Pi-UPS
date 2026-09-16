#ifndef WUPS_MODEM_RECOVERY_H
#define WUPS_MODEM_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

/* One opportunity per backend outage. This is a recovery-policy parameter,
 * not publication proof or an LTE protocol timer. */
#define MODEM_RECOVERY_REG_GRACE_S 90u

typedef struct {
    bool registration_known;
    bool registered;
    bool grace_used;
    uint32_t grace_started_s;
} modem_recovery_t;

void modem_recovery_init(modem_recovery_t *state);
/* Strictly parse a complete CEREG query response. Invalid/ambiguous reads
 * are unknown; they cannot create an unregistered -> registered edge. */
bool modem_recovery_parse_cereg(const char *reply, bool *registered);
/* Returns true only for the first confirmed return of registration since
 * backend health last recovered. Repeated reads/flapping cannot renew it. */
bool modem_recovery_observe_registration(modem_recovery_t *state,
                                        const char *reply, uint32_t now_s);
bool modem_recovery_grace_active(const modem_recovery_t *state, uint32_t now_s);
/* Preserve radio history; only actual backend health rearms the allowance. */
void modem_recovery_backend_healthy(modem_recovery_t *state);

#endif
