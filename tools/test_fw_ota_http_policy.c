/* Real portable HTTP metadata policy; expected outcomes are independent of
 * esp_https_ota transport, image verification and caller retry admission. */
#include "fw_ota_http_policy.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    return 1; } } while (0)

static fw_ota_http_response_t full(uint32_t size)
{
    fw_ota_http_response_t response = {.status = 200};
    char text[16];
    (void)snprintf(text, sizeof(text), "%" PRIu32, size);
    fw_ota_http_header(&response, "Content-Length", text);
    return response;
}

static fw_ota_http_response_t range(uint32_t image_len, uint32_t offset)
{
    fw_ota_http_response_t response = {.status = 206};
    char length[16], content_range[64];
    (void)snprintf(length, sizeof(length), "%" PRIu32, image_len - offset);
    (void)snprintf(content_range, sizeof(content_range), "bytes %" PRIu32 "-%" PRIu32 "/%" PRIu32,
                   offset, image_len - 1, image_len);
    fw_ota_http_header(&response, "Content-Length", length);
    fw_ota_http_header(&response, "Content-Range", content_range);
    return response;
}

static int test_initial_response(void)
{
    fw_ota_http_response_t r = full(4096);
    CHECK(fw_ota_http_response_valid(&r, 0, 4096));
    CHECK(!r.invalid && r.lengths == 1 && r.length == 4096 && r.ranges == 0);
    CHECK(!fw_ota_http_response_valid(&r, 0, 4095));
    CHECK(!fw_ota_http_response_valid(&r, 0, 4097));
    CHECK(!fw_ota_http_response_valid(&r, 1, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 1023, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 4096, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 4097, 4096));
    CHECK(!fw_ota_http_response_valid(&r, UINT32_MAX, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 0, 0));
    r = full(0);
    CHECK(!fw_ota_http_response_valid(&r, 0, 0));
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    r = (fw_ota_http_response_t){.status = 200};
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));

    /* This is HTTP byte accounting, not a substitute for app-image checks. */
    static const uint32_t sizes[] = {1, 1023, 1024, 1025, 4096, UINT32_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        r = full(sizes[i]);
        CHECK(fw_ota_http_response_valid(&r, 0, sizes[i]));
    }
    r = full(4096);
    for (int status = -1; status <= 600; ++status) {
        r.status = status;
        CHECK(fw_ota_http_response_valid(&r, 0, 4096) == (status == 200));
    }
    r = full(4096);
    fw_ota_http_header(&r, "Content-Range", "bytes 0-4095/4096");
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    r.status = 206;
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    return 0;
}

static int test_resume_response(void)
{
    static const uint32_t offsets[] = {0, 1, 1023, 1024, 1025, 2048, 4095, 4096, 4097};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint32_t offset = offsets[i];
        fw_ota_http_response_t r = range(4096, offset);
        CHECK(fw_ota_http_response_valid(&r, offset, 4096) ==
              (offset >= 1024 && offset < 4096));
    }
    fw_ota_http_response_t r = range(4096, 1024);
    CHECK(r.first == 1024 && r.last == 4095 && r.total == 4096 && r.length == 3072);
    CHECK(fw_ota_http_response_valid(&r, 1024, 4096));
    for (int status = -1; status <= 600; ++status) {
        r.status = status;
        CHECK(fw_ota_http_response_valid(&r, 1024, 4096) == (status == 206));
    }
    r = full(3072); /* Server ignores Range and returns200: cannot append. */
    CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));
    r.status = 206;
    CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));
    r = range(4096, 1024);
    CHECK(!fw_ota_http_response_valid(&r, 1025, 4096));
    CHECK(!fw_ota_http_response_valid(&r, 1024, 4097));
    r = range(1025, 1024);
    CHECK(fw_ota_http_response_valid(&r, 1024, 1025));
    r = range(UINT32_MAX, 1024);
    CHECK(fw_ota_http_response_valid(&r, 1024, UINT32_MAX));
    r = range(UINT32_MAX, UINT32_MAX - 1);
    CHECK(fw_ota_http_response_valid(&r, UINT32_MAX - 1, UINT32_MAX));
    CHECK(!fw_ota_http_response_valid(&r, UINT32_MAX, UINT32_MAX));
    return 0;
}

