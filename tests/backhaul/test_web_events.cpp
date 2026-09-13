#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include "web_refresh.h"

static bool failReserve=false;
struct String:std::string {
    using std::string::string;
    String(const std::string &s):std::string(s) {}
    bool reserve(size_t n) { if(failReserve) return false; std::string::reserve(n); return true; }
};
struct Socket {
    int descriptor;
    explicit Socket(int n):descriptor(n) {}
    void stop() { if(descriptor>=0) { close(descriptor); descriptor=-1; } }
    ~Socket() { stop(); }
};
struct WiFiClient {
    std::shared_ptr<Socket> socket;
    WiFiClient()=default;
    explicit WiFiClient(int fd):socket(std::make_shared<Socket>(fd)) {}
    int fd() const { return socket ? socket->descriptor : -1; }
    bool connected() const { return fd()>=0; }
    void stop() { if(socket) socket->stop(); socket.reset(); }
    void setNoDelay(bool) {}
};
static WiFiClient eventsClient;
static std::mutex lock;
static std::mutex *sendEventMutex=&lock;
static std::atomic<uint32_t> eventDrops{0};
static constexpr int pdTRUE=1;
static int xSemaphoreTake(std::mutex *m,unsigned ticks) { assert(ticks==0); return m->try_lock(); }
static void xSemaphoreGive(std::mutex *m) { m->unlock(); }
static bool authorized=true;
static bool is_authenticated() { return authorized; }
struct Server {
    WiFiClient incoming;
    int status=0;
    WiFiClient client() { return incoming; }
    void testSend(int n) { status=n; }
} serverWeb;
static bool forcePartial=false;
static ssize_t testSend(int fd,const void *p,size_t n,int flags) {
    assert(flags & MSG_DONTWAIT);
    return ::send(fd,p,forcePartial ? std::min(n,size_t(5)) : n,flags);
}
// Exact production SSE functions; intercept only the syscall to force partial I/O.
#define send testSend
#include "../../.backhaul-tests/web-events-under-test.inc"
#undef send

struct LegacyClient:WiFiClient {
    using WiFiClient::WiFiClient;
    bool _connected=true;
    size_t write(const uint8_t *,size_t);
};
#define WIFI_CLIENT_MAX_WRITE_RETRY 10
#define WIFI_CLIENT_SELECT_TIMEOUT_US 1000000
#define log_e(...) ((void)0)
#include "../../.backhaul-tests/legacy-web-write.inc"

static unsigned fakeNow=0,generated=0,workMs=0;
static std::vector<unsigned> sleeps;
struct StopTask {};
struct {long refreshLogs=1;} systemCfg;
static String getRootData(bool update) { assert(update); ++generated; fakeNow+=workMs; return "{\"wifiRssi\":-52,\"uptime\":123}"; }
static void vTaskDelay(unsigned ms) { sleeps.push_back(ms); fakeNow+=ms; throw StopTask{}; }
#define pdMS_TO_TICKS(n) (n)
#include "../../.backhaul-tests/web-task-under-test.inc"

static double milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
static int attach() {
    int pair[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    int size=4096; assert(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0);
    serverWeb.incoming=WiFiClient(pair[0]);
    return pair[1];
}
static void fill(int fd) {
    char buf[4096]{};
    while(::send(fd,buf,sizeof(buf),MSG_DONTWAIT)>0) {}
    assert(errno==EAGAIN || errno==EWOULDBLOCK);
}
static std::string readAvailable(int fd) {
    char buf[8192]; std::string s; ssize_t n;
    while((n=recv(fd,buf,sizeof(buf),MSG_DONTWAIT))>0) s.append(buf,n);
    return s;
}
static void taskOnce() { try { updateWebTask(nullptr); } catch(const StopTask &) {} }

int main() {
    signal(SIGPIPE,SIG_IGN);
    // Reproduce the delay with the actual pinned Arduino WiFiClient::write.
    int peer=attach(); fill(serverWeb.incoming.fd());
    LegacyClient old; old.socket=serverWeb.incoming.socket;
    auto start=std::chrono::steady_clock::now();
    assert(old.write(reinterpret_cast<const uint8_t *>("x"),1)==0);
    double oldMs=milliseconds(start); assert(oldMs>=9000);
    printf("PASS old SSE stall reproduced: %.0f ms for one write to a non-reading client\n",oldMs);
    serverWeb.incoming.stop(); close(peer);

    peer=attach(); handleEvents();
    assert(readAvailable(peer).find("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream") == 0);
    sendEventSafe("esp.fp",String("96.25"));
    assert(readAvailable(peer)=="event: esp.fp\ndata: 96.25\n\n");
    taskOnce();
    assert(readAvailable(peer)=="event: root_update\ndata: {\"wifiRssi\":-52,\"uptime\":123}\n\nevent: root_update\ndata: finish\n\n");
    fill(eventsClient.fd()); start=std::chrono::steady_clock::now();
    sendEventSafe("esp.fp",String("100"));
    assert(milliseconds(start)<100 && eventsClient.fd()<0 && eventDrops==1);
    close(peer);

    peer=attach(); handleEvents(); readAvailable(peer);
    forcePartial=true; sendEventSafe("root_update",String("{\"value\":1}")); forcePartial=false;
    assert(eventsClient.fd()<0 && eventDrops==2 && readAvailable(peer)=="event"); close(peer);
    // A new browser gets a clean stream after the interrupted record.
    peer=attach(); handleEvents(); readAvailable(peer);
    taskOnce(); assert(readAvailable(peer).find("data: finish\n\n")!=std::string::npos);
    failReserve=true; sendEventSafe("esp.fi",String("restarting")); failReserve=false;
    assert(eventsClient.fd()<0 && eventDrops==3); close(peer);

    peer=attach(); handleEvents(); readAvailable(peer); close(peer);
    sendEventSafe("esp.fp",String("50")); assert(eventsClient.fd()<0);
    peer=attach(); authorized=false; handleEvents();
    assert(serverWeb.status==401 && eventsClient.fd()<0); authorized=true;
    // A worker holding the lock never blocks an HTTP request or an OTA callback.
    std::atomic<bool> held{false},release{false};
    std::thread worker([&]{lock.lock(); held=true; while(!release) std::this_thread::yield(); lock.unlock();});
    while(!held) std::this_thread::yield();
    start=std::chrono::steady_clock::now(); handleEvents(); sendEventSafe("esp.fp",String("75"));
    assert(milliseconds(start)<100 && serverWeb.incoming.fd()<0);
    release=true; worker.join(); close(peer);

    // No subscriber: no root JSON/sensor/NVS work. Invalid saved or posted
    // intervals must yield, including after a read taking longer than a period.
    unsigned before=generated;
    for(long interval:{-1L,0L,1L,30L,2147483647L}) {
        systemCfg.refreshLogs=interval; taskOnce();
        assert(sleeps.back()>=1000 && sleeps.back()<=30000);
    }
    assert(generated==before);
    peer=attach(); handleEvents(); readAvailable(peer);
    systemCfg.refreshLogs=0; workMs=5000; fakeNow=0; taskOnce();
    assert(fakeNow==6000 && sleeps.back()==1000 && generated==before+1);
    eventsClient.stop(); close(peer);
    puts("PASS web events: backpressure/partial/closed sockets, reconnect, OTA events, allocation failure, lock contention, no-subscriber idle and bounded refresh without catch-up");
}
