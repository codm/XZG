// Production Satellite commissioning state machine with clock/radio boundaries.
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
static uint32_t clockMs=100,bootstrapAt=0,joinStarted=100,lastInfo=0,controllerReset=0;
static uint32_t millis(){return clockMs;}
static bool bootstrapRestart=false,rejoinRequested=true,satelliteInitialized=true;
static const char *joinResult="joining";
static uint8_t localInfo[32]{};
static unsigned disconnects=0,resets=0,rawClosed=0;
static void disconnectPeers(const char *){++disconnects;}
static void rawClose(){++rawClosed;}
static struct { void close(){} } adminTls;
struct ZnpFrame {};
static struct {bool rpc(uint8_t c0,uint8_t cmd,uint8_t *in,unsigned n,ZnpFrame &){assert(c0==0x41 && cmd==0 && n==1 && in[0]==1);++resets;return true;}} radio;
#include "../../.backhaul-tests/commission-under-test.inc"
int main(){
 // Master may spend minutes restoring a backup; keep asking automatically.
 for(clockMs=100;clockMs<180000;clockMs+=100){commissionSatellite();assert(rejoinRequested && !resets && !disconnects);}
 assert(!strcmp(joinResult,"awaiting_master"));
 // Explicit retry starts once, without periodically cancelling the pending request.
 joinResult="requested";commissionSatellite();assert(disconnects==1 && rejoinRequested && !strcmp(joinResult,"joining"));
 // A received profile triggers exactly one reset and then waits for actual router state.
 bootstrapRestart=true;bootstrapAt=clockMs+100;commissionSatellite();assert(!resets);
 clockMs+=100;commissionSatellite();assert(resets==1 && rawClosed==1 && controllerReset==1 && !rejoinRequested && !satelliteInitialized && !strcmp(joinResult,"restoring"));
 clockMs+=2000;commissionSatellite();assert(!strcmp(joinResult,"restoring") && resets==1);
 localInfo[11]=7;commissionSatellite();assert(!strcmp(joinResult,"joined"));
 // Real apply failure still has a bounded deadline; no repeated reset loop.
 localInfo[11]=0;joinResult="restoring";joinStarted=clockMs;clockMs+=30001;commissionSatellite();
 assert(!strcmp(joinResult,"join_timeout") && !rejoinRequested && resets==1);
 puts("PASS Satellite bootstrap wait: pending request survives three minutes of Master restore, single apply/reset, bounded apply timeout");
}
