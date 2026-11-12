#!/usr/bin/env bash
# NHL 19 native port (Linux test release). Usage:
#   ./nhl19.sh /path/to/CUSA11126   the first time: your game folder (CUSA11126-patch beside it
#                                   is merged in automatically); it is remembered
#   ./nhl19.sh                      later: plays with the remembered folder (a folder picker opens
#                                   when none is remembered and zenity or kdialog is installed)
# Settings: ~/.local/share/NHL19Native/nhl19.conf (KEY=value; the environment wins).
# Saves and the shader cache: ~/.local/share/NHL19Native/user. Logs: a logs folder beside this
# script (or the AppImage), or ~/.local/share/NHL19Native/logs when that one can't be written.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
[[ ${1:-} == --play ]] && shift   # accepted for the launchers of the UFC 3 port
data=${BB_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/NHL19Native}
mkdir -p "$data"
say() {  # a message on the terminal, and in a dialog when started from a desktop or Steam
    echo "NHL 19: $1" >&2
    if [[ ! -t 2 ]]; then
        if command -v zenity >/dev/null; then zenity --error --title 'NHL 19' --text "$1" 2>/dev/null || true
        elif command -v kdialog >/dev/null; then kdialog --title 'NHL 19' --error "$1" 2>/dev/null || true; fi
    fi
}
command -v python3 >/dev/null || { say 'python3 is needed: install it with your package manager.'; exit 1; }
[[ -f $data/nhl19.conf ]] || cp "$here/nhl19.conf.example" "$data/nhl19.conf"
if [[ $# -gt 0 && -d $1 ]]; then
    game=$(cd -- "$1" && pwd); shift
elif [[ -f $data/game-folder ]]; then
    game=$(cat "$data/game-folder")
elif command -v zenity >/dev/null; then
    game=$(zenity --file-selection --directory --title 'NHL 19: choose your game folder (CUSA11126)' 2>/dev/null) || exit 1
elif command -v kdialog >/dev/null; then
    game=$(kdialog --getexistingdirectory "$HOME" --title 'NHL 19: choose your game folder (CUSA11126)' 2>/dev/null) || exit 1
else
    sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1
fi
[[ -f $game/eboot.bin ]] || { say "no eboot.bin in $game: choose the CUSA11126 folder of your dump."; rm -f "$data/game-folder"; exit 1; }
printf '%s\n' "$game" > "$data/game-folder"
game=$(python3 "$here/scripts/merge_update.py" "$game" | tail -1) || { say 'merging the 1.60 update into the game folder failed.'; exit 1; }
# nhl19.conf: KEY=value lines; a variable already set in the environment wins.
while IFS= read -r line || [[ -n $line ]]; do
    [[ $line =~ ^([A-Z][A-Z0-9_]*)=(.*)$ ]] || continue
    key=${BASH_REMATCH[1]} value=${BASH_REMATCH[2]}
    [[ -n $value && -z ${!key+x} ]] && export "$key=$value"
done < "$data/nhl19.conf"
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
