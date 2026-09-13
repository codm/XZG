#pragma once
#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <esp_system.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

// TLS 1.2 PSK fixture for protocol rejection tests, using the system Mbed TLS.
class Tls {
public:
    int fd=-1;
    bool ready=false;
    bool initialized=false;
    uint32_t since=0;
    uint32_t failures=0;
    int family=0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    static int rng(void *, unsigned char *p, size_t n) { esp_fill_random(p,n); return 0; }
    int (*randomSource)(void *, unsigned char *, size_t)=rng;
    static int sendBio(void *ctx, const unsigned char *p, size_t n) {
        int r=::send(*static_cast<int *>(ctx),p,n,0);
        if(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return r<0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
    }
    static int recvBio(void *ctx, unsigned char *p, size_t n) {
        int r=::recv(*static_cast<int *>(ctx),p,n,0);
        if(r<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) return MBEDTLS_ERR_SSL_WANT_READ;
        return r<0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : r;
    }
    void close() {
        ready=false; family=0;
        if(fd>=0) { ::shutdown(fd,SHUT_RDWR); ::close(fd); fd=-1; }
        if(initialized) { mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config); initialized=false; }
    }
    bool start(int socket, bool server, const uint8_t key[32], const char *identity) {
        close(); fd=socket; since=millis();
        sockaddr_storage address{}; socklen_t length=sizeof(address);
        if(!getpeername(fd,reinterpret_cast<sockaddr *>(&address),&length)) {
            family=address.ss_family;
            static const uint8_t mappedPrefix[12]={0,0,0,0,0,0,0,0,0,0,255,255};
            if(family==AF_INET6 && !memcmp(&reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr,mappedPrefix,12)) family=AF_INET;
        }
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
        int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
        int idle=10,interval=5,count=3;
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof(idle));
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&interval,sizeof(interval));
        setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&count,sizeof(count));
        mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&config); initialized=true;
        static const int ciphers[]={MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,0};
        if(mbedtls_ssl_config_defaults(&config,server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)) { close(); return false; }
        mbedtls_ssl_conf_min_version(&config,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_max_version(&config,MBEDTLS_SSL_MAJOR_VERSION_3,MBEDTLS_SSL_MINOR_VERSION_3);
        mbedtls_ssl_conf_ciphersuites(&config,ciphers);
        mbedtls_ssl_conf_rng(&config,randomSource,nullptr);
        mbedtls_ssl_conf_renegotiation(&config,MBEDTLS_SSL_RENEGOTIATION_DISABLED);
        if(mbedtls_ssl_conf_psk(&config,key,32,reinterpret_cast<const unsigned char *>(identity),strlen(identity)) ||
           mbedtls_ssl_setup(&ssl,&config)) { close(); return false; }
        mbedtls_ssl_set_bio(&ssl,&fd,sendBio,recvBio,nullptr);
        return true;
    }
    void tick() {
        if(fd<0 || ready) return;
        int r=mbedtls_ssl_handshake(&ssl);
        if(!r) { ready=true; return; }
        if((r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE) || millis()-since>3000) {
            ++failures; close();
        }
    }
    int read(uint8_t *p, size_t n) {
        if(!ready) return 0;
        int r=mbedtls_ssl_read(&ssl,p,n);
        if(r==MBEDTLS_ERR_SSL_WANT_READ || r==MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if(r<=0) { close(); return -1; } return r;
    }
    int write(const uint8_t *p, size_t n) {
        if(!ready) return 0;
        int r=mbedtls_ssl_write(&ssl,p,n);
        if(r==MBEDTLS_ERR_SSL_WANT_READ || r==MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if(r<=0) { close(); return -1; } return r;
    }
};
