#pragma once
#include <stddef.h>
#include "CzcTlsSymbols.h"
void *czc_tls_calloc(size_t count, size_t size);
void czc_tls_free(void *pointer);
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO czc_tls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO czc_tls_free
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_USE_PSA_CRYPTO
#define MBEDTLS_PSA_KEY_SLOT_COUNT 128
// These buffers belong to the single TLS task; no untrusted concurrent writer.
#define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_CIPHERSUITES MBEDTLS_TLS1_3_AES_128_GCM_SHA256
#define MBEDTLS_SSL_OUT_CONTENT_LEN 1024
#define MBEDTLS_PSK_MAX_LEN 32
#ifdef CZC_TLS_ADMIN
// Standard PSK clients may send full-size records.
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#else
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_RECORD_SIZE_LIMIT
#define MBEDTLS_SSL_IN_CONTENT_LEN 1024
#endif
