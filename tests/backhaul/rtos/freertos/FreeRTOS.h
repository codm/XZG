#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
using TickType_t=unsigned;
struct FixtureTask { std::atomic<bool> stop{false}; std::thread thread; };
using TaskHandle_t=FixtureTask *;
struct FixtureExit {};
extern thread_local FixtureTask *fixtureCurrentTask;
inline void vTaskDelay(unsigned ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if(fixtureCurrentTask && fixtureCurrentTask->stop) throw FixtureExit{};
}
inline int xTaskCreatePinnedToCore(void (*fn)(void *),const char *,unsigned,void *arg,unsigned,TaskHandle_t *out,unsigned) {
    auto *t=new FixtureTask; *out=t;
    t->thread=std::thread([=] { fixtureCurrentTask=t; try { fn(arg); } catch(const FixtureExit &) {} }); return pdPASS;
}
inline void vTaskDelete(TaskHandle_t t) { if(!t) throw FixtureExit{}; t->stop=true; t->thread.join(); delete t; }
