#include "modem_signal.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int8_t modem_rssnr_to_sinr(long rssnr)
{
    return rssnr >= 0 && rssnr <= 25
        ? (int8_t)(2 * rssnr - 20) : WUPS_NET_SINR_UNKNOWN;
}

/* strtol alone treats an empty/non-numeric field as zero and accepts a
 * numeric prefix. Both are unsafe for RSSNR, where zero is a real reading. */
static bool field_integer(const char *start, const char *end, long *value)
{
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    if (start == end) return false;
    errno = 0;
    char *parsed_end;
    long parsed = strtol(start, &parsed_end, 10);
    if (errno == ERANGE || parsed_end != end || parsed_end == start) return false;
    *value = parsed;
    return true;
}

void modem_signal_parse_cpsi(const char *reply, int8_t *rsrp_out,
                            int8_t *rsrq_out, int8_t *sinr_out)
{
    if (!reply || !rsrp_out || !rsrq_out || !sinr_out) return;
    for (const char *line = reply; *line;) {
        const char *end = line + strcspn(line, "\r\n");
        const char *start = line;
        while (start < end && isspace((unsigned char)*start)) ++start;
        if (end - start >= 6 && strncmp(start, "+CPSI:", 6) == 0) {
            start += 6;
            while (start < end && isspace((unsigned char)*start)) ++start;
            if (end - start < 4 || strncmp(start, "LTE", 3) != 0 ||
                (start[3] != ',' && !isspace((unsigned char)start[3]))) return;

            /* Preserve empty fields and their positions; stop at this line
             * so unrelated comma-bearing URCs cannot become signal values. */
            const char *fields[20];
            size_t count = 0;
            for (const char *p = start; p < end; ++p) {
                if (*p != ',') continue;
                if (count == sizeof(fields) / sizeof(fields[0])) return;
                fields[count++] = p + 1;
            }
            /* Documented LTE CAT-M/NB-IoT reply has exactly 14 fields.
             * A truncated reply ending at a numeric bandwidth/cell field
             * must not reinterpret that value as an apparently valid RSSNR. */
            if (count != 13) return;

            long value;
            if (field_integer(fields[count - 4], fields[count - 3] - 1, &value)) {
                if (value <= -35 && value >= -340) value /= 10;
                if (value <= -3 && value >= -34) *rsrq_out = (int8_t)value;
            }
            if (field_integer(fields[count - 3], fields[count - 2] - 1, &value)) {
                if (value <= -440 && value >= -1560) value /= 10;
                if (value <= -44 && value >= -128) *rsrp_out = (int8_t)value;
            }
            if (field_integer(fields[count - 1], end, &value)) {
                int8_t sinr = modem_rssnr_to_sinr(value);
                if (sinr != WUPS_NET_SINR_UNKNOWN) *sinr_out = sinr;
            }
            return;
        }
        line = end;
        while (*line == '\r' || *line == '\n') ++line;
    }
}
