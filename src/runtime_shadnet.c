/* SPDX-License-Identifier: GPL-2.0-or-later
 * NHL 19 "Overtime online": the player's own ShadNet account, the PSN service shadPS4's players sign
 * in to (srv.shadps4.net:31313). runtime_net.c gives the game what it gets on shadPS4: the ShadNet
 * account id as its PSN account id, the ShadNet ID as its online id, and authorization codes that
 * ShadNet issues. Only the sign-in and the codes; friends, rooms, scores and trophies stay out.
 *
 * Wire format as shadPS4's src/shadnet (client.h, shadnet.proto; protocol 1): a 15-byte
 * little-endian header {u8 type, u16 command, u32 size including the header, u64 id}; types
 * 0 request, 1 reply, 2 notification, 3 server info (u32 protocol version, sent first by the
 * server). A request carries a u32 length and a protobuf message; a reply starts with an error
 * byte, then the same; a notification is the length and message. The connection stays open for
 * the whole run: a reader thread takes every packet, hands a reply to the request waiting for it
 * and reports the notifications that sign the client out. Passwords and tokens are never logged. */
#define _GNU_SOURCE
#include "runtime.h"
#include "build_version.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ShadSocket;
#define SHAD_BAD INVALID_SOCKET
#define shad_close closesocket
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int ShadSocket;
#define SHAD_BAD (-1)
#define shad_close close
#endif
#ifdef MSG_NOSIGNAL
#define SHAD_SEND_FLAGS MSG_NOSIGNAL /* a closed connection fails the send instead of raising SIGPIPE */
#else
#define SHAD_SEND_FLAGS 0
#endif

/* SIGN_IN_MS caps the whole sign-in (connect, the server's version, Login): the game's PSN calls
 * wait for it, so an unreachable server costs the start-up at most this long. */
enum { HEADER = 15, MAX_PACKET = 0x800000, PROTOCOL = 1, SIGN_IN_MS = 12000, REPLY_MS = 15000, VERSION_MS = 3000,
       CLOSE_MS = 5000 };
enum { T_REQUEST = 0, T_REPLY = 1, T_NOTIFICATION = 2, T_SERVER_INFO = 3 };
enum { C_LOGIN = 0, C_SET_CLIENT_VERSION = 13, C_GET_AUTHORIZATION_CODE = 42 };
enum { N_SERVICE_UNAVAILABLE = 18, N_CLIENT_REJECTED = 19 };
#define CLIENT_VERSION "SacredFire NHL 19 port " BB_BUILD_VERSION " (Overtime online)"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* everything below but the socket's sends */
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER; /* one request in flight */
static ShadSocket sock = SHAD_BAD;
static int connected;  /* the reader runs on sock */
static int generation; /* a new connection each sign-in; an old reader only cleans up its own */
static uint64_t next_id = 1, waiting_id;
static int have_reply;
static uint8_t *reply;
static size_t reply_size;
/* ClientRejected: the server's own words, and no new sign-in for the rest of the session. */
static char rejected[512];
static int refused;

/* ---- bytes ---- */
static void put_le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_le(const uint8_t *p, int n) { uint64_t v = 0; for (int i = n - 1; i >= 0; --i) v = v << 8 | p[i]; return v; }

typedef struct { uint8_t *data; size_t size, cap; int failed; } Buf;
static void put(Buf *b, const void *p, size_t n) {
    if (b->failed || !n) return;
    if (b->size + n > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->size + n) cap *= 2;
        uint8_t *data = realloc(b->data, cap);
        if (!data) { b->failed = 1; return; }
        b->data = data; b->cap = cap;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
}
static void put_varint(Buf *b, uint64_t v) {
    uint8_t t[10]; size_t n = 0;
    do { t[n] = (uint8_t)(v & 0x7f); v >>= 7; if (v) t[n] |= 0x80; ++n; } while (v);
    put(b, t, n);
}
/* A proto3 string field; an empty one is left out, as protobuf itself does. */
static void put_string(Buf *b, unsigned field, const char *s) {
    if (!s || !*s) return;
    const size_t n = strlen(s);
    put_varint(b, (uint64_t)field << 3 | 2); put_varint(b, n); put(b, s, n);
}

