#pragma once
#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_system.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

inline int tlsRandom(void *, unsigned char *p, size_t n) { esp_fill_random(p,n); return 0; }

inline int listener(uint16_t port, bool ipv6=true) {
    int fd=socket(ipv6 ? AF_INET6 : AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(fd<0) return -1;
    int zero=0, one=1;
    setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&zero,sizeof(zero));
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    sockaddr_in6 address{}; address.sin6_family=AF_INET6; address.sin6_port=htons(port);
    sockaddr_in address4{}; address4.sin_family=AF_INET; address4.sin_port=htons(port);
    if(bind(fd,ipv6 ? reinterpret_cast<sockaddr *>(&address) : reinterpret_cast<sockaddr *>(&address4),ipv6 ? sizeof(address) : sizeof(address4)) || listen(fd,8)) { ::close(fd); return -1; }
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK); return fd;
}
