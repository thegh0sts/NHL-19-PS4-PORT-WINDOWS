# NHL 19 native PC port — Linux (test build)

This package runs **EA SPORTS NHL 19** (PlayStation 4, `CUSA11126`, update **1.60**) as a native
Linux program. It is built from the [NHL 19 port](https://github.com/thegh0sts/NHL-19-PS4-PORT-WINDOWS)
and **contains no game files**: you need your own PS4 copy of the game, dumped from your own console.

This Linux build is a **test**: the developer has no Linux PC, so it has only been compiled. Please
report how it went, even if it worked.

## Requirements

- Linux x86-64 with Python 3 and an up-to-date Vulkan 1.3 driver (Mesa for AMD and Intel, the
  proprietary driver for NVIDIA). On a Steam Deck, use desktop mode for the first start.
- The game: the base folder `CUSA11126` and the 1.60 update next to it, e.g. `CUSA11126` and
  `CUSA11126-patch` (also `-PATCH`, `-update`, `_patch`). They are merged automatically into
  `CUSA11126-merged` with hard links (no extra space). A folder with the update already merged in
  works too.

## Starting

AppImage: make `NHL19Native-x86_64.AppImage` executable and run it. tar.gz: unpack it and run
`./nhl19.sh`. The first time, choose your `CUSA11126` folder (a picker opens with zenity or kdialog
installed), or pass it on the command line:

    ./nhl19.sh /path/to/CUSA11126

It is remembered. The first start prepares the game (a minute or so), and the first match builds
the shader cache, so its loading screen can take a minute or two.

On a Steam Deck, adding the AppImage to Steam as a non-Steam game should let you play it in game
mode (not tested yet).

## In game

- **Insert**, or **L3+R3** on a controller: the overlay, to change settings while you play
- **Hold ○ (B on Xbox) for 2 seconds** on any screen: "Quit NHL 19?" — ✕ / A (or Enter) quits,
  ○ / B (or Esc) goes back to the game

## Files

- Settings: `~/.local/share/NHL19Native/nhl19.conf` (KEY=value lines; any `BB_*` variable set when
  starting wins)
- Saves and the shader cache: `~/.local/share/NHL19Native/user`
- Logs: a `logs` folder beside the AppImage or `nhl19.sh` (or `~/.local/share/NHL19Native/logs` if
  that one can't be written)

## Problems

Report them on the project's GitHub issues with your distribution, CPU, GPU and driver, what
happened, and the newest `.log` from the `logs` folder. Please don't attach game files.

## Legal

A fan project, not affiliated with or endorsed by Sony Interactive Entertainment, Electronic Arts,
the NHL or the NHLPA. GPL-2.0-or-later; third-party licences are in `licenses/`.
