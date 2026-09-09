// FreeRTOS.h + task.h shim — host/WASM builds: vTaskDelay is a no-op.
#pragma once
#define pdMS_TO_TICKS(ms) ((ms))
typedef void* TaskHandle_t;
typedef unsigned int UBaseType_t;
typedef int BaseType_t;
#define portTICK_PERIOD_MS 1
