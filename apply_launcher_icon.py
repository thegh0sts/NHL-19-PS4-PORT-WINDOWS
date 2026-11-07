#!/usr/bin/env python3
r"""NHL 19 port: an icon for the launcher (NHL19.exe) and the game window, and NHL 19 in the
programs' Properties -> Details. Run once from the repo root, with the icon in place:

    packaging\windows\NHL19.ico      (your own icon; see below)
    python3 apply_launcher_icon.py
    bash scripts/build-msys2.sh

The launcher already asks for icon resource 1 for its window (LoadIconW(..., MAKEINTRESOURCEW(1)));
its resource file never had one, so Windows showed the default program icon. This adds
`1 ICON "NHL19.ico"` to packaging/windows/sacredfire-launcher.rc (the launcher) and
sacredfire-game.rc (sacredfire-game.exe and bb-gpu-capabilities.exe: Explorer, taskbar and the
game window), and replaces CTR's text in their version blocks with NHL 19's.
"""
import sys
from pathlib import Path

folder = Path('packaging/windows')
if not (folder / 'sacredfire-launcher.rc').exists():
    sys.exit('run this from the repo root (packaging/windows/sacredfire-launcher.rc not found)')
if not (folder / 'NHL19.ico').exists():
    sys.exit('packaging/windows/NHL19.ico is missing: put the icon there first')

OLD_DESC = 'SacredFire: native port of Crash Team Racing Nitro-Fueled (PS4) built on bbport'
OLD_LEGAL = 'GPL-2.0-or-later. Contains code from bbport and shadPS4. Not affiliated with Sony, Activision or Beenox.'
NEW_LEGAL = 'GPL-2.0-or-later. Contains code from bbport and shadPS4. Not affiliated with Sony, Electronic Arts or the NHL.'
for name, description, product in (('sacredfire-launcher.rc', 'NHL 19 native PC port: launcher', 'NHL 19 native PC port'),
                                   ('sacredfire-game.rc', 'NHL 19 native PC port: game', 'NHL 19 native PC port')):
    p = folder / name
    s = p.read_text(encoding='utf-8')
    if '1 ICON "NHL19.ico"' in s:
        print(f'{name}: already has the icon')
        continue
    manifest = f'1 24 "{name.replace(".rc", ".manifest")}"'
    if s.count(manifest) != 1:
        sys.exit(f'{p}: the manifest line was not found as expected (nothing changed in it)')
    s = s.replace(manifest, manifest + '\n1 ICON "NHL19.ico"', 1)
    s = s.replace(f'"FileDescription", "{OLD_DESC}"', f'"FileDescription", "{description}"')
    s = s.replace(f'"LegalCopyright", "{OLD_LEGAL}"', f'"LegalCopyright", "{NEW_LEGAL}"')
    s = s.replace('"ProductName", "SacredFire"', f'"ProductName", "{product}"')
    p.write_text(s, encoding='utf-8', newline='\n')
    print(f'changed: {p} (icon NHL19.ico, NHL 19 version text)')
