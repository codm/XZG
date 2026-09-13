// Actual rawWork/rawClose + actual UART task over loopback TCP. SerialFixture
// stands in for the radio; no Zigbee network or physical device is modified.
#include <Znp.h>
#include <Settings.h>
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <csignal>
#include <memory>
#include <algorithm>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <unistd.h>

SerialFixture Serial2;
thread_local FixtureTask *fixtureCurrentTask=nullptr;
class WiFiClient {
    struct Socket { int fd; explicit Socket(int n):fd(n) {} ~Socket(){ if(fd>=0) ::close(fd); } };
    std::shared_ptr<Socket> socket;
public:
    WiFiClient()=default;
    explicit WiFiClient(int fd):socket(std::make_shared<Socket>(fd)) {}
    int fd() const { return socket ? socket->fd : -1; }
    bool connected() {
        if(fd()<0) return false;
        uint8_t b; int n=recv(fd(),&b,1,MSG_PEEK|MSG_DONTWAIT);
        return n>0 || (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR));
    }
    explicit operator bool() const { return fd()>=0; }
    void stop(){ if(fd()>=0){ shutdown(fd(),SHUT_RDWR); ::close(fd()); socket->fd=-1; } }
    void setNoDelay(bool){}
    int remoteIP() const { return 0x7f000001; }
    int available(){ int n=0; if(fd()>=0) ioctl(fd(),FIONREAD,&n); return n; }
    int read(){ uint8_t b; return recv(fd(),&b,1,MSG_DONTWAIT)==1 ? b : -1; }
    // Match the bundled Arduino SDK: WiFiClient does not override Print.
    int availableForWrite(){ return 0; }
    size_t write(const uint8_t *p,size_t n){ int r=::send(fd(),p,n,MSG_DONTWAIT); return r>0 ? r : 0; }
};
static struct Server {
    std::deque<WiFiClient> waiting;
    bool hasClient() const { return !waiting.empty(); }
    WiFiClient available(){ WiFiClient c=waiting.front(); waiting.pop_front(); return c; }
} server;
static WiFiClient rawClient;
static Radio radio;
static czc::Settings cfg;
static struct { unsigned connectedClients=0; } vars;
static struct { bool fwEnabled=false; int fwIp=0; } systemCfg;
static struct { int fd=-1; } adminTls;
static uint32_t controllerReset=0;
static unsigned connects=0,disconnects=0;
static void socketClientConnected(int,int){ ++vars.connectedClients; ++connects; }
static void socketClientDisconnected(int){ assert(vars.connectedClients==1); --vars.connectedClients; ++disconnects; }
static struct { bool bound=true; struct { uint32_t sequence=0,ticket=0; } mapping[4]; } peers[8];
static struct { bool compatible=true; } radioVersion;
static uint8_t localInfo[32]{};
static bool satelliteInitialized=true,masterInitialized=true;
static uint32_t lastInfo=123;
static unsigned invalidations=0;
static void disconnectPeers(const char *) {
    ++invalidations;
    for(auto &p:peers) {
        assert(!p.bound);
        for(auto &m:p.mapping) assert(!m.sequence && !m.ticket);
    }
}
#include "../../.backhaul-tests/lifecycle-under-test.inc"
static void rawWork();
static void cycle(){ serviceRadioLifecycle(); rawWork(); }

static size_t sendLimit=260;
static std::deque<int> sendErrors;
static int fixtureSend(int fd,const void *p,size_t n,int flags){
    assert(flags&MSG_DONTWAIT);
    if(!sendErrors.empty()){ errno=sendErrors.front(); sendErrors.pop_front(); return -1; }
    return ::send(fd,p,std::min(n,sendLimit),flags);
}
#define send fixtureSend
#include "../../.backhaul-tests/raw-under-test.inc"
#undef send

