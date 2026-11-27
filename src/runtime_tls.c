/* SPDX-License-Identifier: GPL-2.0-or-later
 * NHL 19 "Overtime online": TLS client streams for sceHttp's https:// URLs (runtime_net.c).
 *
 * TLS 1.2 or newer over a connected socket, with SNI when the host is a name. There is no
 * certificate check: the console trusts the game's own CA store and shadPS4 skips the check too;
 * what the game sends there (sign-in, roster and content requests) is what it would send to EA.
 * Windows: Schannel (SSPI, secur32). Linux: the system's OpenSSL 3 (libssl.so.3), loaded at run
 * time so the build and the AppImage need no OpenSSL; without it https:// fails with a log line. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>

struct RuntimeTls {
    SOCKET s;
    CredHandle cred; CtxtHandle ctx; int have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;
    char *in; size_t in_used, in_cap;       /* records received, not yet decrypted */
    char *plain; size_t plain_at, plain_used; /* decrypted bytes not yet read */
    int ended;
};

static int send_all(SOCKET s, const void *data, size_t size) {
    const char *at = data;
    while (size) {
        const int n = send(s, at, size > 65536 ? 65536 : (int)size, 0);
        if (n <= 0) return -1;
        at += n; size -= (size_t)n;
    }
    return 0;
}
/* Appends what the socket has to tls->in: bytes read, 0 at the end, -1 on an error. */
static int receive(RuntimeTls *tls) {
    if (tls->in_used == tls->in_cap) {
        if (tls->in_cap >= (1u << 20)) return -1;
        char *grown = realloc(tls->in, tls->in_cap * 2);
        if (!grown) return -1;
        tls->in = grown; tls->in_cap *= 2;
    }
    const int n = recv(tls->s, tls->in + tls->in_used, (int)(tls->in_cap - tls->in_used), 0);
    if (n > 0) tls->in_used += (size_t)n;
    return n;
}
static void keep_extra(RuntimeTls *tls, const SecBuffer *buffers, int count) {
    for (int i = 0; i < count; ++i)
        if (buffers[i].BufferType == SECBUFFER_EXTRA && buffers[i].cbBuffer) {
            memmove(tls->in, tls->in + tls->in_used - buffers[i].cbBuffer, buffers[i].cbBuffer);
            tls->in_used = buffers[i].cbBuffer;
            return;
        }
    tls->in_used = 0;
}

void runtime_tls_close(RuntimeTls *tls) {
    if (!tls) return;
    if (tls->have_ctx) DeleteSecurityContext(&tls->ctx);
    if (tls->have_cred) FreeCredentialsHandle(&tls->cred);
    free(tls->in); free(tls->plain); free(tls);
}

RuntimeTls *runtime_tls_open(uintptr_t socket, const char *server_name, char *error, size_t error_size) {
    RuntimeTls *tls = calloc(1, sizeof *tls);
    if (!tls || !(tls->in = malloc(tls->in_cap = 65536))) {
        snprintf(error, error_size, "out of memory"); free(tls); return NULL;
    }
    tls->s = (SOCKET)socket;
    SCHANNEL_CRED cred = {.dwVersion = SCHANNEL_CRED_VERSION, .grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT,
                          .dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO};
    SECURITY_STATUS status = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
                                                       &cred, NULL, NULL, &tls->cred, NULL);
    if (status != SEC_E_OK) { snprintf(error, error_size, "Schannel credentials: %#lx", (unsigned long)status); runtime_tls_close(tls); return NULL; }
    tls->have_cred = 1;
    const DWORD want = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                       ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    SEC_CHAR *target = (SEC_CHAR *)(server_name && *server_name ? server_name : NULL);
    DWORD got = 0;
    int first = 1;
    for (;;) {
        SecBuffer in[2] = {{(unsigned long)tls->in_used, SECBUFFER_TOKEN, tls->in}, {0, SECBUFFER_EMPTY, NULL}};
        SecBuffer out[1] = {{0, SECBUFFER_TOKEN, NULL}};
        SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, in}, out_desc = {SECBUFFER_VERSION, 1, out};
        status = InitializeSecurityContextA(&tls->cred, first ? NULL : &tls->ctx, target, want, 0, 0,
                                            first ? NULL : &in_desc, 0, first ? &tls->ctx : NULL, &out_desc, &got, NULL);
        if (first && (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK)) tls->have_ctx = 1;
        first = 0;
        if (out[0].cbBuffer && out[0].pvBuffer) {
            const int sent = send_all(tls->s, out[0].pvBuffer, out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
            if (sent) { snprintf(error, error_size, "handshake send failed"); runtime_tls_close(tls); return NULL; }
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            if (receive(tls) <= 0) { snprintf(error, error_size, "connection closed during the handshake"); runtime_tls_close(tls); return NULL; }
            continue;
        }
        if (status == SEC_E_OK) { keep_extra(tls, in, 2); break; }
        if (status == SEC_I_INCOMPLETE_CREDENTIALS) continue; /* no client certificate: try again without one */
        if (status != SEC_I_CONTINUE_NEEDED) {
            snprintf(error, error_size, "TLS handshake failed: %#lx", (unsigned long)status); runtime_tls_close(tls); return NULL;
        }
        keep_extra(tls, in, 2);
        if (!tls->in_used && receive(tls) <= 0) {
            snprintf(error, error_size, "connection closed during the handshake"); runtime_tls_close(tls); return NULL;
        }
    }
    status = QueryContextAttributesA(&tls->ctx, SECPKG_ATTR_STREAM_SIZES, &tls->sizes);
    if (status != SEC_E_OK || !(tls->plain = malloc(tls->sizes.cbMaximumMessage + 64))) {
        snprintf(error, error_size, "TLS stream sizes: %#lx", (unsigned long)status); runtime_tls_close(tls); return NULL;
    }
    return tls;
}

