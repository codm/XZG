#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdint.h>
#include "../../src/network_ipv4.h"

// Execute the pinned SDK's station IP/DNS and WiFiSTA::config implementations,
// and the production DHCP selection and DNS repair against observable netifs.
struct IPAddress {
    uint32_t value;
    IPAddress(uint32_t v=0):value(v) {}
    operator uint32_t() const {return value;}
    std::string toString() const {return std::to_string(value);}
};
using esp_err_t=int;
using esp_interface_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_IF_WIFI_STA=0,ESP_IF_WIFI_AP=1,ESP_IF_ETH=2;
constexpr int ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED=10,ESP_IPADDR_TYPE_V4=0;
constexpr int ESP_NETIF_DNS_MAIN=0,ESP_NETIF_DNS_BACKUP=1,ESP_NETIF_DNS_FALLBACK=2,WL_CONNECTED=3;
constexpr uint32_t INADDR_NONE=UINT32_MAX;
enum esp_netif_dhcp_status_t {ESP_NETIF_DHCP_INIT,ESP_NETIF_DHCP_STARTED,ESP_NETIF_DHCP_STOPPED};
struct Address {uint32_t addr=0;};
struct esp_netif_ip_info_t {Address ip,gw,netmask;};
struct esp_netif_dns_info_t {struct {int type;struct {Address ip4;} u_addr;} ip;};
struct esp_netif_t {
    bool up=true,dhcp=false,fail=false,failDns=false;
    esp_netif_ip_info_t info;
    uint32_t dns[3]{};
    unsigned sets=0,stops=0,starts=0,dnsSets=0;
} station,ap,ethernet;
static esp_netif_t *esp_netifs[]={&station,&ap,&ethernet};
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    if(!strcmp(key,"WIFI_STA_DEF"))return &station;
    if(!strcmp(key,"ETH_DEF"))return &ethernet;
    return nullptr;
}
static bool esp_netif_is_netif_up(esp_netif_t *n) {return n->up;}
static int esp_netif_dhcpc_get_status(esp_netif_t *n,esp_netif_dhcp_status_t *out) {
    *out=n->dhcp?ESP_NETIF_DHCP_STARTED:ESP_NETIF_DHCP_STOPPED;return n->fail?ESP_FAIL:ESP_OK;
}
static int esp_netif_dhcpc_stop(esp_netif_t *n) {++n->stops;n->dhcp=false;return 0;}
static int esp_netif_dhcpc_start(esp_netif_t *n) {++n->starts;n->dhcp=true;return 0;}
static int esp_netif_set_ip_info(esp_netif_t *n,esp_netif_ip_info_t *in) {++n->sets;n->info=*in;return 0;}
static int esp_netif_set_dns_info(esp_netif_t *n,int which,esp_netif_dns_info_t *in) {
    ++n->dnsSets;assert(in->ip.type==ESP_IPADDR_TYPE_V4);
    if(n->failDns)return ESP_FAIL;
    n->dns[which]=in->ip.u_addr.ip4.addr;return 0;
}
#define log_v(...) ((void)0)
#define log_e(...) ((void)0)
#define LOGD(...) ((void)0)
static unsigned logs=0,beginCalls=0;
static void printLogMsg(const char *) {++logs;}
class WiFiSTAClass {
public:
    bool _useStaticIp=false,connected=true;
    int interface=0;
    bool enableSTA(bool) {return true;}
    bool config(IPAddress,IPAddress,IPAddress,IPAddress=IPAddress(),IPAddress=IPAddress());
    int status() {return connected?WL_CONNECTED:0;}
    bool linkUp() {return connected;}
    IPAddress dnsIP() {return esp_netifs[interface]->dns[0];}
} WiFi,ETH;
static struct {
    bool wifiDhcp=true,wifiEnable=true,ethEnable=true;
    IPAddress wifiIp{159},wifiGate{1},wifiMask{0x00ffffff},wifiDns1{1},wifiDns2{2};
} networkCfg;
static struct {IPAddress savedWifiDNS,savedEthDNS;} vars;
#include "../../.backhaul-tests/sdk-station-ip-under-test.inc"
#include "../../.backhaul-tests/app-ipv4-under-test.inc"

