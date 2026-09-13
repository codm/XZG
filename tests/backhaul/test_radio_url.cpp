#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

struct String: std::string {
    using std::string::string;
    long toInt() const { return std::stol(*this); }
};
struct CCTools {} radio;
struct {
    bool maintenance=true,download=true,flash=true,cleanup=true;
    unsigned removes=0,flashes=0,saves=0,errors=0;
    std::vector<String> logs;
} f;
struct BackhaulMaintenance { explicit operator bool() const { return f.maintenance; } };
enum { LED_OFF,LED_BLINK_3Hz };
struct { struct { int mode=LED_OFF; } modeLED; } ledControl;
struct { bool zbFlashing=false; } vars;
struct Config { long zigBeeFwVersion=1; } systemCfg;
static const char *tagZB_FW_err="error";
static void saveSystemConfig(const Config &) { ++f.saves; }
static void printLogMsg(const String &msg) { f.logs.push_back(msg); }
static void sendEventSafe(const char *,const String &) { ++f.errors; }
static String extractVersionFromURL(const String &) { return "42"; }
static bool removeFileFromFS(const char *) { ++f.removes; return f.cleanup; }
static const char *downloadFirmwareFromGithub(const char *) {
    assert(vars.zbFlashing); return f.download ? "/zigbee/firmware.bin" : nullptr;
}
static bool eraseWriteZbFile(const char *,CCTools &) { ++f.flashes; return f.flash; }
#define DEBUG_PRINTLN(x) ((void)0)

// The actual URL-flash orchestration, including its success return to the role API.
#include "../../.backhaul-tests/radio-url-under-test.inc"

int main() {
    for(bool flash:{false,true}) for(bool cleanup:{false,true}) {
        f={}; f.flash=flash; f.cleanup=cleanup; systemCfg.zigBeeFwVersion=1;
        assert(flashZigbeefromURL("http://firmware.test/image.bin","/zigbee/firmware.bin",radio)==flash);
        assert(f.flashes==1 && f.removes==2);
        assert(f.saves==unsigned(flash) && systemCfg.zigBeeFwVersion==(flash ? 42 : 1));
        assert(f.logs.size()==unsigned(!cleanup));
        assert(!vars.zbFlashing && ledControl.modeLED.mode==LED_OFF);
    }
    f={}; f.download=false;
    assert(!flashZigbeefromURL("http://firmware.test/image.bin","/zigbee/firmware.bin",radio));
    assert(!f.flashes && !f.saves && f.errors==1 && f.removes==1 && !vars.zbFlashing);
    f={}; f.maintenance=false;
    assert(!flashZigbeefromURL("http://firmware.test/image.bin","/zigbee/firmware.bin",radio));
    assert(!f.flashes && !f.removes && !f.saves && !vars.zbFlashing);
    puts("PASS URL radio update: flash result survives cleanup failure; failed writes never change version/role");
}
