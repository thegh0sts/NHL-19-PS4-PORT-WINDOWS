#!/usr/bin/env python3
r"""NHL 19 port: teach scripts/game_check.py the NHL 19 executable. Run once from the repo root:

    python3 apply_nhl19_profile.py

  * SUPPORTED gains NHL 19 CUSA11126 1.60 (US, firmware 5.50 SDK), keyed on its loaded image's
    sha256 like UFC 3's, with the same two system modules to link (libc.prx, libSceFios2.prx);
    profile name "nhl19" (BB_GAME_PROFILE).
  * The config-file check (CONFIG_FILES: UFC 3's samplegame/.../memory.cfg) only runs for UFC 3:
    NHL 19 keeps its data in nhl/ and databigs/.
UFC 3 keeps working as before.
"""
import sys
from pathlib import Path

p = Path('scripts/game_check.py')
if not p.exists():
    sys.exit('run this from the repo root (scripts/game_check.py not found)')
s = p.read_text(encoding='utf-8')
if "name='nhl19'" in s:
    sys.exit('already applied')

old = """        modules=('libc.prx', 'libSceFios2.prx'), patches=False),
}
BY_TITLE"""
new = """        modules=('libc.prx', 'libSceFios2.prx'), patches=False),
    # NHL 19 CUSA11126 (US, UP0006) 1.60, SDK 5.50: same engine family as UFC 3 (EA Sports Ignite).
    '3ff8bd90d63536e6757d8a559a8770cf930ebb1aa2c851ed99d7ff4da763b620': dict(
        name='nhl19', title='CUSA11126', version='01.60', game='NHL 19', update='CUSA11126-patch',
        modules=('libc.prx', 'libSceFios2.prx'), patches=False),
}
BY_TITLE"""
if s.count(old) != 1:
    sys.exit('SUPPORTED was not found as expected (nothing changed)')
s = s.replace(old, new, 1)

old2 = '''    damaged = []
    for name, markers in CONFIG_FILES.items():'''
new2 = '''    damaged = []
    if sfo_info(game).get('TITLE_ID') not in (None, 'CUSA06534'):
        return damaged  # CONFIG_FILES are UFC 3's (samplegame/...); other games keep their data elsewhere
    for name, markers in CONFIG_FILES.items():'''
if s.count(old2) != 1:
    sys.exit('damaged_configs was not found as expected (nothing changed)')
s = s.replace(old2, new2, 1)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: scripts/game_check.py (NHL 19 CUSA11126 1.60 profile)')
