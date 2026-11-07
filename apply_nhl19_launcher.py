#!/usr/bin/env python3
r"""NHL 19 port: turn the Windows launcher (packaging/windows/sacredfire_launcher.c, UFC 3's) into
NHL 19's. Run once from the repo root, then build and package again:

    python3 apply_nhl19_launcher.py
    bash scripts/build-msys2.sh

What changes (UFC 3's code stays where NHL 19 does not touch it):
  * Identity: NHL 19 CUSA11126 with the 1.60 update in every message, the window title and the
    banner; the update and title passed to patches.py (01.60, CUSA11126); the shader cache folder
    (user\cache\CUSA11126); the default game profile nhl19; data in %LOCALAPPDATA%\NHL19Native
    (not UFC 3's UFC3Native, so the two never share settings, saves or caches).
  * Graphics defaults = scripts/run-msys2.sh's for nhl19 (tested): readbacks 1 + linear readbacks
    (3D visible, calm jerseys), write windows and stepped writes off (they shake the jerseys),
    pre-upload on (the menu needs it), flip rate 0 at the real 60 Hz (UFC 3's 480 Hz virtual
    vblank is not used), GPU buffer copies left at the runtime default as in the bash runs.
  * Network: always the network mode without a server (BB_ONLINE_SERVER=127.0.0.1), which gets
    NHL 19 past its offline start-up crash; UFC 3's Online play (Join / Host server with
    tools\ufc3_server.py) is locked to Offline in the window and never started.
  * Video: 720p / 1080p only (UFC 3's 2K / 4K are scene patches for UFC 3); "Skip the intro
    videos" is off and locked (UFC 3's black clip).
An environment variable set before the launcher (a .bat) still wins over each default.
"""
import sys
from pathlib import Path

p = Path('packaging/windows/sacredfire_launcher.c')
if not p.exists():
    sys.exit('run this from the repo root (packaging/windows/sacredfire_launcher.c not found)')
s = p.read_text(encoding='utf-8')
if 'NHL19Native' in s:
    sys.exit('already applied')

failed = []


def sub(old, new, count=1):
    """Replace old with new; count = the number of occurrences expected (0: all, at least one)."""
    global s
    n = s.count(old)
    if n == 0 or (count and n != count):
        failed.append(f'{old[:70]!r}: found {n}, expected {count or "1+"}')
        return
    s = s.replace(old, new)


# ---- identity ----
sub('EA SPORTS UFC 3 CUSA06534 with the 1.14 update (CUSA06534-patch)', 'NHL 19 CUSA11126 with the 1.60 update (CUSA11126-patch)')
sub('the 1.14 update', 'the 1.60 update', 0)
sub('L"--app-version", L"01.14", L"--title-id", L"CUSA06534"', 'L"--app-version", L"01.60", L"--title-id", L"CUSA11126"')
sub('CUSA06534', 'CUSA11126', 0)
sub('if (!profile[0]) snprintf(profile, sizeof(profile), "ufc3");', 'if (!profile[0]) snprintf(profile, sizeof(profile), "nhl19");')
sub('join(data, PATHW, local, L"UFC3Native");', 'join(data, PATHW, local, L"NHL19Native");')
sub('L"UFC3NativeControls"', 'L"NHL19NativeControls"', 2)
sub('L"UFC3NativeLauncher"', 'L"NHL19NativeLauncher"')
sub('EA SPORTS UFC 3 - native PC port', 'NHL 19 - native PC port', 0)
sub('L"EA SPORTS UFC 3 \\x2014 native PC port"', 'L"NHL 19 \\x2014 native PC port"')
sub('logw(L"UFC 3: ', 'logw(L"NHL 19: ', 0)

# ---- network: the server-less network mode, never UFC 3's online play ----
sub('    const wchar_t *online = settings.online_mode == ONLINE_HOST ? settings.host_address :',
    '    /* NHL 19: no online play (UFC 3\'s Join/Host is for its own server); the network mode without a\n'
    '     * server, BB_ONLINE_SERVER=127.0.0.1, gets the game past its offline start-up crash (a null\n'
    '     * online-service global read by the sim thread). */\n'
    '    settings.online_mode = ONLINE_OFFLINE;\n'
    '    const wchar_t *online = settings.online_mode == ONLINE_HOST ? settings.host_address :')
