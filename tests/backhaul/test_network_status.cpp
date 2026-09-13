#include <cassert>
#include <cstring>
#include <cstdio>
#include "../../src/network_status.h"

static esp_netif_t device;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { return !strcmp(key,"missing") ? nullptr : &device; }
bool esp_netif_is_netif_up(esp_netif_t *n) { return n->up; }
int esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *out) { if(n->fail) return -1; *out=n->info; return ESP_OK; }
int esp_netif_get_all_ip6(esp_netif_t *n,esp_ip6_addr_t *out) { if(n->fail) return -1; memcpy(out,n->ipv6,sizeof(n->ipv6)); return n->count; }
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *ip,char *out,int size) { return const_cast<char *>(inet_ntop(AF_INET,&ip->addr,out,size)); }
esp_ip6_addr_type_t esp_netif_ip6_get_addr_type(esp_ip6_addr_t *ip) { auto *b=reinterpret_cast<unsigned char *>(ip->addr); return b[0]==0xfe && (b[1]&0xc0)==0x80 ? ESP_IP6_ADDR_IS_LINK_LOCAL : ESP_IP6_ADDR_IS_GLOBAL; }
int main() {
    StaticJsonDocument<2048> doc;
    device.up=true; inet_pton(AF_INET,"192.168.8.144",&device.info.ip.addr);
    const char *ips[]={"fe80::1234","fd12::144","2001:db8::144"};
    for(int i=0;i<3;i++) inet_pton(AF_INET6,ips[i],device.ipv6[i].addr);
    device.count=3;
    auto read=[&](bool enabled=true,bool connected=true,const char *key="ETH_DEF") {
        doc.clear(); networkInterfaceStatus(doc.to<JsonObject>(),"eth",key,enabled,connected);
    };
    read(); assert(!doc.overflowed());
    assert(doc["connected"]==true && doc["ipv4"]=="192.168.8.144");
    assert(doc["ipv6"].size()==3);
    for(int i=0;i<3;i++) { assert(doc["ipv6"][i]["address"]==ips[i]); assert(doc["ipv6"][i]["link_local"]==(i==0)); }
    read(false); assert(doc["connected"]==false && doc["ipv4"]=="" && doc["ipv6"].size()==0);
    read(true,false); assert(doc["connected"]==false && doc["ipv4"]=="");
    read(true,true,"missing"); assert(doc["connected"]==false);
    device.up=false; read(); assert(doc["connected"]==false && doc["ipv6"].size()==0);
    device.up=true; device.fail=true; read(); assert(doc["ipv4"]=="" && doc["ipv6"].size()==0);
    device.fail=false; device.count=0; device.info.ip.addr=0; read(); assert(doc["ipv4"]=="" && doc["ipv6"].size()==0);
    puts("PASS Network status: live IPv4, all IPv6, link-local classification, down/disabled/missing interfaces and read failures");
}
