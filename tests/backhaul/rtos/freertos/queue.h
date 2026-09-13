#pragma once
#include "FreeRTOS.h"
struct FixtureQueue {
    std::mutex mutex; std::condition_variable cv; size_t capacity,size;
    std::deque<std::vector<uint8_t>> items;
    FixtureQueue(size_t n,size_t s):capacity(n),size(s) {}
};
using QueueHandle_t=FixtureQueue *;
inline QueueHandle_t xQueueCreate(size_t n,size_t s) { return new FixtureQueue(n,s); }
inline int xQueueSend(QueueHandle_t q,const void *in,unsigned) {
    std::lock_guard<std::mutex> lock(q->mutex);
    if(q->items.size()==q->capacity) return pdFALSE;
    const auto *p=static_cast<const uint8_t *>(in); q->items.emplace_back(p,p+q->size); q->cv.notify_one(); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void *out,unsigned ms) {
    std::unique_lock<std::mutex> lock(q->mutex);
    if(!q->cv.wait_for(lock,std::chrono::milliseconds(ms),[&] { return !q->items.empty(); })) return pdFALSE;
    memcpy(out,q->items.front().data(),q->size); q->items.pop_front(); return pdTRUE;
}
inline void xQueueReset(QueueHandle_t q) { std::lock_guard<std::mutex> lock(q->mutex); q->items.clear(); }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
