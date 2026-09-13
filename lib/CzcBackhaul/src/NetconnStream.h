#pragma once
// Use lwIP's sequential API directly: eight peers must not consume the SDK's
// sixteen BSD socket descriptors needed by the stock UI/MQTT/WireGuard/services.
#include <lwip/api.h>
#include <lwip/tcp.h>
#include <lwip/priv/tcpip_priv.h>
#include <algorithm>
class NetconnStream {
    netconn *connection=nullptr;
    pbuf *received=nullptr;
    uint16_t offset=0;
    struct CoreCall { tcpip_api_call_data base; netconn *connection; bool abort,inspect; int state; };
    static err_t core(tcpip_api_call_data *base) {
        auto &call=*reinterpret_cast<CoreCall *>(base);
        if(call.inspect) {
            auto *pcb=call.connection->pcb.tcp;
            call.state=!pcb ? -1 : pcb->state==ESTABLISHED ? 1 :
                (pcb->state==SYN_SENT || pcb->state==SYN_RCVD) ? 0 : -1;
            return ERR_OK;
        }
        if(call.connection->pcb.tcp) {
            if(call.abort) tcp_abort(call.connection->pcb.tcp);
            else tcp_nagle_disable(call.connection->pcb.tcp);
        }
        return ERR_OK;
    }
public:
    int family=0;
    NetconnStream()=default;
    NetconnStream(const NetconnStream &)=delete;
    ~NetconnStream() { close(); }
    int connectState() {
        if(!connection) return -1;
        CoreCall call{}; call.connection=connection; call.inspect=true;
        return tcpip_api_call(core,&call.base)==ERR_OK ? call.state : -1;
    }
    int addressFamily() {
        ip_addr_t address{}; uint16_t port=0;
        if(connection && netconn_peer(connection,&address,&port)==ERR_OK)
            family=IP_IS_V6(&address) && !ip6_addr_isipv4mappedipv6(ip_2_ip6(&address)) ? AF_INET6 : AF_INET;
        return family;
    }
    bool peerAddress(char *out,size_t size) {
        if(!size) return false;
        out[0]=0; ip_addr_t address{}; uint16_t port=0;
        return connection && netconn_peer(connection,&address,&port)==ERR_OK &&
            ipaddr_ntoa_r(&address,out,size)!=nullptr;
    }
    static void threadInit() { netconn_thread_init(); }
    bool start(netconn *handle) {
        close(); connection=handle; if(!connection) return false;
        netconn_set_nonblocking(connection,1);
        CoreCall call{}; call.connection=connection;
        if(tcpip_api_call(core,&call.base)!=ERR_OK) { close(); return false; }
        addressFamily();
        return true;
    }
    void close() {
        if(received) { pbuf_free(received); received=nullptr; } offset=0;
        if(connection) {
            // Abort on the TCP/IP thread, before deletion: never wait for a dead
            // peer to ACK buffered data while other Satellite leases expire.
            CoreCall call{}; call.connection=connection; call.abort=true;
            tcpip_api_call(core,&call.base); netconn_delete(connection); connection=nullptr;
        }
        family=0;
    }
    int write(const unsigned char *data,size_t length) {
        size_t sent=0;
        err_t r=netconn_write_partly(connection,data,length,NETCONN_COPY|NETCONN_DONTBLOCK,&sent);
        if(sent) return sent;
        return r==ERR_OK || r==ERR_WOULDBLOCK || r==ERR_INPROGRESS || r==ERR_MEM ? 0 : -1;
    }
    int read(unsigned char *data,size_t length) {
        if(!received) {
            err_t r=netconn_recv_tcp_pbuf_flags(connection,&received,NETCONN_DONTBLOCK);
            if(r==ERR_WOULDBLOCK || r==ERR_TIMEOUT || r==ERR_INPROGRESS) return 0;
            if(r!=ERR_OK || !received) return -1;
        }
        size_t count=std::min(length,static_cast<size_t>(received->tot_len-offset));
        uint16_t n=pbuf_copy_partial(received,data,count,offset); offset+=n;
        if(offset==received->tot_len) { pbuf_free(received); received=nullptr; offset=0; }
        return n ? n : -1;
    }
    static netconn *listen(uint16_t port,bool ipv6) {
        threadInit(); netconn *server=netconn_new(ipv6 ? NETCONN_TCP_IPV6 : NETCONN_TCP);
        if(!server) return nullptr;
        netconn_set_nonblocking(server,1);
        if(netconn_bind(server,ipv6 ? IP6_ADDR_ANY : IP4_ADDR_ANY,port)!=ERR_OK || netconn_listen_with_backlog(server,8)!=ERR_OK) { netconn_delete(server); return nullptr; }
        return server;
    }
};
