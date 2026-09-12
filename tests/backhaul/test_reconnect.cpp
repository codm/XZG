#include "netconn/fixture.h"
#include <PeerTls.h>
#include <PeerRetry.h>
#include <vector>
uint32_t fixtureNow=0;
static unsigned tlsCalls=0,tlsLive=0;
static int handshakeResult=0;
struct CzcTls {};
CzcTls *czc_tls_new(int,const uint8_t[32],const char *,int (*)(void *,unsigned char *,size_t),
                   int (*)(void *,const unsigned char *,size_t),int (*)(void *,unsigned char *,size_t),void *) {
    ++tlsLive; return new CzcTls;
}
void czc_tls_delete(CzcTls *t) { if(t) { --tlsLive; delete t; } }
int czc_tls_handshake(CzcTls *) { ++tlsCalls; return handshakeResult; }
int czc_tls_read(CzcTls *,uint8_t *,size_t) { return 0; }
int czc_tls_write(CzcTls *,const uint8_t *,size_t) { return 0; }

int main() {
    const uint8_t key[32]{};
    // Five minutes of a black-holed Master, while the application gets every
    // service tick. No TLS into SYN_SENT, bounded core calls, no allocation leak.
    czc::PeerRetry retry; PeerTls peer; tcp_pcb pcb; netconn conn;
    std::vector<uint32_t> attempts;
    unsigned uiTicks=0;
    for(fixtureNow=0;fixtureNow<300000;++fixtureNow) {
        ++uiTicks;
        if(retry.due(fixtureNow,peer.fd>=0,false,0)) {
            if(!attempts.empty()) assert(conn.deleted==1 && tlsLive==0);
            attempts.push_back(fixtureNow); retry.started();
            pcb=tcp_pcb{}; pcb.state=SYN_SENT; conn=netconn{}; conn.pcb.tcp=&pcb;
            assert(peer.start(&conn,false,key,"czc-peer-v2"));
        }
        peer.tick();
    }
    assert(uiTicks==300000 && attempts.size()<=13 && !tlsCalls);
    assert(attempts[1]-attempts[0]>=6000 && attempts[2]-attempts[1]>=8000);
    assert(attempts.back()-attempts[attempts.size()-2]>=34000);
    assert(coreCalls<1200); peer.close(); assert(tlsLive==0);
    retry.retrySoon(fixtureNow);
    assert(!retry.due(fixtureNow+1999,false,false,0));
    assert(retry.due(fixtureNow+2000,false,false,0));

    // A slowly established TCP connection still gets its own full TLS deadline.
    pcb=tcp_pcb{}; pcb.state=SYN_SENT; conn=netconn{}; conn.pcb.tcp=&pcb;
    assert(peer.start(&conn,false,key,"czc-peer-v2"));
    fixtureNow+=3950; peer.tick(); assert(peer.fd>=0 && !tlsCalls);
    pcb.state=ESTABLISHED; fixtureNow+=50; peer.tick(); assert(peer.fd>=0 && tlsCalls==1);
    unsigned oldCalls=tlsCalls;
    for(unsigned n=0;n<2999;++n) { ++fixtureNow; peer.tick(); }
    assert(peer.fd>=0 && tlsCalls-oldCalls<=300);
    fixtureNow+=20; peer.tick(); assert(peer.fd<0 && !strcmp(peer.error,"tls_handshake") && !tlsLive);

    // Fast RST and a later successful connection release/recreate only this peer.
    pcb=tcp_pcb{}; pcb.state=CLOSED; conn=netconn{}; conn.pcb.tcp=&pcb;
    assert(peer.start(&conn,false,key,"czc-peer-v2")); peer.tick();
    assert(peer.fd<0 && !strcmp(peer.error,"peer_connect") && conn.deleted==1);
    pcb=tcp_pcb{}; conn=netconn{}; conn.pcb.tcp=&pcb; handshakeResult=1;
    assert(peer.start(&conn,false,key,"czc-peer-v2")); peer.tick(); assert(peer.ready);
    peer.close(); assert(!tlsLive);

    // Elapsed-time arithmetic survives wrap; a long failure does not consume its pause.
    czc::PeerRetry wrap; uint32_t start=0xfffffff0;
    assert(wrap.due(start,false,false,500)); wrap.started();
    assert(!wrap.due(start+40000,true,false,500));
    assert(!wrap.due(start+40001,false,false,500));
    assert(!wrap.due(start+42500,false,false,500));
    assert(wrap.due(start+42501,false,false,500)); wrap.started();
    assert(!wrap.due(start+42502,true,true,0));
    assert(!wrap.due(start+72502,true,true,0));
    assert(!wrap.due(start+72503,false,false,0));
    assert(wrap.remaining(start+72503)==2000);
    puts("PASS reconnect: black-holed TCP, refused TCP, separate TCP/TLS deadlines, 20 Hz connect polls, 2..30 s quiet backoff, wrap, recovery, no leaks");
}
