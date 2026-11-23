/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "build_version.h"
/* Referenced by --version so LTO retains this searchable, NUL-terminated identity. */
static const char game_build_identity[] __attribute__((used)) = BB_GAME_VERSION_MARKER BB_BUILD_VERSION;
#include "gpu/bbgpu.h"
#ifndef _WIN32
/* Releases before the detector lack this export; still report a mixed installation. */
extern const char *bbgpu_version(void) __attribute__((weak));
#endif
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#include <unistd.h> /* _exit */
/* Host services (signals or vectored exceptions, stacks, memory, restart): src/platform. */

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it.
 * Exits with code 24 so the launchers can name the cause (GitHub #21). */
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. The game's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(24); /* a code of its own: the launchers explain it (GitHub #21) */
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
/* Host memory the guest sees (image, trampolines): below 1 TiB (40-bit guest pointers). */
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
    void *p = platform_map_anonymous(size, PROT_READ | PROT_WRITE);
    if (!p) fail("host memory allocation failed");
    return p;
}
/* Stubs the image jumps to with rel32: low memory, mapped after the image (runtime_low_map). */
static void *map_near_image(size_t size) { return runtime_low_map(size, PROT_READ | PROT_WRITE); }
/* ELF segment flags (1 execute, 2 write, 4 read) to a host protection. */
static void protect(void *p, size_t size, unsigned flags) {
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (platform_protect(p, size, mode)) fail("changing page protection failed");
}
/* BB_TRACE_CALLS=1: every PS4 system function the runtime implements is reached through a stub
 * that logs the call (its name and how often it has been called), to see what the game polls or
 * waits on. The first BB_TRACE_FIRST calls of each (default 20) are logged, then each power of
 * two. The stub keeps every argument register (rdi..r9, rax, xmm0-7) and jumps to the function. */
