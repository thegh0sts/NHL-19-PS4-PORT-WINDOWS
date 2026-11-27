/* ufc3 online (UFC-25): the deterministic rcp/rsqrt patch (src/runtime_fp.c).
 * Small guest-like routines (System V, AT&T-free Intel syntax below) are copied into a buffer,
 * patched as the loader patches the eboot, run, and their results compared with the rule in C
 * (runtime_fp_rcp/runtime_fp_rsqrt): every operand form, the lanes and upper bits each form
 * keeps, registers, flags, the red zone, displaced instructions (RIP-relative, a jump), on edge
 * values and a wide sample of bit patterns, under MXCSR 0x1f80 and 0x9fc0. The rule itself is
 * checked against exact arithmetic and against this CPU's own rcpps/rsqrtps.
 *
 * NHL 19 Overtime online's table rule (shadPS4's on Intel) runs the same routines: with zero
 * deltas the packed results must equal this CPU's own (NaN lanes: the input unchanged), with
 * shadPS4's AMD tables they must be this CPU's estimate XOR the table's delta per lane; the scalar
 * forms stay native. The expanded tables' SHA-256 and the xor values are compared with shadPS4's
 * (the decompressed src/resources/amd_*_index_table.bin.zstd at commit 235dcabf9).
 * Arguments: --all also runs all 2^32 patterns through the 8-lane stubs of each rule (a few minutes). */
