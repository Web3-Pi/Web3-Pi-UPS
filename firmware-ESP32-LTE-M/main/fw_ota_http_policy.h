#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FW_OTA_MAX_ATTEMPTS 5u
#define FW_OTA_RESUME_MIN 1024u

/* Metadata of ONE response, reset even when redirects reuse the socket. */
typedef struct {
    int status;
    bool invalid;
    unsigned lengths, ranges;
    uint32_t length, first, last, total;
} fw_ota_http_response_t;

void fw_ota_http_header(fw_ota_http_response_t *r,
                        const char *key, const char *value);
bool fw_ota_http_response_valid(const fw_ota_http_response_t *r,
                                uint32_t offset, uint32_t image_len);
bool fw_ota_http_status_retryable(int status);
unsigned fw_ota_retry_delay_s(unsigned completed_attempts);