static uint32_t *trace_counts;
static uint32_t trace_first = 20;
static uint64_t trace_start_ns;
static unsigned char *trace_stubs;
enum { TRACE_STUB = 192 }; /* 166 bytes used */
static ABI void trace_call(uint32_t id) {
    const uint32_t n = __atomic_add_fetch(&trace_counts[id], 1, __ATOMIC_RELAXED);
    if (n > trace_first && (n & (n - 1))) return;
    const char *api = runtime_import_name(names[id]);
    printf("Trace %8.3f s: %s #%u\n", (double)(platform_monotonic_ns() - trace_start_ns) / 1e9,
           api ? api : names[id], n);
}
static uintptr_t trace_stub(uint32_t id, uintptr_t target) {
    unsigned char *t = trace_stubs + (size_t)id * TRACE_STUB, *p = t;
    static const unsigned char save[] = {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50, 0x41, 0x52, 0x41, 0x53,
                                         0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}; /* push x9 (rsp 16-aligned), sub rsp,128 */
    memcpy(p, save, sizeof save); p += sizeof save;
    for (int i = 0; i < 8; ++i) { *p++ = 0xf3; *p++ = 0x0f; *p++ = 0x7f; *p++ = (unsigned char)(0x44 | i << 3); *p++ = 0x24; *p++ = (unsigned char)(16 * i); }
    *p++ = 0xbf; memcpy(p, &id, 4); p += 4;                                   /* mov edi, id */
    const uintptr_t logger = (uintptr_t)trace_call;
    *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8;                  /* movabs rax, trace_call */
    *p++ = 0xff; *p++ = 0xd0;                                                 /* call rax */
    for (int i = 0; i < 8; ++i) { *p++ = 0xf3; *p++ = 0x0f; *p++ = 0x6f; *p++ = (unsigned char)(0x44 | i << 3); *p++ = 0x24; *p++ = (unsigned char)(16 * i); }
    static const unsigned char restore[] = {0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,               /* add rsp,128 */
                                            0x41, 0x5b, 0x41, 0x5a, 0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f};
    memcpy(p, restore, sizeof restore); p += sizeof restore;
    *p++ = 0x49; *p++ = 0xbb; memcpy(p, &target, 8); p += 8;                  /* movabs r11, target */
    *p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;                                    /* jmp r11 */
    if (p - t > TRACE_STUB) fail("trace stub too long");
    return (uintptr_t)t;
}
/* BB_GAME_LOG=1 (NHL 19): the game's own debug messages, printed as "Game log: ...". Its EA
 * libraries format every message with one function (text 0x45c1750: string, format, va_list);
 * retail builds then throw the text away. Its entry jumps to a thunk that prints the message and
 * then runs the 20 moved prologue bytes and continues, so the game is unchanged. The va_list is
 * the guest's System V one: gp_offset, fp_offset, overflow area, register save area. */
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes);
typedef struct { uint32_t gp, fp; unsigned char *overflow, *save; } GuestVaList;
static uint64_t game_va_int(GuestVaList *v) {
    uint64_t x;
    if (v->gp < 48) { memcpy(&x, v->save + v->gp, 8); v->gp += 8; }
    else { memcpy(&x, v->overflow, 8); v->overflow += 8; }
    return x;
}
static double game_va_double(GuestVaList *v) {
    double x;
    if (v->fp < 176) { memcpy(&x, v->save + v->fp, 8); v->fp += 16; }
    else { memcpy(&x, v->overflow, 8); v->overflow += 8; }
    return x;
}
static ABI void game_log(const char *fmt, const GuestVaList *va) {
    if (!fmt || (uintptr_t)fmt < 0x10000) return;
    GuestVaList v = *va;
    char out[1024], spec[32];
    size_t n = 0;
    for (const char *f = fmt; *f && n < sizeof out - 1;) {
        if (*f != '%') { out[n++] = *f++; continue; }
        const char *start = f++;
        if (*f == '%') { out[n++] = '%'; ++f; continue; }
        int star_w = 0, star_p = 0, length = 0; /* 1 h, 2 hh, 3 l/ll/z/j/t/q */
        while (*f && strchr("-+ #0", *f)) ++f;
        if (*f == '*') { star_w = 1; ++f; } else while (*f >= '0' && *f <= '9') ++f;
        if (*f == '.') { ++f; if (*f == '*') { star_p = 1; ++f; } else while (*f >= '0' && *f <= '9') ++f; }
        for (;; ++f) {
            if (*f == 'h') length = length == 1 ? 2 : 1;
            else if (*f && strchr("lzjtqL", *f)) length = 3;
            else break;
        }
        const char c = *f;
        if (!c) break;
        ++f;
        /* the spec without its length letters, then this host's own length */
        size_t k = 0;
        for (const char *q = start; q < f - 1 && k < sizeof spec - 4; ++q)
            if (!strchr("hlzjtqL", *q)) spec[k++] = *q;
        int w = star_w ? (int)game_va_int(&v) : 0, pr = star_p ? (int)game_va_int(&v) : 0;
        char piece[600];
        int m;
        if (strchr("diouxXc", c)) {
            uint64_t x = game_va_int(&v);
            if (c == 'c') x = (unsigned char)x;
            else if (length == 1) x = (c == 'd' || c == 'i') ? (uint64_t)(int64_t)(int16_t)x : (uint16_t)x;
            else if (length == 2) x = (c == 'd' || c == 'i') ? (uint64_t)(int64_t)(int8_t)x : (uint8_t)x;
            else if (length == 0) x = (c == 'd' || c == 'i') ? (uint64_t)(int64_t)(int32_t)x : (uint32_t)x;
            if (c != 'c') { spec[k++] = 'l'; spec[k++] = 'l'; }
            spec[k++] = c; spec[k] = 0;
            if (c == 'c') m = star_w ? (star_p ? snprintf(piece, sizeof piece, spec, w, pr, (int)x) : snprintf(piece, sizeof piece, spec, w, (int)x))
                                     : (star_p ? snprintf(piece, sizeof piece, spec, pr, (int)x) : snprintf(piece, sizeof piece, spec, (int)x));
            else m = star_w ? (star_p ? snprintf(piece, sizeof piece, spec, w, pr, (long long)x) : snprintf(piece, sizeof piece, spec, w, (long long)x))
                            : (star_p ? snprintf(piece, sizeof piece, spec, pr, (long long)x) : snprintf(piece, sizeof piece, spec, (long long)x));
        } else if (strchr("fFeEgGaA", c)) {
            if (length == 3 && strchr(start, 'L') && strchr(start, 'L') < f) { snprintf(out + n, sizeof out - n, "<long double>"); n = strlen(out); break; }
            double x = game_va_double(&v);
            spec[k++] = c; spec[k] = 0;
            m = star_w ? (star_p ? snprintf(piece, sizeof piece, spec, w, pr, x) : snprintf(piece, sizeof piece, spec, w, x))
                       : (star_p ? snprintf(piece, sizeof piece, spec, pr, x) : snprintf(piece, sizeof piece, spec, x));
        } else if (c == 's') {
            const char *x = (const char *)(uintptr_t)game_va_int(&v);
            if (!x) x = "(null)";
            else if ((uintptr_t)x < 0x10000 || length == 3) x = "(?)"; /* wide strings are not shown */
            spec[k++] = 's'; spec[k] = 0;
            m = star_w ? (star_p ? snprintf(piece, sizeof piece, spec, w, pr, x) : snprintf(piece, sizeof piece, spec, w, x))
                       : (star_p ? snprintf(piece, sizeof piece, spec, pr, x) : snprintf(piece, sizeof piece, spec, x));
        } else if (c == 'p') {
            m = snprintf(piece, sizeof piece, "0x%llx", (unsigned long long)game_va_int(&v));
        } else if (c == 'n') {
            (void)game_va_int(&v); m = 0; piece[0] = 0;
        } else { m = snprintf(piece, sizeof piece, "%.*s", (int)(f - start), start); }
        if (m < 0) m = 0;
        for (int i = 0; i < m && piece[i] && n < sizeof out - 1; ++i) out[n++] = piece[i];
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) --n;
    out[n] = 0;
    for (size_t i = 0; i < n; ++i) if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    printf("Game log: %s\n", out);
    fflush(stdout);
}
/* Nine entries are hooked (the last seven below). 0x45c1750 (string, format, va_list) formats the identity library's
 * messages. 0x45fbb30 (logger, level, format, ...) is the sports login service's logger: its
 * thunk keeps the argument registers in a System V register save area and points a va_list at
 * it and at the caller's stack arguments. Each entry's first instructions move into the thunk. */
/* 0x45c0f60 (session, result, response) receives the identity library's Login result: a non-zero
 * code at result+0x1c goes to the game's listener with the message at result+0x20 (never logged);
 * zero goes on to the service scope check ("service.%s"). Its hook prints which one it was. */
static ABI void game_login_result(void *session, const unsigned char *result, const void *response) {
    (void)session; (void)response;
    if (!result || (uintptr_t)result < 0x10000) return;
    int32_t code; uint32_t id; const char *message;
    memcpy(&code, result + 0x1c, 4); memcpy(&id, result + 8, 4); memcpy(&message, result + 0x20, 8);
    if (!code) { puts("Game log: identity Login result: success"); fflush(stdout); return; }
    if (!message || (uintptr_t)message < 0x10000) message = "(no message)";
    printf("Game log: identity Login result: error %d (0x%x), task %u: %.400s\n", code, (unsigned)code, id, message);
    fflush(stdout);
}
/* 0x45dadb0 is the Login task's "done" (vtable+0x18, after SetDone): it calls the EASTL function
 * at task+0x88 (invoker; rdi = task+0x88, rsi = task) or else the one at task+0x98 (rdi = task,
 * rsi = result at task+0xa8). Its hook prints what the task holds: which code gets the result. */