#include "runtime.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__asm__(".text\n.intel_syntax noprefix\n.globl fp_test_begin\nfp_test_begin:\n"
        /* ymm reg: out = rcp(in[0..7]) */
        "t_ymm_reg: vmovups ymm9, [rdi]\n vrcpps ymm3, ymm9\n vmovups [rsi], ymm3\n ret\n"
        /* ymm memory with an index: out = rsqrt(in[0..7]) */
        "t_ymm_mem: mov rax, 1\n vrsqrtps ymm0, [rdi+rax*8-8]\n vmovups [rsi], ymm0\n ret\n"
        /* xmm memory (VEX.128 zeroes 255:128): out = rsqrt(in[0..3]), 0 x4 */
        "t_xmm_mem: vmovups ymm12, [rdx]\n vrsqrtps xmm12, [rdi]\n vmovups [rsi], ymm12\n ret\n"
        /* legacy reg, xmm8 (keeps 255:128): out = rcp(in[0..3]), aux[4..7] */
        "t_legacy_reg: vmovups ymm8, [rdx]\n movups xmm1, [rdi]\n rcpps xmm8, xmm1\n vmovups [rsi], ymm8\n ret\n"
        /* legacy memory: out = rsqrt(in[0..3]), aux[4..7] */
        "t_legacy_mem: vmovups ymm10, [rdx]\n rsqrtps xmm10, [rdi]\n vmovups [rsi], ymm10\n ret\n"
        /* legacy scalar memory: out = rcp(in[1]), aux[1..7] */
        "t_ss_mem: vmovups ymm5, [rdx]\n rcpss xmm5, [rdi+4]\n vmovups [rsi], ymm5\n ret\n"
        /* legacy scalar reg: out = rsqrt(in[0]), aux[1..7] */
        "t_ss_reg: vmovups ymm5, [rdx]\n movups xmm14, [rdi]\n rsqrtss xmm5, xmm14\n vmovups [rsi], ymm5\n ret\n"
        /* VEX scalar reg: out = rcp(in[0]), aux[1..3], 0 x4 */
        "t_vss_reg: vmovups ymm7, [rdx]\n movups xmm11, [rdi]\n vmovups ymm2, [rdi]\n vrcpss xmm2, xmm7, xmm11\n vmovups [rsi], ymm2\n ret\n"
        /* VEX scalar memory: out = rsqrt(in[2]), aux[1..3], 0 x4 */
        "t_vss_mem: vmovups ymm3, [rdx]\n vmovups ymm14, [rdi]\n vrsqrtss xmm14, xmm3, [rdi+8]\n vmovups [rsi], ymm14\n ret\n"
        /* adjacent sites (the second is displaced into the first's stub): rcp(in[0..3]), rsqrt(aux[0..3]) */
        "t_pair: vmovups ymm2, [rdi]\n vmovups ymm4, [rdx]\n vrcpps xmm1, xmm2\n vrsqrtps xmm3, xmm4\n"
        " vmovups [rsi], xmm1\n vmovups [rsi+16], xmm3\n ret\n"
        /* RIP-relative source, and a RIP-relative instruction displaced: rcp(k), k */
        "t_rip: vrcpps xmm4, [rip+t_k]\n vmovups [rsi], xmm4\n vmovups xmm6, [rdi]\n vrsqrtps xmm6, xmm6\n vmovups xmm5, [rip+t_k]\n"
        " vmovups [rsi+16], xmm5\n ret\n"
        /* a jump displaced with its flags: 1 when rdi < rsi, else 2 */
        "t_branch: cmp rdi, rsi\n vrsqrtps xmm1, xmm1\n jb 1f\n mov eax, 2\n ret\n1: mov eax, 1\n ret\n"
        /* preserved: ymm0-6 (the stubs' scratch), rax rcx rdx r8 r11, the red zone, CF; out[0..6] ymm,
         * then rcx r8 r11, [rsp-8], [rsp-128], CF, rax, rdx */
        "t_preserve: vmovups ymm0, [rdx]\n vmovups ymm1, [rdx+32]\n vmovups ymm2, [rdx+64]\n vmovups ymm3, [rdx+96]\n"
        " vmovups ymm4, [rdx+128]\n vmovups ymm5, [rdx+160]\n vmovups ymm6, [rdx+192]\n vmovups ymm8, [rdi]\n"
        " mov rcx, 0x1111\n mov r8, 0x2222\n mov r11, 0x3333\n mov rax, 0x5555\n mov rdx, 0x4444\n"
        " mov [rsp-8], rdi\n mov [rsp-128], rsi\n stc\n"
        " vrcpps ymm7, ymm8\n setc r9b\n mov r10, rax\n mov rax, [rsp-8]\n"
        " vmovups [rsi], ymm0\n vmovups [rsi+32], ymm1\n vmovups [rsi+64], ymm2\n vmovups [rsi+96], ymm3\n"
        " vmovups [rsi+128], ymm4\n vmovups [rsi+160], ymm5\n vmovups [rsi+192], ymm6\n"
        " mov [rsi+224], rcx\n mov [rsi+232], r8\n mov [rsi+240], r11\n mov [rsi+248], rax\n mov rax, [rsp-128]\n mov [rsi+256], rax\n"
        " mov [rsi+264], r9b\n mov [rsi+272], r10\n mov [rsi+280], rdx\n ret\n"
        /* timing: edi iterations of an 8-lane rsqrt */
        "t_loop: vmovups ymm0, [rsi]\n2: vrsqrtps ymm1, ymm0\n dec edi\n jnz 2b\n vmovups [rsi], ymm1\n vzeroupper\n ret\n"
        ".p2align 4\nt_k: .long 0x40400000, 0x80000000, 0x00000001, 0x7f800001\n"
        ".globl fp_test_end\nfp_test_end:\n"
        ".globl fp_test_offsets\nfp_test_offsets:\n"
        ".quad t_ymm_reg-fp_test_begin, t_ymm_mem-fp_test_begin, t_xmm_mem-fp_test_begin, t_legacy_reg-fp_test_begin\n"
        ".quad t_legacy_mem-fp_test_begin, t_ss_mem-fp_test_begin, t_ss_reg-fp_test_begin, t_vss_reg-fp_test_begin\n"
        ".quad t_vss_mem-fp_test_begin, t_pair-fp_test_begin, t_rip-fp_test_begin, t_branch-fp_test_begin\n"
        ".quad t_preserve-fp_test_begin, t_loop-fp_test_begin\n"
        ".att_syntax prefix\n");