sub('    game_env(L"BB_ONLINE_SERVER", online);',
    '    (void)online;\n    game_env(L"BB_ONLINE_SERVER", L"127.0.0.1");')
sub('''static void online_controls(void) {
    int mode = (int)SendMessageW(control(ID_MODE), CB_GETCURSEL, 0, 0);''',
    '''static void online_controls(void) {
    /* NHL 19: Online play stays Offline (the launcher sets the network mode itself). */
    SendMessageW(control(ID_MODE), CB_SETCURSEL, ONLINE_OFFLINE, 0);
    EnableWindow(control(ID_MODE), FALSE);
    int mode = ONLINE_OFFLINE;''')

# ---- graphics: run-msys2.sh's nhl19 defaults ----
sub('    game_env(L"BB_READBACKS", L"2"); game_env(L"BB_READBACK_LINEAR", L"1"); game_env(L"BB_GPU_WRITE_TWINS", L"0");',
    '    /* NHL 19 (tested with scripts/run-msys2.sh): readbacks 1 with the linear readbacks draws the\n'
    '     * players (black without the linear ones) with calm jerseys (2 left a slight jitter). */\n'
    '    game_env(L"BB_READBACKS", L"1"); game_env(L"BB_READBACK_LINEAR", L"1"); game_env(L"BB_GPU_WRITE_TWINS", L"0");')
sub('    game_env(L"BB_WRITE_WINDOW", L"1"); game_env(L"BB_WRITE_STEP", L"1");\n'
    '    game_env(L"BB_FLIP_RATE", L"0"); game_env(L"BB_VBLANK_HZ", L"480");',
    '    /* NHL 19: write windows and stepped writes send the players\' cloth vertices to the GPU while\n'
    '     * the game still writes them (the jerseys shake): off. 60 FPS throughout (the game asks for\n'
    '     * 30 in matches) at the real 60 Hz: UFC 3\'s 480 Hz virtual vblank is not NHL 19\'s timing. */\n'
    '    game_env(L"BB_WRITE_WINDOW", L"0"); game_env(L"BB_WRITE_STEP", L"0");\n'
    '    game_env(L"BB_FLIP_RATE", L"0"); game_env(L"BB_VBLANK_HZ", L"60");')
sub('    game_env(L"BB_PREUPLOAD", L"1");',
    '    game_env(L"BB_PREUPLOAD", L"1"); /* NHL 19: the main menu needs it too */')
sub('    game_env(L"BB_COPY_GPU_BUFFERS", L"1");',
    '    game_env(L"BB_COPY_GPU_BUFFERS", NULL); /* NHL 19: the runtime default, as tested */')

# ---- video panel: no UFC 3 scene patches, no UFC 3 movie clip ----
sub('static const wchar_t *const resolution_names[] = {L"1280x720", L"1920x1080", L"2560x1440 (2K, experimental)", L"3840x2160 (4K, experimental)"};',
    '/* NHL 19: 720p / 1080p (UFC 3\'s 2K / 4K are scene patches for UFC 3; a saved 2 or 3 from elsewhere reads as 720p). */\n'
    'static const wchar_t *const resolution_names[] = {L"1280x720", L"1920x1080"};')
sub('.sharpness = 2, .skip_movies = 1};', '.sharpness = 2, .skip_movies = 0};')
sub('        game_env(L"BB_SKIP_MOVIES", settings.skip_movies && exists(clip) ? L"1" : L"0");',
    '        game_env(L"BB_SKIP_MOVIES", L"0"); /* NHL 19: its own videos (the clip is UFC 3\'s) */')
sub('''    set_checked(ID_NEO, forced || neo_user);
    EnableWindow(control(ID_NEO), !running && !forced);''',
    '''    set_checked(ID_NEO, forced || neo_user);
    EnableWindow(control(ID_NEO), !running && !forced);
    set_checked(ID_SKIPMOVIES, 0); EnableWindow(control(ID_SKIPMOVIES), FALSE); /* NHL 19: UFC 3's clip */''')

if failed:
    sys.exit('packaging/windows/sacredfire_launcher.c was not as expected (nothing changed):\n  ' + '\n  '.join(failed))
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: packaging/windows/sacredfire_launcher.c (NHL 19 launcher: CUSA11126 1.60, NHL19Native data, '
      'NHL 19 graphics defaults, network mode, Online play and 2K/4K/skip movies locked)')
