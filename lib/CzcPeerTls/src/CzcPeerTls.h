#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CzcTls CzcTls;
CzcTls *czc_tls_new(int server, const uint8_t key[32], const char *identity,
    int (*rng)(void *, unsigned char *, size_t),
    int (*send)(void *, const unsigned char *, size_t),
    int (*recv)(void *, unsigned char *, size_t), void *io);
#ifdef DEBUG
CzcTls *czc_tls_new_admin(int server, const uint8_t key[32], const char *identity,
    int (*rng)(void *, unsigned char *, size_t),
    int (*send)(void *, const unsigned char *, size_t),
    int (*recv)(void *, unsigned char *, size_t), void *io, int *error);
#endif
int czc_tls_error(const CzcTls *tls);
void czc_tls_delete(CzcTls *tls);
// 0: pending, 1: handshake complete, -1: error. IO uses positive bytes or 0 for WANT.
int czc_tls_handshake(CzcTls *tls);
int czc_tls_read(CzcTls *tls, uint8_t *buffer, size_t size);
int czc_tls_write(CzcTls *tls, const uint8_t *buffer, size_t size);
size_t czc_tls_allocated(void);
size_t czc_tls_peak(void);
#ifdef __cplusplus
}
#endif
