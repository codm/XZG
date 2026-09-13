#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t=std::timed_mutex *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t m,unsigned ms) { return m->try_lock_for(std::chrono::milliseconds(ms)); }
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }
inline void vSemaphoreDelete(SemaphoreHandle_t m) { delete m; }
