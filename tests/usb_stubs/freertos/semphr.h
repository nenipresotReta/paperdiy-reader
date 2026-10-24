#pragma once
using SemaphoreHandle_t=void*;
constexpr int pdTRUE=1;
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t,int);
void xSemaphoreGive(SemaphoreHandle_t);
