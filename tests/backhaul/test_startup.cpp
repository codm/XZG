#include <Settings.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <initializer_list>

static constexpr int WORK_MODE_NETWORK=0;
static constexpr int COORDINATOR=1,ROUTER=2,OPENTHREAD=3;
static struct { int workMode=0,serialSpeed=115200,zbRole=COORDINATOR; } systemCfg;
static czc::Settings cfg;
static bool running=false,bootSelectionMade=false,uartSelected=false;
static const char *fault="unconfigured";
static char csrf[33]="fixture";
static int maintenanceMutex=1;
static uint32_t bootEpoch=0;
static unsigned epochWrites=0,uartStarts=0,taskStarts=0,adminStarts=0,peerStarts=0;
struct Preferences {
    bool begin(const char *,bool){ return true; }
    uint32_t getUInt(const char *,int){ return 7; }
    size_t putUInt(const char *,uint32_t){ ++epochWrites; return 4; }
    void end(){}
};
namespace czc_random { static int fill(void *,unsigned char *,size_t){ return 0; } }
struct TLS { int (*randomSource)(void *,unsigned char *,size_t)=nullptr; };
#ifdef DEBUG
static TLS adminTls;
#endif
static struct { TLS tls; } peers[8];
#ifdef DEBUG
static int adminListen=-1;
#endif
static void *peerListen=nullptr;
#ifdef DEBUG
static int listener(int port,bool){ assert(port==7444); ++adminStarts; return 1; }
#endif
struct NetconnStream { static void *listen(int,bool){ ++peerStarts; return &peerStarts; } };
static struct {
    bool begin(){ ++uartStarts; return true; }
    void stop(){}
} radio;
static void networkTask(void *){}
static void backhaulRoleChanged(){ if(systemCfg.zbRole!=COORDINATOR) cfg.mode=0; }
#define pdPASS 1
static int xTaskCreatePinnedToCore(void (*entry)(void *),const char *,unsigned,void *,unsigned,void *,int){
    assert(entry==networkTask && uartStarts==1); ++taskStarts; return pdPASS;
}
#include "../../.backhaul-tests/begin-under-test.inc"
static void fresh(int mode,int workMode=WORK_MODE_NETWORK){
    cfg.mode=mode; cfg.legacyAdminPort=8999; systemCfg.workMode=workMode; bootSelectionMade=uartSelected=running=false;
    epochWrites=uartStarts=taskStarts=adminStarts=peerStarts=0;
}
int main(){
    for(int mode=0;mode<=2;mode++){
        fresh(mode); assert(backhaulConfigured()==(mode!=0)); backhaulBegin(); backhaulBegin();
        assert(bootSelectionMade && backhaulEnabled()==(mode!=0));
        assert(running==(mode!=0) && taskStarts==unsigned(mode!=0) && uartStarts==taskStarts);
        assert(epochWrites==taskStarts && adminStarts==(czc::debugBuild ? taskStarts : 0) && peerStarts==unsigned(mode==1));
        if(mode) assert(bootEpoch==8);
    }
    fresh(1,1); backhaulBegin(); assert(!backhaulEnabled() && !running && !uartStarts);
    for(int role:{ROUTER,OPENTHREAD}) {
        fresh(2); systemCfg.zbRole=role; backhaulBegin();
        assert(!backhaulConfigured() && !backhaulEnabled() && !uartStarts && !peerStarts && !adminStarts);
    }
    systemCfg.zbRole=COORDINATOR;
    fresh(1); systemCfg.serialSpeed=57600; backhaulBegin(); assert(!running && !uartStarts);
    puts("PASS controller startup: setup after stock UART probes; Master/Satellite start once, disabled/USB/invalid baud leave workers stopped");
}
