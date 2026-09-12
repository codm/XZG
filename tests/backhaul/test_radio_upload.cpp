#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <ArduinoJson.h>
#include <RadioImage.h>

struct String: std::string {
    using std::string::string;
    String() = default;
    String(const std::string &s): std::string(s) {}
    explicit String(uint32_t n): std::string(std::to_string(n)) {}
    bool endsWith(const char *s) const { size_t n=strlen(s); return size()>=n && compare(size()-n,n,s)==0; }
    size_t write(uint8_t c) { push_back(c); return 1; }
    size_t write(const uint8_t *p,size_t n) { append(reinterpret_cast<const char *>(p),n); return n; }
};
struct CCTools;
#include "../../src/zb.h"
struct Fixture {
    bool shortWrite=false,truncateOnClose=false,maintenanceOK=true,eraseOK=true,beginOK=true,verifyOK=true,versionOK=true;
    unsigned locks=0,erases=0,begins=0,restarts=0,blocks=0,failBlock=0,clients=1;
    std::vector<uint8_t> written;
    std::vector<String> logs;
} f;
static std::string diskPath;
// Like Arduino VFS: fwrite is buffered and size() obtains stat(path), not ftell.
struct File {
    struct Stream {
        FILE *fp;
        bool writing;
        char buffer[4096];
        Stream(FILE *p,bool w): fp(p),writing(w) { if(p) setvbuf(p,buffer,_IOFBF,sizeof(buffer)); }
        void close() {
            if(!fp) return;
            if(writing && f.truncateOnClose) { assert(!fflush(fp)); assert(!ftruncate(fileno(fp),4096)); }
            assert(!fclose(fp)); fp=nullptr;
        }
        ~Stream() { close(); }
    };
    std::shared_ptr<Stream> stream;
    File() = default;
    File(FILE *p,bool w) { if(p) stream=std::make_shared<Stream>(p,w); }
    explicit operator bool() const { return stream && stream->fp; }
    void close() { if(stream) stream->close(); }
    size_t size() const { struct stat st; return *this && !stat(diskPath.c_str(),&st) ? st.st_size : 0; }
    size_t write(const uint8_t *p,size_t n) { return fwrite(p,1,f.shortWrite ? n-1 : n,stream->fp); }
    size_t read(uint8_t *p,size_t n) { return fread(p,1,n,stream->fp); }
    int readBytes(char *p,size_t n) { return fread(p,1,n,stream->fp); }
    bool seek(size_t n) { return !fseek(stream->fp,n,SEEK_SET); }
    size_t available() const { return size()-ftell(stream->fp); }
};
struct Filesystem {
    void mkdir(const char *) {}
    void remove(const char *) { unlink(diskPath.c_str()); }
    File open(const char *,const char *mode) { return File(fopen(diskPath.c_str(),mode),mode[0]=='w'); }
} LittleFS;
struct CCTools {
    struct { uint32_t flashSize=720896; } chip;
    static constexpr size_t TRANSFER_SIZE=248;
    bool eraseFlash() { ++f.erases; assert(f.locks && !f.clients); return f.eraseOK; }
    bool beginFlash(int,int) { ++f.begins; return f.beginOK; }
    bool processFlash(uint8_t *p,int n) {
        if(++f.blocks==f.failBlock) return false;
        f.written.insert(f.written.end(),p,p+n); return true;
    }
    bool verifyFlash(uint32_t start,uint32_t size,uint32_t expected) {
        assert(!start && size==f.written.size());
        assert((czc::radioCrcUpdate(0xffffffffu,f.written.data(),f.written.size())^0xffffffffu)==expected);
        return f.verifyOK;
    }
    const char *lastBslError(){return f.verifyOK ? "bsl_ack_timeout" : "bsl_crc_mismatch";}
    uint8_t lastBslStatus(){return 0x40;}
    void restart() { ++f.restarts; }
} CCTool;
struct SysVarsStruct { bool zbFlashing=false; unsigned connectedClients=1; } vars;
struct BackhaulMaintenance {
    BackhaulMaintenance() { ++f.locks; }
    ~BackhaulMaintenance() { --f.locks; }
    explicit operator bool() const { return f.maintenanceOK; }
};
using byte=uint8_t;
static constexpr int BEGIN_ZB_ADDR=0,HTTP_POST=2;
static constexpr const char *tagZB_FW_err="error",*tagZB_FW_info="info",*tagZB_FW_prgs="progress";
static void printLogMsg(const String &s) { f.logs.push_back(s); }
static void sendEventSafe(const char *,const String &) {}
static void backhaulBslHold(bool) {}
enum { UNDEFINED,COORDINATOR,ROUTER,OPENTHREAD,LED_BLINK_1Hz,LED_ON };
struct SystemConfig { int zbRole=UNDEFINED; } systemCfg;
struct { struct { int mode=0; } powerLED; } ledControl;
static unsigned roleUpdates=0,roleNotifications=0,versionChecks=0;
static void saveSystemConfig(const SystemConfig &) { ++roleUpdates; }
static void backhaulRoleChanged() { ++roleNotifications; }
bool zbFwCheck() { ++versionChecks; return f.versionOK; }
static void socketClientsDisconnect() { f.clients=0; }
static void delay(unsigned) {}
float sendPercentageToFrontend(float p,float,const char *) { return p; }
#define DEBUG_PRINTLN(x) ((void)0)
enum UploadStatus { UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED };
struct HTTPUpload {
    UploadStatus status=UPLOAD_FILE_START;
    String filename="radio.bin";
    size_t currentSize=0,totalSize=0;
    uint8_t *buf=nullptr;
};
struct WebServer {
    HTTPUpload part;
    int responseStatus=0;
    unsigned responseCount=0;
    String response;
    String role="coordinator";
    String arg(const char *name) { assert(!strcmp(name,"fwMode")); return role; }
    HTTPUpload &upload() { return part; }
    void sendHeader(const char *,const char *) {}
    void send(int status,const char *,const String &s) {
        // A browser consumes the first HTTP response, not a later replacement.
        if(!responseCount++) { responseStatus=status; response=s; }
    }
    void on(const char *,int,void (*)(),void (*)()) {}
} server;
static WebServer &serverWeb=server;
static constexpr int HTTP_CODE_OK=200,HTTP_CODE_INTERNAL_SERVER_ERROR=500;
static const char *contTypeText="text/plain";
static bool backhaulAuthorizeWebMutation() { return true; }

