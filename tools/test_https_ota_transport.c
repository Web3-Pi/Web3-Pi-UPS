/* Host stubs surround exact read_header/get_description/perform source excerpts.
 * The read-call ceiling is a test escape hatch, never part of firmware code. */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define ESP_ERR_HTTP_CONNECTION_CLOSED 0x7008
#define ESP_ERR_HTTP_INCOMPLETE_DATA 0x700c
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x9001
#define FLASH_ERROR 0x6001
#define VERIFY_ERROR 0x6002
#define IMAGE_HEADER_SIZE 1024
#define OTA_SIZE_UNKNOWN ((size_t)-1)
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-2)
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_BOOTLOADER 2
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432u
#define ESP_BOOTLOADER_DESC_MAGIC_BYTE 0x50
#define ESP_HTTPS_OTA_GET_IMG_DESC 1
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(ret, ...) do { if ((ret) != ESP_OK) return (ret); } while (0)

enum { ESP_HTTPS_OTA_INIT, ESP_HTTPS_OTA_BEGIN, ESP_HTTPS_OTA_IN_PROGRESS,
       ESP_HTTPS_OTA_SUCCESS, ESP_HTTPS_OTA_RESUME };
typedef struct { uint8_t bytes[24]; } esp_image_header_t;
typedef struct { uint8_t bytes[8]; } esp_image_segment_header_t;
typedef struct { uint32_t magic_word; uint8_t bytes[252]; } esp_app_desc_t;
typedef struct { uint8_t magic_byte; } esp_bootloader_desc_t;
typedef struct { int type; } partition_t;
typedef struct {
    int values[8]; size_t value_count, next, position, complete_at;
    unsigned calls;
    uint8_t bytes[2048];
} stream_t;
typedef struct esp_https_ota_handle {
    int update_handle;
    struct { const partition_t *staging, *final; bool finalize_with_copy; } partition;
    stream_t *http_client;
    char *ota_upgrade_buf;
    int binary_file_len, image_length, state, ota_upgrade_buf_size;
    bool bulk_flash_erase;
} esp_https_ota_t;
typedef esp_https_ota_t *esp_https_ota_handle_t;

static jmp_buf read_ceiling;
static int write_error, begin_error, verify_error, write_calls, written;

static bool esp_http_client_is_complete_data_received(stream_t *s)
{ return s->position >= s->complete_at; }

static int esp_http_client_read(stream_t *s, char *out, int capacity)
{
    if (++s->calls > 8) longjmp(read_ceiling, 1);
    int value = s->values[s->next < s->value_count ? s->next++ : s->value_count - 1];
    if (value <= 0) return value;
    if (value > capacity) value = capacity;
    assert(s->position + (size_t)value <= sizeof(s->bytes));
    memcpy(out, s->bytes + s->position, (size_t)value);
    s->position += (size_t)value;
    return value;
}

static void esp_https_ota_dispatch_event(int event, const void *data, size_t size)
{ (void)event; (void)data; (void)size; }
static esp_err_t esp_partition_read(const partition_t *part, size_t offset,
                                    void *out, size_t size)
{ (void)part; (void)offset; memset(out, 0, size); return ESP_OK; }
static esp_err_t esp_ota_begin(const partition_t *part, size_t erase, int *handle)
{ (void)part; (void)erase; *handle = 1; return begin_error; }
static esp_err_t esp_ota_resume(const partition_t *part, size_t erase,
                                size_t offset, int *handle)
{ (void)offset; return esp_ota_begin(part, erase, handle); }
static esp_err_t esp_ota_set_final_partition(int handle, const partition_t *part,
                                            bool copy)
{ (void)handle; (void)part; (void)copy; return ESP_OK; }
static esp_err_t esp_ota_verify_chip_id(const void *data)
{ (void)data; return verify_error; }
static esp_err_t esp_ota_verify_chip_revision(const void *data)
{ (void)data; return ESP_OK; }
static esp_err_t _ota_write(esp_https_ota_t *handle, const void *data, int length)
{
    assert(data == handle->ota_upgrade_buf);
    ++write_calls;
    if (write_error) return write_error;
    written += length;
    handle->binary_file_len += length;
    return ESP_ERR_HTTPS_OTA_IN_PROGRESS;
}

