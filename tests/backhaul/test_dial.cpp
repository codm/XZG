// Production dial(), with resolver/socket boundaries controlled by the fixture.
#include <Settings.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cassert>
#include <cstdio>
#include <set>
#include <string>
static czc::Settings cfg;
using esp_netif_t=int;
static int eth=2,wifi=3;
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){ return !strcmp(key,"WIFI_STA_DEF")?&wifi:&eth; }
static int esp_netif_get_netif_impl_index(esp_netif_t *iface){ return *iface; }
static size_t strlcpy(char *out,const char *in,size_t size){ size_t n=strlen(in);snprintf(out,size,"%s",in);return n; }
struct ip_addr_t { int type=0; struct { uint32_t addr=0; } v4; struct { uint32_t addr[4]{};unsigned zone=0; } v6; };
constexpr int IPADDR_TYPE_V6=6,IPADDR_TYPE_V4=4,NETCONN_TCP_IPV6=6,NETCONN_TCP=4;
#define IP_SET_TYPE_VAL(ip,value) (ip).type=(value)
#define ip_2_ip6(ip) (&(ip)->v6)
#define ip_2_ip4(ip) (&(ip)->v4)
#define ip6_addr_set_zone(ip,value) (ip)->zone=(value)
struct netconn { int type; bool nonblocking=false; uint16_t port=0; ip_addr_t address; };
using err_t=int;
constexpr int ERR_OK=0,ERR_INPROGRESS=-5;
static int result=ERR_INPROGRESS,live=0,resolverFrees=0;
static netconn *netconn_new(int type){ ++live;auto *p=new netconn;p->type=type;return p; }
static void netconn_set_nonblocking(netconn *p,int value){ p->nonblocking=value; }
static err_t netconn_connect(netconn *p,ip_addr_t *address,uint16_t port){ p->address=*address;p->port=port;return result; }
static void netconn_delete(netconn *p){ --live;delete p; }
static addrinfo addresses[2];
static sockaddr_in address4;
static sockaddr_in6 address6;
static int fixtureGetaddrinfo(const char *host,const char *port,const addrinfo *hints,addrinfo **out){
    assert(hints->ai_socktype==SOCK_STREAM && !strcmp(port,"7443"));
    addresses[0]=addrinfo{};addresses[1]=addrinfo{};address4={};address6={};
    bool literal6=strchr(host,':'); bool hostname=!strcmp(host,"master.example");
    assert(hints->ai_family==(literal6?AF_INET6:AF_UNSPEC));
    assert(!strchr(host,'%'));
    auto set=[&](unsigned i,int family){
        addresses[i].ai_family=family;
        if(family==AF_INET6){
            assert(inet_pton(AF_INET6,hostname?"2001:db8::1":host,&address6.sin6_addr)==1);
            addresses[i].ai_addr=reinterpret_cast<sockaddr *>(&address6);
        }else{
            assert(inet_pton(AF_INET,hostname?"192.0.2.1":host,&address4.sin_addr)==1);
            addresses[i].ai_addr=reinterpret_cast<sockaddr *>(&address4);
        }
    };
    set(0,literal6||hostname?AF_INET6:AF_INET);
    if(hostname){set(1,AF_INET);addresses[0].ai_next=&addresses[1];}
    *out=addresses;return 0;
}
static void fixtureFreeaddrinfo(addrinfo *p){ assert(p==addresses);++resolverFrees; }
#define getaddrinfo fixtureGetaddrinfo
#define freeaddrinfo fixtureFreeaddrinfo
#include "../../.backhaul-tests/dial-under-test.inc"
#undef getaddrinfo
#undef freeaddrinfo
int main(){
    cfg.legacyIpv6=0;cfg.legacyRawTcp=0;
    for(const char *host:{"2001:db8::1","fe80::1%eth","fe80::1%wifi","192.0.2.1"}){
        strcpy(cfg.host,host);netconn *p=dial(7443);assert(p && p->nonblocking && p->port==7443);
        assert(p->type==(strchr(host,':')?6:4) && p->address.type==p->type);
        if(strstr(host,"%eth")) assert(p->address.v6.zone==2);
        if(strstr(host,"%wifi")) assert(p->address.v6.zone==3);
        netconn_delete(p);assert(live==0);
    }
    strcpy(cfg.host,"master.example");std::set<int> families;
    for(unsigned i=0;i<4;++i){netconn *p=dial(7443);assert(p);families.insert(p->type);netconn_delete(p);}
    assert(families==std::set<int>({4,6}));
    result=-1;assert(!dial(7443) && live==0 && resolverFrees==9);
    puts("PASS peer address selection: IPv6 literal/scope, IPv4, DNS alternatives and failed-connect cleanup; address determines IP family");
}