typedef struct { const uint8_t *p, *end; } Fields;
static int get_varint(Fields *r, uint64_t *v) {
    uint64_t x = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return 0;
        const uint8_t c = *r->p++;
        x |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) { *v = x; return 1; }
    }
    return 0;
}
/* The next varint or length-delimited field (others are skipped): 1, 0 at the end, -1 if damaged. */
static int next_field(Fields *r, unsigned *field, uint64_t *value, const uint8_t **bytes, size_t *size) {
    for (;;) {
        uint64_t key, n;
        if (r->p >= r->end) return 0;
        if (!get_varint(r, &key)) return -1;
        *field = (unsigned)(key >> 3);
        switch (key & 7) {
        case 0: *bytes = NULL; *size = 0; return get_varint(r, value) ? 1 : -1;
        case 2:
            if (!get_varint(r, &n) || n > (uint64_t)(r->end - r->p)) return -1;
            *bytes = r->p; *size = (size_t)n; *value = n; r->p += n; return 1;
        case 1: if (r->end - r->p < 8) return -1; r->p += 8; break;
        case 5: if (r->end - r->p < 4) return -1; r->p += 4; break;
        default: return -1;
        }
    }
}
/* A u32-length-prefixed message at p (size bytes): its fields, or 0 if it does not fit. */
static int message_at(const uint8_t *p, size_t size, Fields *out) {
    if (size < 4) return 0;
    const uint64_t n = get_le(p, 4);
    if (n > size - 4) return 0;
    out->p = p + 4; out->end = p + 4 + n;
    return 1;
}
static void copy_text(char *out, size_t cap, const uint8_t *p, size_t n) {
    if (!cap) return;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) out[i] = p[i] >= 0x20 ? (char)p[i] : ' '; /* no control bytes in the log */
    out[n] = 0;
}

