#include "czc_backhaul.h"
#include "config.h"
#include "etc.h"
#include "web.h"
#include "backhaul_random.h"
#include "memory_status.h"
#include <Settings.h>
#include <StatusSnapshot.h>
#include <Recovery.h>
#include <PeerRetry.h>
#include <SatelliteMigration.h>
#include <SocketHelpers.h>
#ifdef DEBUG
#include <AdminTls.h>
#endif
#include <PeerTls.h>
#include <Znp.h>
#include <Backhaul.h>
#include <ETH.h>
#include <CCTools.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <lwip/sockets.h>
#include <errno.h>

extern SystemConfigStruct systemCfg;
extern NetworkConfigStruct networkCfg;
extern SysVarsStruct vars;
extern CCTools CCTool;
extern WiFiServer server;
extern void socketClientConnected(int client,IPAddress ip);
extern void socketClientDisconnected(int client);

static czc::Settings cfg;
static czc::Recovery recovery;
static uint8_t ownIEEE[8]{};
static WiFiClient rawClient;
static uint32_t controllerReset=0;
static bool running=false,bootSelectionMade=false,uartSelected=false;
static volatile bool maintenanceRequested=false,maintenanceActive=false,bslHold=false;
static SemaphoreHandle_t maintenanceMutex=nullptr;
static unsigned maintenanceDepth=0;
static portMUX_TYPE statusLock=portMUX_INITIALIZER_UNLOCKED;
static char cachedStatus[3072]="{}";
static char cachedLog[256]{};
static czc::RadioVersion radioVersion;
static char csrf[33]{};
static WebServer *web=nullptr;
static volatile bool rejoinRequested=false;
static const char *joinResult="idle";
static void rawClose();
static void publishStatus(bool force=false);
static Radio radio;
#ifdef DEBUG
static AdminTls adminTls;
static czc::MemoryBudget adminOpenMemory;
#endif
static constexpr unsigned PeerLimit=8;
#ifdef DEBUG
static bool paused=false,holdTx=false;
#else
static constexpr bool paused=false,holdTx=false;
#endif
static uint8_t localInfo[32]{};
static uint32_t bootEpoch=0,connectionEpoch=0,lastInfo=0;
static czc::PeerRetry peerRetry;
static uint32_t reconnects=0,peerMismatch=0;
static netconn *peerListen=nullptr;
#ifdef DEBUG
static int adminListen=-1;
#endif
static const char *fault="unconfigured";
static const char *peerError="none";
static volatile uint32_t restartAt=0;
static bool roleRestart=false;
static bool bootstrapRestart=false,autoJoinChecked=false,satelliteInitialized=false;
static bool masterInitialized=false;
static uint32_t satelliteReadyAt=0;
static uint32_t bootstrapAt=0,joinStarted=0;
static uint8_t bootstrapState=0;
struct Mapping { uint32_t sequence=0,ticket=0; };
struct Peer {
    PeerTls tls;
    backhaul::Endpoint *endpoint=nullptr;
    Mapping mapping[4];
    uint8_t remote[32]{},session[8]{};
    char remoteIp[64]{};
    bool known=false,infoSent=false,sessionOpen=false,bound=false,identified=false,profileNeeded=false,profileSent=false;
    uint32_t lastLease=0,lastPoll=0;
    const char *fault="offline";
};
static Peer peers[PeerLimit];
static unsigned peerCount() { unsigned n=0; for(auto &p:peers) n+=p.known; return n; }
static uint64_t nowMs() { return esp_timer_get_time()/1000; }
static uint16_t r16(const uint8_t *p) { return p[0]|uint16_t(p[1])<<8; }
static uint32_t r32(const uint8_t *p) { return r16(p)|uint32_t(r16(p+2))<<16; }
static void w32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }
static void erase(void *buffer,size_t n) { volatile uint8_t *p=static_cast<volatile uint8_t *>(buffer); while(n--) *p++=0; }
static bool info() {
    ZnpFrame response;
    bool received=radio.command(0xc0,nullptr,0,response);
    const char *error=radioVersion.inspect(received,response.data+4,response.size>=6 ? response.data[1] : 0);
    if(error) { fault=radio.fatal ? "uart_desync" : error; return false; }
    memcpy(localInfo,response.data+4,32); memcpy(ownIEEE,localInfo+12,8);
    if(!czc::validIEEE(ownIEEE)) { radioVersion.compatible=false; fault="own_ieee"; return false; }
    if(cfg.mode==2) { ZnpFrame state; if(radio.command(0xca,nullptr,0,state) && state.data[1]==2) bootstrapState=state.data[5]; }
    return true;
}
static bool bindRadio(Peer &p) {
    uint8_t in[28]; memcpy(in,p.session,8); memcpy(in+8,p.remote+6,2);
    memcpy(in+10,localInfo+8,2); memcpy(in+12,localInfo+20,8); memcpy(in+20,p.remote+12,8);
    ZnpFrame out; p.bound=radio.command(0xc1,in,sizeof(in),out);
    if(!p.bound) p.fault="radio_busy";
    return p.bound;
}
static bool leaseRadio(Peer &p,bool enable) {
    uint8_t in[9]; memcpy(in,p.session,8); in[8]=enable;
    ZnpFrame out; p.lastLease=millis();
    return p.bound && radio.command(0xc5,in,sizeof(in),out);
}
static void complete(Peer &p,uint32_t ticket,bool accepted) {
    uint8_t in[13]; memcpy(in,p.session,8); w32(in+8,ticket); in[12]=accepted ? 0 : 1;
    ZnpFrame out; radio.command(0xc4,in,sizeof(in),out);
}
static void results(Peer &p) {
    if(!p.endpoint) return;
    backhaul::Result result;
    while(p.endpoint->takeResult(result)) {
        for(auto &m:p.mapping) if(m.sequence==result.sequence) {
            complete(p,m.ticket,result.delivery==backhaul::Delivery::AcceptedIntoRadioQueue); m={}; break;
        }
    }
}
static void disconnectPeer(Peer &p,const char *why) {
    p.fault=why; fault=why;
    if(p.bound) leaseRadio(p,false);
    if(p.endpoint) { p.endpoint->close(); results(p); delete p.endpoint; p.endpoint=nullptr; }
    p.tls.close(); p.known=p.infoSent=p.sessionOpen=p.bound=p.identified=p.profileNeeded=p.profileSent=false;
}
static void disconnectPeers(const char *why) { for(auto &p:peers) if(p.tls.fd>=0 || p.endpoint) disconnectPeer(p,why); fault=why; }
static bool sameNetwork(const uint8_t *a,const uint8_t *b) {
    return !memcmp(a+8,b+8,3) && !memcmp(a+20,b+20,8);
}
static uint8_t bootstrapDiagnostic[8]{};
class MigrationNv final : public czc::SatelliteMigration::Nv {
    bool rpc(uint8_t cmd,const uint8_t *in,unsigned len,ZnpFrame &out) {
        return radio.rpc(0x21,cmd,in,len,out,6000);
    }
public:
    bool length(uint16_t id,uint16_t &size) override {
        uint8_t in[2]={uint8_t(id),uint8_t(id>>8)}; ZnpFrame out;
        if(!rpc(0x13,in,2,out) || out.data[1]!=2) return false;
        size=r16(out.data+4); return true;
    }
    bool read(uint16_t id,uint8_t *data,unsigned size) override {
        uint8_t in[3]={uint8_t(id),uint8_t(id>>8),0}; ZnpFrame out;
        bool ok=rpc(8,in,3,out) && out.data[1]==size+2 && !out.data[4] && out.data[5]==size;
        if(ok) memcpy(data,out.data+6,size);
        erase(&out,sizeof(out)); return ok;
    }
    bool write(uint16_t id,const uint8_t *data,unsigned size) override {
        if(size>czc::SatelliteMigration::RecordSize || (id!=czc::SatelliteMigration::Boot && id!=czc::SatelliteMigration::Startup)) return false;
        uint8_t in[czc::SatelliteMigration::RecordSize+4]={uint8_t(id),uint8_t(id>>8),0,uint8_t(size)};
        memcpy(in+4,data,size); ZnpFrame out;
        bool ok=rpc(9,in,size+4,out) && out.data[1]==1 && !out.data[4];
        erase(in,sizeof(in)); return ok;
    }
    bool remove(uint16_t id,uint16_t size) override {
        if(id!=czc::SatelliteMigration::Boot && id!=czc::SatelliteMigration::Nib) return false;
        uint8_t in[4]={uint8_t(id),uint8_t(id>>8),uint8_t(size),uint8_t(size>>8)}; ZnpFrame out;
        return rpc(0x12,in,4,out) && out.data[1]==1 && !out.data[4];
    }
};
static uint32_t migrationRetryAt=0,migrationGeneration=0;
static bool migrationResetSent=false;
static void migrateSatellite() {
    if(static_cast<int32_t>(millis()-migrationRetryAt)<0) return;
    if(migrationResetSent && radio.resetGeneration==migrationGeneration) return;
    if(!info()) return;
    disconnectPeers("network_migrating"); publishStatus(true);
    MigrationNv nv;
    auto result=czc::SatelliteMigration::advance(nv,localInfo);
    if(result==czc::SatelliteMigration::ResetRequired) {
        rawClose();
#ifdef DEBUG
        adminTls.close();
#endif
        ++controllerReset;
        migrationResetSent=true; migrationGeneration=radio.resetGeneration;
        ZnpFrame out; uint8_t reset=1;
        radio.rpc(0x41,0,&reset,1,out);
        satelliteInitialized=false; lastInfo=0;
    } else if(result==czc::SatelliteMigration::Cleared) {
        bootstrapState=0; migrationResetSent=false; migrationRetryAt=0;
        satelliteInitialized=false; lastInfo=0; rejoinRequested=true;
        joinResult="joining"; joinStarted=millis(); fault="awaiting_master";
    } else {
        fault=result==czc::SatelliteMigration::StorageError ? "bootstrap_storage" : "bootstrap_identity";
        joinResult=fault; migrationRetryAt=millis()+10000;
        publishStatus(true);
    }
}
static void readBootstrapFailure(uint8_t status) {
    memset(bootstrapDiagnostic,0,sizeof(bootstrapDiagnostic));
    if(status!=11) return;
    ZnpFrame out;
    if(radio.command(0xcb,nullptr,0,out) && out.data[1]==9)
        memcpy(bootstrapDiagnostic,out.data+5,sizeof(bootstrapDiagnostic));
    erase(&out,sizeof(out));
}
static const char *provisionFailure(uint8_t status) {
    switch(status) {
    case 11: return "bootstrap_storage";
    case 12: return "already_joined";
    case 13: return "counter_exhausted";
    case 14: return "peer_capacity";
    case 15: return "master_network";
    case 16: return "bootstrap_identity";
    default: return "bootstrap_radio";
    }
}
static bool admit(const backhaul::Frame &frame,void *context) {
    Peer &peer=*static_cast<Peer *>(context);
    const uint8_t *p=frame.payload.data();
    if(frame.length==34 && p[0]==1 && !peer.identified) {
        memcpy(peer.remote,p+1,32); const uint8_t *remote=peer.remote;
        if(remote[0] || remote[1]!=2 || r32(remote+2)!=20260917 || p[33]>1 ||
           !czc::validIEEE(remote+12) || !memcmp(remote+12,ownIEEE,8)) {
            ++peerMismatch; peer.fault="peer_identity"; return false;
        }
        // A group PSK authenticates group membership. Each live IEEE/short address is unique.
        for(auto &other:peers) if(&other!=&peer && other.identified &&
            (!memcmp(other.remote+12,remote+12,8) ||
             (other.known && r16(remote+6)<0xfff8 && r16(other.remote+6)==r16(remote+6)))) {
            ++peerMismatch; peer.fault="duplicate_peer"; return false;
        }
        // A disconnected slot is history, not another device. Retire it when
        // the same authenticated IEEE returns through a different free slot.
        for(auto &other:peers) if(&other!=&peer && other.tls.fd<0 && !other.endpoint &&
            !memcmp(other.remote+12,remote+12,8)) {
            memset(other.remote,0,sizeof(other.remote)); other.remoteIp[0]=0;
        }
        const uint8_t *master=cfg.mode==1 ? localInfo : remote;
        const uint8_t *satellite=cfg.mode==1 ? remote : localInfo;
        if(master[11]!=9 || r16(master+6)!=0 || master[10]<11 || master[10]>26) {
            peer.fault="master_network"; return false;
        }
        if(satellite[11]==7 && r16(satellite+6)>0 && r16(satellite+6)<0xfff8 && sameNetwork(master,satellite)) {
            if(!bindRadio(peer) || !leaseRadio(peer,true)) return false;
            peer.identified=peer.known=true; peer.fault=fault="none";
            if(cfg.mode==2 && rejoinRequested) { rejoinRequested=false; joinResult="joined"; }
            return true;
        }
        if(satellite[11]==7 || satellite[11]==9) {
            if(cfg.mode==2 && static_cast<int32_t>(millis()-migrationRetryAt)>=0) {
                MigrationNv nv;
                auto migration=czc::SatelliteMigration::prepare(nv,localInfo,master);
                if(migration==czc::SatelliteMigration::Prepared) {
                    bootstrapState=3; satelliteInitialized=false;
                    joinResult=peer.fault=fault="network_migrating";
                    return false;
                }
                if(migration==czc::SatelliteMigration::StorageError) {
                    migrationRetryAt=millis()+10000; peer.fault="bootstrap_storage"; return false;
                }
            }
            peer.fault="already_joined"; return false;
        }
        if(!(cfg.mode==1 ? p[33] : rejoinRequested)) { peer.fault="bootstrap_required"; return false; }
        peer.identified=true; peer.profileNeeded=cfg.mode==1; peer.fault="provisioning";
        return true;
    }
    if(frame.length==81 && p[0]==3 && cfg.mode==2 && peer.identified && !peer.known && rejoinRequested && !bootstrapRestart) {
        // Only this TLS peer may supply the initial network. Keys never enter status/AF queues.
        const uint8_t *profile=p+1;
        if(profile[0]!=1 || memcmp(profile+8,peer.remote+12,8) || memcmp(profile+16,ownIEEE,8) ||
           profile[1]!=peer.remote[10] || memcmp(profile+2,peer.remote+8,2) || memcmp(profile+24,peer.remote+20,8)) {
            peer.fault="bootstrap_identity"; return false;
        }
        ZnpFrame out;
        bool ok=radio.command(0xc9,profile,80,out);
        if(!ok) {
            uint8_t status=out.size>=6 ? out.data[4] : 255;
            readBootstrapFailure(status); joinResult=peer.fault=provisionFailure(status); rejoinRequested=false;
        }
        erase(&out,sizeof(out));
        if(ok) { bootstrapRestart=true; bootstrapAt=millis()+100; joinResult="joining"; }
        return ok;
    }
    if(p[0]!=2 || !peer.known || paused || frame.length<39 || frame.length>140 ||
       p[12]>127 || frame.length!=13+p[12]) return false;
    uint8_t in[147]; memcpy(in,peer.session,8);
    memcpy(in+8,p+1,frame.length-1); w32(in+8,frame.sequence);
    ZnpFrame out; bool ok=radio.command(0xc3,in,frame.length+7,out);
    return ok || (out.size>=6 && out.data[4]==8 && p[5]==1);
}
static netconn *dial(uint16_t targetPort) {
    char host[sizeof(cfg.host)]; strlcpy(host,cfg.host,sizeof(host));
    unsigned scope=0; char *zone=strchr(host,'%');
    if(zone) {
        *zone++=0;
        esp_netif_t *netif=esp_netif_get_handle_from_ifkey(!strcmp(zone,"wifi") ? "WIFI_STA_DEF" : "ETH_DEF");
        if(!netif) return nullptr;
        scope=esp_netif_get_netif_impl_index(netif);
    }
    addrinfo hints{},*addresses=nullptr; hints.ai_socktype=SOCK_STREAM; hints.ai_family=strchr(host,':') ? AF_INET6 : AF_UNSPEC;
    char port[6]; snprintf(port,sizeof(port),"%u",targetPort);
    if(getaddrinfo(host,port,&hints,&addresses)) return nullptr;
    unsigned count=0; for(auto *a=addresses;a;a=a->ai_next) ++count;
    static unsigned next=0;
    unsigned selected=count ? next++%count : 0,ordinal=0;
    netconn *connected=nullptr;
    for(auto *a=addresses;a;a=a->ai_next,++ordinal) {
        if(ordinal!=selected) continue; // Rotate DNS alternatives after an unsuccessful connection.
        ip_addr_t ip{};
        if(a->ai_family==AF_INET6) {
            IP_SET_TYPE_VAL(ip,IPADDR_TYPE_V6);
            memcpy(ip_2_ip6(&ip)->addr,&reinterpret_cast<sockaddr_in6 *>(a->ai_addr)->sin6_addr,16);
            ip6_addr_set_zone(ip_2_ip6(&ip),scope);
        } else { IP_SET_TYPE_VAL(ip,IPADDR_TYPE_V4); ip_2_ip4(&ip)->addr=reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr.s_addr; }
        connected=netconn_new(a->ai_family==AF_INET6 ? NETCONN_TCP_IPV6 : NETCONN_TCP);
        if(!connected) break;
        netconn_set_nonblocking(connected,1);
        err_t r=netconn_connect(connected,&ip,targetPort);
        if(r!=ERR_OK && r!=ERR_INPROGRESS) { netconn_delete(connected); connected=nullptr; }
    }
    freeaddrinfo(addresses); return connected;
}
static void endpointFailed(Peer &p) {
    auto reason=p.endpoint->reason();
    switch(reason) {
        case backhaul::Reason::HandshakeTimeout: peerError="handshake_timeout"; break;
        case backhaul::Reason::IdleTimeout: peerError="idle_timeout"; break;
        case backhaul::Reason::TransactionTimeout: peerError="confirmation_timeout"; break;
        case backhaul::Reason::ControlOverflow: peerError="control_overflow"; break;
        case backhaul::Reason::SequenceExhausted: peerError="sequence_exhausted"; break;
        default: peerError="protocol"; break;
    }
    bool deadline=reason==backhaul::Reason::HandshakeTimeout || reason==backhaul::Reason::IdleTimeout || reason==backhaul::Reason::TransactionTimeout;
    disconnectPeer(p,deadline ? "peer_deadline" : "peer_protocol");
}
static void workPeer(Peer &p) {
    uint32_t failedBefore=p.tls.failures; p.tls.tick();
    if(p.tls.failures!=failedBefore) fault=p.fault=p.tls.error;
    if(p.sessionOpen && p.tls.fd<0) { disconnectPeer(p,"peer_disconnected"); return; }
    if(!p.tls.ready) return;
    if(!p.sessionOpen) {
        if(++connectionEpoch==0) { disconnectPeer(p,"epoch"); return; }
        uint64_t epoch=(uint64_t(bootEpoch)<<32)|connectionEpoch;
        memset(p.remote,0,sizeof(p.remote)); p.remoteIp[0]=0;
        p.tls.peerAddress(p.remoteIp,sizeof(p.remoteIp));
        for(unsigned i=0;i<8;i++) p.session[i]=epoch>>(8*i);
        p.endpoint=new backhaul::Endpoint(cfg.mode==1 ? 1 : 2,cfg.mode==1 ? 2 : 1);
        if(!p.endpoint || !p.endpoint->open(epoch,nowMs())) { disconnectPeer(p,"memory_or_epoch"); return; }
        p.sessionOpen=true; ++reconnects;
    }
    auto &ep=*p.endpoint;
    uint8_t input[512]; int n=p.tls.read(input,sizeof(input));
    bool valid=n>=0 && (!n || ep.input(input,n,nowMs())); erase(input,sizeof(input));
    if(!valid) {
        if(n<0) disconnectPeer(p,"peer_disconnected"); else endpointFailed(p);
        return;
    }
    ep.tick(nowMs());
    if(ep.state()==backhaul::State::Closed) { endpointFailed(p); return; }
    if(ep.state()==backhaul::State::Ready && !p.infoSent) {
        uint8_t payload[34]; payload[0]=1; memcpy(payload+1,localInfo,32); payload[33]=cfg.mode==2 && rejoinRequested; uint32_t seq;
        p.infoSent=ep.submit(payload,sizeof(payload),nowMs(),seq)==backhaul::Submit::Queued;
    }
    // A Confirm and its radio reply can arrive in the same TLS read. Queue
    // the local MAC confirm first: TI only matches APS ACKs after that confirm.
    results(p);
    ep.serviceIncoming(admit,&p,nowMs());
    if(p.profileNeeded && !p.profileSent) {
        ZnpFrame out; uint8_t profile[81]{}; uint32_t seq;
        bool ok=radio.command(0xc8,p.remote+12,8,out) && out.data[1]==81;
        if(ok) {
            profile[0]=3; memcpy(profile+1,out.data+5,80);
            p.profileSent=ep.submit(profile,sizeof(profile),nowMs(),seq)==backhaul::Submit::Queued;
        } else {
            uint8_t status=out.size>=6 ? out.data[4] : 255;
            readBootstrapFailure(status); p.fault=provisionFailure(status);
        }
        erase(profile,sizeof(profile)); erase(&out,sizeof(out));
        if(!ok) { disconnectPeer(p,p.fault); return; }
    }
    if(p.known) {
        if(millis()-p.lastLease>350 && !leaseRadio(p,true)) { disconnectPeer(p,"radio_lease"); return; }
        if(!holdTx && millis()-p.lastPoll>=10 && ep.pending()<4) {
            unsigned slot=0; while(slot<4 && p.mapping[slot].sequence) ++slot;
            if(slot<4) {
                p.lastPoll=millis(); ZnpFrame out;
                if(radio.command(0xc2,p.session,8,out)) {
                    if(out.data[1]<47 || out.data[1]>148 || out.data[24]>127 || out.data[1]!=21+out.data[24]) { disconnectPeer(p,"radio_frame"); return; }
                    uint8_t payload[140]; payload[0]=2; memcpy(payload+1,out.data+13,out.data[1]-9);
                    uint32_t seq=0,ticket=r32(out.data+13);
                    if(ep.submit(payload,out.data[1]-8,nowMs(),seq)==backhaul::Submit::Queued) { p.mapping[slot].sequence=seq; p.mapping[slot].ticket=ticket; }
                    else complete(p,ticket,false);
                }
            }
        }
    } else if(millis()-p.tls.since>5000) { disconnectPeer(p,p.fault); return; }
    size_t length=0; const uint8_t *data=ep.output(length);
    if(data && length) { int sent=p.tls.write(data,length); if(sent>0) ep.consumeOutput(sent); }
}
static void serviceRadioLifecycle() {
    static uint32_t seenReset=0;
    radio.tick();
    if(seenReset==radio.resetGeneration) return;
    seenReset=radio.resetGeneration;
    // A reset destroys radio sessions and tickets. Do not issue cleanup RPCs
    // against a booting radio, or apply old completions to its new sessions.
    for(auto &p:peers) { p.bound=false; for(auto &m:p.mapping) m={}; }
    disconnectPeers("radio_reset");
    memset(localInfo,0,sizeof(localInfo)); radioVersion.compatible=false;
    satelliteInitialized=false; masterInitialized=false; lastInfo=0;
    // Keep the controller TCP/TLS session and queued resetInd intact.
}
static void initializeMaster() {
    // A controller owns its entire clear/restore/commission sequence. Background
    // BDB is only for unattended reboot with an already saved coordinator network.
    if(cfg.mode!=1 || masterInitialized || rawClient.connected() || server.hasClient()
#ifdef DEBUG
       || adminTls.fd>=0
#endif
       ) return;
    if(localInfo[11]==9) { masterInitialized=true; return; }
    MigrationNv nv; uint8_t on=0,type=0,options=0; uint16_t nibSize=0;
    if(!nv.read(czc::SatelliteMigration::OnNetwork,&on,1) || !nv.read(0x87,&type,1) ||
       !nv.read(czc::SatelliteMigration::Startup,&options,1) || !nv.length(czc::SatelliteMigration::Nib,nibSize)) return;
    if(on!=1 || type!=0 || (options&3) || !nibSize) return;
    fault="radio_initializing"; publishStatus(true);
    ZnpFrame out; uint8_t initialize=0;
    if(radio.rpc(0x2f,5,&initialize,1,out) && out.size>=6 && !out.data[4]) {
        masterInitialized=true; lastInfo=0;
    } else fault=radio.fatal ? "uart_desync" : "bootstrap_radio";
}
static void peerWork() {
    if(paused || radio.fatal) { if(paused || radio.fatal) disconnectPeers(paused ? "paused" : "uart_desync"); return; }
    if(radio.resetWaiting) { fault="radio_reset"; return; }
    if(cfg.mode==2 && bootstrapState==3) { migrateSatellite(); return; }
    if(cfg.mode==2 && !satelliteInitialized) {
        // ZNP routers need BDB initialization after every radio reset; no controller drives Satellite.
        if(lastInfo && millis()-lastInfo<2000) return;
        lastInfo=millis();
        if(!info()) return;
        // Resume durable migration before BDB can restore the previous network.
        if(bootstrapState==3) return;
        ZnpFrame out; uint8_t initialize=0;
        fault="radio_initializing"; publishStatus(true);
        if(!radio.rpc(0x2f,5,&initialize,1,out) || out.size<6 || out.data[4]) { fault="bootstrap_radio"; return; }
        satelliteInitialized=true; satelliteReadyAt=millis()+1200; lastInfo=0;
        // The post-initialization router deadline begins after NV/BDB work,
        // which has its own bounded command deadline.
        if(!strcmp(joinResult,"restoring")) joinStarted=millis();
        fault="awaiting_master";
        return;
    }
    if(cfg.mode==2 && static_cast<int32_t>(millis()-satelliteReadyAt)<0) return;
    if(!lastInfo || millis()-lastInfo>2000) {
        uint8_t old[32]; memcpy(old,localInfo,32); lastInfo=millis();
        if(!info()) { disconnectPeers(fault); return; }
        initializeMaster();
        if(radio.fatal) return;
        if(memcmp(old+6,localInfo+6,22)) disconnectPeers("network_changed");
        else if(cfg.mode==1 && localInfo[11]==9 && r16(localInfo+6)==0 && !strcmp(fault,"network_changed"))
            fault="awaiting_peer"; // Initial BDB startup is not a permanent network-change error.
        if(cfg.mode==2 && !autoJoinChecked) {
            autoJoinChecked=true;
            if(localInfo[11]!=7 && localInfo[11]!=9) { rejoinRequested=true; joinResult="joining"; joinStarted=millis(); }
        }
    }
    if(!radioVersion.compatible) return;
    if(cfg.mode==1) {
        if(localInfo[11]!=9 || r16(localInfo+6)!=0) fault="master_network";
        netconn *fd=nullptr;
        if(peerListen && netconn_accept(peerListen,&fd)==ERR_OK && fd) {
            Peer *free=nullptr; for(auto &p:peers) if(p.tls.fd<0 && !p.endpoint) { free=&p; break; }
            if(free) free->tls.start(fd,true,cfg.key,"czc-peer-v2"); else { NetconnStream rejected; rejected.start(fd); rejected.close(); fault="peer_capacity"; }
        }
    } else if(peerRetry.due(millis(),peers[0].tls.fd>=0 || peers[0].endpoint,peers[0].known,r32(ownIEEE)^bootEpoch^connectionEpoch)) {
        peerRetry.started();
        fault=peers[0].fault="peer_connect";
        netconn *fd=dial(cfg.peerPort);
        if(fd) peers[0].tls.start(fd,false,cfg.key,"czc-peer-v2");
    }
    // Rotate the first serviced peer, so a busy Satellite cannot monopolize UART exports.
    static unsigned first=0;
    for(unsigned i=0;i<PeerLimit;i++) { Peer &p=peers[(first+i)%PeerLimit]; if(p.tls.fd>=0 || p.endpoint) workPeer(p); }
    first=(first+1)%PeerLimit;
}
static void ieeeText(const uint8_t *ieee,char *out) { for(unsigned i=0;i<8;i++) snprintf(out+2*i,3,"%02x",ieee[7-i]); }
static void statusJson(char *out,size_t size,bool compact=false) {
    StaticJsonDocument<4096> doc;
    unsigned connected=peerCount();
    doc["rev"]=20260917; doc["role"]=cfg.mode==1 ? "master" : "satellite";
    doc["peer"]=connected>0; doc["peers_online"]=connected; doc["peer_limit"]=cfg.mode==1 ? PeerLimit : 1;
    doc["paused"]=paused; doc["uart_ok"]=!radio.fatal; doc["debug"]=czc::debugBuild;
    doc["fault"]=connected ? "none" : fault; doc["join"]=joinResult; doc["bootstrap"]=bootstrapState;
    doc["af"]=peers[0].tls.family==AF_INET6 ? 6 : (peers[0].tls.family==AF_INET ? 4 : 0);
    if(!compact) {
        doc["last_peer_error"]=peerError;
        doc["uart_error"]=radio.error; doc["uart_cmd0"]=radio.errorCmd0; doc["uart_cmd1"]=radio.errorCmd1;
        doc["radio_reset_pending"]=radio.resetWaiting;
        doc["radio_revision"]=radioVersion.revision; doc["radio_required"]=uint32_t(czc::RadioVersion::required);
        doc["radio_protocol"]=radioVersion.protocol; doc["radio_compatible"]=radioVersion.compatible;
        char own[17]; ieeeText(ownIEEE,own); doc["own_ieee"]=own;
        doc["psk_tls_bytes"]=czc_tls_allocated();
#ifdef DEBUG
        auto admin=doc.createNestedObject("admin_tls");
        admin["state"]=adminTls.ready ? "connected" : adminTls.fd>=0 ? "handshake" : "listening";
        admin["failures"]=adminTls.failures; admin["last_stage"]=adminTls.errorPhase; admin["last_code"]=adminTls.errorCode;
        admin["open_free"]=adminOpenMemory.free; admin["open_largest"]=adminOpenMemory.largest;
        admin["raw_client"]=bool(rawClient.connected());
#endif
        if(cfg.mode==2) {
            doc["connect_attempts"]=peerRetry.attempts;
            doc["retry_ms"]=peerRetry.remaining(millis());
            doc["master_host"]=cfg.host;
        }
        JsonArray list=doc.createNestedArray("peers");
        for(auto &p:peers) if(p.identified || czc::validIEEE(p.remote+12)) {
            auto item=list.createNestedObject(); char ieee[17]; ieeeText(p.remote+12,ieee);
            item["ieee"]=ieee; item["address"]=r16(p.remote+6); item["online"]=p.known; item["fault"]=p.fault;
            item["ip"]=p.remoteIp;
            item["af"]=p.tls.family==AF_INET6 ? 6 : (p.tls.family==AF_INET ? 4 : 0);
        }
    }
    if(compact) { doc.remove("peer_limit"); }
    serializeJson(doc,out,size);
}
#ifdef DEBUG
static void adminWork() {
    static ZnpParser parser;
    static ZnpFrame outgoing;
    static size_t offset=0;
    static bool had=false;
    static uint32_t seenReset=0,lastProgress=0;
    if(seenReset!=controllerReset) { seenReset=controllerReset; had=false; parser.reset(); parser.errors=0; outgoing.size=0; offset=0; }
    if(adminTls.fd<0) {
        if(had) {
            had=false; parser.reset(); outgoing.size=0; offset=0; radio.adminActive=false; if(!rawClient.connected()) xQueueReset(radio.events);
            // Test controls belong to the authenticated connection, not to NVS.
            // A crashed acceptance client must not leave the pair paused forever.
            paused=holdTx=false;
        }
        int fd=accept(adminListen,nullptr,nullptr);
        if(fd>=0) { if(rawClient.connected()) {
            ::close(fd); adminTls.errorPhase="controller_busy"; adminTls.errorCode=0; ++adminTls.failures;
        } else {
            adminOpenMemory=memoryBudget();
            had=adminTls.start(fd,true,cfg.key,"czc-admin-v1");
        } }
    }
    adminTls.tick();
    radio.adminActive=adminTls.ready || rawClient.connected();
    if(radio.adminOverflow && adminTls.ready) { radio.adminOverflow=false; adminTls.close(); return; }
    if(!adminTls.ready) return;
    if(!outgoing.size && xQueueReceive(radio.events,&outgoing,0)==pdTRUE) lastProgress=millis();
    if(outgoing.size) {
        int n=adminTls.write(outgoing.data+offset,outgoing.size-offset);
        if(n>0) { lastProgress=millis(); offset+=n; if(offset==outgoing.size) { offset=0; outgoing.size=0; } }
        else if(millis()-lastProgress>3000) adminTls.close();
        return;
    }
    if(radio.resetWaiting) return;
    // One request at a time. Read a byte at a time so coalesced requests remain
    // in TLS until their previous SRSP has actually been sent.
    ZnpFrame request; uint8_t byte;
    for(unsigned i=0;i<260;i++) {
        int n=adminTls.read(&byte,1); if(n<=0) break;
        if(!parser.feed(byte,request)) continue;
        uint8_t c0=request.data[2],cmd=request.data[3],len=request.data[1]; const uint8_t *in=request.data+4;
        if(c0==0x3f) {
            uint8_t result=0;
            if(cmd==2 && len==1 && in[0]<=1) {
                paused=in[0]; if(paused) disconnectPeers("paused");
                else peerRetry.retrySoon(millis());
                outgoing.build(0x7f,cmd,&result,1);
            } else if(cmd==1 && !len) {
                char json[251]; statusJson(json,sizeof(json),true); outgoing.build(0x7f,cmd,reinterpret_cast<uint8_t *>(json),strlen(json));
            } else if(cmd==3 && !len && paused) {
                // Explicit acceptance/commissioning action, authenticated admin only.
                ZnpFrame reply;
                for(auto &p:peers) if(r32(p.session) || r32(p.session+4)) radio.command(0xc7,p.session,8,reply);
                outgoing.build(0x7f,cmd,&result,1);
            } else if(cmd==4 && len==1 && in[0]<=1) {
                holdTx=in[0]; outgoing.build(0x7f,cmd,&result,1);
            } else if(cmd==5 && !len) {
                disconnectPeers("admin_reconnect"); peerRetry.retrySoon(millis()); outgoing.build(0x7f,cmd,&result,1);
            } else if(cmd==6 && !len && cfg.mode==2) { rejoinRequested=true; joinResult="requested"; outgoing.build(0x7f,cmd,&result,1); }
            else { result=1; outgoing.build(0x7f,cmd,&result,1); }
        } else if((c0&0xe0)!=0x20 && (c0&0xe0)!=0x40) { adminTls.close(); }
        else if(c0==0x21 && czc::privateRadioCommand(cmd)) {
            uint8_t denied=0x1a; outgoing.build(0x61,cmd,&denied,1);
        } else {
            outgoing.size=0;
            if(!radio.rpc(c0,cmd,in,len,outgoing,Radio::controllerTimeout(c0,cmd))) adminTls.close();
        }
        lastProgress=millis(); break;
    }
    if(parser.errors) { parser.reset(); parser.errors=0; adminTls.close(); }
}

