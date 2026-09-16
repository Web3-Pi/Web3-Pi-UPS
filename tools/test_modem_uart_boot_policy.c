/* Included after the actual fw_ota policy and its shared test SDK boundary. */
typedef enum {
    ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1,
} esp_ota_img_states_t;
typedef struct { int subtype; } esp_partition_t;
enum { ESP_ERR_NOT_FOUND = -11, ESP_FAIL = -1,
       GPIO_MODE_OUTPUT = 2, GPIO_PULLUP_DISABLE = 0,
       GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define MODEM_TAG "modem"
#define ESP_LOGI(tag, ...) log_ignored(__VA_ARGS__)
static const esp_partition_t running_partition = {0};
static esp_ota_img_states_t public_state;
static bool missing_partition;
static esp_err_t query_result, gpio_result;
static unsigned partition_queries, state_queries, gpio_calls;
static const esp_partition_t *esp_ota_get_running_partition(void)
{
    partition_queries++;
    return missing_partition ? NULL : &running_partition;
}
static esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                           esp_ota_img_states_t *state)
{
    CHECK(partition == &running_partition && state != NULL);
    state_queries++;
    if (query_result == ESP_OK) *state = public_state;
    return query_result;
}
static esp_err_t gpio_set_level(int pin, int value)
{
    CHECK((pin == 1 || pin == 5) && value == 0);
    gpio_calls++;
    return gpio_result;
}
static esp_err_t gpio_config(const gpio_config_t *config)
{
    CHECK(config != NULL);
    gpio_calls++;
    return gpio_result;
}

/* PRODUCTION_FUNCTIONS */

static void new_boot(esp_ota_img_states_t state)
{
    reset(0);
    /* BSS reset models a different ESP boot, never a PPP/DCE retry. */
    s_modem_boot_baud = 115200;
    s_modem_boot_baud_latched = false;
    public_state = state;
    query_result = gpio_result = ESP_OK;
    missing_partition = false;
    partition_queries = state_queries = gpio_calls = 0;
}

int main(void)
{
    CHECK(!s_modem_boot_baud_latched && s_modem_boot_baud == 115200);
    CHECK(ota_policy_regression() == 0);
    const esp_ota_img_states_t fallback_states[] = {
        ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_INVALID,
        ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED, (esp_ota_img_states_t)99,
    };
    for (unsigned i = 0; i < sizeof(fallback_states) / sizeof(fallback_states[0]); i++) {
        new_boot(fallback_states[i]);
        /* The selector must read the public state, not fw_ota's cache. */
        s_pending_verify = false;
        CHECK(modem_init() == ESP_OK && s_modem_boot_baud == 115200);
        CHECK(partition_queries == 1 && state_queries == 1);
        CHECK(!valid_calls && !rollback_calls && clock_us == 0);
        public_state = ESP_OTA_IMG_VALID;
        CHECK(modem_init() == ESP_OK && s_modem_boot_baud == 115200);
        CHECK(state_queries == 1);
    }
    new_boot(ESP_OTA_IMG_VALID);
    CHECK(modem_init() == ESP_OK && s_modem_boot_baud == CONFIG_WUPS_MODEM_UART_BAUD);
    CHECK(s_pending_verify); /* Stale application flag cannot override SDK. */

    /* A failed state read or early GPIO setup stays conservative for the
     * whole boot; a later retry must not silently promote the UART. */
    for (unsigned missing = 0; missing <= 1; missing++) {
        new_boot(ESP_OTA_IMG_VALID);
        missing_partition = missing != 0;
        query_result = ESP_FAIL;
        gpio_result = ESP_FAIL;
        CHECK(modem_init() == ESP_FAIL && s_modem_boot_baud == 115200);
        CHECK(partition_queries == 1 && state_queries == 1 - missing);
        missing_partition = false;
        query_result = gpio_result = ESP_OK;
        CHECK(modem_init() == ESP_OK && s_modem_boot_baud == 115200);
        CHECK(partition_queries == 1 && state_queries == 1 - missing);
    }

    new_boot(ESP_OTA_IMG_PENDING_VERIFY);
    CHECK(modem_init() == ESP_OK && s_modem_boot_baud == 115200);
    clock_us = INT64_C(599000000);
    fw_ota_mark_uplink_healthy();
    CHECK(valid_calls == 1 && s_marked_valid && !s_pending_verify);
    public_state = ESP_OTA_IMG_VALID; /* Successful SDK commit is now durable. */
    CHECK(modem_init() == ESP_OK && s_modem_boot_baud == 115200);
    clock_us = INT64_C(600000000);
    fw_ota_rollback_tick();
    CHECK(rollback_calls == 0 && state_queries == 1);
    new_boot(ESP_OTA_IMG_VALID);
    CHECK(modem_init() == ESP_OK && s_modem_boot_baud == CONFIG_WUPS_MODEM_UART_BAUD);

    /* The guard neither confirms an image nor extends the existing 600s
     * deadline, including a mark-valid flash failure just before expiry. */
    for (unsigned fail_validation = 0; fail_validation <= 1; fail_validation++) {
        new_boot(ESP_OTA_IMG_PENDING_VERIFY);
        CHECK(modem_init() == ESP_OK && s_pending_verify);
        clock_us = INT64_C(599000000);
        if (fail_validation) {
            valid_result = ESP_FAIL;
            fw_ota_mark_uplink_healthy();
            CHECK(valid_calls == 1 && s_pending_verify && !s_marked_valid);
        }
        fw_ota_rollback_tick();
        CHECK(rollback_calls == 0 && s_modem_boot_baud == 115200);
        clock_us = INT64_C(600000000);
        unsigned previous_valid_calls = valid_calls;
        fw_ota_mark_uplink_healthy();
        CHECK(valid_calls == (int)previous_valid_calls && s_pending_verify);
        fw_ota_rollback_tick();
        CHECK(rollback_calls == 1 && s_modem_boot_baud == 115200);
        CHECK(state_queries == 1);
    }
    printf("modem_uart_boot_policy: %u checks PASS, configured baud=%d\n",
           checks, CONFIG_WUPS_MODEM_UART_BAUD);
    return 0;
}
