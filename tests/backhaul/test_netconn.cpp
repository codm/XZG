#include "netconn/fixture.h"
int main(){
 tcp_pcb pcb[8];netconn conn[8];NetconnStream stream[8];
 for(unsigned i=0;i<8;i++){conn[i].pcb.tcp=&pcb[i];assert(stream[i].start(&conn[i]));assert(conn[i].nonblocking&&pcb[i].nodelay);}
 uint8_t data[16]{};assert(stream[0].read(data,16)==0);
 conn[0].input=new pbuf{6,{1,2,3,4,5,6}};conn[0].readResult=ERR_OK;
 assert(stream[0].read(data,2)==2&&data[0]==1&&!frees);
 assert(stream[0].read(data,16)==4&&data[0]==3&&data[3]==6&&frees==1);
 conn[0].readResult=ERR_CLSD;assert(stream[0].read(data,16)==-1);
 for(auto err:{ERR_WOULDBLOCK,ERR_MEM,ERR_INPROGRESS}){conn[1].writeResult=err;assert(stream[1].write(data,16)==0);}
 conn[1].writeCount=3;assert(stream[1].write(data,16)==3);conn[1].writeCount=0;conn[1].writeResult=ERR_CLSD;assert(stream[1].write(data,16)==-1);
 conn[2].input=new pbuf{6,{1,2,3,4,5,6}};conn[2].readResult=ERR_OK;assert(stream[2].read(data,1)==1);
 stream[2].close();assert(frees==2&&pcb[2].aborted&&conn[2].deleted==1);
 for(unsigned i=0;i<8;i++)if(i!=2)assert(!pcb[i].aborted&&conn[i].deleted==0);
 conn[3].family=AF_INET;assert(stream[3].addressFamily()==AF_INET);
 char ip[64];assert(stream[3].peerAddress(ip,sizeof(ip)) && !strcmp(ip,"192.168.8.186"));
 assert(stream[0].peerAddress(ip,sizeof(ip)) && !strcmp(ip,"fd12::186"));
 assert(!stream[2].peerAddress(ip,sizeof(ip)) && !ip[0]);
 for(auto &s:stream)s.close();
 assert(coreCalls==16);
 for(auto &c:conn)assert(c.deleted==1);
 puts("PASS netconn adapter: eight channels without BSD descriptors, core-thread TCP options/abort, partial reads/writes, backpressure, EOF, per-channel cleanup");
}
