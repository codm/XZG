#include "LegacyTls.h"
#include <PeerTls.h>
#include "OpenSslClient.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <initializer_list>

static void sockets(int &client,int &server) {
    int listening=listener(0); assert(listening>=0);
    sockaddr_in6 addr{}; socklen_t n=sizeof(addr);
    assert(!getsockname(listening,reinterpret_cast<sockaddr *>(&addr),&n));
    addr.sin6_addr=in6addr_loopback;
    client=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP); assert(client>=0);
    assert(!connect(client,reinterpret_cast<sockaddr *>(&addr),n));
    server=accept(listening,nullptr,nullptr); assert(server>=0); ::close(listening);
}
static unsigned randomCalls;
static int failedRandom(void *,unsigned char *,size_t) { ++randomCalls; return -1; }
int main() {
    const uint8_t key[32]={1};
    // Verify protocol enforcement against a TLS 1.2 peer linked to Mbed TLS 2,
    // including attempts with the correct PSK and identity.
    for(bool newServer:{false,true}) {
        int c,s; sockets(c,s);
        PeerTls current; Tls legacy;
        assert(current.start(newServer?s:c,newServer,key,"czc-peer-v2"));
        assert(legacy.start(newServer?c:s,!newServer,key,"czc-peer-v2"));
        uint32_t start=millis();
        while(current.fd>=0 && legacy.fd>=0) {
            current.tick(); legacy.tick(); assert(!current.ready && !legacy.ready);
            assert(uint32_t(millis()-start)<5000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        current.close(); legacy.close(); assert(czc_tls_allocated()==0);
    }
    // An unavailable application RNG must fail closed, including PSA key setup.
    for(bool server:{false,true}) {
        int c,s; sockets(c,s); PeerTls current; current.randomSource=failedRandom;
        OpenSslClient client; randomCalls=0;
        assert(current.start(server?s:c,server,key,"czc-peer-v2"));
        if(server) assert(client.start(c,false,key,"czc-peer-v2"));
        uint32_t start=millis();
        while(current.fd>=0) {
            if(server) client.tick();
            current.tick(); assert(!current.ready && uint32_t(millis()-start)<2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(randomCalls);
        if(server) client.close(); else ::close(s);
        assert(czc_tls_allocated()==0);
    }
    puts("PASS TLS 1.3 policy: TLS 1.2 rejected in both roles, entropy failure closed, no leaks or SDK symbol collisions");
}
