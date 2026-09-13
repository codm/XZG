#include <PeerTls.h>
#include <Backhaul.h>
#include <cassert>
#include <cstdio>
#include <thread>
#include <algorithm>
using backhaul::Endpoint;
struct Pair {
    PeerTls master,satellite;
    Endpoint a{1,2},b{2,1};
    unsigned receivedA=0,receivedB=0,completed=0;
    bool submitted=false;
};
static uint8_t message[81];
static bool admit(const backhaul::Frame &f,void *ctx) {
    assert(f.length==sizeof(message)); assert(!memcmp(f.payload.data(),message,sizeof(message)));
    ++*static_cast<unsigned *>(ctx); return true;
}
static void sockets(int &client,int &server) {
    int listen=listener(0); assert(listen>=0);
    sockaddr_in6 addr{}; socklen_t len=sizeof(addr); assert(!getsockname(listen,(sockaddr *)&addr,&len));
    addr.sin6_addr=in6addr_loopback; client=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP); assert(client>=0);
    assert(!connect(client,(sockaddr *)&addr,len)); server=accept(listen,nullptr,nullptr); assert(server>=0); ::close(listen);
}
static void pump(PeerTls &from,Endpoint &send,PeerTls &to,Endpoint &receive,unsigned &count) {
    send.tick(millis()); receive.tick(millis());
    size_t len=0; const uint8_t *p=send.output(len);
    if(len) { int n=from.write(p,len); assert(n>=0); if(n) assert(send.consumeOutput(n)); }
    uint8_t bytes[2048]; int peek=recv(to.fd,bytes,sizeof(bytes),MSG_PEEK);
    if(peek>0) assert(std::search(bytes,bytes+peek,message,message+sizeof(message))==bytes+peek);
    int n=to.read(bytes,sizeof(bytes)); assert(n>=0); if(n) assert(receive.input(bytes,n,millis()));
    receive.serviceIncoming(admit,&count,millis());
}
int main() {
    uint8_t key[32]; for(unsigned i=0;i<32;i++) key[i]=i+1;
    for(unsigned i=0;i<sizeof(message);i++) message[i]=i+9;
    Pair pair[8]; uint32_t end=millis()+5000;
    for(auto &p:pair) {
        int c,s; sockets(c,s); assert(p.master.start(s,true,key,"czc-peer-v2")); assert(p.satellite.start(c,false,key,"czc-peer-v2"));
    }
    bool ready=false;
    while(!ready) {
        ready=true;
        for(auto &p:pair) { p.master.tick(); p.satellite.tick(); assert(p.master.fd>=0 && p.satellite.fd>=0); ready&=p.master.ready && p.satellite.ready; }
        assert(static_cast<int32_t>(end-millis())>0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    size_t masterBytes=czc_tls_allocated()/2; assert(masterBytes<70000);
    for(auto &p:pair) assert(p.a.open(1,millis()) && p.b.open(1,millis()));
    unsigned completed=0;
    while(completed<64) {
        for(auto &p:pair) {
            pump(p.master,p.a,p.satellite,p.b,p.receivedB); pump(p.satellite,p.b,p.master,p.a,p.receivedA);
            if(!p.submitted && p.a.state()==backhaul::State::Ready && p.b.state()==backhaul::State::Ready) {
                uint32_t seq;
                for(unsigned i=0;i<4;i++) { assert(p.a.submit(message,sizeof(message),millis(),seq)==backhaul::Submit::Queued); assert(p.b.submit(message,sizeof(message),millis(),seq)==backhaul::Submit::Queued); }
                p.submitted=true;
            }
            backhaul::Result result;
            for(Endpoint *ep:{&p.a,&p.b}) while(ep->takeResult(result)) { assert(result.delivery==backhaul::Delivery::AcceptedIntoRadioQueue); ++completed; ++p.completed; }
        }
        assert(static_cast<int32_t>(end-millis())>0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(auto &p:pair) assert(p.completed==8 && p.receivedA==4 && p.receivedB==4);
    // One failed authentication while seven links keep delivering heartbeats.
    pair[0].master.close(); pair[0].satellite.close(); int c,s; sockets(c,s);
    assert(pair[0].master.start(s,true,key,"czc-peer-v2")); key[0]^=1;
    assert(pair[0].satellite.start(c,false,key,"czc-peer-v2"));
    while(pair[0].master.fd>=0 && pair[0].satellite.fd>=0) {
        pair[0].master.tick(); pair[0].satellite.tick(); assert(!pair[0].master.ready && !pair[0].satellite.ready);
        for(unsigned i=1;i<8;i++) { auto &p=pair[i]; pump(p.master,p.a,p.satellite,p.b,p.receivedB); pump(p.satellite,p.b,p.master,p.a,p.receivedA); }
        assert(static_cast<int32_t>(end-millis())>0); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for(auto &p:pair) { p.master.close(); p.satellite.close(); }
    assert(czc_tls_allocated()==0);
    printf("PASS eight-peer TLS: IPv6, 64 encrypted admissions, wrong PSK isolated, no allocation leaks; eight-server TLS estimate=%zu bytes, endpoints=%zu bytes\n",masterBytes,8*sizeof(Endpoint));
}