int runtime_tls_write(RuntimeTls *tls, const void *data, size_t size) {
    const SecPkgContext_StreamSizes *z = &tls->sizes;
    char *record = malloc(z->cbHeader + z->cbMaximumMessage + z->cbTrailer);
    if (!record) return -1;
    const char *at = data;
    while (size) {
        const size_t n = size < z->cbMaximumMessage ? size : z->cbMaximumMessage;
        memcpy(record + z->cbHeader, at, n);
        SecBuffer b[4] = {{z->cbHeader, SECBUFFER_STREAM_HEADER, record},
                          {(unsigned long)n, SECBUFFER_DATA, record + z->cbHeader},
                          {z->cbTrailer, SECBUFFER_STREAM_TRAILER, record + z->cbHeader + n},
                          {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&tls->ctx, 0, &desc, 0) != SEC_E_OK ||
            send_all(tls->s, record, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) { free(record); return -1; }
        at += n; size -= n;
    }
    free(record);
    return 0;
}

long runtime_tls_read(RuntimeTls *tls, void *data, size_t size) {
    for (;;) {
        if (tls->plain_at < tls->plain_used) {
            const size_t n = size < tls->plain_used - tls->plain_at ? size : tls->plain_used - tls->plain_at;
            memcpy(data, tls->plain + tls->plain_at, n);
            tls->plain_at += n;
            return (long)n;
        }
        if (tls->ended) return 0;
        if (tls->in_used) {
            SecBuffer b[4] = {{(unsigned long)tls->in_used, SECBUFFER_DATA, tls->in}, {0, SECBUFFER_EMPTY, NULL},
                              {0, SECBUFFER_EMPTY, NULL}, {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc desc = {SECBUFFER_VERSION, 4, b};
            const SECURITY_STATUS status = DecryptMessage(&tls->ctx, &desc, 0, NULL);
            if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED) {
                tls->plain_at = tls->plain_used = 0;
                for (int i = 0; i < 4; ++i)
                    if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer <= tls->sizes.cbMaximumMessage + 64) {
                        memcpy(tls->plain, b[i].pvBuffer, b[i].cbBuffer);
                        tls->plain_used = b[i].cbBuffer;
                    }
                keep_extra(tls, b, 4);
                if (status == SEC_I_CONTEXT_EXPIRED) tls->ended = 1; /* close_notify */
                continue;
            }
            if (status != SEC_E_INCOMPLETE_MESSAGE) return -1;
        }
        const int n = receive(tls);
        if (n == 0) return 0; /* closed without close_notify: the response ends with the connection */
        if (n < 0) return -1;
    }
}

#else /* Linux: OpenSSL 3, loaded at run time */
#include <dlfcn.h>
#include <pthread.h>

static struct {
    int tried, ok;
    const void *(*client_method)(void);
    void *(*ctx_new)(const void *);
    void (*ctx_free)(void *);
    long (*ctx_ctrl)(void *, int, long, void *);
    uint64_t (*ctx_set_options)(void *, uint64_t);
    void *(*ssl_new)(void *);
    void (*ssl_free)(void *);
    int (*set_fd)(void *, int);
    long (*ssl_ctrl)(void *, int, long, void *);
    int (*connect)(void *);
    int (*write)(void *, const void *, int);
    int (*read)(void *, void *, int);
    int (*get_error)(const void *, int);
} ossl;
static pthread_once_t ossl_once = PTHREAD_ONCE_INIT;
static void ossl_load(void) {
    void *lib = dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
#define SYM(field, name) if (!(*(void **)&ossl.field = dlsym(lib, name))) return
    SYM(client_method, "TLS_client_method"); SYM(ctx_new, "SSL_CTX_new"); SYM(ctx_free, "SSL_CTX_free");
    SYM(ctx_ctrl, "SSL_CTX_ctrl"); SYM(ctx_set_options, "SSL_CTX_set_options"); SYM(ssl_new, "SSL_new");
    SYM(ssl_free, "SSL_free"); SYM(set_fd, "SSL_set_fd"); SYM(ssl_ctrl, "SSL_ctrl"); SYM(connect, "SSL_connect");
    SYM(write, "SSL_write"); SYM(read, "SSL_read"); SYM(get_error, "SSL_get_error");
#undef SYM
    ossl.ok = 1;
}

struct RuntimeTls { void *ctx, *ssl; };

void runtime_tls_close(RuntimeTls *tls) {
    if (!tls) return;
    if (tls->ssl) ossl.ssl_free(tls->ssl);
    if (tls->ctx) ossl.ctx_free(tls->ctx);
    free(tls);
}

RuntimeTls *runtime_tls_open(uintptr_t socket, const char *server_name, char *error, size_t error_size) {
    pthread_once(&ossl_once, ossl_load);
    if (!ossl.ok) { snprintf(error, error_size, "OpenSSL 3 (libssl.so.3) is not installed"); return NULL; }
    RuntimeTls *tls = calloc(1, sizeof *tls);
    if (!tls) { snprintf(error, error_size, "out of memory"); return NULL; }
    if (!(tls->ctx = ossl.ctx_new(ossl.client_method()))) { snprintf(error, error_size, "SSL_CTX_new failed"); runtime_tls_close(tls); return NULL; }
    ossl.ctx_ctrl(tls->ctx, 123 /* SSL_CTRL_SET_MIN_PROTO_VERSION */, 0x0303 /* TLS 1.2 */, NULL);
    ossl.ctx_set_options(tls->ctx, 1u << 7 /* SSL_OP_IGNORE_UNEXPECTED_EOF: the response ends with the connection */);
    if (!(tls->ssl = ossl.ssl_new(tls->ctx)) || ossl.set_fd(tls->ssl, (int)socket) != 1) {
        snprintf(error, error_size, "SSL_new failed"); runtime_tls_close(tls); return NULL;
    }
    if (server_name && *server_name) ossl.ssl_ctrl(tls->ssl, 55 /* SSL_CTRL_SET_TLSEXT_HOSTNAME */, 0, (void *)server_name);
    const int r = ossl.connect(tls->ssl);
    if (r != 1) {
        snprintf(error, error_size, "TLS handshake failed (SSL error %d)", ossl.get_error(tls->ssl, r));
        runtime_tls_close(tls); return NULL;
    }
    return tls;
}

int runtime_tls_write(RuntimeTls *tls, const void *data, size_t size) {
    const char *at = data;
    while (size) {
        const int n = ossl.write(tls->ssl, at, size > 65536 ? 65536 : (int)size);
        if (n <= 0) return -1;
        at += n; size -= (size_t)n;
    }
    return 0;
}

long runtime_tls_read(RuntimeTls *tls, void *data, size_t size) {
    const int n = ossl.read(tls->ssl, data, size > 65536 ? 65536 : (int)size);
    if (n > 0) return n;
    /* SSL_ERROR_ZERO_RETURN: close_notify, or (SSL_OP_IGNORE_UNEXPECTED_EOF) the connection's end. */
    return ossl.get_error(tls->ssl, n) == 6 ? 0 : -1;
}
#endif
