#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>

static unsigned long now=0;
static unsigned long millis() { return now; }
static void delay(unsigned n) { now+=n; }
static void yield() { ++now; }
enum HTTPClientStatus {HC_NONE,HC_WAIT_READ,HC_WAIT_CLOSE};
static constexpr int HTTP_MAX_DATA_WAIT=5000,HTTP_MAX_SEND_WAIT=5000,HTTP_MAX_CLOSE_WAIT=2000;
static constexpr size_t CONTENT_LENGTH_NOT_SET=size_t(-2);
struct Handle {int fd; explicit Handle(int n):fd(n) {} ~Handle() {::close(fd);} };
class WiFiClient {
    std::shared_ptr<Handle> handle;
public:
    WiFiClient()=default;
    explicit WiFiClient(int n):handle(std::make_shared<Handle>(n)) {}
    int fd() const { return handle ? handle->fd : -1; }
    bool connected() const {return bool(handle);}
    explicit operator bool() const {return connected();}
    int available() const {int n=0; if(handle) assert(ioctl(fd(),FIONREAD,&n)==0); return n;}
    void setTimeout(unsigned) {}
    void stop() {handle.reset();}
};
class Server {
    std::deque<WiFiClient> queue;
public:
    void enqueue(WiFiClient c) {queue.push_back(c);}
    WiFiClient available() {if(queue.empty()) return {}; auto c=queue.front();queue.pop_front();return c;}
};
class WebServer {
public:
    Server _server;
    WiFiClient _currentClient;
    struct IdleHttpClient {WiFiClient client; unsigned long since=0;} _idleHttp[3];
    HTTPClientStatus _currentStatus=HC_NONE;
    unsigned long _statusChange=0;
    bool _nullDelay=true;
    size_t _contentLength=0;
    std::unique_ptr<int> _currentUpload;
    unsigned requests=0;
    std::string last;
    bool _parseRequest(WiFiClient &c) {char b[100]; int n=recv(c.fd(),b,sizeof(b),MSG_DONTWAIT); assert(n>0);last.assign(b,n);return true;}
    void _handleRequest() {++requests;}
    void handleClient();
    void legacyHandleClient();
};
#define log_v(...) ((void)0)
#include "../../.backhaul-tests/web-queue-under-test.inc"

static int enqueue(WebServer &s,const char *request=nullptr) {
    int p[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);
    s._server.enqueue(WiFiClient(p[0]));
    if(request) assert(send(p[1],request,strlen(request),0)==ssize_t(strlen(request)));
    return p[1];
}
int main() {
    {
        WebServer old; now=0;
        int idle=enqueue(old),api=enqueue(old,"GET /api/esp-update HTTP/1.1\r\n\r\n");
        old.legacyHandleClient();
        now=5000; old.legacyHandleClient(); assert(old.requests==0);
        now=5001; old.legacyHandleClient(); assert(old.requests==0);
        old.legacyHandleClient(); assert(old.requests==1);
        close(idle);close(api);
    }
    puts("PASS old HTTP queue stall reproduced: ready API blocked behind idle browser socket for 5000 ms");
    {
        WebServer s; now=0;
        int idle=enqueue(s),api=enqueue(s,"GET /api/esp-update HTTP/1.1\r\n\r\n");
        s.handleClient(); assert(s.requests==0);
        s.handleClient(); assert(s.requests==1 && now<10);
        // Delayed headers are kept, not discarded merely for being slow.
        const char request[]="GET /tools HTTP/1.1\r\n\r\n";
        assert(send(idle,request,sizeof(request)-1,0)==sizeof(request)-1);
        s.handleClient(); assert(s.requests==2 && s.last==request);
        close(idle);close(api);
        int a=enqueue(s),b=enqueue(s),c=enqueue(s),full=enqueue(s);
        for(int i=0;i<4;++i) s.handleClient();
        char buf; assert(recv(full,&buf,1,MSG_DONTWAIT)==0); // fixed bound, no queue growth
        int upload=enqueue(s,"POST /update?size=123 HTTP/1.1\r\n\r\n");
        s.handleClient(); assert(s.requests==3 && s.last.find("POST /update")==0);
        now=6000;s.handleClient();
        for(int fd:{a,b,c}) {assert(recv(fd,&buf,1,MSG_DONTWAIT)==0);close(fd);}
        close(full);close(upload);
    }
    puts("PASS HTTP queue: one stock server, ready API/UI/OTA requests, delayed headers, three-slot bound and expiry");
}
