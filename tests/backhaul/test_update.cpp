#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <ArduinoJson.h>
struct String: std::string {
    using std::string::string;
    String()=default;
    String(const std::string &s):std::string(s) {}
    explicit String(float n):std::string(std::to_string(n)) {}
    long toInt() const { return strtol(c_str(),nullptr,10); }
    size_t write(uint8_t c) { push_back(c); return 1; }
    size_t write(const uint8_t *p,size_t n) { append(reinterpret_cast<const char *>(p),n); return n; }
};
static constexpr int HTTP_CODE_OK=200,HTTPC_STRICT_FOLLOW_REDIRECTS=1;
static constexpr size_t UPDATE_SIZE_UNKNOWN=size_t(-1),slotSize=1310720;
static const char *tagESP_FW_prgs="esp.fp",*contTypeJson="application/json";
struct Fixture {
    int status=200,size=4096;
    size_t cursor=0,expected=0;
    std::vector<uint8_t> body=std::vector<uint8_t>(4096,0x55),written;
    std::vector<String> sequence;
    bool connected=false,beginOK=true,writeOK=true,endOK=true,aborted=false,ended=false,httpEnded=false,maintenanceOK=true,authorized=true;
    unsigned starts=0,restarts=0,locks=0,time=100;
};
static Fixture f;
static bool backhaulMaintenanceBegin() { if(!f.maintenanceOK) return false; ++f.locks; return true; }
static void backhaulMaintenanceEnd() { assert(f.locks); --f.locks; }
struct BackhaulMaintenance {
    bool held=backhaulMaintenanceBegin();
    ~BackhaulMaintenance() { if(held) backhaulMaintenanceEnd(); }
    explicit operator bool() const { return held; }
};
static void checkDNS() {}
static void printLogMsg(const char *) {}
static void progressFunc(unsigned,unsigned);
static uint32_t millis() { return f.time; }
static void delay(unsigned n) { f.time+=n; }
static void sendEventSafe(const char *tag,const String &value) { f.sequence.push_back(String(std::string(tag)+":"+value)); }
struct WiFiClient {
    size_t available() { return f.body.size()-f.cursor; }
    int readBytes(uint8_t *p,size_t n) { n=std::min(n,available()); memcpy(p,f.body.data()+f.cursor,n); f.cursor+=n; return n; }
};
struct WiFiClientSecure:WiFiClient { void setInsecure() {} };
struct HTTPClient {
    WiFiClient stream;
    void setFollowRedirects(int) {}
    void begin(WiFiClientSecure &,const String &) {}
    void addHeader(const char *,const char *) {}
    int GET() { return f.status; }
    int getSize() { return f.size; }
    void end() { f.httpEnded=true; }
    WiFiClient *getStreamPtr() { return &stream; }
    bool connected() { return f.connected; }
};
struct UpdateFixture {
    bool begin(size_t size) { ++f.starts; f.expected=size==UPDATE_SIZE_UNKNOWN ? slotSize : size; return f.beginOK && f.expected<=slotSize; }
    void onProgress(void (*)(unsigned,unsigned)) {}
    size_t write(uint8_t *p,size_t n) { if(!f.writeOK || f.written.size()+n>f.expected) return 0; f.written.insert(f.written.end(),p,p+n); return n; }
    bool end(bool unknown=false) {
        f.ended=true; progressFunc(f.written.size(),f.expected);
        return f.endOK && (unknown || f.written.size()==f.expected);
    }
    void abort() { f.aborted=true; }
} Update;
struct EspFixture { void restart() { ++f.restarts; f.sequence.push_back("restart"); } } ESP;
enum UploadStatus { UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED };
struct HTTPUpload { UploadStatus status=UPLOAD_FILE_START; size_t currentSize=0; uint8_t *buf=nullptr; };
struct WebServer {
    HTTPUpload part;
    String sizeArg="4096",response;
    int responseStatus=0;
    bool hasSize=true;
    HTTPUpload &upload() { return part; }
    bool hasArg(const char *) { return hasSize; }
    String arg(const char *) { return sizeArg; }
    void sendHeader(const char *,const char *) {}
    void send(int code,const char * =nullptr,const String &body="") { responseStatus=code; response=body; f.sequence.push_back("http_response"); }
} serverWeb;
static bool is_authenticated() { return f.authorized; }
// Exact production lifecycle, local handlers, progress callback, and URL updater.
#include "../../.backhaul-tests/update-under-test.inc"

