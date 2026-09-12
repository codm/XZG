#pragma once
#include <stdint.h>

namespace czc {
// Three UART restart attempts, with increasing delay. Only a full minute of
// authenticated, healthy peer traffic replenishes the allowance.
class Recovery {
    bool failing=false,healthy=false;
    uint32_t failureSince=0,healthySince=0;
public:
    unsigned attempts=0;
    uint32_t total=0;
    bool due(uint32_t now,bool fatal,bool online) {
        if(!fatal) {
            failing=false;
            if(!online) healthy=false;
            else if(!healthy) { healthy=true; healthySince=now; }
            else if(uint32_t(now-healthySince)>=60000) attempts=0;
            return false;
        }
        healthy=false;
        if(!failing) { failing=true; failureSince=now; }
        return attempts<3 && uint32_t(now-failureSince)>=(5000u<<attempts);
    }
    void attempted(uint32_t now) { ++attempts; ++total; failureSince=now; }
};
}
