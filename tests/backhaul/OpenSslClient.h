#pragma once
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cassert>
#include <cstring>

// Independent standards client; deliberately keeps full-size TLS records.
struct OpenSslClient {
    int fd=-1;
    bool ready=false;
    SSL_CTX *context=nullptr;
    SSL *session=nullptr;
    uint8_t key[32];
    const char *identity=nullptr;
    static unsigned psk(SSL *ssl,const char *,char *id,unsigned idLen,unsigned char *key,unsigned keyLen) {
        auto *self=static_cast<OpenSslClient *>(SSL_get_app_data(ssl));
        if(keyLen<32 || idLen<=strlen(self->identity)) return 0;
        strcpy(id,self->identity); memcpy(key,self->key,32); return 32;
    }
    bool start(int socket,bool server,const uint8_t input[32],const char *id) {
        assert(!server); close(); fd=socket; identity=id; memcpy(key,input,32);
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
        context=SSL_CTX_new(TLS_client_method()); assert(context);
        assert(SSL_CTX_set_min_proto_version(context,TLS1_3_VERSION));
        assert(SSL_CTX_set_max_proto_version(context,TLS1_3_VERSION));
        assert(SSL_CTX_set_ciphersuites(context,"TLS_AES_128_GCM_SHA256"));
        assert(SSL_CTX_set1_groups_list(context,"X25519"));
        SSL_CTX_set_verify(context,SSL_VERIFY_NONE,nullptr);
        SSL_CTX_set_psk_client_callback(context,psk);
        session=SSL_new(context); assert(session);
        SSL_set_app_data(session,this); SSL_set_fd(session,fd); SSL_set_connect_state(session);
        return true;
    }
    void close() {
        if(session) SSL_free(session);
        if(context) SSL_CTX_free(context);
        session=nullptr; context=nullptr; ready=false;
        if(fd>=0) { ::shutdown(fd,SHUT_RDWR); ::close(fd); fd=-1; }
    }
    bool pending(int result) {
        int err=SSL_get_error(session,result);
        return err==SSL_ERROR_WANT_READ || err==SSL_ERROR_WANT_WRITE;
    }
    void tick() {
        if(fd<0 || ready) return;
        int r=SSL_do_handshake(session);
        if(r==1) {
            ready=true;
            assert(SSL_version(session)==TLS1_3_VERSION);
            assert(!strcmp(SSL_get_cipher_name(session),"TLS_AES_128_GCM_SHA256"));
        } else if(!pending(r)) close();
    }
    int write(const uint8_t *data,size_t len) {
        int r=SSL_write(session,data,static_cast<int>(len));
        if(r>0) return r;
        if(pending(r)) return 0;
        close(); return -1;
    }
    int read(uint8_t *data,size_t len) {
        int r=SSL_read(session,data,static_cast<int>(len));
        if(r>0) return r;
        if(pending(r)) return 0;
        close(); return -1;
    }
};
