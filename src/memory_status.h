#pragma once
#include <MemoryBudget.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

inline czc::MemoryBudget memoryBudget() {
    const uint32_t caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    czc::MemoryBudget value;
    value.free=heap_caps_get_free_size(caps);
    value.largest=heap_caps_get_largest_free_block(caps);
    value.minimum=heap_caps_get_minimum_free_size(caps);
    return value;
}
inline void memoryStatus(JsonObject obj) {
    auto value=memoryBudget();
    obj["free"]=value.free; obj["largest"]=value.largest; obj["minimum"]=value.minimum;
}