#endif // DEBUG

static void rawClose() {
    if(rawClient) rawClient.stop();
    if(vars.connectedClients) socketClientDisconnected(0);
    radio.adminActive=false; radio.adminOverflow=false;
    if(radio.events) xQueueReset(radio.events);
}
static void rawWork() {
    static ZnpParser parser;
    static ZnpFrame outgoing;
    static size_t offset=0;
    static uint32_t seenReset=0,lastProgress=0;
    if(seenReset!=controllerReset || !rawClient.connected()) {
        seenReset=controllerReset; parser.reset(); parser.errors=0; outgoing.size=0; offset=0;
        if(vars.connectedClients) rawClose();
    }
    if(server.hasClient()) {
        WiFiClient candidate=server.available();
        if(rawClient.connected() ||
#ifdef DEBUG
           adminTls.fd>=0 ||
#endif
           (systemCfg.fwEnabled && candidate.remoteIP()!=systemCfg.fwIp)) candidate.stop();
        else {
            rawClient=candidate; rawClient.setNoDelay(true); lastProgress=millis();
            xQueueReset(radio.events); radio.adminActive=true;
            socketClientConnected(0,rawClient.remoteIP());
        }
    }
    if(!rawClient.connected()) return;
    if(radio.fatal || radio.adminOverflow) { rawClose(); return; }
    if(!outgoing.size) { xQueueReceive(radio.events,&outgoing,0); if(outgoing.size) lastProgress=millis(); }
    if(outgoing.size) {
        // Arduino ESP32 2.0.11 inherits Print::availableForWrite() == 0.
        // A bounded socket send also avoids WiFiClient::write() blocking peer leases.
        int n=::send(rawClient.fd(),outgoing.data+offset,outgoing.size-offset,MSG_DONTWAIT);
        if(n>0) {
            offset+=n; lastProgress=millis();
            if(offset==outgoing.size) { outgoing.size=0; offset=0; }
        } else if(!n || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) { rawClose(); return; }
        if(millis()-lastProgress>3000) rawClose();
        return;
    }
    if(radio.resetWaiting) return;
    ZnpFrame request;
    for(unsigned i=0;i<260 && rawClient.available();i++) {
        if(!parser.feed(rawClient.read(),request)) continue;
        uint8_t c0=request.data[2],cmd=request.data[3],len=request.data[1];
        if((c0&0xe0)!=0x20 && (c0&0xe0)!=0x40) { rawClose(); break; }
        // Peer/session ownership and diagnostics are never exposed to ordinary ZNP clients.
        if((c0&31)==31 || (c0==0x21 && czc::privateRadioCommand(cmd))) {
            if((c0&0xe0)==0x20) { uint8_t denied=0x1a; outgoing.build((c0&31)|0x60,cmd,&denied,1); }
        } else if(!radio.rpc(c0,cmd,request.data+4,len,outgoing,Radio::controllerTimeout(c0,cmd))) rawClose();
        lastProgress=millis(); break;
    }
    if(parser.errors) rawClose();
}

