#!/usr/bin/env python3
r"""NHL 19: run the PS4 CPU's AMD-only SSE4a instructions on Intel CPUs. Run once from the repo root:

    python3 apply_sse4a.py

NHL 19's render thread uses INSERTQ (AMD SSE4a, which the PS4's Jaguar CPU has and Intel CPUs do
not): "Fault: exception 0xc000001d (illegal instruction) at guest offset 0x26c0c56". This adds, to
src/platform/platform_win.c's exception handler, an emulator for the four SSE4a instructions
(EXTRQ, INSERTQ in both forms, MOVNTSS, MOVNTSD): the instruction is carried out on the thread's
saved registers and the game continues after it. Only guest code is handled; anything else still
ends in the fault report. The log says when it first happens; BB_SSE4A_LOG=1 lists every one.
On AMD CPUs (which have SSE4a) it never runs.
Then rebuild: bash scripts/build-msys2.sh
"""
import sys
from pathlib import Path

p = Path('src/platform/platform_win.c')
if not p.exists():
    sys.exit('run this from the repo root (src/platform/platform_win.c not found)')
s = p.read_text(encoding='utf-8')
if 'sse4a_emulate' in s:
    sys.exit('already applied')
EMU = r'''/* NHL 19: AMD SSE4a instructions on Intel CPUs. The PS4's Jaguar CPU has SSE4a (EXTRQ, INSERTQ,
 * MOVNTSS, MOVNTSD); Intel CPUs raise an illegal-instruction exception on them. A guest
 * instruction of these four is carried out here on the thread's saved registers and the thread
 * continues after it. Register forms of EXTRQ/INSERTQ and the stores of MOVNTSS/MOVNTSD are
 * covered; anything else (or a segment prefix) is left to the fault report.
 * BB_SSE4A_LOG=1 prints each emulated site the first time and a running count. */
static uint64_t *sse4a_gpr(CONTEXT *c, unsigned n) {
    switch (n & 15) {
    case 0: return (uint64_t *)&c->Rax;  case 1: return (uint64_t *)&c->Rcx;
    case 2: return (uint64_t *)&c->Rdx;  case 3: return (uint64_t *)&c->Rbx;
    case 4: return (uint64_t *)&c->Rsp;  case 5: return (uint64_t *)&c->Rbp;
    case 6: return (uint64_t *)&c->Rsi;  case 7: return (uint64_t *)&c->Rdi;
    case 8: return (uint64_t *)&c->R8;   case 9: return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10; case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12; case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14; default: return (uint64_t *)&c->R15;
    }
}
static M128A *sse4a_xmm(CONTEXT *c, unsigned n) { return &c->Xmm0 + (n & 15); }
static uint64_t sse4a_mask(unsigned length) { return length == 0 || length >= 64 ? ~0ull : (1ull << length) - 1; }
/* The memory operand of the ModRM byte at p (mod != 3): the bytes used from ModRM on; *rip_relative
 * set when the address is RIP + *out (from the end of the instruction). */
static int sse4a_address(CONTEXT *c, const unsigned char *p, unsigned rex, uint64_t *out, int *rip_relative) {
    const unsigned modrm = p[0], mod = modrm >> 6, rm = modrm & 7;
    int used = 1;
    uint64_t address = 0;
    *rip_relative = 0;
    if (rm == 5 && mod == 0) {
        int32_t d; memcpy(&d, p + 1, 4);
        *out = (uint64_t)(int64_t)d;
        *rip_relative = 1;
        return 5;
    }
    if (rm == 4) {  /* SIB */
        const unsigned sib = p[1], scale = sib >> 6, index = ((sib >> 3) & 7) | (rex & 2 ? 8 : 0), base = (sib & 7) | (rex & 1 ? 8 : 0);
        used = 2;
        if (index != 4) address += *sse4a_gpr(c, index) << scale;
        if ((sib & 7) == 5 && mod == 0) { int32_t d; memcpy(&d, p + 2, 4); address += (uint64_t)(int64_t)d; used += 4; }
        else address += *sse4a_gpr(c, base);
    } else {
        address = *sse4a_gpr(c, rm | (rex & 1 ? 8 : 0));
    }
    if (mod == 1) { address += (uint64_t)(int64_t)(int8_t)p[used]; used += 1; }
    else if (mod == 2) { int32_t d; memcpy(&d, p + used, 4); address += (uint64_t)(int64_t)d; used += 4; }
    *out = address;
    return used;
}
static int sse4a_emulate(CONTEXT *c) {
    const unsigned char *p = (const unsigned char *)(uintptr_t)c->Rip;
    unsigned char code[16];
    if (!platform_safe_read((uintptr_t)p, code, sizeof code)) return 0;
    unsigned i = 0, prefix = code[i++], rex = 0;
    if (prefix != 0x66 && prefix != 0xF2 && prefix != 0xF3) return 0;
    if ((code[i] & 0xF0) == 0x40) rex = code[i++] & 0x0F;
    if (code[i++] != 0x0F) return 0;
    const unsigned op = code[i++], modrm = code[i];
    const unsigned mod = modrm >> 6, reg = ((modrm >> 3) & 7) | (rex & 4 ? 8 : 0), rm = (modrm & 7) | (rex & 1 ? 8 : 0);
    const char *name = NULL;
    if ((op == 0x78 || op == 0x79) && mod == 3 && prefix != 0xF3) {
        unsigned length, index;
        if (prefix == 0xF2) {  /* INSERTQ */
            M128A *dst = sse4a_xmm(c, reg); const M128A *src = sse4a_xmm(c, rm);
            if (op == 0x78) { length = code[i + 1] & 63; index = code[i + 2] & 63; i += 3; }
            else { length = (unsigned)((uint64_t)src->High & 63); index = (unsigned)(((uint64_t)src->High >> 8) & 63); i += 1; }
            const uint64_t mask = sse4a_mask(length);
            dst->Low = (dst->Low & ~(mask << index)) | ((src->Low & mask) << index);
            name = "insertq";
        } else {  /* EXTRQ (66) */
            M128A *dst;
            if (op == 0x78) {
                if (((modrm >> 3) & 7) != 0) return 0;
                dst = sse4a_xmm(c, rm); length = code[i + 1] & 63; index = code[i + 2] & 63; i += 3;
            } else {
                const M128A *src = sse4a_xmm(c, rm);
                dst = sse4a_xmm(c, reg); length = (unsigned)(src->Low & 63); index = (unsigned)((src->Low >> 8) & 63); i += 1;
            }
            dst->Low = (dst->Low >> index) & sse4a_mask(length);
            name = "extrq";
        }
    } else if (op == 0x2B && mod != 3 && (prefix == 0xF2 || prefix == 0xF3)) {  /* MOVNTSD / MOVNTSS */
        uint64_t address;
        int rip_relative;
        const int used = sse4a_address(c, code + i, rex, &address, &rip_relative);
        if (rip_relative) address += c->Rip + i + (unsigned)used;  /* no immediate follows these stores */
        const M128A *src = sse4a_xmm(c, reg);
        if (prefix == 0xF2) { uint64_t v = src->Low; memcpy((void *)(uintptr_t)address, &v, 8); name = "movntsd"; }
        else { uint32_t v = (uint32_t)src->Low; memcpy((void *)(uintptr_t)address, &v, 4); name = "movntss"; }
        i += (unsigned)used;
    } else {
        return 0;
    }
    static int log = -1;
    static volatile long total;
    if (log < 0) { const char *e = getenv("BB_SSE4A_LOG"); log = e && e[0] == '1'; }
    const long n = InterlockedIncrement(&total);
    if (n == 1) fprintf(stderr, "SSE4a: emulating AMD-only %s at guest +0x%llx (this CPU lacks SSE4a; BB_SSE4A_LOG=1 for every site)\n",
                        name, (unsigned long long)(c->Rip - (uintptr_t)guest_image));
    else if (log) fprintf(stderr, "SSE4a: %s at guest +0x%llx (%ld so far)\n", name, (unsigned long long)(c->Rip - (uintptr_t)guest_image), n);
    if (!(n & (n - 1)) && n >= 1024) fprintf(stderr, "SSE4a: %ld instructions emulated so far\n", n);
    c->Rip += i;
    return 1;
}
'''
anchor = 'static PlatformStepHandler step_handler;'
call_after = '    if (code == EXCEPTION_SINGLE_STEP && step_handler && step_handler(info)) return EXCEPTION_CONTINUE_EXECUTION;\n'
if s.count(anchor) != 1 or s.count(call_after) != 1:
    sys.exit('platform_win.c: the exception handler was not found as expected (nothing changed)')
s = s.replace(anchor, EMU + anchor, 1)
s = s.replace(call_after, call_after +
              '    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && guest_image &&\n'
              '        (uintptr_t)info->ContextRecord->Rip - (uintptr_t)guest_image < guest_image_size &&\n'
              '        sse4a_emulate(info->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;\n', 1)
for header in ('#include <string.h>', '#include <stdlib.h>'):
    if header not in s:
        s = header + '\n' + s
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/platform/platform_win.c (SSE4a emulation for Intel CPUs)')
