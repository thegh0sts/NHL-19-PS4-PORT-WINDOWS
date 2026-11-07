#!/usr/bin/env python3
r"""NHL 19: the PS4 keyboard library (libSceIme keyboard functions), answered "no keyboard connected".
Run once from the repo root:

    python3 apply_ime_keyboard.py

NHL 19 calls sceImeKeyboardOpen(SCE_USER_SERVICE_USER_ID_EVERYONE, ...) during start-up, to receive
key presses from a USB keyboard; the runtime had no entry for it ("STOP: first unsupported PS4
import ... sceImeKeyboardOpen"). The game is played with the controller (keyboards reach it as pad
buttons through SDL), so these report that no keyboard is connected, as a PS4 without one does:
  sceImeKeyboardOpen, GetResourceId, GetInfo      -> 0x80BC0008 (no keyboard)
  sceImeKeyboardClose, SetMode, sceImeUpdate      -> 0 (nothing to do)
Then rebuild: bash scripts/build-msys2.sh
"""
import sys
from pathlib import Path

p = Path('src/runtime_services.c')
if not p.exists():
    sys.exit('run this from the repo root (src/runtime_services.c not found)')
s = p.read_text(encoding='utf-8')
if 'ime_no_keyboard' in s:
    sys.exit('already applied')

anchor_fn = 'static const RuntimeExport exports[]={'
anchor_tab = '{"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},'
if s.count(anchor_fn) != 1 or s.count(anchor_tab) != 1:
    sys.exit('src/runtime_services.c: the exports table was not found as expected (nothing changed)')
fn = ('/* NHL 19: libSceIme keyboard functions. No USB keyboard is connected (keyboard input reaches the\n'
      ' * game as pad buttons through SDL), so opening it reports exactly that. */\n'
      '#define IME_ERROR_NO_KEYBOARD ((int32_t)0x80BC0008)\n'
      'static ABI int32_t ime_no_keyboard(void) { return IME_ERROR_NO_KEYBOARD; }\n\n')
s = s.replace(anchor_fn, fn + anchor_fn, 1)
s = s.replace(anchor_tab, anchor_tab +
              '\n    {"sceImeKeyboardOpen",ime_no_keyboard}, {"sceImeKeyboardGetResourceId",ime_no_keyboard},'
              ' {"sceImeKeyboardGetInfo",ime_no_keyboard},'
              '\n    {"sceImeKeyboardClose",ok_void}, {"sceImeKeyboardSetMode",ok_void}, {"sceImeUpdate",ok_void},', 1)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/runtime_services.c (libSceIme keyboard: no keyboard connected)')
