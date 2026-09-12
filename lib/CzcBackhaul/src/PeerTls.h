#pragma once
#include "SocketHelpers.h"
#include <CzcPeerTls.h>
#ifdef ARDUINO_ARCH_ESP32
#include "NetconnStream.h"
using PeerHandle=netconn *;
#else
#include <arpa/inet.h>
using PeerHandle=int;
#endif
// PSK-only TLS engine with bounded records.
class PeerTls {
    CzcTls *tls=nullptr;
#ifdef ARDUINO_ARCH_ESP32
    NetconnStream stream;
    bool connecting=false;
    uint32_t connectPoll=0;
#endif
    uint32_t handshakePoll=0;
    static int send(void *ctx,const unsigned char *p,size_t n) {
#ifdef ARDUINO_ARCH_ESP32
        return static_cast<NetconnStream *>(ctx)->write(p,n);
#else
        int r=::send(*static_cast<int *>(ctx),p,n,0);
        return r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) ? 0 : r;
#endif
    }
    static int recv(void *ctx,unsigned char *p,size_t n) {
#ifdef ARDUINO_ARCH_ESP32
        return static_cast<NetconnStream *>(ctx)->read(p,n);
#else
        int r=::recv(*static_cast<int *>(ctx),p,n,0);
        if(!r) return -1;
        return r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) ? 0 : r;
#endif
    }
public:
    int fd=-1,family=0; bool ready=false; uint32_t since=0,failures=0;
    const char *error="tls_handshake";
    int (*randomSource)(void *,unsigned char *,size_t)=tlsRandom;
    PeerTls()=default;
    PeerTls(const PeerTls &)=delete;
    ~PeerTls() { close(); }
    bool peerAddress(char *out,size_t size) {
        if(!size) return false;
        out[0]=0;
#ifdef ARDUINO_ARCH_ESP32
        return stream.peerAddress(out,size);
#else
        sockaddr_storage address{}; socklen_t n=sizeof(address);
        if(fd<0 || getpeername(fd,reinterpret_cast<sockaddr *>(&address),&n)) return false;
        const void *bytes=address.ss_family==AF_INET ?
            static_cast<const void *>(&reinterpret_cast<sockaddr_in *>(&address)->sin_addr) :
            static_cast<const void *>(&reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr);
        return inet_ntop(address.ss_family,bytes,out,size)!=nullptr;
#endif
    }
    void close() {
        ready=false; family=0;
#ifdef ARDUINO_ARCH_ESP32
        stream.close();
#else
        if(fd>=0) { ::shutdown(fd,SHUT_RDWR); ::close(fd); }
#endif
        fd=-1; czc_tls_delete(tls); tls=nullptr;
    }
    bool start(PeerHandle socket,bool server,const uint8_t key[32],const char *identity) {
        close(); since=millis(); handshakePoll=since-10; error="tls_handshake";
#ifdef ARDUINO_ARCH_ESP32
        if(!stream.start(socket)) { ++failures; return false; }
        connecting=true; connectPoll=since-50;
        fd=1; // Active marker only; peer TCP does not use a BSD descriptor on ESP32.
        family=stream.family;
        void *io=&stream;
#else
        fd=socket;
        sockaddr_storage address{}; socklen_t size=sizeof(address);
        if(!getpeername(fd,reinterpret_cast<sockaddr *>(&address),&size)) {
            family=address.ss_family;
            static const uint8_t prefix[12]={0,0,0,0,0,0,0,0,0,0,255,255};
            if(family==AF_INET6 && !memcmp(&reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr,prefix,12)) family=AF_INET;
        }
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
        int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        void *io=&fd;
#endif
        tls=czc_tls_new(server,key,identity,randomSource,send,recv,io);
        if(!tls) { ++failures; close(); return false; } return true;
    }
    void tick() {
        if(fd<0 || ready) return;
        uint32_t now=millis();
#ifdef ARDUINO_ARCH_ESP32
        // Do not send TLS records into SYN_SENT on every FreeRTOS tick. The
        // TCP/IP task also serves the UI; poll establishment at most 20 Hz.
        if(connecting) {
            if(uint32_t(now-connectPoll)<50) return;
            connectPoll=now;
            int state=stream.connectState();
            if(state<0 || (state==0 && uint32_t(now-since)>=4000)) {
                error=state<0 ? "peer_connect" : "peer_connect_timeout"; ++failures; close(); return;
            }
            if(!state) return;
            connecting=false; since=now;
        }
#endif
        if(uint32_t(now-handshakePoll)<10) return;
        handshakePoll=now;
        int r=czc_tls_handshake(tls); if(r==1) {
            ready=true;
#ifdef ARDUINO_ARCH_ESP32
            family=stream.addressFamily();
#endif
        }
        else if(r<0 || millis()-since>3000) { ++failures; close(); }
    }
    int read(uint8_t *p,size_t n) { if(!ready) return 0; int r=czc_tls_read(tls,p,n); if(r<0) close(); return r; }
    int write(const uint8_t *p,size_t n) { if(!ready) return 0; int r=czc_tls_write(tls,p,n); if(r<0) close(); return r; }
};
