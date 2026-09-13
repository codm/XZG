#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
#include <StatusSnapshot.h>

static const char sample[]=R"({"fault":"radio_revision","role":"satellite","radio_revision":20260913,"radio_required":20260917,"peer":false,"peers":[{"ieee":"00124b0000000001","fault":"peer_disconnected","online":false}]})";

int main() {
    // Demonstrate borrowed-string lifetime with a still-live input buffer:
    // overwrite the still-live input buffer after copying its document.
    StaticJsonDocument<4096> previous;
    {
        char buffer[sizeof(sample)]; memcpy(buffer,sample,sizeof(buffer));
        StaticJsonDocument<3072> snapshot; assert(!deserializeJson(snapshot,buffer));
        auto target=previous.to<JsonObject>();
        for(JsonPair pair:snapshot.as<JsonObject>()) target[pair.key()]=pair.value();
        memset(buffer,0,sizeof(buffer));
        assert(target["fault"]!="radio_revision");
        previous.clear();
    }
    StaticJsonDocument<4096> result;
    {
        char buffer[sizeof(sample)]; memcpy(buffer,sample,sizeof(buffer));
        assert(czc::copyStatusSnapshot(result.to<JsonObject>(),buffer));
        memset(buffer,0,sizeof(buffer));
        assert(result["fault"]=="radio_revision");
    }
    assert(result["role"]=="satellite");
    assert(result["radio_revision"]==20260913);
    assert(result["peers"][0]["ieee"]=="00124b0000000001");
    assert(result["peers"][0]["fault"]=="peer_disconnected");
    std::string serialized; serializeJson(result,serialized);
    assert(serialized==sample);
    assert(!czc::copyStatusSnapshot(result.to<JsonObject>(),"{broken"));
    czc::RadioVersion version; uint8_t body[32]{};
    auto set=[&](uint32_t revision,uint8_t protocol) {
        body[1]=protocol;
        for(unsigned i=0;i<4;i++) body[2+i]=revision>>(8*i);
    };
    set(20260913,1);
    assert(!strcmp(version.inspect(true,body,sizeof(body)),"radio_revision"));
    assert(version.revision==20260913 && version.protocol==1 && !version.compatible);
    set(20260917,1); assert(!strcmp(version.inspect(true,body,sizeof(body)),"radio_revision"));
    set(20260913,2); assert(!strcmp(version.inspect(true,body,sizeof(body)),"radio_revision"));
    set(20260917,2); assert(version.inspect(true,body,sizeof(body))==nullptr && version.compatible);
    assert(!strcmp(version.inspect(false,body,sizeof(body)),"radio_info") && !version.compatible);
    assert(!strcmp(version.inspect(true,body,5),"radio_info"));
    assert(!strcmp(version.inspect(true,body,31),"radio_revision") && !version.compatible);
    // Eight peers with full IPv6 addresses must survive the status-copy path.
    DynamicJsonDocument eight(4096); auto list=eight.createNestedArray("peers");
    for(unsigned i=0;i<8;i++) {
        auto p=list.createNestedObject(); char ieee[17];snprintf(ieee,sizeof(ieee),"00124b00000000%02x",i);
        p["ieee"]=ieee;p["ip"]="fe80:1234:5678:9abc:def0:1234:5678:abcd%2";
        p["online"]=true;p["fault"]="none";p["address"]=100+i;p["af"]=6;
    }
    char buffer[3072];assert(measureJson(eight)<sizeof(buffer));serializeJson(eight,buffer,sizeof(buffer));
    result.clear();assert(czc::copyStatusSnapshot(result.to<JsonObject>(),buffer));
    memset(buffer,0,sizeof(buffer));assert(result["peers"].size()==8);
    assert(result["peers"][7]["ip"]=="fe80:1234:5678:9abc:def0:1234:5678:abcd%2");
    // Production Role response: one document, including eight worst-case peer
    // rows, root settings and memory diagnostics. No borrowed cache strings.
    DynamicJsonDocument response(4096);
    assert(czc::loadStatusSnapshot(response,sample));
    assert(response["status"]["fault"]=="radio_revision");
    for(unsigned i=0;i<8;i++) {
        auto p=eight["peers"][i];
        p["fault"]="Ошибка настройки радио / peer_disconnected";
    }
    eight["radio_revision"]=20260917;
    eight["master_host"]="master.gateway.example.net";
    eight["fault"]="peer_deadline";
    eight["own_ieee"]="00124b002e11424d";
    eight["role"]="master";
    for(const char *key:{"rev","peers_online","peer_limit","paused","uart_ok","debug","bootstrap","af", "uart_cmd0","uart_cmd1","radio_reset_pending","radio_required","radio_protocol","radio_compatible","peer_tls_bytes","connect_attempts","retry_ms","mode","maintenance","uart_restarts"}) eight[key]=20260917;
    eight["uart_error"]="srsp_timeout"; eight["join"]="network_migrating";
    eight["last_peer_error"]="confirmation_timeout";
    assert(!eight.overflowed());
    serializeJson(eight,buffer,sizeof(buffer)); assert(measureJson(eight)<sizeof(buffer));
    assert(czc::loadStatusSnapshot(response,buffer));
    memset(buffer,0,sizeof(buffer));
    response["mode"]=1;response["peer_host"]="master.gateway.example.net";
    response["peer_port"]=7443;response["admin_port"]=7444;
    response["ipv6"]=true;response["raw_tcp"]=true;response["debug_mode"]=true;
    response["key_set"]=true;response["token"]=std::string(32,'a');response["psk"]=std::string(64,'b');
    response["status"]["heap"]["free"]=12345;response["status"]["heap"]["largest"]=4321;response["status"]["heap"]["minimum"]=10000;
    assert(!response.overflowed());
    assert(response["status"]["peers"][7]["ip"]=="fe80:1234:5678:9abc:def0:1234:5678:abcd%2");
    assert(response["status"]["peers"].size()==8);
    assert(response["psk"].as<std::string>()==std::string(64,'b'));
    assert(!czc::loadStatusSnapshot(response,"{broken"));
    assert(!czc::loadStatusSnapshot(response,"[]"));
    assert(czc::loadStatusSnapshot(response,"{}"));
    DynamicJsonDocument tooSmall(16);
    assert(!czc::loadStatusSnapshot(tooSmall,sample));
    puts("PASS Status snapshot: reproduced borrowed-buffer regression, owned nested keys/values survive, radio revision/protocol compatibility and failed reads");
    puts("PASS Role memory: single 4096-byte document with eight complete peers/config/diagnostics, owned strings and malformed/low-memory detection");
}
