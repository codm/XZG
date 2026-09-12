// Production Satellite boot branch + info + UART task. Only the physical
// radio is simulated; delayed SRSP must be consumed by the original request.
#include <Znp.h>
#include <Settings.h>
#include <StatusSnapshot.h>
#include <cassert>
#include <cstdio>

SerialFixture Serial2;
thread_local FixtureTask *fixtureCurrentTask=nullptr;
static Radio radio;
static czc::Settings cfg;
static czc::RadioVersion radioVersion;
static bool paused=false,satelliteInitialized=false;
static uint8_t localInfo[32]{},ownIEEE[8]{},bootstrapState=0;
static uint32_t lastInfo=0,satelliteReadyAt=0;
static uint32_t joinStarted=0;
static const char *joinResult="restoring";
static const char *fault="unconfigured";
static unsigned disconnects=0,initializations=0;
static unsigned published=0;
static void publishStatus(bool force) { assert(force && !strcmp(fault,"radio_initializing")); ++published; }
static void disconnectPeers(const char *why) { fault=why; ++disconnects; }
static void migrateSatellite() { assert(false && "migration is covered by test_migration.cpp"); }
#include "../../.backhaul-tests/satellite-startup-under-test.inc"

static void feed(uint8_t c0,uint8_t cmd,const uint8_t *body,size_t size) {
    ZnpFrame f; f.build(c0,cmd,body,size); Serial2.feed(f.data,f.size);
}
int main() {
    assert(radio.begin()); cfg.mode=2;
    std::thread response;
    uint8_t status=0;
    bool drop=false;
    Serial2.onWrite=[&](const uint8_t *p,size_t n) {
        assert(n>=5);
        if(p[2]==0x21 && p[3]==0xc0) {
            uint8_t body[32]={0,2}; uint32_t revision=czc::RadioVersion::required;
            for(unsigned i=0;i<4;i++) body[2+i]=revision>>(8*i);
            const uint8_t ieee[8]={0x84,0x94,0xb8,0x3c,0,0x4b,0x12,0};
            memcpy(body+12,ieee,8); feed(0x61,0xc0,body,sizeof(body));
        } else if(p[2]==0x21 && p[3]==0xca) {
            const uint8_t body[2]={0,0}; feed(0x61,0xca,body,sizeof(body));
        } else {
            assert(p[2]==0x2f && p[3]==5 && p[1]==1 && p[4]==0);
            ++initializations;
            if(!drop) response=std::thread([&] { vTaskDelay(850); feed(0x6f,5,&status,1); });
        }
    };
    // A cold Satellite needs initialization before it can connect to Master.
    // Prior firmware incorrectly declared this 850 ms response lost at 350 ms.
    uint32_t start=millis(); peerWork(); response.join();
    if(!satelliteInitialized || radio.fatal) {
        fprintf(stderr,"FAIL Satellite startup: delayed BDB SRSP rejected; uart=%s cmd=%02X/%02X\n",
                radio.error,radio.errorCmd0,radio.errorCmd1);
        radio.stop(); return 1;
    }
    assert(millis()-start>=850 && !radio.timeouts && !disconnects && initializations==1);
    assert(published==1 && !strcmp(fault,"awaiting_master"));
    assert(uint32_t(joinStarted-start)>=850 && millis()-joinStarted<100);
    assert(!lastInfo && static_cast<int32_t>(satelliteReadyAt-millis())>0);
    peerWork(); vTaskDelay(1250); peerWork(); assert(initializations==1 && !radio.fatal);
    // Reset barrier, Master role, and maintenance pause never initialize early.
    satelliteInitialized=false; radio.expectReset(); unsigned writes=Serial2.writes;
    peerWork(); assert(Serial2.writes==writes && !satelliteInitialized);
    const uint8_t reset[6]={1,2,1,2,7,1}; feed(0x41,0x80,reset,sizeof(reset));
    vTaskDelay(20); assert(!radio.resetWaiting && !radio.fatal);
    cfg.mode=1; peerWork(); assert(Serial2.writes==writes); cfg.mode=2;
    paused=true; peerWork(); assert(Serial2.writes==writes); paused=false;
    // An explicit radio error is not a UART desync and cannot mark it ready.
    lastInfo=0; status=1; peerWork(); response.join();
    assert(!satelliteInitialized && !radio.fatal && !strcmp(fault,"bootstrap_radio"));
    // A later successful initialization reuses the same UART correctly.
    lastInfo=0; status=0; peerWork(); response.join();
    assert(satelliteInitialized && !radio.fatal && !radio.timeouts && initializations==3);
    // Silence must still expire at the real production deadline, never wait forever.
    satelliteInitialized=false; lastInfo=0; drop=true; start=millis(); peerWork();
    uint32_t elapsed=millis()-start;
    assert(elapsed>=40000 && elapsed<43000 && !satelliteInitialized && radio.fatal);
    assert(!strcmp(radio.error,"srsp_timeout") && radio.errorCmd0==0x2f && radio.errorCmd1==5);
    writes=Serial2.writes; peerWork(); assert(Serial2.writes==writes);
    assert(Radio::requestTimeout(0x21,0xc5)==350 && Radio::controllerTimeout(0x21,9)==6000);
    radio.stop();
    puts("PASS Satellite startup: production boot branch + UART, delayed BDB response, reset barrier, status failure, single initialization and real bounded 40 s silence");
}
