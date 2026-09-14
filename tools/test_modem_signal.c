#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "modem_signal.h"

_Static_assert(sizeof(wups_net_status_v1_t) == 20, "legacy v1 size");
_Static_assert(sizeof(wups_net_status_v2_t) == 30, "legacy v2 size");
_Static_assert(sizeof(wups_net_status_v3_t) == 31, "v3 size");
_Static_assert(offsetof(wups_net_status_v3_t, sinr_dB) == 30, "SINR offset");

/* cJSON boundary captures exactly the keys/values emitted by the production
 * helper extracted from http_backend.c, without needing the ESP-IDF SDK. */
typedef struct { const char *key; double value; } entry_t;
typedef struct { entry_t entries[8]; size_t count; bool net; } cJSON;
static cJSON *cJSON_AddObjectToObject(cJSON *root, const char *key)
{
    assert(strcmp(key, "net") == 0);
    root->net = true;
    return root;
}
static void cJSON_AddNumberToObject(cJSON *object, const char *key, double value)
{
    assert(object->count < sizeof(object->entries) / sizeof(object->entries[0]));
    object->entries[object->count++] = (entry_t){key, value};
}
#include "net_telemetry.inc"

static const entry_t *entry(const cJSON *object, const char *key)
{
    for (size_t i = 0; i < object->count; ++i)
        if (strcmp(object->entries[i].key, key) == 0) return &object->entries[i];
    return NULL;
}

static void parse_case(const char *tail, int8_t expected_sinr)
{
    /* Actual observed field layout, anonymized cell/operator identifiers. */
    char reply[768];
    int n = snprintf(reply, sizeof(reply), "\r\n+URC: LTE,1,2,3,4\r\n"
        "+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,%s"
        "\r\n+CEREG: 2,5,\"1234\",\"12345\",7\r\nOK\r\n", tail);
    assert(n > 0 && (size_t)n < sizeof(reply));
    int8_t rsrp = 0, rsrq = 0, sinr = WUPS_NET_SINR_UNKNOWN;
    modem_signal_parse_cpsi(reply, &rsrp, &rsrq, &sinr);
    assert(rsrp == -97 && rsrq == -16 && sinr == expected_sinr);
}

static void test_parser(void)
{
    const struct { long code; int8_t db; } cases[] = {
        {0, -20}, {7, -6}, {8, -4}, {10, 0}, {12, 4}, {25, 30},
        {-1, WUPS_NET_SINR_UNKNOWN}, {26, WUPS_NET_SINR_UNKNOWN},
        {99, WUPS_NET_SINR_UNKNOWN}, {LONG_MIN, WUPS_NET_SINR_UNKNOWN},
        {LONG_MAX, WUPS_NET_SINR_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        assert(modem_rssnr_to_sinr(cases[i].code) == cases[i].db);
    parse_case("-16,-97,-67,7", -6);
    parse_case("-16,-97,-67, 10 \t", 0);
    parse_case("-160,-970,-670,12", 4);
    parse_case("-16,-97,-67,0", -20);
    parse_case("-16,-97,-67,25", 30);
    const char *invalid[] = {"", " ", "unknown", "7junk", "7.0", "7 0",
                            "-1", "26", "99", "9999999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char tail[128];
        snprintf(tail, sizeof(tail), "-16,-97,-67,%s", invalid[i]);
        parse_case(tail, WUPS_NET_SINR_UNKNOWN);
    }
    const char *absent[] = {
        "", "OK\r\n", "+CPSI: NO SERVICE\r\n+URC: LTE,1,2,-16,-97,-67,7\r\n",
        "+CPSI: GSM,Online,0,0,-16,-97,-67,7\r\n",
        "+CPSI: NOT LTE,Online,0,0,-16,-97,-67,7\r\n",
        "+CPSI: LTE CAT-M1,Online\r\n", "+CPSI: LTE\r\n",
        "+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3\r\nOK\r\n",
        "+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-16,-97,-67\r\n",
        "+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-16,-97,-67,7,extra\r\n",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); ++i) {
        int8_t rsrp = 0, rsrq = 0, sinr = WUPS_NET_SINR_UNKNOWN;
        modem_signal_parse_cpsi(absent[i], &rsrp, &rsrq, &sinr);
        assert(rsrp == 0 && rsrq == 0 && sinr == WUPS_NET_SINR_UNKNOWN);
    }
    int8_t rsrp = 0, rsrq = 0, sinr = WUPS_NET_SINR_UNKNOWN;
    modem_signal_parse_cpsi("+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,,-97,-67,7", &rsrp, &rsrq, &sinr);
    assert(rsrp == -97 && rsrq == 0 && sinr == -6); /* empty field preserves positions */
    rsrp = rsrq = 0;
    modem_signal_parse_cpsi("+CPSI: LTE CAT-M1,Online,001-01,0x1234,12345,123,EUTRAN-BAND20,6200,3,3,-16x,-97junk,-67,10", &rsrp, &rsrq, &sinr);
    assert(rsrp == 0 && rsrq == 0 && sinr == 0);
    puts("PASS CPSI: negative/zero/positive SINR, strict numeric input, no-service/URC isolation, legacy RSRP/RSRQ");
}

static void test_wire_and_http(void)
{
    const wups_net_status_v3_t v3 = {
        .version = 3, .state = 4, .rssi_dBm = -67, .rsrp_dBm = -97, .rsrq_dB = -16,
        .reserved = 1, .errors = 0x1234, .ip_addr = 0x12345678,
        .bytes_tx = 123, .bytes_rx = 456, .sys_frames_rx = 789,
        .sys_resync = 7, .sys_link_age_s = 23, .sinr_dB = -6,
    };
    /* Compare to an independently populated old struct: every legacy byte,
     * including diagnostic offsets, must survive the append unchanged. */
    const wups_net_status_v2_t v2 = {
        .version = 3, .state = 4, .rssi_dBm = -67, .rsrp_dBm = -97, .rsrq_dB = -16,
        .reserved = 1, .errors = 0x1234, .ip_addr = 0x12345678,
        .bytes_tx = 123, .bytes_rx = 456, .sys_frames_rx = 789,
        .sys_resync = 7, .sys_link_age_s = 23,
    };
    assert(memcmp(&v3, &v2, sizeof(v2)) == 0);
    uint8_t wire[sizeof(v3)];
    memcpy(wire, &v3, sizeof(wire));
    for (size_t len = 0; len <= sizeof(wire); ++len) {
        cJSON object = {0};
        add_net_telemetry(&object, wire, len);
        assert(object.net == (len >= 20));
        assert((entry(&object, "sinr_db") != NULL) == (len >= 31));
    }
    for (unsigned version = 1; version <= 4; ++version) {
        const int values[] = {-128, -21, -20, -6, 0, 30, 31, 127};
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            wire[0] = (uint8_t)version;
            wire[30] = (uint8_t)values[i];
            cJSON object = {0};
            add_net_telemetry(&object, wire, sizeof(wire));
            assert(entry(&object, "rssi_dbm")->value == -67);
            assert(entry(&object, "bytes_rx")->value == 456);
            bool valid = version == 3 && values[i] >= -20 && values[i] <= 30;
            const entry_t *sinr = entry(&object, "sinr_db");
            assert((sinr != NULL) == valid);
            if (sinr) assert(sinr->value == values[i]);
        }
    }
    puts("PASS wire/HTTP: v1/v2 prefix compatibility, offset 30, truncation safety, measured zero retained, unknown SINR omitted");
}

int main(void)
{
    test_parser();
    test_wire_and_http();
    return 0;
}
