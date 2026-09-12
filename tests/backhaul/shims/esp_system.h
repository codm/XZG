#pragma once
#include <sys/random.h>
#include <cstdlib>
inline void esp_fill_random(void *p,size_t n) { if(getrandom(p,n,0)!=static_cast<ssize_t>(n)) abort(); }
