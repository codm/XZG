#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

struct ZnpFrame {
    uint8_t data[260]{};
    uint16_t size=0;
    void build(uint8_t c0, uint8_t c1, const uint8_t *body, size_t len) {
        size=0; if(len>250) return;
        data[0]=0xfe; data[1]=len; data[2]=c0; data[3]=c1;
        if(len) memcpy(data+4,body,len);
        uint8_t fcs=0; for(size_t i=1;i<len+4;i++) fcs^=data[i];
        data[len+4]=fcs; size=len+5;
    }
};
class ZnpParser {
    ZnpFrame frame;
public:
    uint32_t errors=0;
    bool feed(uint8_t b, ZnpFrame &out) {
        if(!frame.size && b!=0xfe) return false;
        frame.data[frame.size++]=b;
        if(frame.size==2 && b>250) { frame.size=0; ++errors; return false; }
        if(frame.size<2 || frame.size!=frame.data[1]+5) return false;
        uint8_t sum=0; for(unsigned i=1;i<frame.size;i++) sum^=frame.data[i];
        if(!sum) out=frame; else ++errors;
        frame.size=0; return !sum;
    }
    void reset() { frame.size=0; }
};
class Radio {
    SemaphoreHandle_t mutex=nullptr;
    QueueHandle_t replies=nullptr;
    volatile bool waiting=false, suspendRequested=false, suspended=false;
    TaskHandle_t receiver=nullptr;
    volatile uint8_t expect0=0,expect1=0;
    uint32_t resetStarted=0;
    void fail(const char *reason,uint8_t c0=0,uint8_t c1=0) {
        if(fatal) return;
        error=reason; errorCmd0=c0; errorCmd1=c1; fatal=true;
    }
    static void task(void *arg) { static_cast<Radio *>(arg)->run(); }
    void run() {
        ZnpParser parser; ZnpFrame frame;
        for(;;) {
            if(suspendRequested) { suspended=true; vTaskDelay(1); continue; }
            if(suspended) { parser.reset(); parser.errors=0; suspended=false; }
            while(Serial2.available()) if(parser.feed(Serial2.read(),frame)) {
                if(frame.data[2]==0x41 && frame.data[3]==0x80 && frame.data[1]>=6) {
                    parser.errors=0;
                    if(waiting) fail("reset_during_sreq",expect0&0x3f,expect1);
                    if(!resetWaiting) ++resetGeneration;
                    // Queue the real indication before releasing the reset barrier.
                    if(adminActive && xQueueSend(events,&frame,0)!=pdTRUE) { adminOverflow=true; ++overflows; }
                    resetWaiting=false;
                } else if(resetWaiting) {
                    // Boot/BSL residue is not a reply to a new UART transaction.
                } else if(waiting && frame.data[2]==expect0 && frame.data[3]==expect1) {
                    if(xQueueSend(replies,&frame,0)!=pdTRUE) fail("reply_overflow",frame.data[2],frame.data[3]);
                } else if((frame.data[2]&0xe0)==0x60) {
                    // An unsolicited SRSP cannot safely be assigned to a later RPC.
                    fail("unexpected_srsp",frame.data[2],frame.data[3]);
                } else if(adminActive && xQueueSend(events,&frame,0)!=pdTRUE) {
                    adminOverflow=true; ++overflows;
                }
            }
            if(parser.errors) { if(!resetWaiting) fail("frame_parse"); parser.errors=0; }
            vTaskDelay(1);
        }
    }
public:
    QueueHandle_t events=nullptr;
    volatile bool fatal=false,adminActive=false,adminOverflow=false;
    volatile bool resetWaiting=false;
    volatile uint32_t resetGeneration=0;
    const char *volatile error="none";
    volatile uint8_t errorCmd0=0,errorCmd1=0;
    uint32_t timeouts=0,overflows=0;
    // BDB initialization can apply a staged profile and restore NV before its
    // SRSP. Its deadline depends on the command, including on a Satellite
    // without a controller. Frame/lease RPCs retain their short deadline.
    static uint32_t requestTimeout(uint8_t c0,uint8_t cmd,uint32_t normal=350) {
        return (c0==0x2f && cmd==5) || (c0==0x25 && cmd==0x40) ? 40000 : normal;
    }
    static uint32_t controllerTimeout(uint8_t c0,uint8_t cmd) {
        return requestTimeout(c0,cmd,6000);
    }
    void expectReset() {
        resetStarted=millis(); ++resetGeneration; resetWaiting=true;
    }
    void tick(uint32_t now=millis()) {
        if(resetWaiting && uint32_t(now-resetStarted)>=30000) fail("reset_timeout",0x41,0);
    }
    bool begin() {
        mutex=xSemaphoreCreateMutex(); replies=xQueueCreate(1,sizeof(ZnpFrame));
        events=xQueueCreate(24,sizeof(ZnpFrame));
        if(!mutex || !replies || !events) { fatal=true; return false; }
        // Serial2 is already initialized with the hardware table by stock CZC.
        if(xTaskCreatePinnedToCore(task,"znp-rx",4096,this,3,&receiver,1)!=pdPASS) { fatal=true; return false; }
        return true;
    }
    void stop() {
        if(receiver) { vTaskDelete(receiver); receiver=nullptr; }
        if(replies) { vQueueDelete(replies); replies=nullptr; }
        if(events) { vQueueDelete(events); events=nullptr; }
        if(mutex) { vSemaphoreDelete(mutex); mutex=nullptr; }
        fatal=true;
    }
    bool suspend() {
        if(xSemaphoreTake(mutex,pdMS_TO_TICKS(1000))!=pdTRUE) return false;
        suspendRequested=true;
        uint32_t start=millis();
        while(!suspended && millis()-start<1000) vTaskDelay(1);
        if(!suspended) { suspendRequested=false; xSemaphoreGive(mutex); return false; }
        return true; // The network task retains the RPC mutex until resume().
    }
    void resume() {
        // A maintenance operation may have reset or re-flashed the radio.
        // Recovery may already have a resetInd buffered while CCTools held UART.
        if(!resetWaiting) while(Serial2.available()) Serial2.read();
        xQueueReset(replies); xQueueReset(events); fatal=false; waiting=false; adminOverflow=false;
        error="none"; errorCmd0=errorCmd1=0;
        suspendRequested=false;
        while(suspended) vTaskDelay(1);
        xSemaphoreGive(mutex);
    }
    bool rpc(uint8_t c0, uint8_t c1, const uint8_t *in, size_t len, ZnpFrame &out,uint32_t timeout=0) {
        out.size=0;
        if(!timeout) timeout=requestTimeout(c0,c1);
        if(fatal || resetWaiting || suspendRequested || len>250 || xSemaphoreTake(mutex,pdMS_TO_TICKS(500))!=pdTRUE) return false;
        if(fatal || resetWaiting || suspendRequested) { xSemaphoreGive(mutex); return false; }
        ZnpFrame frame; frame.build(c0,c1,in,len);
        expect0=(c0&31)|0x60; expect1=c1; waiting=(c0&0xe0)==0x20;
        if(c0==0x41 && c1==0 && len==1) expectReset();
        Serial2.write(frame.data,frame.size);
        bool ok=true;
        if(waiting) ok=xQueueReceive(replies,&out,pdMS_TO_TICKS(timeout))==pdTRUE;
        waiting=false;
        if(!ok) { ++timeouts; fail("srsp_timeout",c0,c1); }
        xSemaphoreGive(mutex); return ok && !fatal;
    }
    bool command(uint8_t cmd, const uint8_t *in, size_t len, ZnpFrame &out) {
        return rpc(0x21,cmd,in,len,out) && out.size>=6 && out.data[4]==0;
    }
};
