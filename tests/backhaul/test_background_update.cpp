#include <freertos/queue.h>
#include <cassert>
#include <string>
#include <cstdio>
#include <new>
#include <MemoryBudget.h>
static uint32_t now=0;
static uint32_t millis() { return now; }
static czc::MemoryBudget available;
static czc::MemoryBudget memoryBudget() { return available; }
thread_local FixtureTask *fixtureCurrentTask=nullptr;
struct String:std::string {
    using std::string::string;
    String(const std::string &s):std::string(s) {}
    explicit String(unsigned n):std::string(std::to_string(n)) {}
    String substring(size_t n) const { return substr(n); }
};
static struct { bool apStarted=false,zbFlashing=false,updateEspAvail=false,updateZbAvail=false; } vars;
static struct { bool updAutoInst=true; } systemCfg;
static struct { struct { unsigned fwRev=20260917; } chip; } CCTool;
static struct { void restart() { assert(false); } } ESP;
static const auto uiThread=std::this_thread::get_id();
static std::thread worker;
static std::mutex gate;
static std::condition_variable wake;
static bool releaseFetch=false,failCreate=false;
static std::atomic<unsigned> fetching{0};
static unsigned jobs=0,logs=0,dns=0;
static void printLogMsg(const String &) { assert(std::this_thread::get_id()==uiThread); ++logs; }
static void checkDNS() { assert(std::this_thread::get_id()==uiThread); ++dns; }
static bool backhaulConfigured() { return true; }
static bool getEspUpdate(const String &) { assert(false); return false; }
static bool flashZigbeefromURL(const char *,const char *,decltype(CCTool) &) { assert(false); return false; }
static String extractVersionFromURL(const String &s) { return s; }
static int compareVersions(const String &,const String &) { return 1; }
static String fetchLatestEspFw(bool refreshDns) {
    assert(!refreshDns && std::this_thread::get_id()!=uiThread); ++fetching;
    std::unique_lock<std::mutex> lock(gate);
    wake.wait(lock,[] { return releaseFetch; }); return "new-esp";
}
static String fetchLatestZbFw(bool refreshDns) {
    assert(!refreshDns && std::this_thread::get_id()!=uiThread); return "new-zb";
}
static int createWorker(void (*fn)(void *),const char *,unsigned stack,void *arg,unsigned priority,TaskHandle_t *,int core) {
    assert(stack>=8192 && priority==1 && core==0);
    if(failCreate) return pdFALSE;
    ++jobs; worker=std::thread([=] { fn(arg); }); return pdPASS;
}
static void deleteWorker(TaskHandle_t t) { assert(!t); }
#define xTaskCreatePinnedToCore createWorker
#define vTaskDelete deleteWorker
#define portMAX_DELAY 0xffffffffu
#define LOGD(...) ((void)0)
#define DEBUG_PRINTLN(...) ((void)0)
#define VERSION "test-build"
#include "../../.backhaul-tests/background-update-under-test.inc"

int main() {
    available.free=100*1024; available.largest=4096;
    checkUpdateAvail();
    assert(updateCheckDeferred && !updateCheckRunning && !jobs && !dns && logs==1);
    // Fragmentation and low total RAM both postpone HTTPS without creating
    // tasks or repeatedly logging/allocating on the UI loop.
    for(now=1;now<60000;++now) updateCheckLoop();
    assert(logs==1 && !jobs);
    available.free=30*1024; available.largest=30*1024;
    updateCheckLoop(); assert(logs==1 && !jobs);
    available.free=120*1024; available.largest=48*1024;
    now=119999; updateCheckLoop(); assert(!jobs);
    now=120000; updateCheckLoop();
    assert(!updateCheckDeferred && jobs==1);
    logs=0;
    checkUpdateAvail();
    while(!fetching) std::this_thread::yield();
    auto start=std::chrono::steady_clock::now();
    unsigned uiRequests=0;
    // Internet fetch remains blocked. Every UI loop returns, duplicate timer
    // triggers do not create more workers, and UI-owned state is untouched.
    for(unsigned n=0;n<1000;++n) {
        checkUpdateAvail(); updateCheckLoop(); ++uiRequests;
        assert(updateCheckRunning && !vars.updateEspAvail && !vars.updateZbAvail);
    }
    assert(uiRequests==1000 && jobs==1 && dns==1 && logs==0);
    assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
    { std::lock_guard<std::mutex> lock(gate); releaseFetch=true; } wake.notify_one(); worker.join();
    // Flash operation postpones application of the finished result.
    vars.zbFlashing=true; updateCheckLoop(); assert(!vars.updateEspAvail && updateCheckRunning);
    vars.zbFlashing=false; updateCheckLoop();
    assert(vars.updateEspAvail && vars.updateZbAvail && !updateCheckRunning && logs==2);
    updateCheckLoop(); assert(logs==2);
    failCreate=true; checkUpdateAvail(); assert(!updateCheckRunning && logs==3);
    failCreate=false; checkUpdateAvail(); worker.join(); updateCheckLoop(); assert(jobs==2);
    vars.apStarted=true; checkUpdateAvail(); assert(jobs==2);
    vQueueDelete(updateCheckResults);
    puts("PASS background update: low/fragmented RAM defers HTTPS, bounded retry and log, resource recovery, stalled internet leaves UI loop runnable, single job, main-task flags/logs, flash deferral, task-start failure recovery and cleanup");
}
