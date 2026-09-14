#include "fw_ota_http_policy.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

static void skip_space(const char **p)
{
    while (**p == ' ' || **p == '\t') ++*p;
}

static bool decimal(const char **p, uint32_t *out)
{
    if (**p < '0' || **p > '9') return false;
    uint32_t n = 0;
    do {
        unsigned d = (unsigned)(*(*p)++ - '0');
        if (n > (UINT32_MAX - d) / 10) return false;
        n = n * 10 + d;
    } while (**p >= '0' && **p <= '9');
    *out = n;
    return true;
}

void fw_ota_http_header(fw_ota_http_response_t *r,
                        const char *key, const char *value)
{
    if (!key || !value) { r->invalid = true; return; }
    const char *p = value;
    skip_space(&p);
    if (strcasecmp(key, "Content-Length") == 0) {
        if (++r->lengths != 1 || !decimal(&p, &r->length)) {
            r->invalid = true;
            return;
        }
    } else if (strcasecmp(key, "Content-Range") == 0) {
        if (++r->ranges != 1 || strncmp(p, "bytes ", 6) != 0) {
            r->invalid = true;
            return;
        }
        p += 6;
        if (!decimal(&p, &r->first) || *p++ != '-' ||
            !decimal(&p, &r->last) || *p++ != '/' ||
            !decimal(&p, &r->total)) {
            r->invalid = true;
            return;
        }
    } else if (strcasecmp(key, "Content-Encoding") == 0) {
        if (strncasecmp(p, "identity", 8) != 0) {
            r->invalid = true;
            return;
        }
        p += 8;
    } else if (strcasecmp(key, "Transfer-Encoding") == 0) {
        /* Commanded images have an exact size. Chunked/transformed bodies
         * cannot be used as verified byte offsets across requests. */
        r->invalid = true;
        return;
    } else {
        return;
    }
    skip_space(&p);
    if (*p) r->invalid = true;
}

bool fw_ota_http_response_valid(const fw_ota_http_response_t *r,
                                uint32_t offset, uint32_t image_len)
{
    if (r->invalid || !image_len || offset >= image_len ||
        r->lengths != 1 || r->length != image_len - offset) return false;
    if (!offset) return r->status == 200 && !r->ranges;
    return offset >= FW_OTA_RESUME_MIN && r->status == 206 &&
           r->ranges == 1 && r->first == offset &&
           r->last == image_len - 1 && r->total == image_len;
}

bool fw_ota_http_status_retryable(int status)
{
    return status == 408 || status == 429 || status == 500 ||
           status == 502 || status == 503 || status == 504;
}

unsigned fw_ota_retry_delay_s(unsigned completed_attempts)
{
    static const unsigned delays[] = {5, 15, 30, 60};
    return completed_attempts && completed_attempts < FW_OTA_MAX_ATTEMPTS
               ? delays[completed_attempts - 1] : 0;
}
