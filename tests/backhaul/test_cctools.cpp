// Full production CCTools.cpp, ROM UART fixture. No substituted flash methods.
#include <Arduino.h>
#include <CCTools.h>
#include <RadioImage.h>
#include <array>
#include <vector>
#include <fstream>
#include <cassert>
#include <cstdlib>
#include <new>
static size_t arraysLive=0,arraysTotal=0;
void *operator new[](size_t n){void *p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();++arraysLive;++arraysTotal;return p;}
void operator delete[](void *p) noexcept {if(p){assert(arraysLive);--arraysLive;std::free(p);}}
void operator delete[](void *p,size_t) noexcept {operator delete[](p);}
void *operator new[](size_t n,const std::nothrow_t &) noexcept {try{return operator new[](n);}catch(...){return nullptr;}}
void operator delete[](void *p,const std::nothrow_t &) noexcept {operator delete[](p);}
static unsigned long now=0;
unsigned long millis(){return now;}
void delay(unsigned long ms){now+=ms;}
struct Rom:Stream {
 std::vector<uint8_t> flash=std::vector<uint8_t>(720896,0xff);
 std::array<uint8_t,255> request{},response{};
 size_t reqSize=0,rxHead=0,rxSize=0;
 uint32_t offset=0,remaining=0;
 unsigned blocks=0,acks=0,delayByte=0;unsigned long nextByte=0;
 bool awaitingAck=false,noAck=false,noPacket=false,shortHeader=false,shortBody=false,badLength=false,badChecksum=false,corruptFlash=false;
 uint8_t status=0x40;
 uint32_t word(unsigned at){return (uint32_t(request[at])<<24)|(uint32_t(request[at+1])<<16)|(uint32_t(request[at+2])<<8)|request[at+3];}
 int available()override{return rxHead<rxSize && now>=nextByte;}
 int read()override{if(!available())return -1;nextByte=now+delayByte;return response[rxHead++];}
 void reply(const uint8_t *p,size_t n){
   assert(rxHead==rxSize);rxHead=rxSize=0; if(noAck)return;
   response[rxSize++]=0;response[rxSize++]=0xcc;
   if(!n || noPacket)return;
   awaitingAck=true;
   response[rxSize++]=badLength?1:n+2;
   if(shortHeader)return;
   uint8_t sum=0;for(size_t i=0;i<n;i++)sum+=p[i];response[rxSize++]=sum+badChecksum;
   if(!shortBody){for(size_t i=0;i<n;i++)response[rxSize++]=p[i];}
 }
 size_t write(uint8_t b)override{
   if(awaitingAck && !reqSize && b==0){++reqSize;request[0]=0;return 1;}
   if(awaitingAck && reqSize==1 && request[0]==0){assert(b==0xcc || b==0x33);awaitingAck=false;reqSize=0;++acks;return 1;}
   assert(reqSize<request.size());request[reqSize++]=b;
   if(reqSize<request[0])return 1;
   assert(reqSize==request[0]);uint8_t sum=0;for(size_t i=2;i<reqSize;i++)sum+=request[i];assert(sum==request[1]);
   switch(request[2]){
   case 0x2c:std::fill(flash.begin(),flash.end(),0xff);reply(nullptr,0);break;
   case 0x21:assert(reqSize==11);offset=word(3);remaining=word(7);assert(offset+remaining<=flash.size());reply(nullptr,0);break;
   case 0x24:{size_t n=reqSize-3;assert(n<=252 && remaining>=n && offset+n<=flash.size());if(status==0x40){std::copy(request.begin()+3,request.begin()+reqSize,flash.begin()+offset);offset+=n;remaining-=n;++blocks;}reply(nullptr,0);break;}
   case 0x23:reply(&status,1);break;
   case 0x28:{uint8_t id[4]={0x12,2,0,0};reply(id,4);break;}
   case 0x2a:{
     assert(reqSize==9 && request[7]==1 && request[8]==1);uint8_t out[4]={};
     switch(word(3)){
      case 0x50001318:out[1]=0x70;out[2]=0xb7;out[3]=0x1b;break;
      case 0x50001294:out[1]=0x40;break;
      case 0x4003002c:out[0]=88;break;
      case 0x500012f0:out[0]=0x4d;out[1]=0x42;out[2]=0x11;out[3]=0x2e;break;
      case 0x500012f4:out[1]=0x4b;out[2]=0x12;break;
     }
     reply(out,4);break;
   }
   case 0x27:{assert(reqSize==15 && !word(11));uint32_t address=word(3),len=word(7);assert(address+len<=flash.size());uint32_t crc=czc::radioCrcUpdate(0xffffffffu,flash.data()+address,len)^0xffffffffu;if(corruptFlash)crc^=1;uint8_t out[4]={uint8_t(crc>>24),uint8_t(crc>>16),uint8_t(crc>>8),uint8_t(crc)};reply(out,4);break;}
   default:assert(false);
   }
   reqSize=0;return 1;
 }
};
struct Probe:CCTools {using CCTools::CCTools;using CommandInterface::_checkLastCmd;using CommandInterface::_receivePacket;};
static std::vector<uint8_t> load(const char *path){std::ifstream f(path,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
 assert(argc==2);auto image=load(argv[1]);assert(image.size()==720896);
 assert((czc::radioCrcUpdate(0xffffffffu,reinterpret_cast<const uint8_t *>("123456789"),9)^0xffffffffu)==0xcbf43926);
 for(unsigned pass=0;pass<3;pass++){
   Rom rom;Probe radio(rom);radio.chip.flashSize=rom.flash.size();radio.bslActive=true;
   if(pass==1)std::fill(image.begin()+248,image.begin()+744,0xff); // sparse BIN must not shift later bytes.
   size_t total=arraysTotal;assert(radio.eraseFlash());assert(radio.beginFlash(0,image.size()));
   for(size_t off=0;off<image.size();off+=248){assert(radio.processFlash(image.data()+off,std::min(size_t(248),image.size()-off)));assert(arraysTotal==total && !arraysLive);}
   assert(radio.currentAddr==image.size() && rom.flash==image && rom.remaining==0);
   uint32_t crc=czc::radioCrcUpdate(0xffffffffu,image.data(),image.size())^0xffffffffu;
   assert(radio.verifyFlash(0,image.size(),crc));assert(arraysTotal==total && !arraysLive);
   rom.corruptFlash=true;assert(!radio.verifyFlash(0,image.size(),crc));assert(!strcmp(radio.lastBslError(),"bsl_crc_mismatch"));
 }
 // Stock boot probes still identify the real P7 layout and release every word.
 {Rom rom;Probe radio(rom);radio.bslActive=true;
  for(unsigned i=0;i<30;i++){assert(radio.detectChipInfo());assert(radio.chip.hwRev=="CC2652P7" && radio.chip.flashSize==720896);assert(radio.chip.ieee=="00:12:4B:00:2E:11:42:4D");assert(!arraysLive);}
 }
 // A lost block ACK ends the transfer; no unsafe resend at an unknown ROM offset.
 {Rom rom;Probe radio(rom);radio.chip.flashSize=rom.flash.size();assert(radio.beginFlash(0,248));rom.noAck=true;unsigned long start=now;
  assert(!radio.processFlash(image.data(),248));assert(now-start<=1000 && !radio.currentAddr);assert(!radio.processFlash(image.data(),248));}
 // No response, truncated packet, malformed length/checksum and negative status.
 for(unsigned failure=0;failure<7;failure++){
   Rom rom;Probe radio(rom);radio.chip.flashSize=rom.flash.size();radio.bslActive=true;
   switch(failure){case 0:rom.noAck=true;break;case 1:rom.noPacket=true;break;case 2:rom.shortHeader=true;break;case 3:rom.shortBody=true;break;case 4:rom.badLength=true;break;case 5:rom.badChecksum=true;break;case 6:rom.status=0x44;break;}
   unsigned long start=now;assert(!radio.beginFlash(0,248));assert(now-start<=1000 && !arraysLive);
   assert(strcmp(radio.lastBslError(),"none"));
 }
 // Slow but complete bytes are accepted; trickled replies share one deadline.
 {Rom rom;Probe radio(rom);rom.delayByte=20;assert(radio._checkLastCmd());}
 {Rom rom;Probe radio(rom);rom.delayByte=400;unsigned long start=now;assert(!radio._checkLastCmd());assert(now-start<=1500);}
 puts("PASS CCTools ROM: production library, 3 full 704 KiB writes, zero packet allocations, sparse image offsets, CRC32, malformed/truncated/delayed replies and bounded failures");
}
