#include "modem_recovery.h"
#include "modem_radio_policy.h"

#include <stddef.h>
#include <string.h>

void modem_recovery_init(modem_recovery_t *state)
{
    *state = (modem_recovery_t){0};
}

static void skip_space(const char **p)
{
    while (**p == ' ' || **p == '\t') ++*p;
}

/* Bounded decimal parsing: reject signs, overflow, and partial fields. */
static bool field(const char **p, unsigned maximum, unsigned *value)
{
    skip_space(p);
    if (**p < '0' || **p > '9') return false;
    unsigned n = 0;
    do {
        n = n * 10u + (unsigned)(*(*p)++ - '0');
        if (n > maximum) return false;
    } while (**p >= '0' && **p <= '9');
    skip_space(p);
    *value = n;
    return true;
}

bool modem_recovery_parse_cereg(const char *reply, bool *registered)
{
    if (!reply || !registered ||
        modem_radio_response_status(reply, strlen(reply)) != MODEM_RADIO_RESPONSE_OK)
        return false;
    unsigned found = 0, status = 0;
    const char *line = reply;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) break;
        const char *p = line;
        skip_space(&p);
        if (strncmp(p, "+CEREG:", 7) == 0) {
            p += 7;
            unsigned mode;
            if (++found != 1 || !field(&p, 5, &mode) || *p++ != ',' ||
                !field(&p, 10, &status)) return false;
            /* Optional location/AcT fields follow a comma. Their content is
             * diagnostic only; the two complete numeric fields are required. */
            if (*p != ',' && *p != '\r' && *p != '\n') return false;
        }
        line = end + 1;
    }
    if (found != 1) return false;
    *registered = status == 1 || status == 5;
    return true;
}

bool modem_recovery_observe_registration(modem_recovery_t *state,
                                        const char *reply, uint32_t now_s)
{
    bool registered;
    if (!modem_recovery_parse_cereg(reply, &registered)) return false;
    bool returned = state->registration_known && !state->registered && registered;
    state->registration_known = true;
    state->registered = registered;
    if (!returned || state->grace_used) return false;
    state->grace_used = true;
    state->grace_started_s = now_s;
    return true;
}

bool modem_recovery_grace_active(const modem_recovery_t *state, uint32_t now_s)
{
    return state->grace_used &&
        (uint32_t)(now_s - state->grace_started_s) < MODEM_RECOVERY_REG_GRACE_S;
}

void modem_recovery_backend_healthy(modem_recovery_t *state)
{
    state->grace_used = false;
    state->grace_started_s = 0;
}