extern const unsigned char fp_test_begin[], fp_test_end[];
extern const uint64_t fp_test_offsets[];
enum { YMM_REG, YMM_MEM, XMM_MEM, LEGACY_REG, LEGACY_MEM, SS_MEM, SS_REG, VSS_REG, VSS_MEM, PAIR, RIP, BRANCH, PRESERVE, LOOP, ROUTINES };
typedef uint64_t (ABI *Routine)(const void *in, void *out, const void *aux);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { if (failures++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static const char *rule_name(RuntimeFpRule r) {
    return r == RUNTIME_FP_EXACT ? "exact" : r == RUNTIME_FP_AMD_TABLES ? "AMD tables" : r == RUNTIME_FP_ZERO_TABLES ? "zero tables" : "native";
}

static unsigned char *stub_area;
static size_t stub_size;
static void *near_map(size_t bytes) {
    if (bytes > stub_size) return NULL;
    return stub_area;
}
static RuntimeFpRule rule; /* the rule of the routines below */
static Routine patched[ROUTINES], native[ROUTINES];
/* A patched copy (under `r`) and a native copy of the routines, with their own stubs. */
static void load(RuntimeFpRule r) {
    const size_t code = (size_t)(fp_test_end - fp_test_begin), page = platform_page_size();
    const size_t rounded = (code + page - 1) / page * page, area = 2 * rounded + (stub_size = 1 << 20);
    unsigned char *buffer = platform_map_anonymous(area, PROT_READ | PROT_WRITE);
    if (!buffer) { puts("FAIL: no memory"); exit(1); }
    unsigned char *copy = buffer + rounded;
    stub_area = copy + rounded;
    memcpy(buffer, fp_test_begin, code);
    memcpy(copy, fp_test_begin, code);
    const uint64_t address = 0, size = code, flags = 5;
    RuntimeFpStats stats;
    const uint64_t n = runtime_fp_patch_image(buffer, code, &address, &size, &flags, 1, NULL, 0, near_map, r, &stats);
    /* 16 sites, all patched (the pair gets one stub); the table rules leave the 4 scalar ones alone. */
    const uint64_t want = r == RUNTIME_FP_EXACT ? 16 : 12;
    CHECK(n == want && stats.found == want && stats.unpatched == 0, "%s: patched %" PRIu64 " of %" PRIu64 " (%" PRIu64 " kept), want %" PRIu64,
          rule_name(r), n, stats.found, stats.unpatched, want);
    if (platform_protect(buffer, 2 * rounded, PROT_READ | PROT_EXEC)) { puts("FAIL: protect"); exit(1); }
    for (int i = 0; i < ROUTINES; ++i) {
        patched[i] = (Routine)(buffer + fp_test_offsets[i]);
        native[i] = (Routine)(copy + fp_test_offsets[i]);
    }
    rule = r;
    printf("%s: patched %" PRIu64 " sites (%" PRIu64 " with displaced instructions), %" PRIu64 " bytes of stubs\n", rule_name(r), n,
           stats.displaced, stats.stub_bytes);
}

static float f(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static uint32_t bits_of(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static uint32_t lane(const void *p, int i) { uint32_t v; memcpy(&v, (const unsigned char *)p + 4 * i, 4); return v; }

/* ---- shadPS4's tables ---- */
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static void sha256_block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = v[7] + (ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
        const uint32_t t2 = (ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(*v));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) h[i] += v[i];
}
static void sha256_hex(const unsigned char *data, size_t length, char out[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t at = 0;
    for (; at + 64 <= length; at += 64) sha256_block(h, data + at);
    unsigned char tail[128] = {0};
    const size_t rest = length - at, total = rest + 9 <= 64 ? 64 : 128;
    memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    for (int k = 0; k < 8; ++k) tail[total - 1 - (size_t)k] = (unsigned char)((uint64_t)length * 8 >> (8 * k));
    sha256_block(h, tail);
    if (total == 128) sha256_block(h, tail + 64);
    for (int k = 0; k < 8; ++k) snprintf(out + 8 * k, 9, "%08x", h[k]);
}
/* shadPS4 src/core/cpu_patches.cpp rcp_xor_values, rsqrt_xor_values (commit 235dcabf9). */
static const uint32_t shadps4_rcp_xor[17] = {
    0x00000000, 0x00001000, 0x00000800, 0x00001800, 0x00003000, 0x00007000, 0x00003800, 0x00007800, 0x0000f800,
    0x0000f000, 0x0007f800, 0x0003f800, 0x000ff000, 0x0001f000, 0x001ff800, 0x0001f800, 0x000ff800};
static const uint32_t shadps4_rsqrt_xor[22] = {
    0x00000000, 0x00000800, 0x00001800, 0x00001000, 0x00007000, 0x00003000, 0x0000f000, 0x00003800, 0x00007800, 0x0003f800, 0x0000f800,
    0x0001f800, 0x0001f000, 0x0007f000, 0x0003f000, 0x000ff000, 0x000ff800, 0x0007f800, 0x001ff800, 0x003ff800, 0x001ff000, 0x007ff800};
static const uint8_t *index_table[2];
static const uint32_t *xor_values[2];
static void check_tables(void) {
    char hex[65];
    sha256_hex((const unsigned char *)"abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc) = %s", hex);
    static const char *const want[2] = {"a284046f869acef48ade0655f0cf9ff1cde3f62174242950d3bd4fe33e6cb9e6",
                                        "6377ad2082f9367ab66a4a5e4a5319947b9d1841d988b915f556f43d9f43f0f5"};
    static const uint32_t *const shadps4_xor[2] = {shadps4_rcp_xor, shadps4_rsqrt_xor};
    static const unsigned shadps4_count[2] = {17, 22};
    for (int r = 0; r < 2; ++r) {
        unsigned count = 0;
        index_table[r] = runtime_fp_amd_index(r);
        xor_values[r] = runtime_fp_amd_xor(r, &count);
        if (!index_table[r]) { CHECK(0, "no %s table", r ? "rsqrt" : "rcp"); exit(1); }
        sha256_hex(index_table[r], (size_t)1 << 21, hex);
        CHECK(!strcmp(hex, want[r]), "%s index table sha256 %s, shadPS4's %s", r ? "rsqrt" : "rcp", hex, want[r]);
        CHECK(count == shadps4_count[r] && !memcmp(xor_values[r], shadps4_xor[r], count * 4), "%s xor values differ from shadPS4's", r ? "rsqrt" : "rcp");
        uint8_t top = 0;
        for (size_t i = 0; i < (size_t)1 << 21; ++i) if (index_table[r][i] > top) top = index_table[r][i];
        CHECK(top < count, "%s index %u past the %u xor values", r ? "rsqrt" : "rcp", top, count);
        printf("%s table: sha256 %s, %u xor values, as shadPS4's\n", r ? "rsqrt" : "rcp", hex, count);
    }
}

/* The table rule on one lane: x, and this CPU's estimate for it. */
static uint64_t deltas_seen;
static uint32_t table_lane(int rsqrt, uint32_t x, uint32_t estimate) {
    if ((x & 0x7fffffff) > 0x7f800000) return x; /* NaN: the input unchanged */
    if (rule == RUNTIME_FP_ZERO_TABLES) return estimate;
    const uint32_t delta = xor_values[rsqrt][index_table[rsqrt][x >> 11]];
    deltas_seen += delta != 0;
    return estimate ^ delta;
}

/* One input vector through every routine. */
static void check_vector_exact(const uint32_t in[8], const uint32_t aux[8]) {
    uint32_t out[8];
    patched[YMM_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == runtime_fp_rcp(in[i]), "ymm rcp(%08x) = %08x, rule %08x", in[i], out[i], runtime_fp_rcp(in[i]));
    patched[YMM_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == runtime_fp_rsqrt(in[i]), "ymm rsqrt(%08x) = %08x, rule %08x", in[i], out[i], runtime_fp_rsqrt(in[i]));
    patched[XMM_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rsqrt(in[i]) : 0), "xmm rsqrt lane %d of %08x: %08x", i, in[i], out[i]);
    patched[LEGACY_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rcp(in[i]) : aux[i]), "legacy rcp lane %d of %08x: %08x", i, in[i], out[i]);
    patched[LEGACY_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? runtime_fp_rsqrt(in[i]) : aux[i]), "legacy rsqrt lane %d of %08x: %08x", i, in[i], out[i]);
    patched[SS_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i ? aux[i] : runtime_fp_rcp(in[1])), "rcpss mem lane %d: %08x", i, out[i]);
    patched[SS_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i ? aux[i] : runtime_fp_rsqrt(in[0])), "rsqrtss reg lane %d: %08x", i, out[i]);
    patched[VSS_REG](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (!i ? runtime_fp_rcp(in[0]) : i < 4 ? aux[i] : 0), "vrcpss reg lane %d: %08x", i, out[i]);
    patched[VSS_MEM](in, out, aux);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (!i ? runtime_fp_rsqrt(in[2]) : i < 4 ? aux[i] : 0), "vrsqrtss mem lane %d: %08x", i, out[i]);
    patched[PAIR](in, out, aux);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i < 4 ? runtime_fp_rcp(in[i]) : runtime_fp_rsqrt(aux[i - 4])), "pair lane %d: %08x", i, out[i]);
}
/* The table rules: each routine against its native copy (the CPU's estimates, the lanes it keeps). */
static void check_vector_tables(const uint32_t in[8], const uint32_t aux[8]) {
    uint32_t out[8], cpu[8];
#define BOTH(r) (native[r](in, cpu, aux), patched[r](in, out, aux))
    BOTH(YMM_REG);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == table_lane(0, in[i], cpu[i]), "%s ymm rcp(%08x) = %08x, cpu %08x", rule_name(rule), in[i], out[i], cpu[i]);
    BOTH(YMM_MEM);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == table_lane(1, in[i], cpu[i]), "%s ymm rsqrt(%08x) = %08x, cpu %08x", rule_name(rule), in[i], out[i], cpu[i]);
    BOTH(XMM_MEM);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? table_lane(1, in[i], cpu[i]) : cpu[i]), "%s xmm rsqrt lane %d of %08x: %08x", rule_name(rule), i, in[i], out[i]);
    BOTH(LEGACY_REG);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? table_lane(0, in[i], cpu[i]) : cpu[i]), "%s legacy rcp lane %d of %08x: %08x", rule_name(rule), i, in[i], out[i]);
    BOTH(LEGACY_MEM);
    for (int i = 0; i < 8; ++i) CHECK(out[i] == (i < 4 ? table_lane(1, in[i], cpu[i]) : cpu[i]), "%s legacy rsqrt lane %d of %08x: %08x", rule_name(rule), i, in[i], out[i]);
    for (int r = SS_MEM; r <= VSS_MEM; ++r) { /* the scalar forms: native */
        BOTH(r);
        CHECK(!memcmp(out, cpu, sizeof out), "%s scalar routine %d is not native", rule_name(rule), r);
    }
    BOTH(PAIR);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i < 4 ? table_lane(0, in[i], cpu[i]) : table_lane(1, aux[i - 4], cpu[i])), "%s pair lane %d: %08x", rule_name(rule), i, out[i]);
