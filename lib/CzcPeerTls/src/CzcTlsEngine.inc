// TLS adapter; compiled once per buffer profile.
#include "CzcTlsConfig.h"
#include "CzcTlsInternal.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform_util.h"
#include <string.h>

struct Engine {
    CzcTls base;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int (*send)(void *,const unsigned char *,size_t);
    int (*recv)(void *,unsigned char *,size_t);
    void *io;
    int crypto_acquired;
};
static int send_bio(void *ctx,const unsigned char *p,size_t n) {
    struct Engine *t=ctx; int r=t->send(t->io,p,n);
    return r ? r : MBEDTLS_ERR_SSL_WANT_WRITE;
}
static int recv_bio(void *ctx,unsigned char *p,size_t n) {
    struct Engine *t=ctx; int r=t->recv(t->io,p,n);
    return r ? r : MBEDTLS_ERR_SSL_WANT_READ;
}
static void destroy(CzcTls *base) {
    struct Engine *t=(struct Engine *)base;
    czc_tls_crypto_enter(base);
    mbedtls_ssl_free(&t->ssl); mbedtls_ssl_config_free(&t->conf);
    czc_tls_crypto_cleanup(base); czc_tls_crypto_enter(NULL);
    if(t->crypto_acquired) czc_tls_crypto_release();
    mbedtls_platform_zeroize(t,sizeof(*t)); czc_tls_free(t);
}
static int handshake(CzcTls *base) {
    struct Engine *t=(struct Engine *)base;
    czc_tls_crypto_enter(base);
    int r=mbedtls_ssl_handshake(&t->ssl);
    czc_tls_crypto_enter(NULL);
    if(r && r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE) base->error=r;
    return !r ? 1 : (r==MBEDTLS_ERR_SSL_WANT_READ || r==MBEDTLS_ERR_SSL_WANT_WRITE) ? 0 : -1;
}
static int io_result(CzcTls *base,int r) {
    if(r==MBEDTLS_ERR_SSL_WANT_READ || r==MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if(r<=0) { base->error=r; return -1; }
    return r;
}
static int read_data(CzcTls *base,uint8_t *p,size_t n) {
    czc_tls_crypto_enter(base);
    int r=mbedtls_ssl_read(&((struct Engine *)base)->ssl,p,n);
    czc_tls_crypto_enter(NULL); return io_result(base,r);
}
static int write_data(CzcTls *base,const uint8_t *p,size_t n) {
    czc_tls_crypto_enter(base);
    int r=mbedtls_ssl_write(&((struct Engine *)base)->ssl,p,n>1024?1024:n);
    czc_tls_crypto_enter(NULL); return io_result(base,r);
}
static const struct CzcTlsOps ops={destroy,handshake,read_data,write_data};
#ifdef CZC_TLS_ADMIN
#define CREATE czc_tls_admin_create
#else
#define CREATE czc_tls_peer_create
#endif
CzcTls *CREATE(int server,const uint8_t key[32],const char *identity,
    int (*rng)(void *,unsigned char *,size_t),int (*send)(void *,const unsigned char *,size_t),
    int (*recv)(void *,unsigned char *,size_t),void *io,int *error) {
    if(error) *error=MBEDTLS_ERR_SSL_ALLOC_FAILED;
    struct Engine *t=czc_tls_calloc(1,sizeof(*t)); if(!t) return NULL;
    t->base.ops=&ops; t->send=send; t->recv=recv; t->io=io;
    mbedtls_ssl_init(&t->ssl); mbedtls_ssl_config_init(&t->conf);
    if(czc_tls_crypto_acquire(rng)) {
        if(error) *error=MBEDTLS_ERR_SSL_NO_RNG;
        destroy(&t->base); return NULL;
    }
    t->crypto_acquired=1;
    czc_tls_crypto_enter(&t->base);
    int result=mbedtls_ssl_config_defaults(&t->conf,server?MBEDTLS_SSL_IS_SERVER:MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    if(result) goto fail;
    mbedtls_ssl_conf_rng(&t->conf,rng,NULL);
    result=mbedtls_ssl_conf_psk(&t->conf,key,32,(const unsigned char *)identity,strlen(identity));
    if(result) goto fail;
    result=mbedtls_ssl_setup(&t->ssl,&t->conf);
    if(result) goto fail;
    mbedtls_ssl_set_bio(&t->ssl,t,send_bio,recv_bio,NULL);
    czc_tls_crypto_enter(NULL);
    if(error) *error=0;
    return &t->base;
fail:
    if(error) *error=result;
    destroy(&t->base); return NULL;
}
#undef CREATE