static int test_length_parser(void)
{
    static const char *good[] = {"4096", "04096", "00004096", " 4096", "\t4096",
                                 "4096 ", "4096\t", " \t4096\t "};
    static const char *bad[] = {
        "", " ", "\t", "0", "4095", "4097", "+4096", "-4096", "-0",
        "4 096", "4096,4096", "4096, 4096", "4096junk", "4096.0", "4e3",
        "4096/", "4096;", "4096\r", "4096\n", "4096\r\n", "\r4096", "\n4096",
        "4096\v", "4096\f", "4096\x7f", "4294967296", "18446744073709551615",
        "18446744073709551616", "00000000004294967296",
        "99999999999999999999999999999999999999999999999999999999999999999"
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        fw_ota_http_response_t r = {.status = 200};
        fw_ota_http_header(&r, "cOnTeNt-LeNgTh", good[i]);
        CHECK(fw_ota_http_response_valid(&r, 0, 4096));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fw_ota_http_response_t r = {.status = 200};
        fw_ota_http_header(&r, "Content-Length", bad[i]);
        if (fw_ota_http_response_valid(&r, 0, 4096)) {
            fprintf(stderr, "accepted bad Content-Length case%zu\n", i);
            CHECK(0);
        }
        CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    }
    return 0;
}

static int test_range_parser(void)
{
    static const char *good[] = {"bytes 1024-4095/4096", " bytes 1024-4095/4096",
                                 "\tbytes 1024-4095/4096\t ", "bytes 01024-04095/04096"};
    static const char *bad[] = {
        "", "bytes", "bytes ", "bytes 1024", "bytes 1024-", "bytes 1024-4095",
        "bytes 1024-4095/", "bytes 1024-4095/*", "bytes */4096", "bytes */*",
        "bytes -1024-4095/4096", "bytes +1024-4095/4096", "bytes 1024-+4095/4096",
        "bytes 1024-4095/+4096", "bytes 1024-4095/-4096", "bytes 1024-4095/0",
        "bytes 1023-4095/4096", "bytes 1025-4095/4096", "bytes 1024-4094/4096",
        "bytes 1024-4096/4096", "bytes 1024-4095/4095", "bytes 1024-4095/4097",
        "bytes 4095-1024/4096", "bytes 4096-4095/4096", "bytes 4294967296-4095/4096",
        "bytes 1024-4294967296/4096", "bytes 1024-4095/4294967296",
        "bytes 18446744073709551615-4095/4096", "bytes 1024-4095/18446744073709551616",
        "bytes 1024-4095/4096junk", "bytes 1024-4095/4096;", "bytes 1024-4095/4096/",
        "bytes 1024-4095/4096,1024-4095/4096", "bytes 1024-4095/4096\r\n",
        "bytes 1024 -4095/4096", "bytes 1024- 4095/4096", "bytes 1024-4095 /4096",
        "bytes 1024-4095/ 4096", "bytes  1024-4095/4096", "bytes\t1024-4095/4096",
        "Bytes 1024-4095/4096", "BYTES 1024-4095/4096", "items 1024-4095/4096"
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        fw_ota_http_response_t r = full(3072);
        r.status = 206;
        fw_ota_http_header(&r, "cOnTeNt-RaNgE", good[i]);
        CHECK(fw_ota_http_response_valid(&r, 1024, 4096));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fw_ota_http_response_t r = full(3072);
        r.status = 206;
        fw_ota_http_header(&r, "Content-Range", bad[i]);
        if (fw_ota_http_response_valid(&r, 1024, 4096)) {
            fprintf(stderr, "accepted bad Content-Range case%zu\n", i);
            CHECK(0);
        }
        CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));
    }
    return 0;
}