static bool completed() { return std::find(f.sequence.begin(),f.sequence.end(),"esp.fp:100")!=f.sequence.end(); }
static void reset() {
    f=Fixture(); serverWeb=WebServer(); espUpdateRestartAt=0; espUpdateState="idle"; espUpdateError="";
    espUploadHeld=espUploadSeen=espUploadReady=espUploadStarted=false; espUploadExpected=espUploadReceived=0;
}
static void request(UploadStatus status) { serverWeb.part.status=status; handleEspUpdateUpload(); }
static void write() { serverWeb.part.buf=f.body.data(); serverWeb.part.currentSize=f.body.size(); request(UPLOAD_FILE_WRITE); }
static void local() { request(UPLOAD_FILE_START); write(); request(UPLOAD_FILE_END); }
static void finish(const char *result,int code) {
    handleUpdateRequest(); assert(serverWeb.responseStatus==code);
    StaticJsonDocument<192> doc; assert(!deserializeJson(doc,serverWeb.response.c_str()));
    assert(doc["result"].as<std::string>()==result);
    assert(!f.restarts);
    if(code!=200) { assert(!completed() && !f.locks); f.time+=20000; espUpdateLoop(); assert(!f.restarts); }
}
static void success() {
    assert(completed() && !f.restarts);
    f.time=espUpdateRestartAt-1; espUpdateLoop(); assert(!f.restarts);
    ++f.time; espUpdateLoop(); assert(f.restarts==1);
    espUpdateLoop(); assert(f.restarts==1);
}
static void url() { getEspUpdate("https://firmware.invalid/app.bin"); assert(!f.locks && f.httpEnded); }
int main() {
    url(); assert(f.ended && f.written==f.body && !f.aborted); success();
    reset(); f.status=404; url(); assert(!f.starts && !completed());
    reset(); f.size=-1; url(); assert(!f.starts && !completed());
    reset(); f.beginOK=false; url(); assert(!f.ended && !completed());
    reset(); f.body.resize(2048); url(); assert(f.aborted && !f.ended && !completed());
    reset(); f.body.clear(); f.connected=true; url(); assert(f.time<=10102 && f.aborted && !completed());
    reset(); f.writeOK=false; url(); assert(f.aborted && !f.ended && !completed());
    reset(); f.endOK=false; url(); assert(f.aborted && f.ended && !completed());
    reset(); f.maintenanceOK=false; assert(!getEspUpdate("url") && !f.starts && !f.locks);
    puts("PASS ESP URL updater: complete image, HTTP/size/slot rejection, truncated/stalled download, flash-write/validation failure, delayed restart and UART guard release");
    reset(); local(); assert(!f.ended && !completed() && !f.restarts);
    finish("esp_updated",200); assert(f.expected==f.body.size());
    assert(std::find(f.sequence.begin(),f.sequence.end(),"http_response")<std::find(f.sequence.begin(),f.sequence.end(),"esp.fp:100"));
    success();
    // Without a file size, slot-based progress reaches 96.25% before completion.
    reset(); serverWeb.hasSize=false; f.body.resize(1261616); local(); finish("esp_updated",200);
    assert(f.expected==slotSize && f.sequence.front().find("esp.fp:96.25")==0); success();
    reset(); f.endOK=false; local(); finish("ota_incomplete_or_invalid",400);
    reset(); f.body.resize(2048); local(); finish("ota_incomplete_or_invalid",400); assert(!f.ended);
    reset(); f.body.clear(); local(); finish("ota_incomplete_or_invalid",400); assert(!f.ended);
    reset(); f.writeOK=false; local(); finish("ota_write_failed",400);
    reset(); f.beginOK=false; local(); finish("ota_begin_failed",400);
    reset(); serverWeb.sizeArg="1400000"; local(); finish("ota_begin_failed",400);
    for(const char *s:{"0","-1","+4096","4096x","999999999999"}) { reset(); serverWeb.sizeArg=s; local(); finish("invalid_size",400); assert(!f.starts); }
    reset(); f.maintenanceOK=false; local(); finish("radio_busy",400);
    reset(); request(UPLOAD_FILE_START); write(); request(UPLOAD_FILE_ABORTED); finish("upload_aborted",400);
    reset(); local(); request(UPLOAD_FILE_START); write(); request(UPLOAD_FILE_END); finish("unexpected_file",400); assert(!f.ended);
    reset(); finish("missing_file",400);
    reset(); f.authorized=false; local(); handleUpdateRequest(); assert(serverWeb.responseStatus==401 && !f.starts);
    // A failed transfer can be retried on the same running device.
    reset(); f.writeOK=false; local(); finish("ota_write_failed",400);
    f.writeOK=true; f.aborted=false; local(); finish("esp_updated",200); success();
    puts("PASS ESP local updater: 96.25% regression, exact size, HTTP-before-restart, validation before 100%, truncation/abort/extra-file rejection, retry, authentication");
}
