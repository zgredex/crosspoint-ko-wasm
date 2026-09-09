// semphr.h shim (see FreeRTOS.h) — no-op semaphores for host/WASM builds.
#pragma once
#include "FreeRTOS.h"

#define semBINARY_SEMAPHORE_QUEUE_LENGTH 1
typedef void* SemaphoreHandle_t;

#define xSemaphoreCreateMutex() ((SemaphoreHandle_t)1)
#define xSemaphoreCreateBinary() ((SemaphoreHandle_t)1)
#define xSemaphoreTake(m, t) ((BaseType_t)1)
#define xSemaphoreGive(m) ((BaseType_t)1)
#define vSemaphoreDelete(m) ((void)0)
