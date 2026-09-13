#pragma once
#include <stdint.h>

namespace czc {
struct MemoryBudget {
    uint32_t free=0,largest=0,minimum=0;
    // Stock HTTPS uses two 16 KiB TLS records in addition to its task stack,
    // certificates, HTTP body and Wi-Fi buffers. It is optional background work.
    bool backgroundHttps() const { return free>=80*1024 && largest>=24*1024; }
};
}
