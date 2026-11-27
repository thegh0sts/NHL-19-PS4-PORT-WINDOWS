/* SPDX-License-Identifier: GPL-2.0-or-later
 * NHL 19 "Overtime online" (BB_ONLINE_PROFILE=overtime) in runtime_net.c, without the game:
 *   overtime-net-test <user dir> <account id | signed-out> <code | none>     resolver and the PSN account
 *   ... --https <port> --http <port>                                         + sceHttp, TLS and plain
 * The ShadNet account comes from BB_SHADNET_SERVER / _ID / _PASSWORD as in the game. The servers (a
 * fake ShadNet among them) and the checks of what reached them are in tests/test_overtime_net.py. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
int platform_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    return nanosleep(&t, NULL);
}
void runtime_thread_attach_host(const char *name) { (void)name; }
uintptr_t platform_thread_stack_top(void) { return 0; }
uint64_t platform_monotonic_ns(void) {
#ifdef _WIN32
    return GetTickCount64() * 1000000ull;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}
static const char *user_dir = ".";
const char *runtime_file_user_dir(void) { return user_dir; }

#define GET(type, name) ((type)runtime_net_resolve(name))
typedef int32_t (ABI *Ntoa)(int, const char *, uint32_t *, int, int, int);
typedef int32_t (ABI *AccountId)(int32_t, uint64_t *);
typedef int32_t (ABI *NpState)(int32_t, uint32_t *);
typedef int32_t (ABI *OnlineId)(int32_t, char *);
typedef int32_t (ABI *AuthCode)(int32_t, const void *, char *, int32_t *);
typedef int32_t (ABI *HttpTemplate)(int, const char *, int, int);
typedef int32_t (ABI *HttpConnection)(int, const char *, int);
typedef int32_t (ABI *HttpRequest)(int, const char *, const char *, uint64_t);
typedef int32_t (ABI *HttpHeaderAdd)(int, const char *, const char *, int);
typedef int32_t (ABI *HttpSend)(int, const void *, uint64_t);
typedef int32_t (ABI *HttpStatus)(int, int *);
typedef int32_t (ABI *HttpLength)(int, int *, uint64_t *);
typedef int32_t (ABI *HttpRead)(int, void *, uint64_t);
typedef int32_t (ABI *HttpCallback)(int, void *, void *);
typedef int32_t (ABI *HttpNonblock)(int, int);
typedef int32_t (ABI *Close)(int);
#define EAGAIN_PENDING 0x80431082u

static void resolver(void) {
    Ntoa ntoa = GET(Ntoa, "sceNetResolverStartNtoa");
    uint32_t a = 0;
    assert(ntoa(1, "127.0.0.1", &a, 0, 0, 0) == 0 && a == htonl(0x7f000001));
    a = 0; /* through this PC's resolver, not a fixed server address */
    assert(ntoa(1, "localhost", &a, 0, 0, 0) == 0 && a == htonl(0x7f000001));
    assert((uint32_t)ntoa(1, "no-such-host.invalid", &a, 0, 0, 0) == 0x804101e2);
    puts("resolver OK");
}

#define NP_SIGNED_OUT ((int32_t)0x80550006)
/* The PSN account: ShadNet's account id and the ShadNet ID, a code from ShadNet for the game's own
 * client id and scope (sceNpAuthGetAuthorizationCodeA's parameter), or signed out everywhere. */
