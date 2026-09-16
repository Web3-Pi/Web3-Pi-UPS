#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "modem_recovery.h"

#define REGISTERED "+CEREG: 0,5\r\nOK\r\n"
#define SEARCHING "+CEREG: 0,2\r\nOK\r\n"

static void parser(void)
{
    bool registered = false;
    assert(modem_recovery_parse_cereg(REGISTERED, &registered) && registered);
    assert(modem_recovery_parse_cereg("AT+CEREG?\r\n+CEREG: 2,1,\"1234\",\"5678\",7\r\nOK\r\n", &registered) && registered);
    assert(modem_recovery_parse_cereg(SEARCHING, &registered) && !registered);
    const char *invalid[] = {
        "", "OK\r\n", "+CEREG: 5\r\nOK\r\n", "+CEREG: 0,5",
        "+CEREG: 0,5\r\nERROR\r\n", "+CEREG: 0,5junk\r\nOK\r\n",
        "+CEREG: 0,500000000000000000000000\r\nOK\r\n",
        "+CEREG: 0,-1\r\nOK\r\n", "+CEREG: 0,11\r\nOK\r\n",
        "+CEREG: 9,5\r\nOK\r\n", "+CEREG: 0,\r\nOK\r\n",
        "+CEREG: 0,5\r\n+CEREG: 0,2\r\nOK\r\n",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!modem_recovery_parse_cereg(invalid[i], &registered));
}

static void one_budget_per_outage(void)
{
    modem_recovery_t state;
    modem_recovery_init(&state);
    assert(!modem_recovery_observe_registration(&state, REGISTERED, 1));
    assert(!modem_recovery_grace_active(&state, 1));
    assert(!modem_recovery_observe_registration(&state, SEARCHING, 100));
    /* Missing/invalid data is not a new registration edge. */
    assert(!modem_recovery_observe_registration(&state, "+CEREG: 0,5\r\n", 110));
    assert(modem_recovery_observe_registration(&state, REGISTERED, 299));
    assert(modem_recovery_grace_active(&state, 300));
    assert(!modem_recovery_observe_registration(&state, REGISTERED, 330));
    assert(!modem_recovery_observe_registration(&state, SEARCHING, 350));
    assert(!modem_recovery_observe_registration(&state, REGISTERED, 370));
    assert(modem_recovery_grace_active(&state, 388));
    assert(!modem_recovery_grace_active(&state, 389));
    /* Flapping even long after the deadline cannot extend this incident. */
    assert(!modem_recovery_observe_registration(&state, SEARCHING, 500));
    assert(!modem_recovery_observe_registration(&state, REGISTERED, 600));
    assert(!modem_recovery_grace_active(&state, 600));
    modem_recovery_backend_healthy(&state);
    assert(!modem_recovery_observe_registration(&state, SEARCHING, 700));
    assert(modem_recovery_observe_registration(&state, REGISTERED, 800));
    assert(modem_recovery_grace_active(&state, 801));
}

static void uptime_wrap(void)
{
    modem_recovery_t state;
    modem_recovery_init(&state);
    assert(!modem_recovery_observe_registration(&state, SEARCHING, UINT32_MAX - 40));
    assert(modem_recovery_observe_registration(&state, REGISTERED, UINT32_MAX - 20));
    assert(modem_recovery_grace_active(&state, 68));
    assert(!modem_recovery_grace_active(&state, 69));
}

int main(void)
{
    parser();
    one_budget_per_outage();
    uptime_wrap();
    puts("modem recovery: CEREG validation, bounded grace, flapping and wrap PASS");
    return 0;
}
