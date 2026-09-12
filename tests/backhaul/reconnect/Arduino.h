#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstring>
extern uint32_t fixtureNow;
inline uint32_t millis() { return fixtureNow; }