static void statusLogLine(char *out,size_t size) {
    snprintf(out,size,"[BH] %s state=%s radio=%lu/protocol-%u required=%lu/protocol-2 peers=%u/%u debug=%u",
        cfg.mode==1 ? "Master" : cfg.mode==2 ? "Satellite" : "disabled",peerCount() ? "connected" : fault,
        static_cast<unsigned long>(radioVersion.revision),radioVersion.protocol,
        static_cast<unsigned long>(czc::RadioVersion::required),peerCount(),cfg.mode==1 ? PeerLimit : 1,czc::debugBuild);
    if(czc::debugBuild && (!strcmp(fault,"peer_deadline") || !strcmp(fault,"peer_protocol"))) {
        size_t used=strlen(out);
        if(used<size) snprintf(out+used,size-used," detail=%s",peerError);
    }
    if(czc::debugBuild && !strcmp(fault,"bootstrap_storage") && bootstrapDiagnostic[0]) {
        size_t used=strlen(out);
        if(used<size) snprintf(out+used,size-used," nv_op=%u item=%04X/%04X len=%u status=%u",
            bootstrapDiagnostic[0],r16(bootstrapDiagnostic+1),r16(bootstrapDiagnostic+3),r16(bootstrapDiagnostic+5),bootstrapDiagnostic[7]);
    }
    if(radio.fatal) {
        size_t used=strlen(out);
        if(used<size) snprintf(out+used,size-used," uart=%s cmd=%02X/%02X",radio.error,radio.errorCmd0,radio.errorCmd1);
    }
#ifdef DEBUG
    if(adminTls.failures) {
        size_t used=strlen(out);
        if(used<size) snprintf(out+used,size-used," admin=%s code=%d failures=%lu",
            adminTls.errorPhase,adminTls.errorCode,static_cast<unsigned long>(adminTls.failures));
    }
#endif
}
static void publishStatus(bool force) {
    static uint32_t last=0; if(!force && millis()-last<250) return; last=millis();
    char json[sizeof(cachedStatus)]{}; statusJson(json,sizeof(json));
    char line[256]{}; statusLogLine(line,sizeof(line));
    portENTER_CRITICAL(&statusLock);
    memcpy(cachedStatus,json,sizeof(cachedStatus)); memcpy(cachedLog,line,sizeof(cachedLog));
    portEXIT_CRITICAL(&statusLock);
}
// Apply the staged profile only on this fresh Satellite. Master and other peers stay live.
static void commissionSatellite() {
    if(bootstrapRestart && static_cast<int32_t>(millis()-bootstrapAt)>=0) {
        bootstrapRestart=false; rejoinRequested=false; disconnectPeers("joining");
        rawClose();
#ifdef DEBUG
        adminTls.close();
#endif
        ++controllerReset;
        ZnpFrame out; uint8_t reset=1;
        bool ok=radio.rpc(0x41,0,&reset,1,out);
        // peerWork initializes BDB only after the real resetInd arrives.
        joinResult=ok ? "restoring" : "bootstrap_radio"; joinStarted=millis(); lastInfo=0;
        satelliteInitialized=false;
    }
    if(rejoinRequested && strcmp(joinResult,"joining") && strcmp(joinResult,"awaiting_master")) {
        joinResult="joining"; joinStarted=millis(); disconnectPeers("joining"); lastInfo=0;
    }
    if(!strcmp(joinResult,"restoring") && localInfo[11]==7) joinResult="joined";
    if(!strcmp(joinResult,"restoring") && millis()-joinStarted>30000) {
        rejoinRequested=false; joinResult="join_timeout"; disconnectPeers(joinResult);
    } else if(rejoinRequested && !strcmp(joinResult,"joining") && millis()-joinStarted>30000) {
        // Waiting for a restored Master does not revoke the initial bootstrap
        // request. Keep advertising it on retries, without tearing down TLS.
        joinResult="awaiting_master";
    }
}

