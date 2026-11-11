#!/usr/bin/env python3
r"""Red-zone patch: leave alone functions that do not use the red zone the way compilers do.
Run once from the repo root (after apply_nhl24_redzone.py and apply_redzone_range.py), rebuild:

    python3 apply_redzone_strict.py
    bash scripts/build-msys2.sh
    BB_READBACKS=2 BB_SKIP_PREPARE=1 bash scripts/run-msys2.sh nhl19

NHL 19 stopped at its splash video with the red-zone patch on its own code (fine on the system
libraries, fine in NHL 24). A compiler only uses the red zone in a function that calls nothing,
and never moves rsp after using it except to pop saved registers and return. Hand-written code
(video decoders often have some) may store below rsp and then move rsp (sub/add/lea/mov/push) and
reach the same bytes with a positive offset; moving only the negative-offset accesses breaks that.

src/runtime_fp.c, runtime_redzone_patch_image()
    A function's red-zone accesses are left alone (all of them) if the function contains a call,
    or changes rsp other than by pop/ret after its first red-zone access. The log says how many:
    "Red zone: N accesses in M functions left alone (a call, or rsp moved after the red zone is
    used)". BB_REDZONE_ALL=1 patches them anyway (the previous behaviour).
"""
import sys
from pathlib import Path

p = Path('src/runtime_fp.c')
if not p.exists():
    sys.exit('run this from the repo root (src/runtime_fp.c not found)')
s = p.read_text(encoding='utf-8')
if 'BB_REDZONE_ALL' in s:
    sys.exit('already applied')
if 'BB_REDZONE_RANGE' not in s:
    sys.exit('apply apply_nhl24_redzone.py and apply_redzone_range.py first (nothing changed)')

subs = [
    ('    /* Pass 1: the sites, direct targets and switch-table entries. */\n'
     '    for (uint64_t r = 0; r < range_count; ++r) {\n'
     '        for (uint64_t at = ranges[r].start, end = ranges[r].end; at < end;) {\n'
     '            ZydisDecodedInstruction in;\n'
     '            ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];\n',
     '    /* Functions whose red zone is not used the compiler way are left alone (BB_REDZONE_ALL=1: not). */\n'
     '    const int all_functions = getenv("BB_REDZONE_ALL") && getenv("BB_REDZONE_ALL")[0] == \'1\';\n'
     '    uint64_t skipped_accesses = 0, skipped_functions = 0;\n'
     '    /* Pass 1: the sites, direct targets and switch-table entries. */\n'
     '    for (uint64_t r = 0; r < range_count; ++r) {\n'
     '        const uint64_t function_first = count;\n'
     '        int used = 0, unusual = 0;\n'
     '        for (uint64_t at = ranges[r].start, end = ranges[r].end; at < end;) {\n'
     '            ZydisDecodedInstruction in;\n'
     '            ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];\n'),
    ('                sites[2 * count] = at; sites[2 * count++ + 1] = end;\n'
     '            }\n'
     '            at = next;\n'
     '        }\n'
     '    }\n'
     '    free(ranges);\n'
     '    stats->found = count;\n',
     '                sites[2 * count] = at; sites[2 * count++ + 1] = end;\n'
     '                used = 1;\n'
     '            }\n'
     '            if (in.meta.category == ZYDIS_CATEGORY_CALL) unusual = 1;\n'
     '            else if (used && in.mnemonic != ZYDIS_MNEMONIC_POP && in.meta.category != ZYDIS_CATEGORY_RET)\n'
     '                for (unsigned i = 0; i < in.operand_count; ++i)\n'
     '                    if (op[i].type == ZYDIS_OPERAND_TYPE_REGISTER && (op[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&\n'
     '                        (op[i].reg.value == ZYDIS_REGISTER_RSP || op[i].reg.value == ZYDIS_REGISTER_ESP || op[i].reg.value == ZYDIS_REGISTER_SP))\n'
     '                        unusual = 1;\n'
     '            at = next;\n'
     '        }\n'
     '        if (unusual && !all_functions && count > function_first && range_count && stats->functions) {\n'
     '            skipped_accesses += count - function_first;\n'
     '            ++skipped_functions;\n'
     '            count = function_first;\n'
     '        }\n'
     '    }\n'
     '    free(ranges);\n'
     '    if (skipped_functions)\n'
     '        printf("Red zone: %" PRIu64 " accesses in %" PRIu64 " functions left alone (a call, or rsp moved after the red zone is used)\\n",\n'
     '               skipped_accesses, skipped_functions);\n'
     '    stats->found = count;\n'),
]
missing = [old[:70] for old, _ in subs if s.count(old) != 1]
if missing:
    sys.exit('src/runtime_fp.c was not as expected (nothing changed); not found:\n  ' + '\n  '.join(missing))
for old, new in subs:
    s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/runtime_fp.c (functions with an unusual red zone are left alone; BB_REDZONE_ALL=1 for all)')
