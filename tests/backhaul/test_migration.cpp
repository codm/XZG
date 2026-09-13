// Real ESP NV adapter + recovery handler; only persistent radio storage,
// reset/zgInit side effects, clock, and network sockets are fixtures.
#include <SatelliteMigration.h>
#include <map>
#include <vector>
#include <stdexcept>
#include <cassert>
#include <cstdio>
using Migration=czc::SatelliteMigration;
static std::map<uint16_t,std::vector<uint8_t>> flash;
static uint8_t localInfo[32]{},masterInfo[32]{},bootstrapState=2;
static bool satelliteInitialized=true,rejoinRequested=false;
static uint32_t clockMs=1,lastInfo=0,joinStarted=0,controllerReset=0;
static unsigned mutations=0,cut=0,resets=0,closed=0;
static unsigned initializations=0,rpcs=0;
static bool afterWrite=false,storageFailure=false;
static const char *fault="already_joined",*joinResult="idle";
static uint32_t millis(){return clockMs;}
static uint16_t r16(const uint8_t *p){return p[0]|uint16_t(p[1])<<8;}
static void erase(void *p,size_t n){memset(p,0,n);}
static void disconnectPeers(const char *why){fault=why;}
static void publishStatus(bool){ }
static void rawClose(){++closed;}
static struct {int fd=-1;void close(){++closed;fd=-1;}} adminTls;
struct ZnpFrame {uint8_t data[260]{};unsigned size=0;};
struct PowerCut {};
static void mutation(bool after){if(after==afterWrite && mutations==cut)throw PowerCut{};}
static void radioBoot(){
    localInfo[11]=0;
    if(flash[Migration::Startup][0]&1){
        flash[Migration::OnNetwork][0]=0;flash[0x87][0]=0;
        flash[Migration::Startup][0]&=~1;
        // ZDSecMgrNwkKeyInit(TRUE) retains NWK material unless bit 0x80 is set.
        assert(!(flash[Migration::Startup][0]&0x80));
    }
}
static struct {
    uint32_t resetGeneration=0;
    bool fatal=false;
    bool rpc(uint8_t c0,uint8_t cmd,const uint8_t *in,unsigned len,ZnpFrame &out,unsigned timeout=0){
        ++rpcs;
        if(c0==0x2f){assert(cmd==5 && len==1 && !in[0]);++initializations;out.size=6;out.data[4]=0;return true;}
        if(c0==0x41){assert(cmd==0 && len==1 && in[0]==1);++resets;radioBoot();++resetGeneration;return true;}
        assert(c0==0x21 && timeout==6000);
        uint16_t id=r16(in);std::vector<uint8_t> reply;
        if(cmd==0x13){auto size=flash[id].size();reply={uint8_t(size),uint8_t(size>>8)};}
        else if(cmd==8){assert(len==3 && !in[2]);reply={0,uint8_t(flash[id].size())};reply.insert(reply.end(),flash[id].begin(),flash[id].end());}
        else {
            ++mutations;mutation(false);
            if(storageFailure)return false;
            if(cmd==9){
                assert(id==Migration::Boot || id==Migration::Startup);
                assert(!in[2] && len==unsigned(4+in[3]) && flash[id].size()==in[3]);
                flash[id].assign(in+4,in+len);
            }else{
                assert(cmd==0x12 && len==4 && flash[id].size()==r16(in+2));
                assert(id==Migration::Nib || id==Migration::Boot);flash.erase(id);
            }
            mutation(true);reply={0};
        }
        out.data[1]=reply.size();memcpy(out.data+4,reply.data(),reply.size());out.size=reply.size()+5;return true;
    }
} radio;
static bool info(){bootstrapState=flash[Migration::Boot].empty()?0:flash[Migration::Boot][4];return true;}
#include "../../.backhaul-tests/migration-under-test.inc"
static bool masterInitialized=false;
static struct {int mode=1;} cfg;
static bool controllerConnected=false,controllerIncoming=false;
static struct {bool connected(){return controllerConnected;}} rawClient;
static struct {bool hasClient(){return controllerIncoming;}} server;
#include "../../.backhaul-tests/master-startup-under-test.inc"
static void setup(){
    flash.clear();memset(localInfo,0,32);memset(masterInfo,0,32);
    // User's old Satellite PAN/channel and newly restored Master network.
    localInfo[11]=7;localInfo[8]=0xa8;localInfo[9]=0x3e;localInfo[10]=14;localInfo[12]=2;localInfo[20]=7;
    masterInfo[11]=9;masterInfo[8]=0x0e;masterInfo[9]=0xba;masterInfo[10]=11;masterInfo[12]=1;masterInfo[20]=8;
    std::vector<uint8_t> boot(88);boot[0]=0x32;boot[1]=0x43;boot[2]=0x48;boot[3]=0x42;boot[4]=2;
    boot[5]=1;boot[6]=14;boot[7]=0xa8;boot[8]=0x3e;boot[13]=1;boot[21]=2;boot[29]=7;
    flash[Migration::Boot]=boot;flash[Migration::Nib]=std::vector<uint8_t>(110,0xa5);
    flash[Migration::Startup]={0};flash[Migration::OnNetwork]={1};flash[0x87]={1};
    // Unrelated storage, registry and counter floors must not be deleted.
    flash[0x3f1]={42};flash[0x2ff]={0xaa,0xbb};
    clockMs=1;mutations=cut=resets=closed=controllerReset=0;storageFailure=false;
    bootstrapState=2;satelliteInitialized=true;rejoinRequested=false;joinResult="idle";
    migrationResetSent=false;migrationRetryAt=0;
}
static void finish(){
    for(unsigned i=0;i<5 && !flash[Migration::Boot].empty();i++)migrateSatellite();
    assert(flash[Migration::Boot].empty() && flash[Migration::Nib].empty());
    assert(flash[Migration::OnNetwork][0]==0 && flash[Migration::Startup][0]==2);
    assert(flash[0x3f1]==std::vector<uint8_t>({42}) && flash[0x2ff]==std::vector<uint8_t>({0xaa,0xbb}));
}
int main(){
    MigrationNv nv;
    setup();assert(Migration::prepare(nv,localInfo,masterInfo)==Migration::Prepared);
    assert(flash[Migration::Nib].size()==110 && flash[Migration::OnNetwork][0]==1 && !resets);
    bootstrapState=3;finish();assert(resets==1 && closed==2 && rejoinRequested && !satelliteInitialized);
    assert(bootstrapState==0 && !strcmp(joinResult,"joining"));
    // No reset/writes for coordinator, foreign Master, unrelated/pending profile,
    // identity mismatch, corrupt marker, changed old network, or matching network.
    for(unsigned n=0;n<9;n++){
        setup();
        if(n==0)localInfo[11]=9;
        if(n==1)masterInfo[12]=3;
        if(n==2)flash[Migration::Boot][4]=1;
        if(n==3)flash[Migration::Boot][21]=3;
        if(n==4)flash[Migration::Boot][0]=0;
        if(n==5)flash[Migration::Boot].clear();
        if(n==6)localInfo[10]=20;
        if(n==7){memcpy(masterInfo+8,localInfo+8,3);memcpy(masterInfo+20,localInfo+20,8);}
        if(n==8)masterInfo[11]=0;
        assert(Migration::prepare(nv,localInfo,masterInfo)==Migration::Unmanaged && !mutations && !resets);
    }
    // Interrupt before AND after each persistent operation, then cold-boot.
    // Before intent commits the old network survives; after it commits recovery
    // finishes autonomously, including a cut after the last marker deletion.
    for(unsigned when=0;when<2;when++)for(unsigned point=1;point<=4;point++){
        setup();cut=point;afterWrite=when;
        try{assert(Migration::prepare(nv,localInfo,masterInfo)==Migration::Prepared);finish();assert(false);}
        catch(const PowerCut &){ }
        cut=0;radioBoot();migrationResetSent=false;migrationRetryAt=0;info();
        if(bootstrapState==2){assert(flash[Migration::OnNetwork][0]==1 && flash[Migration::Nib].size()==110);}
        else if(bootstrapState==3)finish();
        else assert(!bootstrapState && flash[Migration::Nib].empty());
    }
    // Storage failure is visible and retries are bounded, without radio resets.
    setup();assert(Migration::prepare(nv,localInfo,masterInfo)==Migration::Prepared);
    storageFailure=true;migrateSatellite();unsigned attempts=mutations;
    for(unsigned i=0;i<100;i++)migrateSatellite();
    assert(!resets && mutations==attempts && !strcmp(fault,"bootstrap_storage"));
    storageFailure=false;clockMs+=10001;finish();assert(resets==1);
    // Only the two radio NV IDs may ever be removed; Master registry is protected.
    assert(!nv.remove(0x3f1,1));uint8_t zero=0;assert(!nv.write(0x3f1,&zero,1));
    // Master reboot restores only existing NV, once; no forming a temporary
    // network and no interference with an active/arriving Z2M controller.
    setup();localInfo[11]=0;flash[0x87][0]=0;
    for(unsigned gate=0;gate<8;gate++){
        controllerConnected=gate==0;controllerIncoming=gate==1;adminTls.fd=gate==2 ? 1 : -1;
        cfg.mode=gate==3 ? 2 : 1;masterInitialized=gate==4;
        flash[Migration::OnNetwork][0]=gate==5 ? 0 : 1;
        flash[Migration::Startup][0]=gate==6 ? 2 : 0;
        if(gate==7)flash[Migration::Nib].clear();
        initializeMaster();assert(!initializations && !resets && !mutations);
    }
    flash[Migration::Nib]=std::vector<uint8_t>(110,0xa5);
    initializeMaster();assert(masterInitialized && initializations==1 && !mutations && !resets);
    unsigned previousRpcs=rpcs;initializeMaster();assert(rpcs==previousRpcs);
    masterInitialized=false;localInfo[11]=9;initializeMaster();assert(masterInitialized && rpcs==previousRpcs);
    puts("PASS Satellite migration: production NV adapter/recovery, same-Master guard, one reset, every persistent step interrupted before/after, idempotent recovery, retained unrelated NV and bounded storage failure");
    puts("PASS Master startup: saved coordinator network resumes once, missing NV/reset flags/Satellite/active and arriving controllers never initialize or create a network");
}
