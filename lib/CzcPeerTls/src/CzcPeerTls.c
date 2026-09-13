#include "CzcTlsInternal.h"
#include <stdlib.h>
static size_t allocated,peak;
typedef union { size_t size; long double alignment; } Allocation;
void *czc_tls_calloc(size_t n,size_t s) {
    if(s && n>(SIZE_MAX-sizeof(Allocation))/s) return NULL;
    size_t bytes=n*s+sizeof(Allocation);
    Allocation *p=calloc(1,bytes); if(!p) return NULL;
    p->size=bytes; allocated+=bytes; if(allocated>peak) peak=allocated; return p+1;
}
void czc_tls_free(void *ptr) {
    if(ptr) { Allocation *p=(Allocation *)ptr-1; allocated-=p->size; free(p); }
}
size_t czc_tls_allocated(void) { return allocated; }
size_t czc_tls_peak(void) { return peak; }
CzcTls *czc_tls_new(int server,const uint8_t key[32],const char *identity,
    int (*rng)(void *,unsigned char *,size_t),int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *io) {
    return czc_tls_peer_create(server,key,identity,rng,send,recv,io,NULL);
}
#ifdef DEBUG
CzcTls *czc_tls_new_admin(int server,const uint8_t key[32],const char *identity,
    int (*rng)(void *,unsigned char *,size_t),int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *io,int *error) {
    return czc_tls_admin_create(server,key,identity,rng,send,recv,io,error);
}
#endif
void czc_tls_delete(CzcTls *t) { if(t) t->ops->destroy(t); }
int czc_tls_error(const CzcTls *t) { return t ? t->error : -0x7f00; }
int czc_tls_handshake(CzcTls *t) { return t->ops->handshake(t); }
int czc_tls_read(CzcTls *t,uint8_t *p,size_t n) { return t->ops->read(t,p,n); }
int czc_tls_write(CzcTls *t,const uint8_t *p,size_t n) { return t->ops->write(t,p,n); }