#undef BOTH
}
static void check_vector(const uint32_t in[8], const uint32_t aux[8]) {
    if (rule == RUNTIME_FP_EXACT) check_vector_exact(in, aux);
    else check_vector_tables(in, aux);
}

static void check_fixed(void) {
    uint32_t out[8], cpu[8], in[8] = {0x40400000};
    static const uint32_t k[4] = {0x40400000, 0x80000000, 0x00000001, 0x7f800001};
    patched[RIP](in, out, NULL);
    native[RIP](in, cpu, NULL);
    for (int i = 0; i < 4; ++i) {
        const uint32_t want = rule == RUNTIME_FP_EXACT ? runtime_fp_rcp(k[i]) : table_lane(0, k[i], cpu[i]);
        CHECK(out[i] == want && out[4 + i] == k[i], "%s rip lane %d: %08x %08x", rule_name(rule), i, out[i], out[4 + i]);
    }
    CHECK(patched[BRANCH]((void *)1, (void *)2, NULL) == 1 && patched[BRANCH]((void *)2, (void *)1, NULL) == 2, "%s: displaced jb", rule_name(rule));
    uint32_t keep[56], result[72];
    for (int i = 0; i < 56; ++i) keep[i] = 0x9e3779b9u * (uint32_t)(i + 1);
    for (int i = 0; i < 8; ++i) in[i] = 0x3f800000 + (uint32_t)i * 0x123457;
    memset(result, 0, sizeof result);
    patched[PRESERVE](in, result, keep);
    CHECK(!memcmp(result, keep, 224), "%s: ymm0-6 changed", rule_name(rule));
    uint64_t g[7];
    memcpy(g, result + 56, sizeof g);
    CHECK(g[0] == 0x1111 && g[1] == 0x2222 && g[2] == 0x3333, "%s: rcx/r8/r11 changed", rule_name(rule));
    CHECK(g[3] == (uintptr_t)in && g[4] == (uintptr_t)result, "%s: red zone changed: %" PRIx64 " %" PRIx64, rule_name(rule), g[3], g[4]);
    CHECK((result[66] & 0xff) == 1, "%s: carry flag lost", rule_name(rule));
    CHECK(g[6] == 0x5555, "%s: rax changed: %" PRIx64, rule_name(rule), g[6]);
    uint64_t rdx;
    memcpy(&rdx, result + 70, 8);
    CHECK(rdx == 0x4444, "%s: rdx changed: %" PRIx64, rule_name(rule), rdx);
}

