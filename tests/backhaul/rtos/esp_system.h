#pragma once
#include <sys/random.h>
#include <cassert>
#include <stddef.h>
extern bool fixtureEntropyEnabled;
inline void esp_fill_random(void *out,size_t length) {
    assert(fixtureEntropyEnabled); // No low-entropy hardware polling after Wi-Fi takes over.
    assert(getrandom(out,length,0)==static_cast<ssize_t>(length));
}
