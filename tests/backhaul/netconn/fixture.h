#pragma once
#include <NetconnStream.h>
#include <cassert>
#include <cstring>
#include <cstdio>
static bool onCore; static unsigned frees,coreCalls;
ip_addr_t any;
void netconn_thread_init(){}
void netconn_set_nonblocking(netconn *c,int value){c->nonblocking=value;}
err_t netconn_peer(netconn *c,ip_addr_t *addr,uint16_t *){addr->v6=c->family==AF_INET6;return ERR_OK;}
char *ipaddr_ntoa_r(const ip_addr_t *addr,char *out,int size){const char *ip=addr->v6 ? "fd12::186" : "192.168.8.186";return snprintf(out,size,"%s",ip)<size ? out : nullptr;}
err_t netconn_delete(netconn *c){assert(!c->pcb.tcp || c->pcb.tcp->aborted);++c->deleted;return ERR_OK;}
err_t netconn_write_partly(netconn *c,const void *,size_t len,int flags,size_t *sent){assert(flags==(NETCONN_COPY|NETCONN_DONTBLOCK));*sent=std::min(len,c->writeCount);return c->writeResult;}
err_t netconn_recv_tcp_pbuf_flags(netconn *c,pbuf **p,int flags){assert(flags==NETCONN_DONTBLOCK);*p=c->input;c->input=nullptr;return c->readResult;}
netconn *netconn_new(int){return nullptr;}
err_t netconn_bind(netconn *,const ip_addr_t *,uint16_t){return ERR_OK;}
err_t netconn_listen_with_backlog(netconn *,int n){assert(n==8);return ERR_OK;}
void pbuf_free(pbuf *p){++frees;delete p;}
uint16_t pbuf_copy_partial(pbuf *p,void *to,uint16_t n,uint16_t offset){assert(n+offset<=p->tot_len);memcpy(to,p->bytes+offset,n);return n;}
void tcp_abort(tcp_pcb *p){assert(onCore);p->aborted=true;}
void tcp_nagle_disable(tcp_pcb *p){assert(onCore);p->nodelay=true;}
err_t tcpip_api_call(err_t (*fn)(tcpip_api_call_data *),tcpip_api_call_data *data){assert(!onCore);onCore=true;++coreCalls;auto r=fn(data);onCore=false;return r;}
