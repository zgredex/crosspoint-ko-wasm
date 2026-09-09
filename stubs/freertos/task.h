// task.h shim (see FreeRTOS.h)
#pragma once
#include "FreeRTOS.h"
#define vTaskDelay(ticks) ((void)0)
#define xTaskGetTickCount() ((TickType_t)0)
typedef unsigned int TickType_t;
#define tskNO_AFFINITY (-1)
