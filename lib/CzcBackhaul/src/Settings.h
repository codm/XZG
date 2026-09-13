#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

namespace czc {
#ifdef DEBUG
constexpr bool debugBuild=true;
#else
constexpr bool debugBuild=false;
#endif
constexpr uint16_t debugAdminPort=7444;
// Stored as a single versioned NVS blob.
struct Settings {
    uint32_t magic=0x42484332;
    uint8_t mode=0; // 0: ordinary CZC, 1: Master, 2: Satellite
    uint8_t legacyIpv6=1, legacyRawTcp=1, hasKey=0; // Reserved NVS bytes; both transports are always available.
    uint8_t key[32]{};
    uint8_t peerIEEE[8]{}; // Reserved NVS bytes; peers exchange radio identity inside TLS.
    uint16_t peerPort=7443, legacyAdminPort=7444;
    char host[128]{};
    uint8_t legacyDiagnostics=0; // Ignored; debug capabilities are selected at build time.
    uint8_t reserved[3]{};
};
static_assert(offsetof(Settings,legacyDiagnostics)==180, "Settings NVS record layout");
inline bool privateRadioCommand(uint8_t cmd) { return (cmd>=0xc1 && cmd<=0xc5) || (cmd>=0xc7 && cmd<=0xcb) || cmd>=0xd0; }
inline bool decodeHex(const char *s,uint8_t *out,size_t n,bool reverse=false) {
    if(!s) return false;
    if(s[0]=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
    if(strlen(s)!=2*n) return false;
    uint8_t temp[32]; if(n>sizeof(temp)) return false;
    for(size_t i=0;i<n;i++) {
        unsigned value=0;
        for(unsigned j=0;j<2;j++) {
            unsigned char c=s[2*i+j];
            if(!isxdigit(c)) return false;
            value=(value<<4)|(isdigit(c) ? c-'0' : tolower(c)-'a'+10);
        }
        temp[reverse ? n-1-i : i]=value;
    }
    memcpy(out,temp,n); memset(temp,0,sizeof(temp)); return true;
}
inline bool validIEEE(const uint8_t *p) {
    bool zero=true,ones=true;
    for(unsigned i=0;i<8;i++) { zero&=p[i]==0; ones&=p[i]==255; }
    return !zero && !ones;
}
inline const char *validate(const Settings &s,uint16_t controllerPort) {
    if(s.magic!=0x42484332 || s.mode>2 || s.hasKey>1) return "invalid_mode";
    if(!memchr(s.host,0,sizeof(s.host))) return "invalid_host";
    if(s.peerPort<1024 || s.peerPort==controllerPort ||
       (debugBuild && (s.peerPort==debugAdminPort || controllerPort==debugAdminPort))) return "port_conflict";
    for(const char *p=s.host;*p;p++) if(!isalnum(static_cast<unsigned char>(*p)) && !strchr(".-:%_",*p)) return "invalid_host";
    if(s.mode && !s.hasKey) return "key_required";
    if(s.mode==2 && !s.host[0]) return "master_address_required";
    return nullptr;
}
}
