#!/usr/bin/env python3
"""What a game module needs from the system, and how much of it SacredFire already provides.
Run from the repo root (reads only; writes out/<module>-imports.txt):

    python3 tools/module_imports.py nhl19/nhl/data/kettle/eawebkit.prx

For each library the module imports from: how many functions, how many the port already has
(the runtime's own tables in src/*.c, shadPS4's HLE libraries in gpu/shadps4/core/libraries, or
the natively linked libc.prx / libSceFios2.prx of the game), and which are missing, by name
(names from share/aerolib.inl or out/aerolib.inl; "?" when unknown). Also the module's exports,
which the game would look up by name with sceKernelDlsym.
"""
import argparse
import collections
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / 'scripts'))
from link_modules import module  # noqa: E402


def aerolib():
    for path in (root / 'share' / 'aerolib.inl', root / 'out' / 'aerolib.inl'):
        if path.exists():
            return dict(re.findall(r'STUB\("([^"]+)",\s*([^)\s]+)\s*\)', path.read_text(errors='replace')))
    print('module_imports: no aerolib.inl in share/ or out/ (run the game once): names will be "?"', file=sys.stderr)
    return {}


def provided(game):
    runtime = set()
    for f in (root / 'src').glob('*.c'):
        runtime.update(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,', f.read_text(errors='replace')))
    hle = set()
    for f in (root / 'gpu' / 'shadps4' / 'core' / 'libraries').rglob('*.cpp'):
        hle.update(re.findall(r'LIB_FUNCTION\(\s*"([A-Za-z0-9+\-]{11})"', f.read_text(errors='replace')))
    native = set()
    for name in ('libc.prx', 'libSceFios2.prx'):
        path = game / 'sce_module' / name
        if path.exists():
            m = module(path)
            native.update(s['identity'][0] for s in m['symbols'] if s['section'] and s['identity'])
    return runtime, hle, native


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('prx', type=Path)
    p.add_argument('--game', type=Path, default=None, help='game folder (default: the folder with eboot.bin above the module)')
    args = p.parse_args()
    game = args.game or next((d for d in args.prx.resolve().parents if (d / 'eboot.bin').exists()), None)
    if game is None:
        sys.exit('module_imports: no eboot.bin above the module: pass --game')
    names = aerolib()
    runtime, hle, native = provided(game)
    m = module(args.prx)

    print(f'{args.prx.name}: {len(m["elf"]):,} bytes; needs modules: '
          + ', '.join(sorted({f'{n} (v{v})' for n, v in m['modules'].values()})))
    imports, exports = collections.defaultdict(list), []
    seen = set()
    for s in m['symbols']:
        if not s['identity']:
            continue
        nid, (lib, _), _ = s['identity']
        if s['section']:
            if s['binding'] in (1, 2):
                exports.append((lib, nid, names.get(nid, '?')))
        elif (nid, lib) not in seen:
            seen.add((nid, lib))
            name = names.get(nid, '?')
            where = ('runtime' if name in runtime else 'shadPS4' if nid in hle else 'native' if nid in native else None)
            imports[lib].append((nid, name, where))

    total = sum(len(v) for v in imports.values())
    have = sum(1 for v in imports.values() for _, _, w in v if w)
    print(f'imports: {total} functions from {len(imports)} libraries; the port has {have}, missing {total - have}')
    lines = []
    for lib in sorted(imports, key=lambda k: -sum(1 for *_, w in imports[k] if not w)):
        entries = imports[lib]
        missing = [e for e in entries if not e[2]]
        print(f'  {lib:32} {len(entries):5} imported, {len(missing):5} missing')
        lines.append(f'[{lib}] {len(entries)} imported, {len(missing)} missing')
        for nid, name, where in sorted(entries, key=lambda e: (bool(e[2]), e[1])):
            lines.append(f'  {"ok " + where if where else "MISSING":14} {nid} {name}')
    lines.append(f'[exports] {len(exports)}')
    lines += [f'  {lib} {nid} {name}' for lib, nid, name in sorted(exports)]
    target = root / 'out' / f'{args.prx.stem}-imports.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'exports: {len(exports)} (libraries: {", ".join(sorted({e[0] for e in exports})) or "none"})')
    print(f'full list: {target}')


if __name__ == '__main__':
    main()