/* The unchanged upstream description function has a signed/unsigned compare.
 * Keep its original text; do not weaken warnings for our host fixture. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "ota_functions_under_test.inc"
#pragma GCC diagnostic pop

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    stream_t stream = { .values = {1024}, .value_count = 1, .complete_at = 1024 };
    const partition_t part = { .type = ESP_PARTITION_TYPE_APP };
    _Alignas(esp_app_desc_t) char buffer[2048] = {0};
    esp_https_ota_t handle = {
        .partition = { .staging = &part, .final = &part },
        .http_client = &stream, .ota_upgrade_buf = buffer,
        .state = ESP_HTTPS_OTA_BEGIN, .image_length = 1024,
        .ota_upgrade_buf_size = sizeof(buffer),
    };
    for (size_t i = 0; i < sizeof(stream.bytes); ++i) stream.bytes[i] = (uint8_t)i;
    const uint32_t magic = ESP_APP_DESC_MAGIC_WORD;
    memcpy(stream.bytes + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           &magic, sizeof(magic));

    /* These values remain defined across the baseline-spin longjmp on GCC. */
    volatile int expected = ESP_OK;
    volatile unsigned expected_calls = 1;
    if (strstr(scenario, "eagain")) {
        stream.values[0] = -ESP_ERR_HTTP_EAGAIN;
        expected = ESP_ERR_HTTP_EAGAIN;
    } else if (strstr(scenario, "zero")) {
        stream.values[0] = 0;
        expected = ESP_ERR_HTTP_INCOMPLETE_DATA;
    } else if (strstr(scenario, "closed")) {
        stream.values[0] = -1;
        expected = ESP_ERR_HTTP_CONNECTION_CLOSED;
    } else if (strstr(scenario, "short")) {
        stream.values[0] = 512;
        stream.complete_at = 512;
        expected = ESP_ERR_HTTP_INCOMPLETE_DATA;
    } else if (strstr(scenario, "fragmented")) {
        stream.values[0] = 300; stream.values[1] = 500; stream.values[2] = 224;
        stream.value_count = 3;
        expected_calls = 3;
    } else if (strstr(scenario, "flash")) {
        write_error = FLASH_ERROR;
        expected = FLASH_ERROR;
    } else if (strstr(scenario, "begin")) {
        begin_error = FLASH_ERROR;
        expected = FLASH_ERROR;
        expected_calls = 0;
    } else if (strstr(scenario, "verify")) {
        verify_error = VERIFY_ERROR;
        expected = VERIFY_ERROR;
    } else if (!strstr(scenario, "complete")) {
        fprintf(stderr, "Unknown scenario: %s\n", scenario);
        return 2;
    }
    if (strncmp(scenario, "body_", 5) == 0) {
        handle.state = ESP_HTTPS_OTA_IN_PROGRESS;
        if (strstr(scenario, "eagain")) expected = ESP_ERR_HTTPS_OTA_IN_PROGRESS;
        if (strstr(scenario, "complete")) {
            stream.position = 1024; stream.values[0] = 0;
        }
    } else if (strncmp(scenario, "perform_", 8) == 0 && expected == ESP_OK) {
        expected = ESP_ERR_HTTPS_OTA_IN_PROGRESS;
    }

    if (setjmp(read_ceiling)) {
        fprintf(stderr, "%s: blocked after 8 SDK read calls (baseline spin)\n", scenario);
        return 77;
    }
    int result;
    esp_app_desc_t description;
    if (strncmp(scenario, "desc_", 5) == 0)
        result = get_description_from_image(&handle, &description);
    else if (strncmp(scenario, "header_", 7) == 0)
        result = read_header(&handle);
    else
        result = esp_https_ota_perform(&handle);
    if (result != expected || stream.calls != expected_calls) {
        fprintf(stderr, "%s: result=%#x expected=%#x reads=%u expected=%u\n",
                scenario, result, expected, stream.calls, expected_calls);
        return 1;
    }
    if (result == ESP_OK && strncmp(scenario, "header_", 7) == 0) {
        assert(handle.binary_file_len == 1024 && written == 0);
        assert(memcmp(buffer, stream.bytes, 1024) == 0);
    }
    if (result == ESP_OK && strncmp(scenario, "desc_", 5) == 0)
        assert(description.magic_word == ESP_APP_DESC_MAGIC_WORD && written == 0);
    if (result == ESP_ERR_HTTPS_OTA_IN_PROGRESS &&
        strncmp(scenario, "perform_", 8) == 0)
        assert(written == 1024 && write_calls == 1 && handle.binary_file_len == 1024);
    if (result != ESP_OK && result != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        assert(written == 0);
    printf("PASS %s\n", scenario);
    return 0;
}
