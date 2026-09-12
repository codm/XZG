#pragma once
#include <stdint.h>

// Reject the unset and broadcast sentinels. This is not subnet validation.
inline bool ipv4Assigned(uint32_t address) {
    return address!=0 && address!=UINT32_MAX;
}