static int test_duplicates_and_invalid_stickiness(void)
{
    fw_ota_http_response_t r = full(4096);
    fw_ota_http_header(&r, "content-length", "4096");
    CHECK(r.invalid && r.lengths == 2);
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    fw_ota_http_header(&r, "Content-Encoding", "identity");
    fw_ota_http_header(&r, "X-Test", "ignored");
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    r = full(4096);
    fw_ota_http_header(&r, "Content-Length", "4095");
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    r = range(4096, 1024);
    fw_ota_http_header(&r, "content-range", "bytes 1024-4095/4096");
    CHECK(r.invalid && r.ranges == 2);
    CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));

    r = (fw_ota_http_response_t){.status = 200};
    fw_ota_http_header(&r, "Content-Length", "malformed");
    fw_ota_http_header(&r, "Content-Length", "4096");
    CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    r = full(4096);
    fw_ota_http_header(&r, NULL, "4096");
    CHECK(r.invalid && !fw_ota_http_response_valid(&r, 0, 4096));
    r = full(4096);
    fw_ota_http_header(&r, "Content-Length", NULL);
    CHECK(r.invalid && !fw_ota_http_response_valid(&r, 0, 4096));

    /* Header-count wrap must never restore acceptance. */
    r = full(4096);
    r.lengths = UINT_MAX;
    fw_ota_http_header(&r, "Content-Length", "4096");
    fw_ota_http_header(&r, "Content-Length", "4096");
    CHECK(r.invalid && !fw_ota_http_response_valid(&r, 0, 4096));
    r = range(4096, 1024);
    r.ranges = UINT_MAX;
    fw_ota_http_header(&r, "Content-Range", "bytes 1024-4095/4096");
    fw_ota_http_header(&r, "Content-Range", "bytes 1024-4095/4096");
    CHECK(r.invalid && !fw_ota_http_response_valid(&r, 1024, 4096));
    return 0;
}

static int test_encodings_and_ignored_headers(void)
{
    static const char *good[] = {"identity", "Identity", "IDENTITY", " \tidentity\t "};
    static const char *bad[] = {"", " ", "gzip", "br", "deflate", "chunked", "identity,gzip",
                               "gzip,identity", "identity, identity", "identityjunk", "identity;",
                               "identity\r\n", "identity\v", "identity\x7f"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        fw_ota_http_response_t r = full(4096);
        fw_ota_http_header(&r, "content-encoding", good[i]);
        CHECK(fw_ota_http_response_valid(&r, 0, 4096));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        fw_ota_http_response_t r = full(4096);
        fw_ota_http_header(&r, "Content-Encoding", bad[i]);
        CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
        fw_ota_http_header(&r, "Content-Encoding", "identity");
        CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
    }
    static const char *transfer[] = {"chunked", "identity", "gzip", "", "chunked, gzip"};
    for (size_t i = 0; i < sizeof(transfer) / sizeof(transfer[0]); ++i) {
        fw_ota_http_response_t r = full(4096);
        fw_ota_http_header(&r, "TRANSFER-ENCODING", transfer[i]);
        CHECK(!fw_ota_http_response_valid(&r, 0, 4096));
        r = range(4096, 1024);
        fw_ota_http_header(&r, "Transfer-Encoding", transfer[i]);
        CHECK(!fw_ota_http_response_valid(&r, 1024, 4096));
    }
    fw_ota_http_response_t r = full(4096);
    fw_ota_http_header(&r, "X-Content-Encoding", "gzip");
    fw_ota_http_header(&r, "Server", "test");
    fw_ota_http_header(&r, "ETag", "test");
    CHECK(fw_ota_http_response_valid(&r, 0, 4096));
    return 0;
}