static void game_log_address(char *out, size_t size, uint64_t value) {
    const uint64_t base = (uint64_t)(uintptr_t)image;
    if (value >= base && value - base < 0x10000000) snprintf(out, size, "eboot 0x%llx", (unsigned long long)(value - base));
    else snprintf(out, size, "0x%llx", (unsigned long long)value);
}
static ABI void game_task_done(const unsigned char *task, const void *unused1, const void *unused2) {
    (void)unused1; (void)unused2;
    if (!task || (uintptr_t)task < 0x10000) return;
    uint32_t id; const char *name; uint64_t f[4], vtable;
    memcpy(&vtable, task, 8); memcpy(&id, task + 8, 4); memcpy(&name, task + 0x10, 8); memcpy(f, task + 0x88, sizeof f);
    if (!name || (uintptr_t)name < 0x10000) name = "?";
    char v[48], a[48], b[48], c[48], d[48];
    game_log_address(v, sizeof v, vtable); game_log_address(a, sizeof a, f[0]); game_log_address(b, sizeof b, f[1]);
    game_log_address(c, sizeof c, f[2]); game_log_address(d, sizeof d, f[3]);
    printf("Game log: task %.40s:%04u done (vtable %s): +0x88 %s, +0x90 %s, +0x98 %s, +0xa0 %s\n", name, id, v, a, b, c, d);
    fflush(stdout);
}
/* 0x45c2040 (session, task, result) is the method the Login task's callback runs: a non-zero task
 * code (+0x1c, message +0x20) goes to the game's listener (session+0x158) method +0x48; success
 * stores the tokens and calls the listener's +0x10 and +0x38. Its hook prints which, and where. */
static ABI void game_login_done(const unsigned char *session, const void *task_pointer, const void *result) {
    (void)result;
    const unsigned char *task = task_pointer;
    if (!session || !task || (uintptr_t)session < 0x10000 || (uintptr_t)task < 0x10000) return;
    int32_t code; const char *message; uint64_t listener = 0, vt = 0, m[3] = {0};
    memcpy(&code, task + 0x1c, 4); memcpy(&message, task + 0x20, 8); memcpy(&listener, session + 0x158, 8);
    if (listener >= 0x10000) {
        memcpy(&vt, (const void *)(uintptr_t)listener, 8);
        if (vt >= 0x10000) { memcpy(&m[0], (const void *)(uintptr_t)(vt + 0x10), 8); memcpy(&m[1], (const void *)(uintptr_t)(vt + 0x38), 8); memcpy(&m[2], (const void *)(uintptr_t)(vt + 0x48), 8); }
    }
    char a[48], b[48], c[48], d[48];
    game_log_address(a, sizeof a, vt); game_log_address(b, sizeof b, m[0]); game_log_address(c, sizeof c, m[1]); game_log_address(d, sizeof d, m[2]);
    if (code) {
        if (!message || (uintptr_t)message < 0x10000) message = "(no message)";
        printf("Game log: identity Login done: error %d (0x%x): %.300s\n", code, (unsigned)code, message);
    } else puts("Game log: identity Login done: success");
    printf("Game log:   listener vtable %s: +0x10 %s, +0x38 %s, +0x48 %s\n", a, b, c, d);
    fflush(stdout);
}
/* 0x2feba20 is the game's listener method the identity session calls on success (vtable 0x5824f80
 * +0x10; rdi = listener, esi = request id). It looks the id up in a sorted table at
 * listener-0x10+0x48..+0x50 (8-byte entries: int request id, int job id, 0x2febd40) and completes
 * that job; with no entry nothing happens. Its hook prints the id and the table. */
static ABI void game_listener_done(const unsigned char *listener, const void *id_value, const void *name) {
    if (!listener || (uintptr_t)listener < 0x10000) return;
    const uint32_t id = (uint32_t)(uintptr_t)id_value;
    const unsigned char *object = listener - 0x10;
    uint64_t begin, end;
    memcpy(&begin, object + 0x48, 8); memcpy(&end, object + 0x50, 8);
    char text[64] = "";
    if (name && (uintptr_t)name >= 0x10000) snprintf(text, sizeof text, " (%.40s)", (const char *)name);
    printf("Game log: listener: request %u%s finished; pending table has %lld entries:", id, text,
           end >= begin && begin >= 0x10000 ? (long long)((end - begin) / 8) : -1LL);
    if (end >= begin && begin >= 0x10000)
        for (uint64_t p = begin; p < end && p < begin + 16 * 8; p += 8) {
            int32_t key, job;
            memcpy(&key, (const void *)(uintptr_t)p, 4); memcpy(&job, (const void *)(uintptr_t)(p + 4), 4);
            printf(" [request %d -> job %d]", key, job);
        }
    putchar('\n');
    fflush(stdout);
}
/* 0x2feba3b, inside that listener method once the job is found (rbx = the job): next it calls
 * job->vtable+0x30(job, 0, 0) (complete) and job->vtable+0 (release). Its hook prints the job's
 * class and those methods, so the code that should go on after the login can be read. */