static void identity(const char *expected_account, const char *expected_code, char *code) {
    static const char client[136] = "0a1b2c3d-test-client";
    struct { uint64_t size; int32_t user; uint8_t pad[4]; const char *client_id; const char *scope; } param =
        {sizeof param, 1, {0}, client, "psn:s2s"};
    uint32_t state = 0; uint64_t id = 99; char online[20]; int32_t issuer = 0;
    memset(code, 0x55, 136);
    if (!strcmp(expected_account, "signed-out")) {
        assert(GET(NpState, "sceNpGetState")(1, &state) == 0 && state == 1);
        assert(GET(AccountId, "sceNpGetAccountIdA")(1, &id) == NP_SIGNED_OUT && id == 0);
        assert(GET(OnlineId, "sceNpGetOnlineId")(1, online) == NP_SIGNED_OUT);
        assert(GET(AuthCode, "sceNpAuthGetAuthorizationCodeA")(1, &param, code, &issuer) == NP_SIGNED_OUT && !code[0]);
        puts("signed out OK");
        return;
    }
    assert(GET(NpState, "sceNpGetState")(1, &state) == 0 && state == 2);
    assert(GET(AccountId, "sceNpGetAccountIdA")(1, &id) == 0 && id == strtoull(expected_account, NULL, 10));
    memset(online, 0x55, sizeof online);
    assert(GET(OnlineId, "sceNpGetOnlineId")(1, online) == 0 && !strcmp(online, getenv("BB_SHADNET_ID")));
    const int32_t r = GET(AuthCode, "sceNpAuthGetAuthorizationCodeA")(1, &param, code, &issuer);
    if (!strcmp(expected_code, "none")) {
        assert(r != 0 && !code[0]);
        printf("account %llu\nno code (%#x)\n", (unsigned long long)id, (unsigned)r);
        return;
    }
    assert(r == 0 && !strcmp(code, expected_code) && issuer == 256);
    printf("account %llu\ncode %s\n", (unsigned long long)id, code);
}

static int callbacks; static char redirect_location[256];
static ABI int redirect_callback(int id, int status, int *method, const char *location, void *arg) {
    (void)id; (void)method;
    assert(status == 302 && arg == &callbacks);
    snprintf(redirect_location, sizeof redirect_location, "%s", location);
    ++callbacks;
    return -1;
}
typedef struct { int tmpl, conn, req; } Request;
static Request open_request(const char *method, const char *url, int nonblocking) {
    Request r;
    r.tmpl = GET(HttpTemplate, "sceHttpCreateTemplate")(1, "test", 1, 0);
    assert(r.tmpl > 0);
    assert(GET(HttpNonblock, "sceHttpSetNonblock")(r.tmpl, nonblocking) == 0);
    assert(GET(HttpCallback, "sceHttpSetRedirectCallback")(r.tmpl, redirect_callback, &callbacks) == 0);
    r.conn = GET(HttpConnection, "sceHttpCreateConnectionWithURL")(r.tmpl, url, 0);
    r.req = GET(HttpRequest, "sceHttpCreateRequestWithURL2")(r.conn, method, url, 0);
    assert(r.conn > 0 && r.req > 0);
    return r;
}
static void close_request(Request r) {
    assert(GET(Close, "sceHttpDeleteRequest")(r.req) == 0);
    assert(GET(Close, "sceHttpDeleteConnection")(r.conn) == 0);
    assert(GET(Close, "sceHttpDeleteTemplate")(r.tmpl) == 0);
}
static uint64_t read_all(int req, char *out, uint64_t cap) {
    uint64_t used = 0;
    for (;;) {
        const int32_t n = GET(HttpRead, "sceHttpReadData")(req, out + used, cap - used > 65536 ? 65536 : cap - used);
        assert(n >= 0);
        if (n == 0) return used;
        used += (uint64_t)n;
    }
}

