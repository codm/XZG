#include <AdminTls.h>
#include "OpenSslClient.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int failAfter=-1;
static size_t ceiling=SIZE_MAX,largest=SIZE_MAX,allocationCalls=0;
extern "C" void *__real_calloc(size_t,size_t);
extern "C" void *__wrap_calloc(size_t n,size_t s) {
    ++allocationCalls;
    if(failAfter==0 || (s && n>SIZE_MAX/s) || n*s>largest || czc_tls_allocated()+n*s>ceiling) return nullptr;
    if(failAfter>0) --failAfter;
    return __real_calloc(n,s);
}
static void sockets(int &client,int &server) {
    int listening=listener(0); assert(listening>=0);
    sockaddr_in6 addr{}; socklen_t length=sizeof(addr);
    assert(!getsockname(listening,reinterpret_cast<sockaddr *>(&addr),&length));
    addr.sin6_addr=in6addr_loopback;
    client=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP); assert(client>=0);
    assert(!connect(client,reinterpret_cast<sockaddr *>(&addr),length));
    server=accept(listening,nullptr,nullptr); assert(server>=0); ::close(listening);
}
static void handshake(AdminTls &server,OpenSslClient &client) {
    uint32_t start=millis();
    while(server.fd>=0 && client.fd>=0 && (!server.ready || !client.ready)) {
        server.tick(); client.tick();
        assert(uint32_t(millis()-start)<5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
int main() {
    uint8_t key[32]; for(unsigned i=0;i<32;++i) key[i]=i;
    AdminTls server; OpenSslClient client;
    int c,s; sockets(c,s);
    assert(server.start(s,true,key,"czc-admin-v1"));
    size_t setupCalls=allocationCalls;
    server.close(); ::close(c); assert(czc_tls_allocated()==0);
    // Fail every setup allocation. All partially initialized state and the
    // accepted descriptor must be released, with an actionable setup error.
    for(size_t i=0;i<setupCalls;++i) {
        sockets(c,s); failAfter=i;
        assert(!server.start(s,true,key,"czc-admin-v1"));
        assert(server.fd==-1 && !server.ready && !strcmp(server.errorPhase,"setup") && server.errorCode<0);
        assert(fcntl(s,F_GETFD)==-1 && errno==EBADF);
        assert(czc_tls_allocated()==0); ::close(c); failAfter=-1;
    }
    // A constrained/fragmented budget must accommodate the real server while
    // the standard client sends an entire 16 KiB TLS application record.
    ceiling=24*1024; largest=17*1024;
    sockets(c,s); assert(server.start(s,true,key,"czc-admin-v1"));
    assert(client.start(c,false,key,"czc-admin-v1"));
    size_t startCalls=allocationCalls;
    handshake(server,client); assert(server.ready && client.ready);
    size_t handshakeCalls=allocationCalls-startCalls;
    static uint8_t input[16384],output[16384];
    for(unsigned i=0;i<sizeof(input);++i) input[i]=i%251;
    size_t sent=0,received=0; uint32_t start=millis();
    while(received<sizeof(input)) {
        if(sent<sizeof(input)) { int n=client.write(input+sent,sizeof(input)-sent); assert(n>=0); sent+=n; }
        int n=server.read(output+received,sizeof(output)-received);
        if(n<0) fprintf(stderr,"admin read: %s code=%d allocated=%zu peak=%zu\n",server.errorPhase,server.errorCode,czc_tls_allocated(),czc_tls_peak());
        assert(n>=0); received+=n;
        assert(uint32_t(millis()-start)<3000);
    }
    assert(!memcmp(input,output,sizeof(input)));
    server.close(); client.close(); assert(!czc_tls_allocated());
    size_t peak=czc_tls_peak(); assert(peak<=ceiling);
    ceiling=largest=SIZE_MAX;
    // Keep another session alive so global PSA cleanup cannot hide abandoned
    // keys from a failed handshake. Every failure must preserve this session.
    AdminTls anchor; OpenSslClient anchorClient;
    sockets(c,s); assert(anchor.start(s,true,key,"czc-admin-v1"));
    assert(anchorClient.start(c,false,key,"czc-admin-v1"));
    handshake(anchor,anchorClient); assert(anchor.ready && anchorClient.ready);
    size_t anchorBytes=czc_tls_allocated();
    for(size_t i=0;i<handshakeCalls;++i) {
        sockets(c,s); assert(server.start(s,true,key,"czc-admin-v1"));
        assert(client.start(c,false,key,"czc-admin-v1")); failAfter=i;
        handshake(server,client);
        // Required allocations must fail closed. If an upstream allocation is
        // optional, the resulting session must still deliver correct records.
        if(server.ready && client.ready) {
            uint8_t byte=42,out=0; int sentByte=client.write(&byte,1); assert(sentByte==1);
            uint32_t began=millis(); int got=0;
            while(!got) { got=server.read(&out,1); assert(got>=0 && uint32_t(millis()-began)<1000); }
            assert(out==byte);
        } else {
            assert(server.fd<0 || client.fd<0);
            if(server.fd<0) assert(!strcmp(server.errorPhase,"handshake") && server.errorCode<0);
        }
        server.close(); client.close(); failAfter=-1;
        if(czc_tls_allocated()!=anchorBytes) fprintf(stderr,"allocation fault %zu/%zu: allocated=%zu expected=%zu\n",i,handshakeCalls,czc_tls_allocated(),anchorBytes);
        assert(czc_tls_allocated()==anchorBytes);
        if(i%250==0) {
            uint8_t byte=73,out=0; assert(anchorClient.write(&byte,1)==1);
            uint32_t began=millis(); int got=0;
            while(!got) { got=anchor.read(&out,1); assert(got>=0 && uint32_t(millis()-began)<1000); }
            assert(out==byte);
        }
    }
    anchor.close(); anchorClient.close(); assert(!czc_tls_allocated());
    // A new session after all failures must work without reinitializing firmware.
    for(unsigned i=0;i<40;++i) {
        sockets(c,s); assert(server.start(s,true,key,"czc-admin-v1"));
        assert(client.start(c,false,key,"czc-admin-v1"));
        handshake(server,client); assert(server.ready && client.ready);
        server.close(); client.close(); assert(!czc_tls_allocated());
    }
    // A stalled unauthenticated socket has a finite life and an explicit reason.
    sockets(c,s); assert(server.start(s,true,key,"czc-admin-v1"));
    server.since=millis()-3001; server.tick();
    assert(server.fd<0 && !server.ready && !strcmp(server.errorPhase,"handshake_timeout"));
    ::close(c); assert(!czc_tls_allocated());
    printf("PASS admin TLS: TLS 1.3/OpenSSL, 16 KiB records, %zu setup and %zu handshake allocation faults, live-session isolation, 40 reconnects, timeout, no leaks; peak=%zu bytes, largest allocation <=17408\n",setupCalls,handshakeCalls,peak);
}
