#pragma once
#include <cstdint>
void vTaskDelay(uint32_t ticks);
uint32_t uxTaskGetStackHighWaterMark(void* task);