// These are extracted verbatim from the firmware, excluding includes only.
#include "../../.backhaul-tests/radio-role-under-test.inc"
#include "../../.backhaul-tests/radio-flash-under-test.inc"
#include "../../.backhaul-tests/radio-upload-under-test.inc"

static std::vector<uint8_t> image(size_t n=4100) {
    std::vector<uint8_t> b(n,0x55);
    const uint8_t header[]={0,0,2,0x20,1,1,0,0};
    std::copy(header,header+8,b.begin()); return b;
}
static void start() {
    server.part=HTTPUpload(); upload();
}
static void write(std::vector<uint8_t> &b) {
    server.part.status=UPLOAD_FILE_WRITE;
    for(size_t p=0;p<b.size();p+=server.part.currentSize) {
        server.part.buf=b.data()+p;
        server.part.currentSize=std::min(size_t(1436),b.size()-p);
        upload();
    }
}
static void end(size_t n) {
    server.part.status=UPLOAD_FILE_END; server.part.totalSize=n; upload();
}
static StaticJsonDocument<1024> result(const char *code,int status) {
    finish();
    assert(server.responseCount==1 && "Radio update must send exactly one HTTP response");
    assert(server.responseStatus==status && !vars.zbFlashing && !f.locks);
    const bool validRole=server.role=="coordinator" || server.role=="router" || server.role=="thread";
    assert(roleUpdates==unsigned(status==200 && validRole));
    assert(roleNotifications==roleUpdates);
    if(roleUpdates) assert(systemCfg.zbRole==(server.role=="coordinator" ? COORDINATOR : server.role=="router" ? ROUTER : OPENTHREAD));
    assert(versionChecks==unsigned(roleUpdates && server.role=="coordinator"));
    roleUpdates=roleNotifications=versionChecks=0;
    StaticJsonDocument<1024> d;
    assert(!deserializeJson(d,server.response.c_str()));
    assert(d["result"].as<std::string>()==code);
    if(status!=200) {
        assert(!f.logs.empty());
        assert(f.logs.back().find(code)!=std::string::npos);
        assert(f.logs.back().find("erase_started")!=std::string::npos);
    }
    assert(access(diskPath.c_str(),F_OK)!=0); return d;
}
static void reset() { f=Fixture(); CCTool=CCTools(); server=WebServer(); systemCfg=SystemConfig(); roleUpdates=roleNotifications=versionChecks=0; }
int main() {
    char name[]=".backhaul-tests/radio-upload-XXXXXX";
    int fd=mkstemp(name); assert(fd>=0); close(fd); diskPath=name;
    registerRadioUpload(server);
    // The explicit version-check API still owns its own HTTP response.
    for(bool ok : {false,true}) {
        reset(); f.versionOK=ok; String reply="ok"; apiCmdZbCheckFirmware(reply);
        assert(server.responseCount==1 && server.responseStatus==(ok ? 200 : 500));
        assert(server.response=="ok" && versionChecks==1);
    }
    reset();
    auto b=image();
    start(); write(b);
    // Buffered data is fully accepted while stat still lags behind until fclose.
    assert(uploadedBytes==b.size() && imageFile.size()!=b.size());
    end(b.size()); assert(received && storedBytes==b.size());
    auto d=result("radio_updated",200);
    assert(d.size()==1 && f.erases==1 && f.restarts==1 && f.written==b);

    reset(); b=image(720896); start(); write(b); end(b.size()); result("radio_updated",200);
    assert(f.written==b && f.erases==1);

    for(const char *role : {"router","thread","","unknown"}) {
        reset(); server.role=role; b=image(); start(); write(b); end(b.size()); result("radio_updated",200);
    }
    // A version-query failure after a verified write must not send a second response either.
    reset(); f.versionOK=false; b=image(); start(); write(b); end(b.size()); result("radio_updated",200);

    reset(); b=image(); start(); write(b); end(b.size()+4); d=result("incomplete_upload",400);
    assert(!f.erases && f.clients==1 && d["expected_bytes"]==4104 && d["received_bytes"]==4100 && d["stored_bytes"]==4100);
    assert(!d["erase_started"].as<bool>() && d["stage"]=="upload");

    reset(); f.truncateOnClose=true; start(); write(b); end(b.size()); d=result("incomplete_upload",400);
    assert(!f.erases && d["received_bytes"]==4100 && d["stored_bytes"]==4096);

    reset(); f.shortWrite=true; start(); write(b); end(b.size()); result("storage_write_failed",400); assert(!f.erases);
    reset(); start(); write(b); server.part.status=UPLOAD_FILE_ABORTED; upload(); result("upload_aborted",400); assert(!f.erases);
    reset(); CCTool.chip.flashSize=4096; start(); write(b); end(b.size()); result("radio_image_too_large",400); assert(!f.erases);
    reset(); result("missing_file",400); assert(!f.erases);

    reset(); b[0]=0xff; start(); write(b); end(b.size()); d=result("radio_image_invalid",400);
    assert(!f.erases && !d["erase_started"].as<bool>() && d["stage"]=="validate");
    reset(); b=image(); start(); write(b); end(b.size()); f.maintenanceOK=false; result("radio_busy",400); assert(!f.erases);
    reset(); f.eraseOK=false; start(); write(b); end(b.size()); d=result("radio_erase_failed",400);
    assert(d["stage"]=="erase" && d["erase_started"]==true && f.erases==1 && !f.begins && f.restarts==1);
    reset(); f.beginOK=false; start(); write(b); end(b.size()); d=result("radio_begin_failed",400);
    assert(d["stage"]=="begin_write" && !f.blocks && f.restarts==1);
    reset(); f.failBlock=2; start(); write(b); end(b.size()); d=result("radio_write_failed",400);
    assert(d["stage"]=="write" && d["written_bytes"]==248 && f.restarts==1);
    reset(); f.verifyOK=false; start(); write(b); end(b.size()); d=result("radio_verify_failed",400);
    assert(d["stage"]=="verify" && d["bsl_error"]=="bsl_crc_mismatch" && d["written_bytes"]==b.size() && f.restarts==1);
    // A failed request must not poison the next upload in the same web server.
    reset(); start(); write(b); end(b.size()); result("radio_updated",200); assert(f.written==b);
    puts("PASS radio uploader: actual handlers + role update + BSL writer, one final HTTP response, buffered filesize regression, full image, partial/write/abort/size/header rejection before erase, exact BSL errors, retry and UART release");
}