static void lease(esp_netif_t &n,uint32_t ip) {
    n.info.ip.addr=ip;n.info.gw.addr=1;n.info.netmask.addr=0x00ffffff;n.dns[0]=1;
}
int main() {
    ETH.interface=ESP_IF_ETH;
    // Exact old DHCP call: the SDK stores 255.255.255.255 and never starts DHCP.
    assert(WiFi.config(INADDR_NONE,INADDR_NONE,INADDR_NONE,INADDR_NONE));
    assert(station.info.ip.addr==UINT32_MAX && !station.dhcp && station.starts==0);
    puts("PASS old DHCP failure reproduced: INADDR_NONE becomes static broadcast IP with DHCP stopped in pinned SDK");
    // New app branch clears stale addressing and starts the DHCP client.
    configureFromApp();assert(beginCalls==1);
    assert(station.info.ip.addr==0 && station.info.gw.addr==0 && station.info.netmask.addr==0 && station.dhcp);
    lease(station,159);checkDNS(true);assert(uint32_t(vars.savedWifiDNS)==1);
    // Exact old DNS call switches a valid lease into static addressing.
    assert(WiFi.config(station.info.ip.addr,station.info.gw.addr,station.info.netmask.addr,IPAddress(1)));
    assert(!station.dhcp);
    puts("PASS old DNS failure reproduced: DNS-only repair through config stops DHCP");
    configureFromApp();lease(station,159);ethernet.dhcp=true;lease(ethernet,143);
    checkDNS(true);
    auto before=station,ethBefore=ethernet;
    station.dns[0]=0;ethernet.dns[0]=0;
    checkDNS(false);
    assert(station.dhcp && ethernet.dhcp && station.dns[0]==1 && ethernet.dns[0]==1);
    assert(station.starts==before.starts && station.stops==before.stops && station.sets==before.sets);
    assert(ethernet.starts==ethBefore.starts && ethernet.stops==ethBefore.stops && ethernet.sets==ethBefore.sets);
    assert(!memcmp(&station.info,&before.info,sizeof(before.info)));
    assert(!memcmp(&ethernet.info,&ethBefore.info,sizeof(ethBefore.info)));
    // New DNS from DHCP is not overwritten by a cached resolver.
    station.dns[0]=256;checkDNS(false);assert(station.dns[0]==256);
    checkDNS(true);assert(uint32_t(vars.savedWifiDNS)==256);
    station.dns[0]=0;station.failDns=true;auto oldLogs=logs;
    checkDNS(false);assert(station.dns[0]==0 && logs==oldLogs);station.failDns=false;
    WiFi.connected=false;unsigned writes=station.dnsSets;checkDNS(false);assert(writes==station.dnsSets);
    WiFi.connected=true;vars.savedWifiDNS=UINT32_MAX;checkDNS(false);assert(writes==station.dnsSets);
    assert(!restoreNetworkDns("absent",IPAddress(1)));
    station.up=false;assert(!restoreNetworkDns("WIFI_STA_DEF",IPAddress(1)));station.up=true;
    // Saved static configuration still uses all supplied fields.
    networkCfg.wifiDhcp=false;configureFromApp();
    assert(!station.dhcp && station.info.ip.addr==159 && station.info.netmask.addr==0x00ffffff);
    assert(station.dns[0]==1 && station.dns[1]==2);
    networkCfg.wifiDhcp=true;station.fail=true;auto oldBegin=beginCalls;configureFromApp();assert(beginCalls==oldBegin);
    puts("PASS IPv4 DHCP/DNS: production app and SDK; zero-address DHCP, stale IP clearing, preserved static settings, DNS-only repair keeps IP/lease, new resolver, disconnect/invalid DNS/failures");
}
