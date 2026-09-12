// Exercise the production TLS 1.3 administrative server with OpenSSL clients.
#include <AdminTls.h>
using TestTls=AdminTls;
#include <cstdio>
#include <thread>
int main(int argc,char **argv) {
    if(argc!=2) return 2;
    int server=listener(atoi(argv[1])); if(server<0) return 3;
    uint8_t key[32]; for(unsigned i=0;i<32;i++) key[i]=i; // Public fixture ONLY.
    TestTls connection;
    auto until=millis()+30000;
    while(static_cast<int32_t>(until-millis())>0) {
        if(connection.fd<0) {
            int fd=accept(server,nullptr,nullptr);
            if(fd>=0) connection.start(fd,true,key,"czc-admin-v1");
        }
        connection.tick();
        if(connection.ready) {
            static uint8_t bytes[1024]; static int size=0,offset=0;
            if(!size) { size=connection.read(bytes,sizeof(bytes)); if(size<0) size=0; }
            if(size) {
                int n=connection.write(bytes+offset,size-offset);
                if(n>0) { offset+=n; if(offset==size) size=offset=0; }
                else if(n<0) size=offset=0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    connection.close(); ::close(server);
}