/* ---- the connection ---- */
static int send_all(ShadSocket s, const uint8_t *p, size_t n) {
    while (n) {
        const int r = send(s, (const char *)p, n > 65536 ? 65536 : (int)n, SHAD_SEND_FLAGS);
        if (r <= 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}
static int recv_all(ShadSocket s, uint8_t *p, size_t n) {
    while (n) {
        const int r = recv(s, (char *)p, n > 65536 ? 65536 : (int)n, 0);
        if (r <= 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}
static void set_receive_timeout(ShadSocket s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
}
static int send_request(uint16_t command, uint64_t id, const Buf *message) {
    const size_t body = message ? message->size : 0, size = HEADER + 4 + body;
    uint8_t *packet = malloc(size);
    if (!packet) return 0;
    packet[0] = T_REQUEST;
    put_le(packet + 1, command, 2); put_le(packet + 3, size, 4); put_le(packet + 7, id, 8);
    put_le(packet + HEADER, body, 4);
    if (body) memcpy(packet + HEADER + 4, message->data, body);
    pthread_mutex_lock(&lock);
    const ShadSocket s = connected ? sock : SHAD_BAD;
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&send_lock);
    const int ok = s != SHAD_BAD && send_all(s, packet, size);
    pthread_mutex_unlock(&send_lock);
    free(packet);
    return ok;
}
static void notification(uint16_t type, const uint8_t *p, size_t n) {
    Fields f; unsigned field; uint64_t v; const uint8_t *bytes; size_t size;
    if (type == N_CLIENT_REJECTED && message_at(p, n, &f)) {
        uint64_t reason = 0; char message[256] = "", version[128] = ""; /* the server echoes the version it was sent */
        while (next_field(&f, &field, &v, &bytes, &size) > 0) {
            if (field == 1 && !bytes) reason = v;
            else if (field == 2 && bytes) copy_text(message, sizeof message, bytes, size);
            else if (field == 3 && bytes) copy_text(version, sizeof version, bytes, size);
        }
        pthread_mutex_lock(&lock);
        snprintf(rejected, sizeof rejected, "ShadNet signed this client out (reason %llu%s%s): %s", (unsigned long long)reason,
                 version[0] ? ", version " : "", version, message[0] ? message : "no reason given");
        refused = 1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
        printf("ShadNet: the server signed this client out (reason %llu, version \"%s\"): %s\n", (unsigned long long)reason, version, message);
    } else if (type == N_SERVICE_UNAVAILABLE) {
        printf("ShadNet: the server refused a service (notification 18)\n");
    }
}
static void *reader(void *arg) {
    const int mine = (int)(intptr_t)arg;
    pthread_mutex_lock(&lock);
    const ShadSocket s = sock;
    pthread_mutex_unlock(&lock);
    for (;;) {
        uint8_t h[HEADER];
        if (!recv_all(s, h, HEADER)) break;
        const uint64_t total = get_le(h + 3, 4);
        if (total < HEADER || total > MAX_PACKET) { printf("ShadNet: a damaged packet (%llu bytes): disconnecting\n", (unsigned long long)total); break; }
        const size_t n = (size_t)total - HEADER;
        uint8_t *p = n ? malloc(n) : NULL;
        if (n && (!p || !recv_all(s, p, n))) { free(p); break; }
        if (h[0] == T_REPLY) {
            pthread_mutex_lock(&lock);
            if (mine == generation && waiting_id && get_le(h + 7, 8) == waiting_id && !have_reply) {
                reply = p; reply_size = n; have_reply = 1; p = NULL;
                pthread_cond_broadcast(&changed);
            }
            pthread_mutex_unlock(&lock);
        } else if (h[0] == T_NOTIFICATION) notification((uint16_t)get_le(h + 1, 2), p, n);
        free(p);
    }
    pthread_mutex_lock(&lock);
    const int current = mine == generation && connected;
    if (current) { connected = 0; shad_close(sock); sock = SHAD_BAD; pthread_cond_broadcast(&changed); }
    pthread_mutex_unlock(&lock);
    if (current) printf("ShadNet: disconnected\n");
    return NULL;
}
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int left_ms(uint64_t until) { const uint64_t now = now_ms(); return now + 1 >= until ? 1 : (int)(until - now); }
static struct timespec deadline(int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec += 1; t.tv_nsec -= 1000000000L; }
    return t;
}
/* Sends a request and waits up to ms for its reply: 1 with the reply's error byte and message (body,
 * to be freed), 0 when the connection is gone, the server refused this client or the reply did not come. */
static int call_within(uint16_t command, const Buf *message, int *error, uint8_t **body, size_t *body_size, int ms) {
    *error = -1; *body = NULL; *body_size = 0;
    if (message && message->failed) return 0;
    pthread_mutex_lock(&request_lock);
    pthread_mutex_lock(&lock);
    const uint64_t id = next_id++;
    waiting_id = id; have_reply = 0; free(reply); reply = NULL;
    pthread_mutex_unlock(&lock);
    int ok = 0;
    if (send_request(command, id, message)) {
        pthread_mutex_lock(&lock);
        const struct timespec until = deadline(ms);
        while (connected && !have_reply && !refused)
            if (pthread_cond_timedwait(&changed, &lock, &until) == ETIMEDOUT) break;
        if (have_reply && reply_size >= 1) {
            ok = 1;
            *error = reply[0];
            *body = reply; *body_size = reply_size; /* the message starts at body + 1 */
            reply = NULL;
        }
        have_reply = 0; waiting_id = 0;
        pthread_mutex_unlock(&lock);
    } else {
        /* A failed send: the connection is gone. Close it and wait for the reader, so the caller
         * sees "not connected" and can sign in again. */
        pthread_mutex_lock(&lock);
        waiting_id = 0;
        if (connected && sock != SHAD_BAD) shutdown(sock, 2);
        const struct timespec until = deadline(CLOSE_MS);
        while (connected)
            if (pthread_cond_timedwait(&changed, &lock, &until) == ETIMEDOUT) break;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_unlock(&request_lock);
    return ok;
}
static int call(uint16_t command, const Buf *message, int *error, uint8_t **body, size_t *body_size) {
    return call_within(command, message, error, body, body_size, REPLY_MS);
}
static void disconnect(void) {
    pthread_mutex_lock(&lock);
    if (connected && sock != SHAD_BAD) shutdown(sock, 2); /* the reader sees it and closes */
    pthread_mutex_unlock(&lock);
}
static ShadSocket connect_to(const char *host, const char *port, uint64_t until, char *error, size_t error_size) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *list = NULL;
    const int lookup = getaddrinfo(host, port, &hints, &list);
    if (lookup || !list) { snprintf(error, error_size, "cannot find %s (DNS)", host); return SHAD_BAD; }
    ShadSocket s = SHAD_BAD;
    for (struct addrinfo *a = list; a && s == SHAD_BAD; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == SHAD_BAD) continue;
        /* connect with a time limit: non-blocking, then wait for it to become writable */
#ifdef _WIN32
        u_long on = 1; ioctlsocket(s, FIONBIO, &on);
        int r = connect(s, a->ai_addr, (int)a->ai_addrlen);
        int pending = r && WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const int flags = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int r = connect(s, a->ai_addr, a->ai_addrlen);
        int pending = r && errno == EINPROGRESS;
#endif
        if (pending) {
            fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(s, &w); FD_SET(s, &x);
            const int ms = left_ms(until);
            struct timeval t = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
            int soerr = 0; socklen_t len = sizeof soerr;
            r = select((int)s + 1, NULL, &w, &x, &t) == 1 && FD_ISSET(s, &w) &&
                !getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) && !soerr ? 0 : -1;
        }
        if (r) { shad_close(s); s = SHAD_BAD; continue; }
#ifdef _WIN32
        on = 0; ioctlsocket(s, FIONBIO, &on);
#else
        fcntl(s, F_SETFL, flags);
#endif
    }
    freeaddrinfo(list);
    if (s == SHAD_BAD) { snprintf(error, error_size, "cannot reach %s:%s", host, port); return SHAD_BAD; }
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof one);
    return s;
}
static const char *login_error(int e) {
    switch (e) {
    case 4: return "too many sign-ins just now: try again in a minute";
    case 6: return "this ShadNet account is signed in somewhere else (shadPS4 or another copy of the game): sign it out there first";
    case 7: return "there is no ShadNet account with this ID";
    case 8: return "wrong ShadNet password";
    case 9: return "this ShadNet account needs its activation token (from its e-mail)";
    case 5: return "ShadNet refused the sign-in";
    default: return NULL;
    }
}

