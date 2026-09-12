#pragma once
#include <stdint.h>

namespace czc {
// The quiet interval starts AFTER an attempt ends, including DNS/TCP/TLS time.
// Only a stable authenticated connection resets backoff; flapping is not success.
class PeerRetry {
    bool attempted=false,waiting=false,healthy=false;
    uint32_t ended=0,delay=0,nextDelay=2000,healthySince=0;
public:
    uint32_t attempts=0;
    bool due(uint32_t now,bool active,bool online,uint32_t jitter) {
        if(active) {
            if(!online) healthy=false;
            else if(!healthy) { healthy=true; healthySince=now; }
            else if(uint32_t(now-healthySince)>=30000) nextDelay=2000;
            return false;
        }
        healthy=false;
        if(attempted) {
            attempted=false; waiting=true; ended=now;
            delay=nextDelay+jitter%501;
            nextDelay=nextDelay<16000 ? nextDelay*2 : 30000;
        }
        return !waiting || uint32_t(now-ended)>=delay;
    }
    void started() { attempted=true; waiting=false; ++attempts; }
    // Authenticated maintenance/test actions explicitly request a fresh session.
    void retrySoon(uint32_t now) {
        attempted=healthy=false; waiting=true; ended=now; delay=nextDelay=2000;
    }
    uint32_t remaining(uint32_t now) const {
        const uint32_t elapsed=now-ended;
        return waiting && elapsed<delay ? delay-elapsed : 0;
    }
};
}