static ABI void game_job_complete(const unsigned char *job, const void *unused1, const void *unused2) {
    (void)unused1; (void)unused2;
    if (!job || (uintptr_t)job < 0x10000) return;
    uint64_t vt = 0, m[8] = {0}, fields[6] = {0};
    memcpy(&vt, job, 8); memcpy(fields, job + 8, sizeof fields);
    if (vt >= 0x10000) memcpy(m, (const void *)(uintptr_t)vt, sizeof m);
    char a[48], b[48], c[48], d[48], e[48];
    game_log_address(a, sizeof a, vt); game_log_address(b, sizeof b, m[0]); game_log_address(c, sizeof c, m[6]);
    game_log_address(d, sizeof d, m[2]); game_log_address(e, sizeof e, m[4]);
    printf("Game log: job completed: vtable %s: +0x0 %s, +0x30 %s (complete), +0x10 %s, +0x20 %s\n", a, b, c, d, e);
    printf("Game log:   job fields +0x08..+0x30: %llx %llx %llx %llx %llx %llx\n",
           (unsigned long long)fields[0], (unsigned long long)fields[1], (unsigned long long)fields[2],
           (unsigned long long)fields[3], (unsigned long long)fields[4], (unsigned long long)fields[5]);
    /* complete (0x2fed120): an EASTL function at job+0x40 (invoker; +0x48.. its storage) or else a
     * plain function at +0x50, called with (result, job id at +0x30) */
    uint64_t cb[4]; /* the job is 0x68 bytes: +0x40..+0x60 */
    memcpy(cb, job + 0x40, sizeof cb);
    char f0[48], f1[48], f2[48], f3[48];
    game_log_address(f0, sizeof f0, cb[0]); game_log_address(f1, sizeof f1, cb[1]);
    game_log_address(f2, sizeof f2, cb[2]); game_log_address(f3, sizeof f3, cb[3]);
    printf("Game log:   job callback +0x40 %s, +0x48 %s, +0x50 %s, +0x58 %s\n", f0, f1, f2, f3);
    if (cb[1] >= 0x10000 && !(cb[1] & 7)) { /* the storage's object, if any: its class */
        uint64_t ovt = 0; char o[48];
        memcpy(&ovt, (const void *)(uintptr_t)cb[1], 8);
        game_log_address(o, sizeof o, ovt);
        printf("Game log:   callback object 0x%llx, its vtable %s\n", (unsigned long long)cb[1], o);
    }
    fflush(stdout);
}
/* 0x396ae22, in the game's Lua error reporter (0x396aa10; "LUA runtime error" and the rest):
 * r14 = lua_State, r15d = the error code (2 runtime, 4 memory, 5 error handler). The message is
 * the value on top of the Lua stack (L+0x10 = top; a TValue is 16 bytes with its type at +8; type
 * 4 is a string whose length is at +0x10 and its bytes at +0x18, as lua_tolstring 0x3b68500). */
static ABI void game_lua_error(const unsigned char *lua, const void *code_value, const void *unused) {
    (void)unused;
    if (!lua || (uintptr_t)lua < 0x10000) return;
    uint64_t top = 0, value = 0, length = 0;
    int32_t type = 0;
    memcpy(&top, lua + 0x10, 8);
    const char *message = "(not a string)";
    if (top >= 0x10010) {
        memcpy(&value, (const void *)(uintptr_t)(top - 0x10), 8);
        memcpy(&type, (const void *)(uintptr_t)(top - 0x10 + 8), 4);
        if (type == 4 && value >= 0x10000) {
            memcpy(&length, (const void *)(uintptr_t)(value + 0x10), 8);
            message = (const char *)(uintptr_t)(value + 0x18);
        }
    }
    printf("Game log: LUA error (code %d): %.*s\n", (int)(uintptr_t)code_value,
           (int)(length && length < 2000 ? length : (type == 4 ? 2000 : 14)), message);
    fflush(stdout);
}
/* 0x2fe41a0 is the game's online login state machine update (object vtable 0x5824ae8, state at
 * +0x98, error code at +0x9c; switch at 0x2fe4418 on state - 2). Its hook prints each change. */
static ABI void game_login_state(const unsigned char *object, const void *unused1, const void *unused2) {
    (void)unused1; (void)unused2;
    static int32_t last = -1;
    if (!object || (uintptr_t)object < 0x10000) return;
    int32_t state, error;
    memcpy(&state, object + 0x98, 4); memcpy(&error, object + 0x9c, 4);
    if (state == last) return;
    printf("Game log: online login state %d -> %d (error code 0x%x)\n", last, state, (unsigned)error);
    fflush(stdout);
    last = state;
}
/* kind 0: (string, format, va_list); 1: (logger, level, format, ...); 2: handler(rdi, rsi, rdx);
 * 3: handler(rbx); 4: handler(r14, r15). 3 and 4 are entered mid-function. */
