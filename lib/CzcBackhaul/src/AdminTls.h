#pragma once
#include "SocketHelpers.h"
#include <CzcPeerTls.h>

// The admin stream accepts full 16 KiB TLS records from ordinary PSK clients.
// Outgoing records are split at 1024 bytes; no change to the ZNP byte stream.
class AdminTls {
    CzcTls *tls=nullptr;
    static int send(void *ctx,const unsigned char *p,size_t n) {
        int r=::send(*static_cast<int *>(ctx),p,n,0);
        return r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) ? 0 : r;
    }
    static int recv(void *ctx,unsigned char *p,size_t n) {
        int r=::recv(*static_cast<int *>(ctx),p,n,0);
        if(!r) return -1;
        return r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) ? 0 : r;
    }
    void failed(const char *phase,int code) {
        errorPhase=phase; errorCode=code; ++failures; close();
    }
public:
    int fd=-1,family=0;
    bool ready=false;
    uint32_t since=0,failures=0;
    const char *errorPhase="none";
    int errorCode=0;
    int (*randomSource)(void *,unsigned char *,size_t)=tlsRandom;
    AdminTls()=default;
    AdminTls(const AdminTls &)=delete;
    AdminTls &operator=(const AdminTls &)=delete;
    ~AdminTls() { close(); }
    void close() {
        ready=false; family=0;
        if(fd>=0) { ::shutdown(fd,SHUT_RDWR); ::close(fd); fd=-1; }
        czc_tls_delete(tls); tls=nullptr;
    }
    bool start(int socket,bool server,const uint8_t key[32],const char *identity) {
        close(); fd=socket; since=millis();
        sockaddr_storage address{}; socklen_t size=sizeof(address);
        if(!getpeername(fd,reinterpret_cast<sockaddr *>(&address),&size)) {
            family=address.ss_family;
            static const uint8_t prefix[12]={0,0,0,0,0,0,0,0,0,0,255,255};
            if(family==AF_INET6 && !memcmp(&reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr,prefix,12)) family=AF_INET;
        }
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
        int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
        int idle=10,interval=5,count=3;
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof(idle));
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&interval,sizeof(interval));
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&count,sizeof(count));
        int error=0;
        tls=czc_tls_new_admin(server,key,identity,randomSource,send,recv,&fd,&error);
        if(!tls) { failed("setup",error); return false; }
        return true;
    }
    void tick() {
        if(fd<0 || ready) return;
        int r=czc_tls_handshake(tls);
        if(r==1) { ready=true; return; }
        if(r<0) failed("handshake",czc_tls_error(tls));
        else if(uint32_t(millis()-since)>3000) failed("handshake_timeout",-0x6800);
    }
    int read(uint8_t *p,size_t n) {
        if(!ready) return 0;
        int r=czc_tls_read(tls,p,n);
        if(r<0) {
            int code=czc_tls_error(tls);
            // Peer close/close_notify is normal controller teardown.
            if(code==0 || code==-1 || code==-0x7880) close();
            else failed("read",code);
        }
        return r;
    }
    int write(const uint8_t *p,size_t n) {
        if(!ready) return 0;
        int r=czc_tls_write(tls,p,n);
        if(r<0) failed("write",czc_tls_error(tls));
        return r;
    }
};
