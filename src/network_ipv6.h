#pragma once
#include <esp_netif.h>
#include <sdkconfig.h>
#include <stdint.h>
#include "network_ipv4.h"

// Wi-Fi IPv6 starts only after the IPv4 connection has been stable for 5 s.
// Do not set WIFI_WANT_IP6_BIT: in the pinned SDK that also switches every
// stock WiFiClient hostname connection to a different DNS/address policy.
class NetworkIpv6 {
    esp_netif_t *active=nullptr;
    uint32_t ipv4=0;
    bool done=false;
    unsigned tries=0;
    uint32_t next=0;
public:
    int last=ESP_OK;
    unsigned attempts=0;
    bool ensure(const char *key, bool connected, uint32_t now) {
        esp_netif_t *netif=connected ? esp_netif_get_handle_from_ifkey(key) : nullptr;
        esp_netif_ip_info_t info{};
        if(!netif || !esp_netif_is_netif_up(netif) ||
           esp_netif_get_ip_info(netif,&info)!=ESP_OK || !ipv4Assigned(info.ip.addr)) {
            active=nullptr; return false;
        }
        if(active!=netif || ipv4!=info.ip.addr) {
            active=netif; ipv4=info.ip.addr; done=false; tries=0; next=now+5000;
            return false;
        }
        if(done || tries>=3 || static_cast<int32_t>(now-next)<0) return false;
        next=now+5000;
        esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
        int count=esp_netif_get_all_ip6(netif,addresses);
        for(int i=0;i<count && i<CONFIG_LWIP_IPV6_NUM_ADDRESSES;++i) {
            // Include tentative addresses: recreating one during DAD would
            // restart validation. Do not wait for a global RA-assigned address.
            if(esp_netif_ip6_get_addr_type(&addresses[i])==ESP_IP6_ADDR_IS_LINK_LOCAL)
                { done=true; return false; }
        }
        last=esp_netif_create_ip6_linklocal(netif); ++attempts; ++tries;
        done=last==ESP_OK;
        return true;
    }
};
