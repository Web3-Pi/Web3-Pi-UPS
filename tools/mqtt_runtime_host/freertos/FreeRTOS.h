#pragma once
#include <pthread.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
void host_enter_critical(portMUX_TYPE *lock);
void host_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) host_enter_critical(lock)
#define portEXIT_CRITICAL(lock) host_exit_critical(lock)