/* The rule: R(fl(1/x)), R(fl(1/fl(sqrt x))) within 2^-13 + 2^-23 of exact; specials as listed. */
static void check_rule(uint32_t x) {
    const uint32_t r = runtime_fp_rcp(x), s = runtime_fp_rsqrt(x), a = x & 0x7fffffff;
    const double v = f(x);
    CHECK(!(r & 0x7ff) || a > 0x7f800000, "rcp(%08x) = %08x has more than 12 fraction bits", x, r);
    CHECK(!(s & 0x7ff) || a > 0x7f800000 || s == 0xffc00000, "rsqrt(%08x) = %08x has more than 12 fraction bits", x, s);
    if (a > 0x7f800000) { CHECK(r == (x | 0x400000) && s == (x | 0x400000), "NaN %08x: %08x %08x", x, r, s); return; }
    if (a < 0x00800000) { CHECK(r == ((x & 0x80000000) | 0x7f800000) && s == r, "zero/denormal %08x: %08x %08x", x, r, s); return; }
    if (a > 0x7e800000) CHECK(r == (x & 0x80000000), "rcp(%08x) = %08x, not zero", x, r);
    else CHECK(fabs(f(r) - 1 / v) <= fabs(1 / v) * (0x1p-13 + 0x1p-23), "rcp(%08x) = %08x: too far from %a", x, r, 1 / v);
    if (x & 0x80000000) CHECK(s == 0xffc00000, "rsqrt(%08x) = %08x, not the default NaN", x, s);
    else if (a == 0x7f800000) CHECK(s == 0, "rsqrt(inf) = %08x", s);
    else CHECK(fabs(f(s) - 1 / sqrt(v)) <= 1 / sqrt(v) * (0x1p-13 + 0x1p-22), "rsqrt(%08x) = %08x: too far from %a", x, s, 1 / sqrt(v));
}
/* This CPU's rcpps/rsqrtps: the same special results, and within the specified error. */
static void check_hardware(uint32_t x) {
    float rf, sf;
    __asm__("rcpss %1, %0" : "=x"(rf) : "x"(f(x)));
    __asm__("rsqrtss %1, %0" : "=x"(sf) : "x"(f(x)));
    const uint32_t r = bits_of(rf), s = bits_of(sf), a = x & 0x7fffffff;
    const uint32_t rule_r = runtime_fp_rcp(x), rule_s = runtime_fp_rsqrt(x);
    if (a < 0x00800000 || a >= 0x7f800000) {
        CHECK(r == rule_r && s == rule_s, "special %08x: cpu %08x %08x, rule %08x %08x", x, r, s, rule_r, rule_s);
        return;
    }
    const double v = f(x);
    /* rcp: where results turn tiny each CPU decides on its own */
    if (a < 0x7e000000) CHECK(fabs(f(r) - f(rule_r)) <= fabs(1 / v) * 0x1.8p-11, "rcp(%08x): cpu %08x, rule %08x", x, r, rule_r);
    if (x & 0x80000000) CHECK(s == rule_s, "rsqrt(%08x): cpu %08x, rule %08x", x, s, rule_s);
    else CHECK(fabs(f(s) - f(rule_s)) <= 1 / sqrt(v) * 0x1.8p-11, "rsqrt(%08x): cpu %08x, rule %08x", x, s, rule_s);
}

