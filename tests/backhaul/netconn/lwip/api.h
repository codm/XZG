#pragma once
#include <cstdint>
#include <cstddef>
#include <sys/socket.h>
using err_t=int;
constexpr int ERR_OK=0,ERR_WOULDBLOCK=-7,ERR_INPROGRESS=-5,ERR_TIMEOUT=-3,ERR_MEM=-1,ERR_CLSD=-15;
constexpr int NETCONN_COPY=1,NETCONN_DONTBLOCK=4,NETCONN_TCP=1,NETCONN_TCP_IPV6=2;
enum tcp_state { CLOSED, SYN_SENT, SYN_RCVD, ESTABLISHED };
struct tcp_pcb {bool aborted=false,nodelay=false; tcp_state state=ESTABLISHED;};
struct pbuf {uint16_t tot_len; uint8_t bytes[16];};
struct ip_addr_t {bool v6=false,mapped=false;};
struct netconn {struct {tcp_pcb *tcp;} pcb{}; pbuf *input=nullptr; err_t readResult=ERR_WOULDBLOCK,writeResult=ERR_OK; size_t writeCount=0; bool nonblocking=false; int family=AF_INET6; unsigned deleted=0;};
extern ip_addr_t any;
#define IP6_ADDR_ANY (&any)
#define IP4_ADDR_ANY (&any)
#define IP_IS_V6(p) ((p)->v6)
#define ip_2_ip6(p) (p)
#define ip6_addr_isipv4mappedipv6(p) ((p)->mapped)
void netconn_thread_init();
void netconn_set_nonblocking(netconn *,int);
err_t netconn_peer(netconn *,ip_addr_t *,uint16_t *);
char *ipaddr_ntoa_r(const ip_addr_t *,char *,int);
err_t netconn_delete(netconn *);
err_t netconn_write_partly(netconn *,const void *,size_t,int,size_t *);
err_t netconn_recv_tcp_pbuf_flags(netconn *,pbuf **,int);
netconn *netconn_new(int);
err_t netconn_bind(netconn *,const ip_addr_t *,uint16_t);
err_t netconn_listen_with_backlog(netconn *,int);
void pbuf_free(pbuf *);
uint16_t pbuf_copy_partial(pbuf *,void *,uint16_t,uint16_t);
