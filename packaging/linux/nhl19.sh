#!/usr/bin/env bash
# NHL 19 native port (Linux test release). Usage:
#   ./nhl19.sh                      the launcher window (game folder, settings, Play)
#   ./nhl19.sh --play               plays with the launcher's settings, without the window
#   ./nhl19.sh /path/to/CUSA11126   plays that game folder (CUSA11126-patch beside it is merged in
#                                   automatically) and remembers it
# Settings: ~/.local/share/NHL19Native/sacredfire.ini (the launcher's, the same file as on Windows);
# nhl19.conf there takes extra BB_* variables. A variable set in the environment wins over both.
# Saves and the shader cache: ~/.local/share/NHL19Native/user. Logs: a logs folder beside this
# script (or the AppImage), or ~/.local/share/NHL19Native/logs when that one can't be written.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
data=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/NHL19Native}
mkdir -p "$data"
ini=$data/sacredfire.ini
say() {  # a message on the terminal, and in a dialog when started from a desktop or Steam
    echo "NHL 19: $1" >&2
    if [[ ! -t 2 ]]; then
        if command -v zenity >/dev/null; then zenity --error --title 'NHL 19' --text "$1" 2>/dev/null || true
        elif command -v kdialog >/dev/null; then kdialog --title 'NHL 19' --error "$1" 2>/dev/null || true; fi
    fi
}
command -v python3 >/dev/null || { say 'python3 is needed: install it with your package manager.'; exit 1; }
# Reads (one key) or sets (key value) a line of sacredfire.ini, keeping the others.
ini_value() {
    python3 - "$ini" "$@" <<'PY'
import sys
path, key = sys.argv[1], sys.argv[2]
try:
    lines = open(path, encoding='utf-8-sig').read().splitlines()
except FileNotFoundError:
    lines = []
if len(sys.argv) == 3:
    print(next((l.split('=', 1)[1] for l in lines if l.split('=', 1)[0] == key and '=' in l), ''))
else:
    rest = [l for l in lines if l.split('=', 1)[0] != key]
    open(path, 'w', encoding='utf-8').write('\n'.join(rest + [key + '=' + sys.argv[3]]) + '\n')
PY
}

play=0
if [[ ${1:-} == --play ]]; then play=1; shift; fi
if [[ $# -gt 0 && -d $1 ]]; then
    ini_value game "$(cd -- "$1" && pwd)"; shift; play=1
fi
# Without --play or a folder: the launcher window, when there is a display to show it on.
if (( !play )) && [[ -x $here/bin/nhl19-launcher && ( -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ) ]]; then
    exec "$here/bin/nhl19-launcher"
fi

game=$(ini_value game)
if [[ -z $game && -f $data/game-folder ]]; then game=$(cat "$data/game-folder"); fi  # the first test build's
if [[ -z $game ]]; then
    if command -v zenity >/dev/null; then
        game=$(zenity --file-selection --directory --title 'NHL 19: choose your game folder (CUSA11126)' 2>/dev/null) || exit 1
    elif command -v kdialog >/dev/null; then
        game=$(kdialog --getexistingdirectory "$HOME" --title 'NHL 19: choose your game folder (CUSA11126)' 2>/dev/null) || exit 1
    else
        sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1
    fi
    ini_value game "$game"
fi
[[ -f $game/eboot.bin ]] || { say "no eboot.bin in $game: choose the CUSA11126 folder of your dump."; exit 1; }
game=$(python3 "$here/scripts/merge_update.py" "$game" | tail -1) || { say 'merging the 1.60 update into the game folder failed.'; exit 1; }

# The launcher's settings as the game's variables (the Windows launcher's mapping), then
# nhl19.conf's; a variable already set (the environment, or the line before) wins.
export_unset() {
    local line key value
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line =~ ^([A-Z][A-Z0-9_]*)=(.*)$ ]] || continue
        key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
        [[ -n $value && -z ${!key+x} ]] && export "$key=$value"
    done
}
[[ -f $ini ]] && export_unset < <(python3 - "$ini" <<'PY'
import sys
s = {}
for line in open(sys.argv[1], encoding='utf-8-sig').read().splitlines():
    if '=' in line:
        k, v = line.split('=', 1); s[k.strip()] = v.strip()
def pick(key, values, default):
    try: return values[int(s.get(key, default))]
    except (ValueError, IndexError): return values[default]
out = {}
if s.get('fullscreen') == '1': out['BB_FULLSCREEN'] = '1'
if s.get('validation') == '1': out['BB_VK_VALIDATION'] = '1'
if s.get('frame_stats') == '1': out['BB_FRAME_STATS'] = '1'
if s.get('clear_cache') == '1': out['BB_CLEAR_SHADER_CACHE'] = '1'
if s.get('gamepad'): out['BB_GAMEPAD'] = s['gamepad']
if s.get('player'): out['BB_USER_NAME'] = s['player']
if s.get('language'): out['BB_LANGUAGE'] = s['language']
if s.get('log_dir'): out['BB_LOG_DIR'] = s['log_dir']
out['BB_NEO'] = '1' if s.get('neo') == '1' else '0'
if s.get('fps_limit'): out['BB_FPS_LIMIT'] = s['fps_limit']
out['BB_PRESENT_MODE'] = pick('present_mode', ['Mailbox', 'Fifo', 'Immediate'], 1)
w, h = pick('resolution', [('1280', '720'), ('1920', '1080')], 1)
out['BB_INTERNAL_WIDTH'], out['BB_INTERNAL_HEIGHT'] = w, h
size = pick('display', ['', '1920x1080', '2560x1440', '3840x2160'], 0)
if size: out['BB_WINDOW_SIZE'] = size
out['BB_FSR1'] = '1' if s.get('upscaler') == '1' else '0'
sharp = pick('sharpness', ['2000', '1000', '500', '200'], 2)  # Off, Low, Medium, High
out['BB_RCAS'] = '0' if s.get('sharpness') == '0' else '1'
out['BB_RCAS_ATTENUATION'] = sharp
for k, v in out.items():
    if '\n' not in v: print(f'{k}={v}')
PY
)
[[ -f $data/nhl19.conf ]] && export_unset < "$data/nhl19.conf"

# Logs beside the AppImage (or this script), as NHL19.exe does on Windows; the data folder when
# that one can't be written (an AppImage in a read-only place, a system install).
if [[ -z ${BB_LOG_DIR:-} ]]; then
    beside=$here; [[ -n ${APPIMAGE:-} ]] && beside=$(dirname -- "$APPIMAGE")
    if mkdir -p "$beside/logs" 2>/dev/null && [[ -w $beside/logs ]]; then export BB_LOG_DIR=$beside/logs
    else export BB_LOG_DIR=$data/logs; fi
fi
export BB_PREBUILT=1 BB_DATA_DIR="$data" BB_GAME_DIR="$game" BB_SAVE_LOG=${BB_SAVE_LOG:-1}
export LD_LIBRARY_PATH="$here/bin/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# run.sh compares the release VERSION with this binary and writes both into the run log.
export BB_VERSION_EXE="$here/bin/sacredfire"
status=0
"$here/run.sh" "$@" || status=$?
if (( status != 0 && status != 130 && status != 143 )); then
    say "the game stopped with an error ($status). The log is in $BB_LOG_DIR: please attach it to a report on GitHub."
fi
exit "$status"