static uint64_t state = 0x853c49e6748fea9bu;
static uint32_t random32(void) { state = state * 6364136223846793005u + 1442695040888963407u; return (uint32_t)(state >> 32); }

static void run_sample(void) {
    static const uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
        0x00800001, 0x3f800000, 0xbf800000, 0x3f7fffff, 0x3f800001, 0x40400000, 0x40800000, 0x7e7fffff, 0x7e800000,
        0x7e800001, 0xfe800000, 0xfe800001, 0x7effffff, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7f800001,
        0xff800001, 0x7fbfffff, 0x7fc00000, 0xffc00000, 0x7fffffff, 0xffffffff, 0x3f7ff800, 0x3f7ffc00, 0x3f7ffbff,
        0x3f7ffc01, 0x5f000000, 0x1f800000, 0x00000400, 0x00400000, 0x7e7ff000, 0x7e801000, 0x3eaaaaab};
    const size_t n = sizeof(edges) / sizeof(*edges);
    const int exact = rule == RUNTIME_FP_EXACT;
    uint32_t in[8], aux[8];
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 8; ++k) { in[k] = edges[(i + (size_t)k) % n]; aux[k] = edges[(i * 7 + (size_t)k) % n]; }
        check_vector(in, aux);
        if (exact) { check_rule(edges[i]); check_hardware(edges[i]); }
    }
    /* Random patterns, then every exponent with random significands. */
    for (int i = 0; i < 1 << 18; ++i) {
        for (int k = 0; k < 8; ++k) { in[k] = random32(); aux[k] = random32(); }
        if (i & 1) for (int k = 0; k < 8; ++k) in[k] = (in[k] & 0x807fffff) | (uint32_t)((i >> 1) & 0xff) << 23;
        check_vector(in, aux);
        if (exact) { check_rule(in[0]); check_hardware(in[0]); }
    }
}

