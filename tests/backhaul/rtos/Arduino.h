#pragma once
#include "../shims/Arduino.h"
#include <mutex>
#include <deque>
#include <vector>
#include <functional>
class SerialFixture {
    std::mutex mutex;
    std::deque<uint8_t> input;
public:
    std::function<void(const uint8_t *,size_t)> onWrite;
    unsigned writes=0,reads=0;
    int available() { std::lock_guard<std::mutex> lock(mutex); return input.size(); }
    int read() { std::lock_guard<std::mutex> lock(mutex); if(input.empty()) return -1; int b=input.front(); input.pop_front(); ++reads; return b; }
    size_t write(const uint8_t *p,size_t n) { ++writes; if(onWrite) onWrite(p,n); return n; }
    void feed(const uint8_t *p,size_t n) { std::lock_guard<std::mutex> lock(mutex); input.insert(input.end(),p,p+n); }
};
extern SerialFixture Serial2;