static void http(int https_port, int http_port, const char *code) {
    char url[256];
    /* 1. The sign-in: TLS by name (SNI), the game's ticket header kept, EAGAIN while the
     * status is pending (the ProtoHttp block waits for it), the 302 to the game's callback. */
    snprintf(url, sizeof url, "https://localhost:%d/connect/auth?client_id=overtime-nhl19-ps4", https_port);
    Request r = open_request("GET", url, 1);
    assert(GET(HttpHeaderAdd, "sceHttpAddRequestHeader")(r.req, "ps4_ticket", code, 0) == 0);
    callbacks = 0;
    assert((uint32_t)GET(HttpSend, "sceHttpSendRequest")(r.req, NULL, 0) == EAGAIN_PENDING);
    int status = 0;
    assert((uint32_t)GET(HttpStatus, "sceHttpGetStatusCode")(r.req, &status) == EAGAIN_PENDING); /* the server waits 300 ms */
    int32_t result = (int32_t)EAGAIN_PENDING;
    for (int i = 0; i < 10000 && (uint32_t)result == EAGAIN_PENDING; ++i) {
        result = GET(HttpSend, "sceHttpSendRequest")(r.req, NULL, 0);
        if ((uint32_t)result == EAGAIN_PENDING) platform_sleep_ns(1000000);
    }
    assert(result == 0);
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(r.req, &status) == 0 && status == 302);
    assert(callbacks == 1 && !strcmp(redirect_location, "nucleus:rest?code=OTC-test"));
    close_request(r);
    puts("sign-in OK");

    /* 2. A POST to an IP literal (no SNI): one Host, one Content-Length, no X-UFC3 headers. */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/connect/token", https_port);
    r = open_request("POST", url, 0);
    assert(GET(HttpHeaderAdd, "sceHttpAddRequestHeader")(r.req, "Content-Type", "application/x-www-form-urlencoded", 0) == 0);
    assert(GET(HttpHeaderAdd, "sceHttpAddRequestHeader")(r.req, "Host", "wrong.example", 0) == 0);
    assert(GET(HttpHeaderAdd, "sceHttpAddRequestHeader")(r.req, "Content-Length", "999", 0) == 0);
    assert(GET(HttpHeaderAdd, "sceHttpAddRequestHeader")(r.req, "Connection", "keep-alive", 0) == 0);
    const char form[] = "grant_type=authorization_code&code=OTC-test";
    assert(GET(HttpSend, "sceHttpSendRequest")(r.req, form, sizeof form - 1) == 0);
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(r.req, &status) == 0 && status == 200);
    char small[512];
    const uint64_t got = read_all(r.req, small, sizeof small - 1);
    small[got] = 0;
    assert(!strcmp(small, "{\"access_token\":\"AT\"}"));
    close_request(r);
    puts("token OK");

    /* 3. A large answer (a roster) from a server that keeps the connection open: complete at
     * Content-Length, not after the receive timeout. */
    snprintf(url, sizeof url, "https://localhost:%d/big", https_port);
    r = open_request("GET", url, 0);
    const uint64_t started = platform_monotonic_ns();
    assert(GET(HttpSend, "sceHttpSendRequest")(r.req, NULL, 0) == 0);
    const uint64_t took_ms = (platform_monotonic_ns() - started) / 1000000;
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(r.req, &status) == 0 && status == 200);
    int has = -1; uint64_t length = 0;
    const uint64_t big = 3u * 1024 * 1024 + 17;
    assert(GET(HttpLength, "sceHttpGetResponseContentLength")(r.req, &has, &length) == 0 && length == big);
    char *data = malloc(big + 1);
    assert(data && read_all(r.req, data, big + 1) == big);
    for (uint64_t i = 0; i < big; ++i) assert((unsigned char)data[i] == (unsigned char)(i * 7));
    free(data);
    close_request(r);
    printf("big OK (%llu ms)\n", (unsigned long long)took_ms);
    assert(took_ms < 4000);

    /* 4. http:// stays plain HTTP, on the URL's own port. */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/nv2/auth?accesstoken=AT", http_port);
    r = open_request("POST", url, 0);
    assert(GET(HttpSend, "sceHttpSendRequest")(r.req, NULL, 0) == 0);
    assert(GET(HttpStatus, "sceHttpGetStatusCode")(r.req, &status) == 0 && status == 200);
    close_request(r);
    puts("plain OK");

    /* 5. A refused handshake fails the request instead of sending in the clear. */
    snprintf(url, sizeof url, "https://127.0.0.1:%d/connect/auth", http_port);
    r = open_request("GET", url, 0);
    assert((uint32_t)GET(HttpSend, "sceHttpSendRequest")(r.req, NULL, 0) == 0x80431063);
    close_request(r);
    puts("no TLS OK");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 4 && argc != 8) {
        fprintf(stderr, "usage: %s <user dir> <account id | signed-out> <code | none> [--https PORT --http PORT]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    _putenv_s("BB_ONLINE_PROFILE", "overtime");
    _putenv_s("BB_OVERTIME_HOST", "127.0.0.1");
    _putenv_s("BB_NET_LOG", "1");
#else
    setenv("BB_ONLINE_PROFILE", "overtime", 1);
    setenv("BB_OVERTIME_HOST", "127.0.0.1", 1);
    setenv("BB_NET_LOG", "1", 1);
#endif
    user_dir = argv[1];
    assert(runtime_net_overtime());
    resolver();
    char code[136];
    identity(argv[2], argv[3], code);
    if (argc == 8) http(atoi(argv[5]), atoi(argv[7]), code);
    puts("Overtime online checks OK");
    return 0;
}