/* Which rule each setting gives (runtime_fp_rule_for: overtime, BB_DETERMINISTIC_FP, BB_ONLINE_SERVER,
 * Intel, BB_OVERTIME_FP). */
static void check_rule_choice(void) {
    static const struct { int overtime; const char *deterministic, *server; int intel; const char *force; RuntimeFpRule want; } cases[] = {
        {0, NULL, NULL, 0, NULL, RUNTIME_FP_NATIVE},           /* offline */
        {0, NULL, NULL, 1, NULL, RUNTIME_FP_NATIVE},
        {0, NULL, "192.168.1.15", 0, NULL, RUNTIME_FP_EXACT},  /* ufc3 online */
        {0, NULL, "192.168.1.15", 1, NULL, RUNTIME_FP_EXACT},
        {0, "0", "192.168.1.15", 0, NULL, RUNTIME_FP_NATIVE},
        {0, "1", NULL, 1, NULL, RUNTIME_FP_EXACT},
        {0, NULL, NULL, 1, "tables", RUNTIME_FP_NATIVE},       /* BB_OVERTIME_FP only with Overtime */
        {1, NULL, "ot.puckpeak.com", 0, NULL, RUNTIME_FP_NATIVE}, /* Overtime: AMD and others native */
        {1, NULL, "ot.puckpeak.com", 1, NULL, RUNTIME_FP_AMD_TABLES}, /* Intel: AMD's results */
        {1, "1", "ot.puckpeak.com", 0, NULL, RUNTIME_FP_NATIVE}, /* BB_DETERMINISTIC_FP has no effect */
        {1, "0", "ot.puckpeak.com", 1, NULL, RUNTIME_FP_AMD_TABLES},
        {1, NULL, NULL, 0, "tables", RUNTIME_FP_AMD_TABLES},   /* the test switch */
        {1, NULL, NULL, 1, "native", RUNTIME_FP_NATIVE},
        {1, NULL, NULL, 0, "zero", RUNTIME_FP_ZERO_TABLES},
        {1, NULL, NULL, 1, "zero", RUNTIME_FP_ZERO_TABLES},
        {1, NULL, NULL, 1, "bogus", RUNTIME_FP_AMD_TABLES},
        {1, NULL, NULL, 0, "", RUNTIME_FP_NATIVE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const RuntimeFpRule got = runtime_fp_rule_for(cases[i].overtime, cases[i].deterministic, cases[i].server, cases[i].intel, cases[i].force);
        CHECK(got == cases[i].want, "rule case %zu: %s, want %s", i, rule_name(got), rule_name(cases[i].want));
    }
    printf("this CPU: %s (Overtime online uses the %s rule here)\n", runtime_fp_cpu_intel() ? "Intel" : "not Intel",
           runtime_fp_cpu_intel() ? "AMD-table" : "native");
}

static void check_all(void) {
    uint32_t in[8], out[8], cpu[8];
    for (uint64_t x = 0; x < UINT64_C(1) << 32; x += 8) {
        for (int k = 0; k < 8; ++k) in[k] = (uint32_t)(x + (uint64_t)k);
        patched[YMM_REG](in, out, NULL);
        if (rule == RUNTIME_FP_EXACT) {
            for (int k = 0; k < 8; ++k) CHECK(out[k] == runtime_fp_rcp(in[k]), "rcp(%08x) = %08x", in[k], out[k]);
            patched[YMM_MEM](in, out, NULL);
            for (int k = 0; k < 8; ++k) CHECK(out[k] == runtime_fp_rsqrt(in[k]), "rsqrt(%08x) = %08x", in[k], out[k]);
            check_rule(in[0]);
        } else {
            native[YMM_REG](in, cpu, NULL);
            for (int k = 0; k < 8; ++k) CHECK(out[k] == table_lane(0, in[k], cpu[k]), "%s rcp(%08x) = %08x", rule_name(rule), in[k], out[k]);
            patched[YMM_MEM](in, out, NULL);
            native[YMM_MEM](in, cpu, NULL);
            for (int k = 0; k < 8; ++k) CHECK(out[k] == table_lane(1, in[k], cpu[k]), "%s rsqrt(%08x) = %08x", rule_name(rule), in[k], out[k]);
        }
    }
    printf("%s: all 2^32 patterns checked\n", rule_name(rule));
}

int main(int argc, char **argv) {
    static const RuntimeFpRule rules[] = {RUNTIME_FP_EXACT, RUNTIME_FP_ZERO_TABLES, RUNTIME_FP_AMD_TABLES};
    const int all = argc > 1 && !strcmp(argv[1], "--all");
    unsigned int saved;
    __asm__ volatile("stmxcsr %0" : "=m"(saved));
    check_rule_choice();
    check_tables();
    for (size_t r = 0; r < sizeof(rules) / sizeof(*rules); ++r) {
        load(rules[r]);
        deltas_seen = 0;
        check_fixed();
        state = 0x853c49e6748fea9bu;
        run_sample();
        unsigned int ps4 = 0x9fc0;
        __asm__ volatile("ldmxcsr %0" : : "m"(ps4));
        state = 0x853c49e6748fea9bu;
        run_sample(); /* the same results with FTZ/DAZ: the stubs do not depend on MXCSR */
        check_fixed();
        __asm__ volatile("ldmxcsr %0" : : "m"(saved));
        if (rule == RUNTIME_FP_AMD_TABLES) {
            CHECK(deltas_seen > 0, "AMD tables: no lane got a nonzero delta");
            printf("AMD tables: %" PRIu64 " lanes got a nonzero delta\n", deltas_seen);
        }
        if (all) check_all();
        /* Cost of a patched 8-lane rsqrt against the CPU's own (a loop of nothing else). */
        uint32_t v[8], cpu[8];
        for (int k = 0; k < 8; ++k) v[k] = cpu[k] = 0x40400000;
        const int iterations = 2000000;
        uint64_t t0 = platform_monotonic_ns();
        native[LOOP]((void *)(uintptr_t)iterations, cpu, NULL);
        uint64_t t1 = platform_monotonic_ns();
        patched[LOOP]((void *)(uintptr_t)iterations, v, NULL);
        uint64_t t2 = platform_monotonic_ns();
        const uint32_t want = rule == RUNTIME_FP_EXACT ? runtime_fp_rsqrt(0x40400000) : table_lane(1, 0x40400000, lane(cpu, 7));
        CHECK(lane(v, 7) == want, "%s: loop result %08x, want %08x", rule_name(rule), lane(v, 7), want);
        printf("%s: vrsqrtps ymm: %.1f ns native, %.1f ns patched\n", rule_name(rule), (double)(t1 - t0) / iterations,
               (double)(t2 - t1) / iterations);
    }
    if (failures) { printf("%d failures\n", failures); return 1; }
    puts("deterministic FP: all checks passed");
    return 0;
}
