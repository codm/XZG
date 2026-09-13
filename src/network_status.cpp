#include "network_status.h"
#include "config.h"
#include "network_ipv6.h"
#include "web.h"
#include <ETH.h>
#include <WiFi.h>

extern NetworkConfigStruct networkCfg;
extern SysVarsStruct vars;

static NetworkIpv6 ipv6Wifi;

void networkIpv6Loop() {
    static uint32_t last=0;
    uint32_t now=millis();
    if(now-last<1000) return;
    last=now;
    auto start=[&](NetworkIpv6 &state,const char *key,const char *label,bool enabled) {
        unsigned before=state.attempts; int previous=state.last;
        if(state.ensure(key,enabled,now) && (!before || previous!=state.last)) {
            printLogMsg(String("[IPv6] ")+label+(state.last==ESP_OK ? " link-local started" : " init failed: "+String(state.last)));
        }
    };
    start(ipv6Wifi,"WIFI_STA_DEF","WiFi",networkCfg.wifiEnable && WiFi.status()==WL_CONNECTED);
    // Ethernet and AP retain their existing event-driven initialization.
}

void networkStatus(JsonObject obj) {
    JsonArray interfaces=obj.createNestedArray("interfaces");
    networkInterfaceStatus(interfaces.createNestedObject(),"eth","ETH_DEF",networkCfg.ethEnable,ETH.linkUp());
    JsonObject wifi=interfaces.createNestedObject();
    networkInterfaceStatus(wifi,"wifi","WIFI_STA_DEF",networkCfg.wifiEnable,WiFi.status()==WL_CONNECTED);
    wifi["ipv6_init_attempts"]=ipv6Wifi.attempts;
    wifi["ipv6_init_status"]=ipv6Wifi.last;
    if(vars.apStarted) networkInterfaceStatus(interfaces.createNestedObject(),"ap","WIFI_AP_DEF",true,true);
}
