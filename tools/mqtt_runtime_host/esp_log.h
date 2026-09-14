#pragma once
#define ESP_LOG_VERBOSE 5
/* Still type-check and evaluate arguments, without printing fixture data. */
void host_log(const char *format, ...);
#define ESP_LOGI(tag, ...) host_log(__VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(__VA_ARGS__)
#define ESP_LOGE(tag, ...) host_log(__VA_ARGS__)
#define ESP_LOGD(tag, ...) host_log(__VA_ARGS__)
void esp_log_level_set(const char *tag, int level);
