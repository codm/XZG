// Execute the production NVS loader and web handlers in both build variants.
#include <Settings.h>
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
using String=std::string;
static czc::Settings cfg;
static std::vector<uint8_t> persisted;
static unsigned saves=0;
struct Preferences {
    bool begin(const char *,bool){ return true; }
    size_t getBytesLength(const char *){ return persisted.size(); }
    size_t getBytes(const char *,void *out,size_t n){ memcpy(out,persisted.data(),n); return n; }
    size_t putBytes(const char *,const void *in,size_t n){
        auto p=static_cast<const uint8_t *>(in); persisted.assign(p,p+n); ++saves; return n;
    }
    void end(){}
};
static constexpr int UNDEFINED=0,COORDINATOR=1,ROUTER=2,OPENTHREAD=3;
static struct { unsigned socketPort=6638,serialSpeed=115200; int workMode=0,zbRole=COORDINATOR; } systemCfg;
static struct { bool ethEnable=true,wifiEnable=false; } networkCfg;
static struct { bool zbFlashing=false; } vars;
constexpr int WORK_MODE_NETWORK=0;
static bool maintenanceRequested=false,maintenanceActive=false,bootstrapRestart=false;
static const char *joinResult="idle";
static uint32_t restartAt=0;
static bool running=false,bslHold=false,roleRestart=false;
static bool backhaulRoleAllowed(){ return systemCfg.zbRole==COORDINATOR; }
struct BackhaulMaintenance { ~BackhaulMaintenance(){} };
static void printLogMsg(const char *){}
static size_t strlcpy(char *out,const char *in,size_t size){ size_t n=strlen(in); if(size){ size_t copy=n<size-1?n:size-1;memcpy(out,in,copy);out[copy]=0;} return n; }
static uint32_t millis(){ return 1000; }
#include "../../.backhaul-tests/role-change-under-test.inc"
static int maintenanceMutex=0;
static int xSemaphoreCreateRecursiveMutex(){ return 1; }
static char csrf[33]{};
namespace czc_random {
static void begin(){}
static int fill(void *,uint8_t *out,size_t n){ memset(out,0xab,n); return 0; }
}
static struct Web {
    String request,response;
    int code=0;
    String arg(const char *){ return request; }
    void send(int status,const char *,const String &body){ code=status; response=body; }
} endpoint;
static Web *web=&endpoint;
static bool webAuthorize(bool =false){ return true; }
static void webReply(int code,const char *result){ web->code=code; web->response=result; }
static bool backhaulStatus(JsonDocument &doc){ doc.createNestedObject("status"); return true; }
static void memoryStatus(JsonObject obj){ obj["free"]=64000; }
#include "../../.backhaul-tests/debug-config-under-test.inc"

static void assertReadOnlyBuildSettings(){
    webConfig(); assert(web->code==200);
    DynamicJsonDocument result(4096); assert(!deserializeJson(result,web->response));
    assert(result["debug_mode"].as<bool>()==czc::debugBuild);
    assert(result.containsKey("admin_port")==czc::debugBuild);
    if(czc::debugBuild) assert(result["admin_port"]==7444);
    assert(result["peer_port"]==cfg.peerPort);
    assert(!result.containsKey("ipv6") && !result.containsKey("raw_tcp"));
    assert(result["psk"].as<String>()==String(64,'a'));
}
int main(){
    czc::Settings legacy; legacy.mode=2; legacy.hasKey=1;
    memset(legacy.key,0xaa,sizeof(legacy.key)); strcpy(legacy.host,"master.example");
    legacy.legacyIpv6=0; legacy.legacyRawTcp=0; legacy.legacyDiagnostics=1; legacy.legacyAdminPort=8999;
    // Both supported record lengths retain role, key, address and peer port.
    for(size_t size:{size_t(180),sizeof(legacy)}){
        auto bytes=reinterpret_cast<const uint8_t *>(&legacy); persisted.assign(bytes,bytes+size);
        cfg=czc::Settings(); backhaulLoad();
        assert(cfg.mode==2 && cfg.hasKey && !memcmp(cfg.key,legacy.key,32));
        assert(!strcmp(cfg.host,legacy.host) && cfg.peerPort==7443);
        assertReadOnlyBuildSettings();
    }
    // POST accepts peer settings; build capabilities are read-only.
    const String base=R"({"mode":2,"peer_host":"master.example","peer_port":7443,"psk":"")";
    for(const char *extra:{"}",",\"debug_mode\":true,\"admin_port\":7443,\"ipv6\":false,\"raw_tcp\":false}",
                          ",\"debug_mode\":false,\"admin_port\":8999}"}){
        web->request=base+extra; unsigned before=saves; webSave();
        assert(web->code==200 && saves==before+1 && restartAt==2500);
        cfg=czc::Settings(); backhaulLoad();
        assert(cfg.legacyIpv6==1 && cfg.legacyRawTcp==1);
        assert(!cfg.legacyDiagnostics && cfg.legacyAdminPort==7444);
        assert(cfg.mode==2 && cfg.peerPort==7443 && !memcmp(cfg.key,legacy.key,32));
        assertReadOnlyBuildSettings();
    }
    cfg.peerPort=czc::debugAdminPort;
    assert(bool(czc::validate(cfg,6638))==czc::debugBuild);
    cfg.peerPort=7443;
    assert(bool(czc::validate(cfg,7444))==czc::debugBuild);
    cfg.peerPort=6638; assert(czc::validate(cfg,6638));
    cfg.peerPort=7443;
    for(int role:{ROUTER,OPENTHREAD}) {
        cfg=legacy; systemCfg.zbRole=role;
        web->request=base+"}"; unsigned before=saves; webSave();
        assert(web->code==400 && web->response=="coordinator_required" && saves==before);
        running=true; bslHold=roleRestart=false; backhaulRoleChanged();
        assert(bslHold && roleRestart && restartAt==2500 && saves==before+1);
        webConfig(); DynamicJsonDocument result(4096); assert(!deserializeJson(result,web->response));
        assert(result["mode"]==0 && result["radio_role"]==role && !result["role_supported"].as<bool>());
        assert(result["peer_host"].as<String>()==legacy.host && result["psk"].as<String>()==String(64,'a'));
        running=false; cfg=czc::Settings(); backhaulLoad();
        assert(cfg.mode==0 && !memcmp(cfg.key,legacy.key,32));
        systemCfg.zbRole=COORDINATOR; backhaulLoad(); assert(cfg.mode==0);
        auto bytes=reinterpret_cast<const uint8_t *>(&legacy); persisted.assign(bytes,bytes+sizeof(legacy));
        systemCfg.zbRole=role; backhaulLoad(); assert(cfg.mode==0);
    }
    puts(czc::debugBuild ? "PASS debug: fixed admin port, automatic diagnostics, NVS loading and read-only API capabilities"
                        : "PASS prod: NVS loading and POST retain build capabilities and role/key/peer settings");
}
