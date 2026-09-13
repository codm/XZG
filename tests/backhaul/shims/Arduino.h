#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include <chrono>
inline uint32_t millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
