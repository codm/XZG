#pragma once
#include "CzcPeerTls.h"
struct CzcTlsOps {
    void (*destroy)(CzcTls *);
    int (*handshake)(CzcTls *);
    int (*read)(CzcTls *,uint8_t *,size_t);
    int (*write)(CzcTls *,const uint8_t *,size_t);
};
struct CzcTls { const struct CzcTlsOps *ops; int error; uint32_t imported_keys[16]; };
int czc_tls_crypto_acquire(int (*rng)(void *,unsigned char *,size_t));
void czc_tls_crypto_release(void);
void czc_tls_crypto_enter(CzcTls *);
void czc_tls_crypto_cleanup(CzcTls *);
CzcTls *czc_tls_peer_create(int server,const uint8_t key[32],const char *identity,
    int (*rng)(void *,unsigned char *,size_t),
    int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *io,int *error);
#ifdef DEBUG
CzcTls *czc_tls_admin_create(int server,const uint8_t key[32],const char *identity,
    int (*rng)(void *,unsigned char *,size_t),
    int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *io,int *error);
#endif
