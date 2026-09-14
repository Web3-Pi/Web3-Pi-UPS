#pragma once
#include "FreeRTOS.h"
typedef struct host_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                       void *argument, unsigned priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskDelay(TickType_t ticks);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