int runtime_shadnet_sign_in(const char *server, const char *npid, const char *password, const char *token,
                            const char *title_id, const char *title_name, uint64_t *account_id, char *error, size_t error_size) {
    *account_id = 0;
    char host[256], port[16] = "31313";
    snprintf(host, sizeof host, "%s", server ? server : "");
    char *colon = strrchr(host, ':');
    if (colon && !strchr(host, ']')) { snprintf(port, sizeof port, "%s", colon + 1); *colon = 0; }
    if (!host[0]) { snprintf(error, error_size, "no ShadNet server"); return 0; }
    pthread_mutex_lock(&lock);
    const int was_refused = refused;
    if (was_refused) snprintf(error, error_size, "%s", rejected);
    pthread_mutex_unlock(&lock);
    if (was_refused) return 0;
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    disconnect();
    const uint64_t until = now_ms() + SIGN_IN_MS;
    ShadSocket s = connect_to(host, port, until, error, error_size);
    if (s == SHAD_BAD) return 0;
    /* The server speaks first: its protocol version. */
    set_receive_timeout(s, left_ms(until));
    uint8_t h[HEADER], info[64];
    uint64_t total = 0;
    if (!recv_all(s, h, HEADER) || h[0] != T_SERVER_INFO || (total = get_le(h + 3, 4)) < HEADER || total > HEADER + sizeof info ||
        !recv_all(s, info, (size_t)total - HEADER)) {
        shad_close(s);
        snprintf(error, error_size, "%s:%s did not answer as a ShadNet server", host, port);
        return 0;
    }
    if (total - HEADER >= 4 && get_le(info, 4) != PROTOCOL) {
        shad_close(s);
        snprintf(error, error_size, "the ShadNet server speaks protocol %llu, this build %d: update the game",
                 (unsigned long long)get_le(info, 4), PROTOCOL);
        return 0;
    }
    set_receive_timeout(s, 0);
    pthread_mutex_lock(&lock);
    while (connected) pthread_cond_wait(&changed, &lock); /* an earlier connection's reader is closing */
    sock = s; connected = 1;
    const int mine = ++generation;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, reader, (void *)(intptr_t)mine)) {
        pthread_mutex_lock(&lock); connected = 0; sock = SHAD_BAD; pthread_mutex_unlock(&lock);
        shad_close(s);
        snprintf(error, error_size, "no thread for the connection");
        return 0;
    }
    pthread_detach(t);

    Buf login = {0};
    put_string(&login, 1, npid); put_string(&login, 2, password); put_string(&login, 3, token);
    put_string(&login, 4, title_id); put_string(&login, 5, title_name);
    int e; uint8_t *body; size_t size;
    const int answered = call_within(C_LOGIN, &login, &e, &body, &size, left_ms(until));
    free(login.data);
    uint64_t user = 0;
    if (answered && e == 0) {
        Fields f; unsigned field; uint64_t v; const uint8_t *bytes; size_t n;
        if (message_at(body + 1, size - 1, &f))
            while (next_field(&f, &field, &v, &bytes, &n) > 0)
                if (field == 2 && !bytes) user = v;
    }
    free(body);
    if (!answered || e != 0 || !user) {
        if (!answered) {
            pthread_mutex_lock(&lock);
            snprintf(error, error_size, "%s", refused ? rejected : "ShadNet did not answer the sign-in");
            pthread_mutex_unlock(&lock);
        } else if (e != 0) {
            const char *why = login_error(e);
            if (why) snprintf(error, error_size, "%s", why); else snprintf(error, error_size, "ShadNet error %d", e);
        } else snprintf(error, error_size, "ShadNet's sign-in answer has no account id");
        disconnect();
        return 0;
    }
    /* Names this client to the server, as shadPS4 does after its sign-in. Its answer is awaited
     * briefly, so a server that refuses this client is seen here, before the game is told it is
     * signed in. */
    Buf version = {0};
    put_string(&version, 1, CLIENT_VERSION);
    call_within(C_SET_CLIENT_VERSION, &version, &e, &body, &size, VERSION_MS);
    free(version.data);
    free(body);
    pthread_mutex_lock(&lock);
    const int now_refused = refused;
    if (now_refused) snprintf(error, error_size, "%s", rejected);
    pthread_mutex_unlock(&lock);
    if (now_refused) { disconnect(); return 0; }
    *account_id = user;
    return 1;
}

