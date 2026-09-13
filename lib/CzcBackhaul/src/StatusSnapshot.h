#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stddef.h>

namespace czc {
struct RadioVersion {
    static constexpr uint32_t required=20260917;
    uint32_t revision=0;
    uint8_t protocol=0;
    bool compatible=false;
    const char *inspect(bool received,const uint8_t *body,size_t length) {
        compatible=false;
        if(!received || length<6 || body[0]) return "radio_info";
        protocol=body[1]; revision=0;
        for(unsigned i=0;i<4;i++) revision|=uint32_t(body[2+i])<<(8*i);
        if(length!=32 || protocol!=2 || revision!=required) return "radio_revision";
        compatible=true; return nullptr;
    }
};

inline bool copyStatusSnapshot(JsonObject destination,const char *json) {
    // The const input forces owned strings. Both JSON keys and nested values
    // must survive the source buffer AND this temporary document.
    DynamicJsonDocument snapshot(4096);
    if(deserializeJson(snapshot,json)) return false;
    for(JsonPair pair:snapshot.as<JsonObject>()) destination[pair.key()]=pair.value();
    return true;
}

// Deserialize the cached status directly into the web response document.
// A Reader forces owned strings, without a second 4 KiB document or a String
// containing a copy of the entire JSON response. The peer task owns the cache;
// its caller supplies a stable local snapshot for this short operation.
class StatusReader {
    const char *body;
    size_t prefix=0;
    bool ended=false;
public:
    explicit StatusReader(const char *json):body(json) {}
    int read() {
        static const char begin[]="{\"status\":";
        if(prefix<sizeof(begin)-1) return begin[prefix++];
        if(*body) return static_cast<unsigned char>(*body++);
        if(!ended) { ended=true; return '}'; }
        return -1;
    }
    size_t readBytes(char *out,size_t count) {
        size_t n=0; int ch;
        while(n<count && (ch=read())>=0) out[n++]=ch;
        return n;
    }
};
inline bool loadStatusSnapshot(JsonDocument &destination,const char *json) {
    StatusReader reader(json);
    return !deserializeJson(destination,reader) && destination["status"].is<JsonObject>();
}
}
