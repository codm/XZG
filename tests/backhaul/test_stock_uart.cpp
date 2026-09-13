#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#define BUFFER_SIZE 256
#define DEC 10
struct String:std::string {
    using std::string::string;
    String(unsigned long value,int):std::string(std::to_string(value)) {}
};
static std::string logText;
static void logPush(char c) {logText+=c;}
static unsigned long millis() {return 123;}
#include "../../.backhaul-tests/stock-uart-under-test.inc"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-overflow"
namespace legacy {
#include "../../.backhaul-tests/old-uart-log.inc"
}
#pragma GCC diagnostic pop
int main(int argc,char **) {
    uint8_t data[BUFFER_SIZE];
    for(unsigned i=0;i<BUFFER_SIZE;++i) data[i]=i;
    if(argc>1) {legacy::printSendSocket(1,data);return 0;}
    std::string expected;
    for(auto byte:data) {char b[4]; snprintf(b,sizeof(b)," %02x",byte);expected+=b;}
    printRecvSocket(BUFFER_SIZE,data);assert(logText=="[123] ->"+expected+"\n");
    logText.clear();printSendSocket(BUFFER_SIZE,data);assert(logText=="[123] <-"+expected+"\n");
    logText.clear();printRecvSocket(0,data);assert(logText.empty());
    std::deque<uint8_t> input;
    std::vector<uint8_t> received;
    struct Reader {
        std::deque<uint8_t> &input;
        size_t available() {return input.size();}
        uint8_t read() {auto b=input.front();input.pop_front();return b;}
    } Serial2{input},client[]{Serial2};
    unsigned cln=0;
    uint8_t net_buf[BUFFER_SIZE],serial_buf[BUFFER_SIZE];
    uint16_t net_bytes_read=0,serial_bytes_read=0;
    for(int direction=0;direction<2;++direction) {
        input.clear();received.clear();
        for(unsigned i=0;i<1000;++i) input.push_back(i%256);
        while(!input.empty()) {
            net_bytes_read=serial_bytes_read=0;
            if(direction==0) {
#include "../../.backhaul-tests/stock-net-read.inc"
                assert(net_bytes_read>0 && net_bytes_read<=BUFFER_SIZE);
                received.insert(received.end(),net_buf,net_buf+net_bytes_read);
            } else {
#include "../../.backhaul-tests/stock-radio-read.inc"
                assert(serial_bytes_read>0 && serial_bytes_read<=BUFFER_SIZE);
                received.insert(received.end(),serial_buf,serial_buf+serial_bytes_read);
            }
        }
        assert(received.size()==1000);
        for(unsigned i=0;i<1000;++i) assert(received[i]==i%256);
    }
    puts("PASS stock UART Off: all hex bytes within buffer, bounded reads and 1000-byte streams without truncation in both directions");
}