int runtime_shadnet_refused(void) {
    pthread_mutex_lock(&lock);
    const int r = refused;
    pthread_mutex_unlock(&lock);
    return r;
}

int runtime_shadnet_connected(void) {
    pthread_mutex_lock(&lock);
    const int up = connected;
    pthread_mutex_unlock(&lock);
    return up;
}

int runtime_shadnet_auth_code(const char *client_id, const char *scope, char *code, size_t code_size, int *shadnet_error,
                              char *error, size_t error_size) {
    *shadnet_error = -1;
    if (code_size) code[0] = 0;
    pthread_mutex_lock(&lock);
    const int was_refused = refused;
    if (was_refused) { snprintf(error, error_size, "%s", rejected); *shadnet_error = -3; }
    pthread_mutex_unlock(&lock);
    if (was_refused) return 0;
    Buf request = {0};
    put_string(&request, 1, client_id); put_string(&request, 2, scope);
    int e; uint8_t *body; size_t size;
    const int answered = call(C_GET_AUTHORIZATION_CODE, &request, &e, &body, &size);
    free(request.data);
    int ok = 0;
    if (!answered) {
        pthread_mutex_lock(&lock);
        if (rejected[0]) { snprintf(error, error_size, "%s", rejected); *shadnet_error = -3; }
        else snprintf(error, error_size, "%s", connected ? "ShadNet did not answer" : "not connected to ShadNet");
        pthread_mutex_unlock(&lock);
    } else if (e != 0) {
        *shadnet_error = e;
        snprintf(error, error_size, "ShadNet error %d", e);
    } else {
        Fields f; unsigned field; uint64_t v; const uint8_t *bytes; size_t n;
        if (message_at(body + 1, size - 1, &f))
            while (next_field(&f, &field, &v, &bytes, &n) > 0)
                if (field == 1 && bytes) {
                    if (n && n < code_size) { memcpy(code, bytes, n); code[n] = 0; ok = 1; }
                    break;
                }
        if (!ok) { *shadnet_error = -2; snprintf(error, error_size, "ShadNet's answer has no usable code"); }
        else *shadnet_error = 0;
    }
    free(body);
    return ok;
}

void runtime_shadnet_close(void) { disconnect(); }