static ZnpFrame frame(uint8_t c0,uint8_t cmd,std::initializer_list<uint8_t> body={}){
    ZnpFrame f; f.build(c0,cmd,body.begin(),body.size()); return f;
}
static void respond(const uint8_t *p,size_t){
    ZnpFrame f;
    if(p[2]==0x41 && p[3]==0) f=frame(0x41,0x80,{1,2,1,2,7,1}); // SYS resetInd
    else if(p[2]==0x21 && p[3]==1) f=frame(0x61,1,{0x59,0x06}); // SYS ping
    else if(p[2]==0x21 && p[3]==2) f=frame(0x61,2,{2,1,2,7,1,0x62,0x28,0x35,1}); // SYS version
    else f=frame((p[2]&31)|0x60,p[3],{0,p[3]});
    Serial2.feed(f.data,f.size);
}
static int connectClient(){
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(listener,reinterpret_cast<sockaddr *>(&address),sizeof(address)));
    assert(!listen(listener,1)); socklen_t length=sizeof(address);
    assert(!getsockname(listener,reinterpret_cast<sockaddr *>(&address),&length));
    int controller=socket(AF_INET,SOCK_STREAM,0); assert(controller>=0);
    assert(!connect(controller,reinterpret_cast<sockaddr *>(&address),length));
    int accepted=accept(listener,nullptr,nullptr); assert(accepted>=0); close(listener);
    server.waiting.emplace_back(accepted); rawWork(); return controller;
}
static void transmit(int fd,const uint8_t *p,size_t n){ assert(::send(fd,p,n,0)==static_cast<int>(n)); }
static void transmit(int fd,const ZnpFrame &f){ transmit(fd,f.data,f.size); }
static void expect(int fd,const ZnpFrame &f,unsigned timeout=500){
    std::vector<uint8_t> bytes;
    uint32_t start=millis();
    while(bytes.size()<f.size && millis()-start<timeout){
        cycle(); uint8_t buf[260]; int n=recv(fd,buf,f.size-bytes.size(),MSG_DONTWAIT);
        if(n>0) bytes.insert(bytes.end(),buf,buf+n);
        else assert(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR));
        vTaskDelay(1);
    }
    assert(bytes.size()==f.size && !memcmp(bytes.data(),f.data,f.size));
}
static void finish(int fd){ close(fd); rawWork(); assert(!vars.connectedClients); }
int main(){
    signal(SIGPIPE,SIG_IGN); assert(radio.begin()); Serial2.onWrite=respond; cfg.legacyRawTcp=0;
    int fd=connectClient(); assert(vars.connectedClients==1);
    // herdsman 10.9.2 TCP bootloader skip noise, SYS ping, resetInd, version.
    uint8_t skip=0xef; transmit(fd,&skip,1); rawWork(); assert(!Serial2.writes);
    ZnpFrame ping=frame(0x21,1),pong=frame(0x61,1,{0x59,0x06});
    transmit(fd,ping.data,2); rawWork(); assert(!Serial2.writes);
    transmit(fd,ping.data+2,ping.size-2); expect(fd,pong);
    transmit(fd,frame(0x41,0,{1})); expect(fd,frame(0x41,0x80,{1,2,1,2,7,1}));
    transmit(fd,frame(0x21,2)); expect(fd,frame(0x61,2,{2,1,2,7,1,0x62,0x28,0x35,1}));
    // Two SREQs in one TCP write; partial sends and temporary socket errors.
    std::vector<uint8_t> doubled(ping.data,ping.data+ping.size);
    doubled.insert(doubled.end(),ping.data,ping.data+ping.size);
    transmit(fd,doubled.data(),doubled.size()); sendLimit=2; sendErrors={EAGAIN,EINTR};
    expect(fd,pong); expect(fd,pong); sendLimit=260;
    // Interleaved private peer RPC and controller exchanges keep SRSP ownership.
    std::thread peer([]{ for(unsigned i=0;i<24;i++){ ZnpFrame out; assert(radio.command(0xc6,nullptr,0,out)); assert(out.data[5]==0xc6); } });
    for(unsigned i=0;i<24;i++){ transmit(fd,ping); expect(fd,pong); }
    peer.join(); assert(!radio.fatal);
    ZnpFrame event=frame(0x44,0x81,{9,8,7}); Serial2.feed(event.data,event.size); expect(fd,event);
    unsigned before=Serial2.writes;
    for(uint8_t cmd=0xc7;cmd<=0xcb;cmd++){ transmit(fd,frame(0x21,cmd)); expect(fd,frame(0x61,cmd,{0x1a})); }
    transmit(fd,frame(0x3f,1)); expect(fd,frame(0x7f,1,{0x1a})); assert(Serial2.writes==before);
    // An unfinished incoming or outgoing frame cannot leak into a new session.
    transmit(fd,ping.data,2); rawWork(); finish(fd);
    fd=connectClient(); transmit(fd,ping); expect(fd,pong);
    transmit(fd,ping); rawWork(); sendLimit=2; rawWork(); finish(fd); sendLimit=260;
    fd=connectClient(); transmit(fd,ping); expect(fd,pong);
    ++controllerReset; rawWork(); assert(!vars.connectedClients); close(fd);
    fd=connectClient(); transmit(fd,ping); expect(fd,pong);
    // A second controller or TLS owner must never steal the UART connection.
    int second=connectClient(); assert(vars.connectedClients==1); close(second);
    transmit(fd,ping); expect(fd,pong); finish(fd);
    adminTls.fd=123; second=connectClient(); assert(!vars.connectedClients); close(second); adminTls.fd=-1;
    systemCfg.fwEnabled=true; second=connectClient(); assert(!vars.connectedClients); close(second);
    systemCfg.fwIp=0x7f000001; fd=connectClient(); transmit(fd,ping); expect(fd,pong); systemCfg.fwEnabled=false;
    // Broken frame, real send failure, and stalled reader all close boundedly.
    ZnpFrame bad=ping; bad.data[bad.size-1]^=1; transmit(fd,bad); rawWork(); assert(!vars.connectedClients); close(fd);
    fd=connectClient(); transmit(fd,ping); rawWork(); sendErrors={EPIPE}; rawWork(); assert(!vars.connectedClients); close(fd);
    fd=connectClient(); transmit(fd,ping); rawWork(); sendErrors={EAGAIN}; rawWork(); assert(vars.connectedClients==1);
    vTaskDelay(3010); sendErrors={EAGAIN}; rawWork(); assert(!vars.connectedClients); close(fd);
    fd=connectClient(); transmit(fd,ping); expect(fd,pong); finish(fd);
    // Restore traffic: delayed NV writes and repeated resets while background
    // commands are due. A real radio stays silent while STARTUP_OPTION clears NV.
    std::vector<std::thread> responses;
    unsigned resets=0;
    Serial2.onWrite=[&](const uint8_t *p,size_t n){
        if(p[2]==0x41 && !p[3]) {
            unsigned delay=++resets==1 ? 2200 : 900;
            responses.emplace_back([=]{ vTaskDelay(delay); ZnpFrame f=frame(0x41,0x80,{1,2,1,2,7,1}); Serial2.feed(f.data,f.size); });
        } else if((p[2]==0x21 && p[3]==9) || (p[2]==0x2f && p[3]==5)) {
            ZnpFrame f=frame((p[2]&31)|0x60,p[3],{0});
            responses.emplace_back([=]{ vTaskDelay(850); Serial2.feed(f.data,f.size); });
        } else respond(p,n);
    };
    fd=connectClient();
    for(unsigned step=0;step<3;step++) {
        transmit(fd,frame(0x21,9,{3,0,0,1,uint8_t(step ? 0 : 3)})); expect(fd,frame(0x61,9,{0}),2000);
        for(auto &p:peers) { p.bound=true; for(auto &m:p.mapping) { m.sequence=7; m.ticket=11; } }
        unsigned generation=invalidations;
        transmit(fd,frame(0x41,0,{uint8_t(step ? 0 : 1)})); cycle();
        assert(radio.resetWaiting && !radio.fatal);
        cycle(); assert(invalidations==generation+1 && !radioVersion.compatible && !satelliteInitialized && !masterInitialized && !lastInfo);
        unsigned writes=Serial2.writes;
        for(unsigned i=0;i<20;i++) {
            ZnpFrame out; assert(!radio.command(0xc0,nullptr,0,out));
            cycle(); vTaskDelay(10);
        }
        // Coalesced next request is held in TCP until resetInd, never sent into boot.
        transmit(fd,ping); cycle(); assert(Serial2.writes==writes && !radio.fatal && rawClient.connected());
        expect(fd,frame(0x41,0x80,{1,2,1,2,7,1}),3000); expect(fd,pong);
        assert(!radio.resetWaiting && !radio.fatal);
        ZnpFrame out; assert(radio.command(0xc0,nullptr,0,out));
        transmit(fd,frame(0x2f,5,{0})); expect(fd,frame(0x6f,5,{0}),2000);
        ZnpFrame ready=frame(0x45,0xc0,{9}); Serial2.feed(ready.data,ready.size); expect(fd,ready);
    }
    for(auto &response:responses) response.join();
    assert(Radio::controllerTimeout(0x2f,5)==40000 && Radio::controllerTimeout(0x25,0x40)==40000);
    assert(Radio::controllerTimeout(0x21,9)==6000);
    // A missing resetInd has a finite deadline and a specific diagnostic.
    Serial2.onWrite=nullptr; transmit(fd,frame(0x41,0,{1})); cycle();
    radio.tick(millis()+29000); assert(!radio.fatal);
    radio.tick(millis()+30001); assert(radio.fatal && !strcmp(radio.error,"reset_timeout"));
    cycle(); assert(!vars.connectedClients); close(fd);
    // Hardware recovery holds the receiver; retain resetInd buffered during the
    // CCTools reset delay, and tolerate BSL residue before that valid indication.
    assert(radio.suspend()); radio.expectReset();
    uint8_t residue[]={0xcc,0xfe,0,0,0,1}; Serial2.feed(residue,sizeof(residue));
    ZnpFrame resetInd=frame(0x41,0x80,{1,2,1,2,7,1}); Serial2.feed(resetInd.data,resetInd.size);
    radio.resume(); vTaskDelay(10); cycle(); assert(!radio.resetWaiting && !radio.fatal);
    Serial2.onWrite=respond; fd=connectClient(); transmit(fd,ping); expect(fd,pong); finish(fd);
    assert(connects==disconnects && !radio.fatal); radio.stop();
    puts("PASS controller TCP: actual handler + UART task; bootloader/ping/reset/version, fragmentation, partial writes/backpressure, peer coexistence, events, access rules and reconnect cleanup");
    puts("PASS controller restore: delayed NV writes and resetInd, repeated clear/restore/commissioning, queued next request, eight stale peer sessions, reset deadline and hardware recovery");
}