static void networkTask(void *) {
    NetconnStream::threadInit();
    for(;;) {
        if(maintenanceRequested || bslHold) {
            if(!maintenanceActive) {
                disconnectPeers("maintenance");
#ifdef DEBUG
                adminTls.close();
#endif
                rawClose(); ++controllerReset;
                // Let the radio finish ownership cleanup before giving its UART to CCTools.
                uint32_t start=millis(); ZnpFrame out;
                while(!radio.fatal && millis()-start<1600) {
                    for(auto &p:peers) if(r32(p.session) || r32(p.session+4)) radio.command(0xc7,p.session,8,out);
                    if(info() && localInfo[28]==0) break;
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if(radio.suspend()) maintenanceActive=true;
            }
            publishStatus(); vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        if(maintenanceActive) {
            radio.resume(); maintenanceActive=false; satelliteInitialized=false; lastInfo=0; radioVersion.compatible=false; fault="awaiting_peer";
        }
        commissionSatellite();
        serviceRadioLifecycle();
        peerWork();
#ifdef DEBUG
        adminWork();
#endif
        rawWork();
        publishStatus(); vTaskDelay(1);
    }
}

static bool backhaulRoleAllowed() { return systemCfg.zbRole==COORDINATOR; }
bool backhaulConfigured() { return cfg.mode!=0 && backhaulRoleAllowed(); }
bool backhaulEnabled() { return bootSelectionMade ? uartSelected : backhaulConfigured() && systemCfg.workMode==WORK_MODE_NETWORK; }
void backhaulLoad() {
    czc_random::begin();
    maintenanceMutex=xSemaphoreCreateRecursiveMutex();
    Preferences store;
    if(store.begin("czc-peer",true)) {
        czc::Settings saved;
        size_t length=store.getBytesLength("settings");
        if((length==sizeof(saved) || length==offsetof(czc::Settings,legacyDiagnostics)) && store.getBytes("settings",&saved,length)==length &&
           !czc::validate(saved,systemCfg.socketPort)) cfg=saved;
        memset(&saved,0,sizeof(saved)); store.end();
    }
    uint8_t token[16];
    if(!czc_random::fill(nullptr,token,sizeof(token))) {
        for(unsigned i=0;i<sizeof(token);i++) snprintf(csrf+i*2,3,"%02x",token[i]);
    }
    memset(token,0,sizeof(token));
    if(systemCfg.zbRole!=UNDEFINED) backhaulRoleChanged();
}
void backhaulBegin() {
    if(bootSelectionMade) return;
    backhaulRoleChanged();
    uartSelected=backhaulEnabled(); bootSelectionMade=true;
    if(!backhaulEnabled()) { fault=cfg.mode ? "usb_mode" : "disabled"; return; }
    if(!csrf[0] || !maintenanceMutex) { fault="entropy_or_memory"; return; }
    if(systemCfg.serialSpeed!=115200) { fault="radio_baud_115200_required"; return; }
    Preferences store;
    if(!store.begin("czc-peer",false)) { fault="storage"; return; }
    bootEpoch=store.getUInt("boot",0)+1;
    bool saved=bootEpoch && store.putUInt("boot",bootEpoch)==4; store.end();
    if(!saved) { fault="epoch_storage"; return; }
    for(auto &p:peers) p.tls.randomSource=czc_random::fill;
#ifdef DEBUG
    adminTls.randomSource=czc_random::fill;
    adminListen=listener(czc::debugAdminPort,true);
    if(adminListen<0) { fault="listen_failed"; return; }
#endif
    if(cfg.mode==1) peerListen=NetconnStream::listen(cfg.peerPort,true);
    if(cfg.mode==1 && !peerListen) { fault="listen_failed"; return; }
    if(!radio.begin()) { fault="uart_task_failed"; return; }
    fault="awaiting_peer";
    if(xTaskCreatePinnedToCore(networkTask,"czc-peer",16384,nullptr,1,nullptr,0)!=pdPASS) { radio.stop(); fault="peer_task_failed"; return; }
    running=true;
}
void backhaulLoop() {
    // The stock web-console buffer is owned by the main/UI task. Do not write
    // it from the peer task, and do not fill it with repeated polls or frames.
    static uint32_t logAt=0; static char previous[256]{};
    if(millis()-logAt>=1000) {
        logAt=millis(); char line[256]{};
        if(running) { portENTER_CRITICAL(&statusLock); memcpy(line,cachedLog,sizeof(line)); portEXIT_CRITICAL(&statusLock); }
        else if(cfg.mode) statusLogLine(line,sizeof(line));
        if(line[0] && strcmp(line,previous)) {
            // Compare the stable state first: changing heap/RSSI must not turn
            // the console into a once-per-second telemetry dump.
            memcpy(previous,line,sizeof(previous));
            if(czc::debugBuild) {
                auto memory=memoryBudget(); char detail[128];
                snprintf(detail,sizeof(detail)," heap=%lu max=%lu min=%lu wifi=%d esp=%s",
                    static_cast<unsigned long>(memory.free),static_cast<unsigned long>(memory.largest),
                    static_cast<unsigned long>(memory.minimum),WiFi.status()==WL_CONNECTED ? WiFi.RSSI() : 0,VERSION);
                printLogMsg(String(line)+detail);
            } else printLogMsg(String(line));
        }
    }
    if(restartAt && !vars.zbFlashing && (!maintenanceActive || roleRestart) && static_cast<int32_t>(millis()-restartAt)>=0) ESP.restart();
    if(!running || restartAt || vars.zbFlashing || maintenanceRequested || maintenanceActive || bslHold ||
       paused || bootstrapRestart || !strcmp(joinResult,"restoring")) return;
    if(recovery.due(millis(),radio.fatal,peerCount()>0)) {
        recovery.attempted(millis());
        BackhaulMaintenance maintenance;
        if(maintenance) { radio.expectReset(); CCTool.restart(); }
    }
}
bool backhaulMaintenanceBegin() {
    if(!running) return true;
    if(xSemaphoreTakeRecursive(maintenanceMutex,pdMS_TO_TICKS(5000))!=pdTRUE) return false;
    if(maintenanceDepth++) return true;
    maintenanceRequested=true;
    uint32_t start=millis();
    while(!maintenanceActive && millis()-start<5000) vTaskDelay(1);
    if(maintenanceActive) return true;
    maintenanceRequested=false; --maintenanceDepth; xSemaphoreGiveRecursive(maintenanceMutex); return false;
}
void backhaulMaintenanceEnd() {
    if(!running) return;
    if(!--maintenanceDepth) {
        maintenanceRequested=false;
        while(maintenanceActive && !bslHold) vTaskDelay(1);
    }
    xSemaphoreGiveRecursive(maintenanceMutex);
}
void backhaulBslHold(bool hold) { bslHold=hold; }
void backhaulRoleChanged() {
    // This is the installed firmware role, not the router state of a Satellite ZNP.
    if(backhaulRoleAllowed() || !cfg.mode) return;
    czc::Settings disabled=cfg; disabled.mode=0;
    Preferences store;
    bool saved=store.begin("czc-peer",false) && store.putBytes("settings",&disabled,sizeof(disabled))==sizeof(disabled);
    store.end(); memset(&disabled,0,sizeof(disabled));
    if(!saved) printLogMsg("[BH] Cannot save disabled mode; incompatible radio role remains blocked");
    if(running) {
        BackhaulMaintenance maintenance;
        // Keep the broker suspended until boot selects the ordinary UART path.
        bslHold=true; roleRestart=true; restartAt=millis()+1500;
    } else cfg.mode=0;
}
bool backhaulStatus(JsonDocument &document) {
    char json[sizeof(cachedStatus)];
    portENTER_CRITICAL(&statusLock); memcpy(json,cachedStatus,sizeof(json)); portEXIT_CRITICAL(&statusLock);
    if(!czc::loadStatusSnapshot(document,json)) {
        return false;
    }
    JsonObject obj=document["status"].as<JsonObject>();
    if(!running) { obj["fault"]=fault; obj["peer"]=false; }
    obj["mode"]=cfg.mode;
    if(!running) obj["own_ieee"]=CCTool.chip.ieee;
    obj["join"]=joinResult;
    obj["maintenance"]=maintenanceActive;
    obj["uart_restarts"]=recovery.total;
    if(radio.fatal && recovery.attempts>=3) obj["fault"]="uart_recovery_exhausted";
    if(maintenanceActive) { obj["peer"]=false; obj["fault"]="maintenance"; }
    else if(!strcmp(joinResult,"joining")) {
        const char *reason=obj["fault"] | "unconfigured";
        if(!strcmp(reason,"none") || !strcmp(reason,"awaiting_peer") || !strcmp(reason,"network_changed") || !strcmp(reason,"provisioning")) {
            obj["peer"]=false; obj["fault"]="joining";
        }
    }
    return !document.overflowed();
}
static void webReply(int code,const char *result) {
    StaticJsonDocument<160> doc; doc["result"]=result;
    String body; serializeJson(doc,body); web->send(code,"application/json",body);
}
static bool webAuthorize(bool mutation=false) {
    web->sendHeader("Cache-Control","no-store");
    if(!is_authenticated()) { webReply(401,"authentication_required"); return false; }
    if(mutation && (!csrf[0] || web->header("X-Backhaul-Token")!=csrf)) { webReply(403,"reload_page"); return false; }
    return true;
}
bool backhaulAuthorizeWebMutation() { return webAuthorize(true); }
static void webConfig() {
    if(!webAuthorize()) return;
    DynamicJsonDocument doc(4096);
    if(!doc.capacity() || !backhaulStatus(doc)) { webReply(503,"status_unavailable"); return; }
    doc["mode"]=backhaulRoleAllowed() ? cfg.mode : 0;
    doc["radio_role"]=systemCfg.zbRole; doc["role_supported"]=backhaulRoleAllowed();
    doc["peer_host"]=cfg.host; doc["peer_port"]=cfg.peerPort;
#ifdef DEBUG
    doc["admin_port"]=czc::debugAdminPort;
#endif
    doc["debug_mode"]=czc::debugBuild;
    doc["key_set"]=bool(cfg.hasKey); doc["token"]=csrf;
    char savedKey[65]{};
    if(cfg.hasKey) for(unsigned i=0;i<32;i++) snprintf(savedKey+2*i,3,"%02x",cfg.key[i]);
    doc["psk"]=savedKey; // Available through the existing web authentication policy.

    memoryStatus(doc["status"]["heap"].to<JsonObject>());
    if(doc.overflowed()) { webReply(503,"status_unavailable"); return; }
    String body; serializeJson(doc,body);
    if(body.length()!=measureJson(doc)) { webReply(503,"status_unavailable"); return; }
    web->send(200,"application/json",body);
}
static void webSave() {
    if(!webAuthorize(true)) return;
    if(vars.zbFlashing || maintenanceRequested || maintenanceActive || bootstrapRestart || !strcmp(joinResult,"restoring")) { webReply(409,"busy"); return; }
    String body=web->arg("plain");
    if(body.length()>1024) { webReply(413,"invalid_settings"); return; }
    DynamicJsonDocument doc(1536);
    if(deserializeJson(doc,body) || !doc["mode"].is<unsigned>() || !doc["peer_port"].is<unsigned>() ||
       !doc["peer_host"].is<const char *>() || !doc["psk"].is<const char *>()) {
        webReply(400,"invalid_settings"); return;
    }
    czc::Settings fresh=cfg;
    unsigned mode=doc["mode"],peerPort=doc["peer_port"];
    if(mode>2 || peerPort>65535 || strlen(doc["peer_host"])>=sizeof(fresh.host)) {
        webReply(400,"invalid_settings"); return;
    }
    fresh.mode=mode; fresh.legacyIpv6=1; fresh.legacyRawTcp=1;
    // Initialize reserved NVS bytes; diagnostics are selected at build time.
    fresh.legacyDiagnostics=0; fresh.legacyAdminPort=czc::debugAdminPort;
    fresh.peerPort=peerPort; strlcpy(fresh.host,doc["peer_host"],sizeof(fresh.host));
    const char *key=doc["psk"];
    bool valid=true;
    if(*key) { valid=czc::decodeHex(key,fresh.key,32); if(valid) fresh.hasKey=1; }
    memset(fresh.peerIEEE,0,sizeof(fresh.peerIEEE)); // Reserved NVS bytes.
    const char *error=valid ? czc::validate(fresh,systemCfg.socketPort) : "invalid_key";
    if(!error && fresh.mode && !backhaulRoleAllowed()) error="coordinator_required";
    if(!error && fresh.mode && (!networkCfg.ethEnable && !networkCfg.wifiEnable)) error="network_required";
    if(!error && fresh.mode && systemCfg.workMode!=WORK_MODE_NETWORK) error="select_network_mode";
    if(!error && fresh.mode && systemCfg.serialSpeed!=115200) error="radio_baud_115200_required";
    if(error) { memset(&fresh,0,sizeof(fresh)); webReply(400,error); return; }
    Preferences store;
    bool saved=store.begin("czc-peer",false) && store.putBytes("settings",&fresh,sizeof(fresh))==sizeof(fresh);
    store.end(); memset(&fresh,0,sizeof(fresh)); doc.clear();
    if(saved) { webReply(200,"saved_restarting"); restartAt=millis()+1500; }
    else webReply(500,"storage_error");
}
static void webKey() {
    if(!webAuthorize(true)) return;
    uint8_t key[32]; char encoded[65];
    if(czc_random::fill(nullptr,key,sizeof(key))) { webReply(503,"entropy_unavailable"); return; }
    for(unsigned i=0;i<32;i++) snprintf(encoded+2*i,3,"%02x",key[i]);
    StaticJsonDocument<128> doc; doc["psk"]=encoded;
    String body; serializeJson(doc,body); web->send(200,"application/json",body);
    memset(key,0,sizeof(key)); memset(encoded,0,sizeof(encoded)); doc.clear();
}
static void webJoin() {
    if(!webAuthorize(true)) return;
    if(!backhaulRoleAllowed()) { webReply(409,"coordinator_required"); return; }
    if(!running || cfg.mode!=2) { webReply(409,"satellite_required"); return; }
    if(rejoinRequested || !strcmp(joinResult,"joining") || maintenanceRequested) { webReply(409,"busy"); return; }
    rejoinRequested=true; joinResult="requested"; webReply(202,"joining");
}
void backhaulWebRoutes(WebServer &serverWeb) {
    web=&serverWeb;
    web->on("/api/backhaul",HTTP_GET,webConfig);
    web->on("/api/backhaul",HTTP_POST,webSave);
    web->on("/api/backhaul/key",HTTP_POST,webKey);
    web->on("/api/backhaul/join",HTTP_POST,webJoin);
}
