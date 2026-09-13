#pragma once
#include <ArduinoJson.h>
#include <esp_netif.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

// Read the active interface, never the saved static configuration or peer-task cache.
inline void networkInterfaceStatus(JsonObject obj, const char *id, const char *key,
                                   bool enabled, bool connected) {
    obj["id"]=id; obj["enabled"]=enabled;
    esp_netif_t *netif=esp_netif_get_handle_from_ifkey(key);
    connected=enabled && connected && netif && esp_netif_is_netif_up(netif);
    obj["connected"]=connected; obj["ipv4"]="";
    JsonArray ipv6=obj.createNestedArray("ipv6");
    if(!connected) return;
    esp_netif_ip_info_t info{};
    char address[INET6_ADDRSTRLEN]{};
    if(esp_netif_get_ip_info(netif,&info)==ESP_OK && info.ip.addr && esp_ip4addr_ntoa(&info.ip,address,sizeof(address)))
        obj["ipv4"]=address;
    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES]{};
    int count=esp_netif_get_all_ip6(netif,addresses);
    for(int i=0;i<count && i<CONFIG_LWIP_IPV6_NUM_ADDRESSES;i++) {
        auto &ip=addresses[i];
        if(!(ip.addr[0]|ip.addr[1]|ip.addr[2]|ip.addr[3]) || !inet_ntop(AF_INET6,ip.addr,address,sizeof(address))) continue;
        JsonObject item=ipv6.createNestedObject(); item["address"]=address;
        item["link_local"]=esp_netif_ip6_get_addr_type(&ip)==ESP_IP6_ADDR_IS_LINK_LOCAL;
    }
}

void networkStatus(JsonObject obj);
void networkIpv6Loop();
