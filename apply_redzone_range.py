#!/usr/bin/env python3
r"""Red-zone patch: limit it to a range of the image (to find what it breaks in NHL 19).
Run once from the repo root (after apply_nhl24_redzone.py), then rebuild:

    python3 apply_redzone_range.py
    bash scripts/build-msys2.sh

    BB_REDZONE_RANGE=0-6320000 ...        only the game's own code (NHL 19: modules start at 0x6320000)
    BB_REDZONE_RANGE=6320000-ffffffff ... only the linked system modules (libc, Fios2, ...)

BB_REDZONE_RANGE=<from>-<to> (hex image offsets; the log's "Starting linked module N at image
offset 0x..." lines show where each module starts) patches only red-zone accesses in
[from, to). Without it everything is patched, as before. The summary line says which range.

src/runtime_fp.c   runtime_redzone_patch_image(): the range filter and the note in its summary
"""
import sys
from pathlib import Path

p = Path('src/runtime_fp.c')
if not p.exists():
    sys.exit('run this from the repo root (src/runtime_fp.c not found)')
s = p.read_text(encoding='utf-8')
if 'BB_REDZONE_RANGE' in s:
    sys.exit('already applied')
if 'runtime_redzone_patch_image' not in s:
    sys.exit('apply apply_nhl24_redzone.py first (nothing changed)')

subs = [
    ('    /* Pass 1: the sites, direct targets and switch-table entries. */\n',
     '    /* BB_REDZONE_RANGE=<from>-<to> (hex image offsets): only accesses in [from, to). */\n'
     '    uint64_t range_from = 0, range_to = UINT64_MAX;\n'
     '    if (getenv("BB_REDZONE_RANGE")) {\n'
     '        char *rest = NULL;\n'
     '        range_from = strtoull(getenv("BB_REDZONE_RANGE"), &rest, 16);\n'
     '        if (rest && *rest == \'-\') range_to = strtoull(rest + 1, NULL, 16);\n'
     '        printf("Red zone: only accesses at image offsets 0x%" PRIx64 "-0x%" PRIx64 " (BB_REDZONE_RANGE)\\n", range_from, range_to);\n'
     '    }\n'
     '    /* Pass 1: the sites, direct targets and switch-table entries. */\n'),
    ('            if (redzone_site(&in, op, image + at)) {\n',
     '            if (at >= range_from && at < range_to && redzone_site(&in, op, image + at)) {\n'),
    ('            if (redzone_site(&in, op, image + here)) q = emit_shifted(q, image + here, &in);\n',
     '            if (here >= range_from && here < range_to && redzone_site(&in, op, image + here)) q = emit_shifted(q, image + here, &in);\n'
     '            else if (redzone_site(&in, op, image + here)) ok = 0; /* outside the range: stays where it is */\n'),
]
missing = [old[:70] for old, _ in subs if s.count(old) != 1]
if missing:
    sys.exit('src/runtime_fp.c was not as expected (nothing changed); not found:\n  ' + '\n  '.join(missing))
for old, new in subs:
    s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/runtime_fp.c (BB_REDZONE_RANGE limits the red-zone patch to a range of the image)')
