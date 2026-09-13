#include "CzcTlsConfig.h"
#include "CzcTlsInternal.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"

// The gateway owns all TLS engines on one task. PSA uses the same application
// DRBG callback as TLS; it never opens a second hardware entropy source.
static unsigned users;
static int (*random_source)(void *,unsigned char *,size_t);
static CzcTls *owner;
void czc_tls_crypto_enter(CzcTls *tls) { owner=tls; }
// Track TLS imports through the PSA API. Mbed TLS 3.6.7 can abandon an
// already imported encryption key if importing the decryption key fails.
// Reclaim this session's leftover keys without disturbing other live sessions.
psa_status_t czc_tls_guard_psa_import_key(const psa_key_attributes_t *attributes,
    const uint8_t *data,size_t size,mbedtls_svc_key_id_t *key) {
    if(!owner) return PSA_ERROR_BAD_STATE;
    for(size_t i=0;i<sizeof(owner->imported_keys)/sizeof(owner->imported_keys[0]);++i) {
        if(owner->imported_keys[i]) continue;
        psa_status_t status=psa_import_key(attributes,data,size,key);
        if(status==PSA_SUCCESS) owner->imported_keys[i]=*key;
        return status;
    }
    return PSA_ERROR_INSUFFICIENT_MEMORY;
}
psa_status_t czc_tls_guard_psa_destroy_key(mbedtls_svc_key_id_t key) {
    psa_status_t status=psa_destroy_key(key);
    if(owner && (status==PSA_SUCCESS || status==PSA_ERROR_INVALID_HANDLE)) {
        for(size_t i=0;i<sizeof(owner->imported_keys)/sizeof(owner->imported_keys[0]);++i)
            if(owner->imported_keys[i]==key) owner->imported_keys[i]=0;
    }
    return status;
}
void czc_tls_crypto_cleanup(CzcTls *tls) {
    for(size_t i=0;i<sizeof(tls->imported_keys)/sizeof(tls->imported_keys[0]);++i) {
        if(tls->imported_keys[i]) psa_destroy_key(tls->imported_keys[i]);
        tls->imported_keys[i]=0;
    }
}
int czc_tls_crypto_acquire(int (*rng)(void *,unsigned char *,size_t)) {
    if(!rng || (users && rng!=random_source)) return -1;
    random_source=rng; ++users; return 0;
}
void czc_tls_crypto_release(void) {
    if(--users==0) { mbedtls_psa_crypto_free(); random_source=NULL; }
}
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *context,
    uint8_t *output,size_t size,size_t *length) {
    (void)context; *length=0;
    if(!random_source || random_source(NULL,output,size)) return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *length=size; return PSA_SUCCESS;
}