static int test_exact_allocation_truncations(void)
{
    static const struct { const char *key, *value; uint32_t offset; } cases[] = {
        {"Content-Length", "4096", 0},
        {"Content-Range", "bytes 1024-4095/4096", 1024},
        {"Content-Encoding", "identity", 0},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        size_t length = strlen(cases[c].value);
        for (size_t n = 0; n < length; ++n) {
            char *value = malloc(n + 1);
            CHECK(value != NULL);
            memcpy(value, cases[c].value, n);
            value[n] = '\0';
            fw_ota_http_response_t r = {.status = cases[c].offset ? 206 : 200};
            if (c) fw_ota_http_header(&r, "Content-Length", cases[c].offset ? "3072" : "4096");
            fw_ota_http_header(&r, cases[c].key, value);
            bool accepted = fw_ota_http_response_valid(&r, cases[c].offset, 4096);
            free(value);
            CHECK(!accepted);
        }
        for (size_t n = 0; n < length; ++n) {
            char *value = malloc(length + 1);
            CHECK(value != NULL);
            memcpy(value, cases[c].value, length + 1);
            value[n] = '!';
            fw_ota_http_response_t r = {.status = cases[c].offset ? 206 : 200};
            if (c) fw_ota_http_header(&r, "Content-Length", cases[c].offset ? "3072" : "4096");
            fw_ota_http_header(&r, cases[c].key, value);
            bool accepted = fw_ota_http_response_valid(&r, cases[c].offset, 4096);
            free(value);
            CHECK(!accepted);
        }
    }
    return 0;
}

static int test_generated_boundaries(void)
{
    uint32_t random = 0x4f544131u;
    for (unsigned i = 0; i < 256; ++i) {
        random = random * 1664525u + 1013904223u;
        uint32_t image = 1025u + random % (UINT32_MAX - 1024u);
        random = random * 1664525u + 1013904223u;
        uint32_t offset = 1024u + random % (image - 1024u);
        fw_ota_http_response_t r = range(image, offset);
        CHECK(fw_ota_http_response_valid(&r, offset, image));
        CHECK(!fw_ota_http_response_valid(&r, offset + 1, image));
        CHECK(!fw_ota_http_response_valid(&r, offset, image - 1));
        r.length--;
        CHECK(!fw_ota_http_response_valid(&r, offset, image));
        r = range(image, offset);
        r.last++; /* Server supplied one byte beyond the requested image. */
        CHECK(!fw_ota_http_response_valid(&r, offset, image));
    }
    return 0;
}

static int test_retry_policy(void)
{
    static const int transient[] = {408, 429, 500, 502, 503, 504};
    for (int status = -1; status <= 999; ++status) {
        bool expected = false;
        for (size_t i = 0; i < sizeof(transient) / sizeof(transient[0]); ++i)
            if (status == transient[i]) expected = true;
        CHECK(fw_ota_http_status_retryable(status) == expected);
    }
    CHECK(!fw_ota_http_status_retryable(INT_MIN));
    CHECK(!fw_ota_http_status_retryable(INT_MAX));
    CHECK(FW_OTA_MAX_ATTEMPTS == 5);
    CHECK(FW_OTA_RESUME_MIN == 1024);
    static const unsigned delay[] = {0, 5, 15, 30, 60, 0};
    unsigned total = 0;
    for (unsigned completed = 0; completed < sizeof(delay) / sizeof(delay[0]); ++completed) {
        CHECK(fw_ota_retry_delay_s(completed) == delay[completed]);
        total += fw_ota_retry_delay_s(completed);
    }
    CHECK(total == 110);
    for (unsigned completed = 6; completed <= 100; ++completed)
        CHECK(fw_ota_retry_delay_s(completed) == 0);
    CHECK(fw_ota_retry_delay_s(UINT_MAX) == 0);
    return 0;
}

int main(void)
{
    if (test_initial_response() || test_resume_response() || test_length_parser() ||
        test_range_parser() || test_duplicates_and_invalid_stickiness() ||
        test_encodings_and_ignored_headers() || test_exact_allocation_truncations() ||
        test_generated_boundaries() || test_retry_policy()) return 1;
    printf("fw_ota_http_policy PASS: %u checks; metadata boundaries, exact-buffer truncations, retry limits\n", checks);
    return 0;
}