typedef struct { uint64_t text; unsigned char moved[24]; unsigned length; int kind; ABI void (*handler)(const unsigned char *, const void *, const void *); } GameLogHook;
static const GameLogHook game_log_hooks[] = {
    {0x45c1750, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, /* push rbp; mov rbp,rsp; push r15..r12, rbx */
                 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}, 20, 0, NULL},                               /* sub rsp,0x88 */
    {0x45fbb30, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec, 0xd8, 0x00, 0x00, 0x00,           /* push rbp; mov rbp,rsp; push rbx; sub rsp,0xd8 */
                 0x84, 0xc0}, 14, 1, NULL},                                                               /* test al,al (flags kept for its je) */
    {0x45c0f60, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, /* push rbp; mov rbp,rsp; push r15..r12, rbx */
                 0x48, 0x83, 0xec, 0x68}, 17, 2, (ABI void (*)(const unsigned char *, const void *, const void *))game_login_result},
    {0x45dadb0, {0x48, 0x89, 0xf8, 0x48, 0x8b, 0x88, 0x88, 0x00, 0x00, 0x00,                       /* mov rax,rdi; mov rcx,[rax+0x88] */
                 0x48, 0x8d, 0x90, 0xa8, 0x00, 0x00, 0x00}, 17, 2, game_task_done},                  /* lea rdx,[rax+0xa8] */
    {0x45c2040, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50}, /* push rbp; mov rbp,rsp; push r15..r12, rbx, rax */
                 14, 2, game_login_done},
    {0x2feba20, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0xc6, 0x47, 0x30, 0x01,                       /* push rbp; mov rbp,rsp; push rbx, rax; mov byte [rdi+0x30],1 */
                 0x48, 0x83, 0xc7, 0xf0}, 14, 2,                                                      /* add rdi,-0x10 */
     (ABI void (*)(const unsigned char *, const void *, const void *))game_listener_done},
    {0x2feba3b, {0xc6, 0x43, 0x38, 0x01, 0x31, 0xf6, 0x31, 0xd2,                                   /* mov byte [rbx+0x38],1; xor esi,esi; xor edx,edx */
                 0x48, 0x89, 0xdf, 0x48, 0x8b, 0x03}, 14, 3, game_job_complete},                     /* mov rdi,rbx; mov rax,[rbx] */
    {0x396ae22, {0xbe, 0xff, 0xff, 0xff, 0xff, 0x31, 0xd2, 0x4c, 0x89, 0xf7,                       /* mov esi,-1; xor edx,edx; mov rdi,r14 */
                 0xbb, 0xff, 0xff, 0xff, 0xff}, 15, 4,                                                /* mov ebx,-1 */
     (ABI void (*)(const unsigned char *, const void *, const void *))game_lua_error},
    {0x2fe41a0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,    /* push rbp; mov rbp,rsp; push r15..r12, rbx */
                 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00}, 20, 2, game_login_state},                /* sub rsp,0xc8 */
};
enum { GAME_LOG_THUNK = 400 }; /* about 320 bytes used */
static unsigned char *game_log_thunk;
static unsigned char *game_log_emit(unsigned char *p, const void *bytes, size_t n) { memcpy(p, bytes, n); return p + n; }
static void game_log_install(unsigned char *base, Segment *segments, uint64_t ns) {
    const char *env = getenv("BB_GAME_LOG");
    if (!env || !*env || !strcmp(env, "0")) return;
    const size_t count = sizeof game_log_hooks / sizeof *game_log_hooks;
    unsigned hooked = 0;
    for (size_t h = 0; h < count; ++h) {
        const GameLogHook *k = &game_log_hooks[h];
        if (!mapped(segments, ns, k->text, k->length) || memcmp(base + k->text, k->moved, k->length)) {
            printf("Game log (BB_GAME_LOG): no NHL 19 logger at 0x%llx; not hooked\n", (unsigned long long)k->text);
            continue;
        }
        if (!game_log_thunk) game_log_thunk = allocate(page_size);
        unsigned char *t = game_log_thunk + h * GAME_LOG_THUNK, *p = t;
        if (k->kind != 1) {
            static const unsigned char head[] = {
                0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50, /* push rdi rsi rdx rcx r8 r9 rax: rsp 16-aligned */
                0x48, 0x89, 0xf7,                                     /* mov rdi,rsi (format) */
                0x48, 0x89, 0xd6};                                    /* mov rsi,rdx (va_list) */
            p = game_log_emit(p, head, k->kind >= 2 ? 9 : sizeof head); /* kind 2 keeps rdi, rsi, rdx */
            /* kind 3 is entered mid-function with rsp 16-aligned (not 8 as at an entry): 8 more */
            if (k->kind == 3) { static const unsigned char rbx[] = {0x48, 0x89, 0xdf, 0x48, 0x83, 0xec, 0x08}; p = game_log_emit(p, rbx, sizeof rbx); } /* mov rdi,rbx; sub rsp,8 */
            if (k->kind == 4) { static const unsigned char r[] = {0x4c, 0x89, 0xf7, 0x4c, 0x89, 0xfe, 0x48, 0x83, 0xec, 0x08}; p = game_log_emit(p, r, sizeof r); } /* mov rdi,r14; mov rsi,r15; sub rsp,8 */
        } else {
            /* sub rsp,0xd8 (16-aligned); [0..47] rdi..r9, [48..175] xmm0-7, [176..199] va_list, [200] rax */
            static const unsigned char head[] = {
                0x48, 0x81, 0xec, 0xd8, 0x00, 0x00, 0x00,
                0x48, 0x89, 0x3c, 0x24, 0x48, 0x89, 0x74, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, /* mov [rsp],rdi; [rsp+8],rsi; [rsp+16],rdx */
                0x48, 0x89, 0x4c, 0x24, 0x18, 0x4c, 0x89, 0x44, 0x24, 0x20, 0x4c, 0x89, 0x4c, 0x24, 0x28, /* rcx, r8, r9 */
                0x48, 0x89, 0x84, 0x24, 0xc8, 0x00, 0x00, 0x00};       /* mov [rsp+200],rax */
            p = game_log_emit(p, head, sizeof head);
            for (int i = 0; i < 8; ++i) { /* movdqu [rsp+48+16i], xmm_i */
                const unsigned char x[] = {0xf3, 0x0f, 0x7f, (unsigned char)(0x84 | i << 3), 0x24, (unsigned char)(48 + 16 * i), 0, 0, 0};
                p = game_log_emit(p, x, sizeof x);
            }
            static const unsigned char va[] = {
                0xc7, 0x84, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, /* va.gp = 24: rdi, rsi, rdx named */
                0xc7, 0x84, 0x24, 0xb4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, /* va.fp = 48 */
                0x48, 0x8d, 0x84, 0x24, 0xe0, 0x00, 0x00, 0x00,                   /* lea rax,[rsp+0xe0]: caller's stack arguments */
                0x48, 0x89, 0x84, 0x24, 0xb8, 0x00, 0x00, 0x00,                   /* va.overflow */
                0x48, 0x89, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00,                   /* va.save = rsp */
                0x48, 0x8b, 0x7c, 0x24, 0x10,                                     /* mov rdi,[rsp+16] (format) */
                0x48, 0x8d, 0xb4, 0x24, 0xb0, 0x00, 0x00, 0x00};                  /* lea rsi,[rsp+176] (va_list) */
            p = game_log_emit(p, va, sizeof va);
        }
        const uintptr_t logger = k->kind >= 2 ? (uintptr_t)k->handler : (uintptr_t)game_log;
        *p++ = 0x48; *p++ = 0xb8; memcpy(p, &logger, 8); p += 8; /* movabs rax, handler */
        *p++ = 0xff; *p++ = 0xd0;                                /* call rax */
        if (k->kind >= 3) { static const unsigned char add8[] = {0x48, 0x83, 0xc4, 0x08}; p = game_log_emit(p, add8, sizeof add8); } /* add rsp,8 */
        if (k->kind != 1) {
            static const unsigned char tail[] = {0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}; /* pop in reverse */
            p = game_log_emit(p, tail, sizeof tail);
        } else {
            for (int i = 0; i < 8; ++i) { /* movdqu xmm_i, [rsp+48+16i] */
                const unsigned char x[] = {0xf3, 0x0f, 0x6f, (unsigned char)(0x84 | i << 3), 0x24, (unsigned char)(48 + 16 * i), 0, 0, 0};
                p = game_log_emit(p, x, sizeof x);
            }
            static const unsigned char tail[] = {
                0x48, 0x8b, 0x3c, 0x24, 0x48, 0x8b, 0x74, 0x24, 0x08, 0x48, 0x8b, 0x54, 0x24, 0x10,
                0x48, 0x8b, 0x4c, 0x24, 0x18, 0x4c, 0x8b, 0x44, 0x24, 0x20, 0x4c, 0x8b, 0x4c, 0x24, 0x28,
                0x48, 0x8b, 0x84, 0x24, 0xc8, 0x00, 0x00, 0x00,
                0x48, 0x81, 0xc4, 0xd8, 0x00, 0x00, 0x00};             /* add rsp,0xd8 */
            p = game_log_emit(p, tail, sizeof tail);
        }
        p = game_log_emit(p, k->moved, k->length);
        const uintptr_t back = (uintptr_t)(base + k->text + k->length);
        *p++ = 0x49; *p++ = 0xbb; memcpy(p, &back, 8); p += 8;  /* movabs r11, entry + moved */
        *p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;                   /* jmp r11 */
        if (p - t > GAME_LOG_THUNK) fail("game log thunk too long");
        unsigned char *e = base + k->text;
        const uintptr_t thunk = (uintptr_t)t;
        e[0] = 0x49; e[1] = 0xbb; memcpy(e + 2, &thunk, 8);      /* movabs r11, thunk */
        e[10] = 0x41; e[11] = 0xff; e[12] = 0xe3;                /* jmp r11 */
        memset(e + 13, 0x90, k->length - 13);
        ++hooked;
    }
    if (hooked) printf("Game log (BB_GAME_LOG): %u of %u loggers hooked; the game's own debug messages are printed as \"Game log:\" lines\n",
                       hooked, (unsigned)count);
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* Fault handling (platform_install_fault_handlers): GPU page tracking first, then a speculative
 * read's recovery point, then the fatal report with the guest write log. */
/* ufc3: a speculative read (a draw-preparation worker walking guest tables under the pipeline
 * cache's shared lock) must not be resolved by a GPU readback: the readback waits for the GPU
 * thread, which waits for that lock. Its recovery point takes over and the GPU thread redoes it. */
static int gpu_fault(void *context, void *address) {
    if (runtime_fault_recover) return 0;
    return gpu_enabled && bbgpu_handle_fault(context, address);
}
static int gpu_step(void *context) { return gpu_enabled && bbgpu_handle_step(context); }
static bb_fault_jmp_buf *recover_point(void) { bb_fault_jmp_buf *r = runtime_fault_recover; runtime_fault_recover = NULL; return r; }
/* UFC-57: Career crashes with a null write at +0x1a3d204, in the UI scene's snapshot builder
 * (+0x1a3d100, this in r13). Each update it copies every node of its list at this+0x20 into a
 * 0x1e8-byte block from the allocator at this+0x18 (+0x1a3d442) and stores the block in
 * node+0x20 without checking for NULL; the next build writes through it. The allocator ran dry.
 * Built snapshots wait for the renderer in the vector at this+0xc0 (count +0xc8), finished ones
 * in this+0xf0 (count +0xf8); +0x108 counts builds. This names the allocator and shows whether
 * snapshots piled up (renderer not consuming) or the heap is simply full. */
static void ui_snapshot_report(void *context) {
    uint64_t r[17];
    platform_fault_registers(context, r);
    const uintptr_t base = (uintptr_t)image;
    if (r[16] != base + 0x1a3d204) return;
    const uintptr_t scene = r[13];
    uint64_t f[0x110 / 8];
    if (!platform_safe_read(scene, f, sizeof(f))) { fprintf(stderr, "  UFC-57: UI scene %#" PRIxPTR " unreadable\n", scene); return; }
    fprintf(stderr, "  UFC-57: UI scene %#" PRIxPTR ": snapshots waiting for the renderer %" PRIu64 ", finished not yet freed %" PRIu64
            ", stack %" PRIu64 ", builds %" PRIu64 "\n", scene, f[0xc8 / 8], f[0xf8 / 8], f[0xb0 / 8], f[0x108 / 8]);
    for (int slot = 0; slot < 2; ++slot) {
        const uintptr_t allocator = f[slot ? 0x18 / 8 : 0];
        uint64_t vtable = 0, entry = 0;
        if (allocator) platform_safe_read(allocator, &vtable, 8);
        if (vtable) platform_safe_read(vtable + (slot ? 0x58 : 0x50), &entry, 8);
        fprintf(stderr, "  UFC-57: allocator at scene+%#x: %#" PRIxPTR ", vtable +%#" PRIx64 ", alloc +%#" PRIx64 "\n",
                slot ? 0x18 : 0, allocator, vtable ? vtable - base : 0, entry ? entry - base : 0);
    }
    /* Oldest and newest waiting snapshot (their build number is at +0x48). */
    const uint64_t waiting = f[0xc8 / 8];
    if (waiting && waiting < (1u << 24)) {
        uint64_t first = 0, last = 0, n_first = 0, n_last = 0;
        platform_safe_read(f[0xc0 / 8], &first, 8);
        platform_safe_read(f[0xc0 / 8] + (waiting - 1) * 8, &last, 8);
        if (first) platform_safe_read(first + 0x48, &n_first, 8);
        if (last) platform_safe_read(last + 0x48, &n_last, 8);
        fprintf(stderr, "  UFC-57: waiting snapshots span builds %" PRIu64 "..%" PRIu64 "\n", n_first, n_last);
    }
    /* Nodes of the list at scene+0x20, and how many got no copy. */
    uint64_t nodes = 0, empty = 0, node = f[0x28 / 8];
    while (node && node != scene + 0x20 && nodes < 100000) {
        uint64_t n[5];
        if (!platform_safe_read(node, n, sizeof(n))) break;
        ++nodes; empty += !n[4];
        node = n[1];
    }
    fprintf(stderr, "  UFC-57: %" PRIu64 " nodes, %" PRIu64 " without a copy\n", nodes, empty);
}
static void fault_extra(void *context) {
    fprintf(stderr, "  Version: game %s\n", BB_BUILD_VERSION);
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    ui_snapshot_report(context);
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game (the settings menu: a new render resolution is a patch applied at start):
 * run.sh again on Linux, the launcher's command line on Windows (platform_restart). Descriptors
 * are closed first so the old GPU device and its memory are released before the new process
 * opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    /* The GPU threads still run until the new process starts: their Vulkan calls fail once the
     * device is closed, and an assertion there must not end the process (exit 23) first. */
    runtime_restarting = 1;
    __sync_synchronize();
    platform_restart();
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts(game_build_identity + sizeof(BB_GAME_VERSION_MARKER) - 1);
        return 0;
    }
#ifdef __linux__
    /* ufc3: BB_ALLOW_PTRACE=1 lets a debugger attach later (Yama ptrace_scope 1 allows only
     * descendants otherwise): stacks of a hang that does not happen under gdb. */
    if (getenv("BB_ALLOW_PTRACE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Host preparation before anything else allocates: glibc's heap below 1 TiB (Linux); the
     * PS4 user range reserved and the guest TCB's TEB TLS slot claimed before any driver loads
     * (Windows). The guest packs pointers into 40 bits. */
    platform_process_init();
    runtime_memory_reserve();
    runtime_thread_host_init();
#ifdef _WIN32
    const char *platform = "Windows";
#else
    const char *platform = "Linux";
#endif
#ifdef _WIN32
    const char *gpu_version = bbgpu_version();
#else
    const char *gpu_version = bbgpu_version ? bbgpu_version() : "unknown";
#endif
    bb_print_versions(stdout, BB_BUILD_VERSION, gpu_version, platform);
    fflush(stdout);
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
    int cpu_only = 0, strict_imports = 0, list_imports = 0;
    unsigned timeout_seconds = 10;
    int64_t tls_loads = -1; /* --tls-loads: the preparation step's count of guest TCB loads (link.json) */
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *import_names=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        /* SacredFire: --list-imports resolves every import, prints the outcome and exits without running. */
        else if (!strcmp(argv[i], "--list-imports")) { list_imports = 1; cpu_only = 1; }
        else if (!strcmp(argv[i], "--import-names") && i+1<argc) import_names=argv[++i];
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--tls-loads") && i+1<argc) tls_loads=strtoll(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--list-imports] [--import-names file] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] [--tls-loads count] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (import_names) {
        int loaded = runtime_load_import_names(import_names);
        if (loaded < 0) fail("cannot read the import names file");
        printf("Import names: %d symbols from %s\n", loaded, import_names);
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    }
    bbgpu_register_kernel();
    page_size = platform_page_size();
    platform_install_fault_handlers(gpu_fault, recover_point, fault_extra, NULL, 0);
    platform_install_step_handler(gpu_step);
    platform_watchdog_start(timeout_seconds, NULL); /* 0 disables the watchdog */
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    runtime_set_linked_modules(argv[1]);
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    /* Fault reports and thread dumps name guest offsets from here on. */
    platform_install_fault_handlers(gpu_fault, recover_point, fault_extra, image, round_page(size));
    platform_watchdog_start(0, image);
    runtime_ui_backpressure_init(image, size); /* UFC-57 */
    runtime_appearance_init(image, size); /* UFC-58 */
    if (!cpu_only) {
        char title[128]="PS4 game", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        char icon[4200];
        snprintf(icon,sizeof(icon),"%s/sce_sys/icon0.png",app0);
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080,icon};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    const char *trace_env = getenv("BB_TRACE_CALLS");
    if (trace_env && *trace_env && strcmp(trace_env, "0") && !list_imports) {
        const char *first = getenv("BB_TRACE_FIRST");
        if (first && *first) trace_first = (uint32_t)strtoul(first, NULL, 10);
        trace_counts = calloc(import_count ? import_count : 1, sizeof *trace_counts);
        trace_stubs = allocate(round_page((import_count + 1) * TRACE_STUB));
        if (!trace_counts) fail("allocation failed");
        trace_start_ns = platform_monotonic_ns();
        printf("Trace: calls to the runtime's PS4 functions are logged (BB_TRACE_CALLS; first %u of each, then powers of two)\n", trace_first);
    }
    /* SacredFire (--list-imports): how each import was bound: 1 runtime, 2 native module, 3 trap. */
    unsigned char *outcome = calloc(import_count ? import_count : 1, 1);
    if (!outcome) fail("allocation failed");
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) {
                value = resolved + relocs[i].addend; outcome[relocs[i].value] = 1;
                if (trace_stubs && relocs[i].kind == 1 && !relocs[i].addend)
                    value = trace_stub((uint32_t)relocs[i].value, resolved);
            }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
                outcome[relocs[i].value] = 2;
            } else outcome[relocs[i].value] = 3;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (list_imports) {
        static const char *words[] = {"unreferenced", "runtime", "native", "unresolved"};
        unsigned counts[4] = {0};
        for (uint64_t i = 0; i < import_count; ++i) {
            const char *symbol = runtime_symbol(names[i]);
            printf("%-12s %-40s %s\n", words[outcome[i]], names[i], symbol ? symbol : "?");
            ++counts[outcome[i]];
        }
        printf("Imports: %u runtime, %u native, %u unresolved, %u unreferenced\n", counts[1], counts[2], counts[3], counts[0]);
        return 0;
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) bbgpu_patch_image(image, size);
#ifdef _WIN32
    /* Guest code reads its TCB with `mov rax, gs:[0]` (link_modules.py rewrote fs:[0]); on Windows GS
     * is the TEB, so the displacement becomes the TEB TLS slot holding the guest TCB. Every site is
     * decoded first, and the count must agree with the preparation step's (--tls-loads). */
    {
        uint64_t *addresses = calloc(ns ? ns : 1, sizeof(uint64_t)), *sizes = calloc(ns ? ns : 1, sizeof(uint64_t)), *flags = calloc(ns ? ns : 1, sizeof(uint64_t));
        if (!addresses || !sizes || !flags) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) { addresses[i] = segments[i].address; sizes[i] = segments[i].size; flags[i] = segments[i].flags; }
        printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "%s\n", platform_patch_guest_tls_loads(image, addresses, sizes, flags, ns, tls_loads),
               tls_loads < 0 ? " (count not cross-checked: --tls-loads not given)" : " (matches the preparation step)");
        free(addresses); free(sizes); free(flags);
    }
#else
    (void)tls_loads;
#endif
    /* Windows: the guest red zone out of reach of exception frames (runtime_fp.c). */
    if (runtime_redzone_enabled()) {
        uint64_t *addresses = calloc(ns ? ns : 1, sizeof(uint64_t)), *sizes = calloc(ns ? ns : 1, sizeof(uint64_t)), *flags = calloc(ns ? ns : 1, sizeof(uint64_t));
        uint64_t *pointers = calloc(nr ? nr : 1, sizeof(uint64_t)), pointer_count = 0;
        if (!addresses || !sizes || !flags || !pointers) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) { addresses[i] = segments[i].address; sizes[i] = segments[i].size; flags[i] = segments[i].flags; }
        for (uint64_t i = 0; i < nr; ++i) if (!relocs[i].kind) pointers[pointer_count++] = relocs[i].value;
        RuntimeFpStats rz;
        const uint64_t start = platform_monotonic_ns();
        runtime_redzone_patch_image(image, size, addresses, sizes, flags, ns, pointers, pointer_count, map_near_image, &rz);
        printf("Red zone (BB_REDZONE_SHIFT): %" PRIu64 " of %" PRIu64 " accesses moved 64 KiB lower in %" PRIu64 " functions "
               "(%" PRIu64 " kept, %" PRIu64 " switch-table entries seen), %" PRIu64 " KiB of stubs, %.0f ms\n",
               rz.patched, rz.found, rz.functions, rz.unpatched, rz.displaced, rz.stub_bytes / 1024,
               (double)(platform_monotonic_ns() - start) / 1e6);
        free(addresses); free(sizes); free(flags); free(pointers);
    }
    /* ufc3 online (UFC-25): approximate reciprocals that give the same bits on every CPU. */
    if (runtime_fp_deterministic()) {
        uint64_t *addresses = calloc(ns, sizeof(uint64_t)), *sizes = calloc(ns, sizeof(uint64_t)), *flags = calloc(ns, sizeof(uint64_t));
        uint64_t *pointers = calloc(nr ? nr : 1, sizeof(uint64_t)), pointer_count = 0;
        if (!addresses || !sizes || !flags || !pointers) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i) { addresses[i] = segments[i].address; sizes[i] = segments[i].size; flags[i] = segments[i].flags; }
        for (uint64_t i = 0; i < nr; ++i) if (!relocs[i].kind) pointers[pointer_count++] = relocs[i].value;
        RuntimeFpStats fp;
        const uint64_t start = platform_monotonic_ns();
        runtime_fp_patch_image(image, size, addresses, sizes, flags, ns, pointers, pointer_count, map_near_image, &fp);
        printf("Deterministic FP (BB_DETERMINISTIC_FP): %" PRIu64 " of %" PRIu64 " rcp/rsqrt instructions patched in %" PRIu64 " functions "
               "(%" PRIu64 " kept as the CPU computes them), %" PRIu64 " KiB of stubs, %.0f ms; guest threads use MXCSR 0x9fc0\n",
               fp.patched, fp.found, fp.functions, fp.unpatched, fp.stub_bytes / 1024, (double)(platform_monotonic_ns() - start) / 1e6);
        free(addresses); free(sizes); free(flags); free(pointers);
    }
    game_log_install(image, segments, ns);
    if (game_log_thunk) protect(game_log_thunk, page_size, 5);
    protect(traps, round_page((import_count + 1) * 32), 5);
    if (trace_stubs) protect(trace_stubs, round_page((import_count + 1) * TRACE_STUB), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            /* module_start: 0 = started and resident; 1 = SCE_KERNEL_START_NO_RESIDENT (EAWebKit). */
            if (result != 0 && result != 1) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks (platform_enter_guest_main). */
    runtime_fp_thread_setup(); /* also when no linked module attached the main thread */
    platform_enter_guest_main(image+entry,&params,(void *)guest_exit);
    fail("entry unexpectedly returned");
}
