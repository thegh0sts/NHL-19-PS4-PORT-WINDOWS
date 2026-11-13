# NHL 19 PS4 Port for Windows

This project runs the **PS4 version** of *EA SPORTS NHL 19* natively on Windows. It isn't an emulator
running a whole console. It loads the game's own x86-64 code directly, gives it Windows versions of
the PS4 system libraries it calls, and translates its graphics to Vulkan.

It's a fork of [GNSB's UFC 3 port](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS): both games
run on EA's Ignite engine, so most of the work carries over.

> [!IMPORTANT]
> This project contains **no game files**. You need your own PS4 copy of the game (**CUSA11126**,
> with update 1.60), dumped from your own console. Game files, and files made from them such as
> `boot-linked.bin`, must not be shared.

> [!WARNING]
> **This port is an early work in progress.** Expect bugs, crashes and missing features, and back up
> your saves often. If you hit a bug, please report it **with your saves** so it can be reproduced:
> see [Reporting problems](#reporting-problems).

<!-- Add a screenshot here, e.g. ![NHL 19 in game](docs/screenshot.png) -->

## Status

The game boots to the main menu, and Play Now matches play from face-off with the 3D scene, players
and HUD drawn, at 60 FPS on a modern PC. Most other modes haven't been tested yet.

| Area | State |
|---|---|
| Boot, intro videos, menus | Working |
| Play Now matches | Working |
| Other modes (Franchise, Be a Pro, Ones, EASHL offline...) | Not tested yet |
| Main menu hub tiles | Missing: they're drawn by the game's web engine (EAWebKit), which isn't loaded yet |
| Frame rate | 60 FPS throughout (the game asks for 30 in matches; the port keeps 60) |
| Player jerseys (cloth) | Calm with the default settings; a slight jitter at the hem in team select |
| Bearded players | Drawn correctly (since v0.2.0) |
| Saves | Profile and game-mode saves work, without the low-storage warning (since v0.2.1) |
| Controller | Working (one local player for now) |
| Local multiplayer (several controllers) | Not yet |
| Online play | Not available: EA's NHL 19 servers are gone (see [Online](#online)) |
| Trophies | Recorded locally |
| Intel CPUs | Working: the game's AMD-only SSE4a instructions are emulated |
| Linux | Test builds only, not tested by the developer (see [Linux](#linux-test-builds)) |

Found a problem? See [Reporting problems](#reporting-problems).

## Features

- **Launcher** (`NHL19.exe`) that sets everything up and starts the game:
  - checks your game folder, and merges a base game and update dumped separately
  - game language, controller for player 1, player name
  - video: output 720p or 1080p, window or screen size, FSR 1.0 upscaling with sharpening, FPS
    limit, presentation mode, fullscreen, FPS counter, PS4 Pro mode
  - developer switches (frame statistics, Vulkan validation, shader cache, logs)
  - a Steam shortcut, and `--play` to start without the window
- **In-game overlay** (Insert, or L3+R3 on a controller) to change settings while you play
- **Hold ○ for 2 seconds** on any screen to quit the game (✕ yes, ○ no)
- The NHL 19 graphics settings that fix black 3D and shaking jerseys, applied for you

## Requirements

- Windows 10 or 11, 64-bit
- A CPU from Intel Haswell (4th generation Core) or newer, or any AMD Ryzen
- A GPU with Vulkan 1.3 support and an up-to-date driver
- Your PS4 dump of *NHL 19*: the base game and update **1.60**

### Supported version

| Serial | Region | Update | Status |
|---|---|---|---|
| **CUSA11126** | North America (UP0006) | 01.60 | Tested |

The port recognises the game by a fingerprint of its executable, so other regions or updates are
refused until they're added. Reports are welcome. You can find a dump's serial in its
`sce_sys\param.sfo` (`TITLE_ID`).

## Getting started

1. Download the latest release from the [Releases](../../releases) page and unzip it somewhere
   outside OneDrive-synced folders, for example `C:\Games\NHL19Native`.
2. Run `NHL19.exe`.
3. Choose your game folder: the one with `eboot.bin` in it. If the base game and the 1.60 update
   are separate folders side by side (`CUSA11126` and `CUSA11126-patch`), choose the base game:
   the launcher merges them once, with links that take no extra space.
4. Click **Play**. The first start prepares the files the loader needs from *your* game, which
   takes a little while; later starts are quicker.

The first matches also build the shader cache, so expect some stutter the first time you see each
new scene.

### Where your files are

Logs go to the `logs` folder beside `NHL19.exe` (or to `%LOCALAPPDATA%\NHL19Native\logs` if that
folder can't be written, for example under Program Files; the launcher's Log folder setting picks
another). Settings, saves and the shader cache live in `%LOCALAPPDATA%\NHL19Native` (paste that
into the Explorer address bar). Set `BB_DATA_DIR=.` before starting the launcher to keep everything
beside the program instead.

### Updating

Unzip the new release over the old one, or into a new folder. Your settings and saves stay in
`%LOCALAPPDATA%\NHL19Native`.

## Online

NHL 19's online modes needed EA's servers, which no longer run for this game. The port starts the
game in a network mode with no server behind it: without a network, the game crashes during
start-up, so this is on all the time. The game then behaves like a PS4 that can't reach EA, and
plays offline.

The UFC 3 port has its own replacement server for online fights. NHL 19 uses the same EA online
system underneath, so a replacement server may be possible later, but it would be a large project.

## Linux (test builds)

The code this port grew from also runs on Linux, and NHL 19 test builds for Linux are made by
GitHub (`.github/workflows/linux.yml`) as an AppImage and a tar.gz. The developer has no Linux PC, so
**these builds have only been compiled, not run**: they're published as pre-releases, and reports
from anyone who tries them are very welcome. See
[`packaging/linux/README-Linux.md`](packaging/linux/README-Linux.md) for how to start them.

- Same game fixes and settings as the Windows version, and the AMD-only instruction emulation for
  Intel CPUs. Red-zone protection isn't needed: Linux leaves the stack's red zone alone.
- The launcher window opens when the AppImage starts: the same new launcher that is in testing on
  Windows (see below). `nhl19.sh --play` plays without it.
- A test build is made by pushing a tag like `v0.2.1-linux-test1`: the workflow builds it and
  publishes a pre-release with `docs/release-notes/linux-test.md` as its notes. The workflow can
  also be run by hand (Actions > linux > Run workflow) to get the files without a release.

## The new launcher (testing)

A new launcher window, made with SDL3 and Dear ImGui like the in-game menu, works the same on
Windows and Linux and with a mouse, a keyboard or a controller (Steam Deck included):
`launcher/imgui`. It shows the game folder with a check of what's in it, the settings in four tabs,
and Play. It shares its settings with `NHL19.exe` (`sacredfire.ini` in the data folder), and Play
starts `NHL19.exe --play` (on Linux `nhl19.sh --play`), so the game starts exactly as from the
current launcher. On Windows it's `bin\NHL19Launcher.exe` while it's tested; `NHL19.exe` stays the
main launcher until it can replace it.

## Release layout

```
NHL19Native\
  NHL19.exe             start here (the launcher)
  bin\                  sacredfire-game.exe (the loader), bb-gpu-capabilities.exe and their DLLs;
                        NHL19Launcher.exe, the new launcher window being tested
  python\               embedded Python, used to prepare the game
  scripts\  tools\      the preparation scripts
  patches\              game patches
  share\                shadPS4's import name list (aerolib.inl)
  licenses\             licences of the port and its components
  VERSION               the build's version
```

## Building from source

The port builds natively on Windows with [MSYS2](https://www.msys2.org/)'s **CLANG64** environment.

1. Install MSYS2, open the **MSYS2 CLANG64** shell and install the tools and libraries (press Enter
   at each group prompt to take all of it):
   ```sh
   pacman -S --needed git mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python,glslang,vulkan-devel,vulkan-memory-allocator,fmt,boost,robin-map,xxhash,sdl3,zydis}
   ```
2. Clone and build:
   ```sh
   git clone --recursive https://github.com/thegh0sts/NHL-19-PS4-PORT-WINDOWS.git
   cd NHL-19-PS4-PORT-WINDOWS
   bash scripts/build-msys2.sh
   ```
   The first build also builds a few pinned libraries MSYS2 doesn't package (magic_enum, miniz,
   VulkanMemoryAllocator, robin-map, xbyak). Programs and their DLLs go to `out/windows/bin`.
   `BB_LTO=ON` gives a slower build and a faster game.
3. Put your game dump in `nhl19\` inside the repo (it's in `.gitignore`, so it's never committed),
   then run it:
   ```sh
   bash scripts/run-msys2.sh nhl19
   BB_SKIP_PREPARE=1 bash scripts/run-msys2.sh nhl19   # later runs: skip the preparation
   ```
   The whole log goes to `out/run.txt` as well as the terminal.

To make a release zip (`dist/NHL19Native-windows-x86_64.zip`):

```sh
BB_PRODUCT=NHL19Native BB_LAUNCHER_EXE=NHL19.exe bash scripts/package-msys2.sh
```

The script refuses to package uncommitted changes (`BB_ALLOW_DIRTY=1` for a test zip) or any game
file. The first run downloads the embeddable Python and checks it against a pinned checksum.

### Versions and releases

The version comes from git: `git describe --tags`, so a build of the commit tagged `v0.1.0` is
version `v0.1.0`, and later commits read like `v0.1.0-3-g2abb064`. A `-dirty` ending means the build
had uncommitted changes. To make a release, tag the commit, build and package, then upload. With the
[GitHub CLI](https://cli.github.com/):

```sh
git tag v0.1.0 && git push --tags
bash scripts/build-msys2.sh && BB_PRODUCT=NHL19Native BB_LAUNCHER_EXE=NHL19.exe bash scripts/package-msys2.sh
gh release create v0.1.0 dist/NHL19Native-windows-x86_64.zip --title "NHL 19 port v0.1.0" --notes "What's new..."
```

### Settings for testing

`run-msys2.sh` and the launcher set NHL 19's defaults. Any of them can be changed for one run by
putting it in front of the command, for example `BB_FLIP_RATE=-1 bash scripts/run-msys2.sh nhl19`.

| Variable | NHL 19 default | Effect |
|---|---|---|
| `BB_READBACKS` / `BB_READBACK_LINEAR` | 2 / 1 | Reads of GPU-written memory. 2 is precise (1 leaves a streak from bearded players' chins to their skates). Without the linear ones, the players and rink render black |
| `BB_WRITE_WINDOW` / `BB_WRITE_STEP` | 0 / 0 | UFC 3's write handling. On, the players' jerseys shake |
| `BB_PREUPLOAD` | 1 | Background upload of rewritten memory to the GPU |
| `BB_FLIP_RATE` | 0 | 0: 60 FPS throughout. `-1`: the game's own choice (30 FPS in matches, as on a PS4) |
| `BB_ONLINE_SERVER` | 127.0.0.1 | The network mode the game needs to start (no server behind it) |
| `BB_SSE4A_LOG=1` | off | Logs each emulated AMD-only instruction |
| `BB_LOG_UNRESOLVED=1` | off | Logs every unhandled system call (very large logs) |
| `BB_FRAME_STATS=1` | off | Frame timing in the log |
| `BB_REDZONE_SHIFT` | on | Protects the game's stack data from Windows exception handling (see v0.2.0). `0` turns it off, for comparison |
| `BB_HOLD_CIRCLE_QUIT` | 1 | Holding ○ for 2 seconds asks "Quit NHL 19?". `0` turns it off |

`python3 tools/module_imports.py <module.prx>` lists what a game module needs from the system and
how much of it the port already provides.

## How it works

1. **Preparation** (`scripts/prepare.py`, `link_libc.py`, `link_modules.py`) reads the game's
   `eboot.bin` and links it with the PS4 libraries the game ships (`libc.prx`, `libSceFios2.prx`)
   into one image, `boot-linked.bin`. `game_check.py` identifies the game by a fingerprint of its
   executable, `gen_import_names.py` names the system functions it imports, and
   `content_profile.py` records its content details.
2. **The loader** (`sacredfire-game.exe`, `src/`) maps that image into memory and runs the game's
   code natively. Each call into a PS4 system library (kernel, threads, files, saves, pads, audio,
   video output, network and so on) goes to a Windows implementation in `src/runtime_*.c`.
3. **AMD-only instructions:** the PS4's CPU has AMD's SSE4a extension, which Intel CPUs lack. When
   the game uses one, the loader catches the error and carries out the instruction itself
   (`src/platform/sse4a.inc`).
4. **Graphics** go through a Vulkan backend based on [shadPS4](https://github.com/shadps4-emu/shadPS4)'s
   `video_core` (`gpu/`), which translates the game's GNM command buffers and shaders to Vulkan.

## Reporting problems

The port is at an early stage, and bug reports are the fastest way to get problems fixed. Open an
issue with:

- your CPU, GPU and driver version, and Windows version
- what happened, in which mode and where (menu, face-off, intermission...), and what you were doing
- the log of that session: the newest file in the `logs` folder beside `NHL19.exe`, or
  `out/run.txt` when running from source
- **your saves**, so the bug can be reproduced:
  1. Close the game.
  2. Open `%LOCALAPPDATA%\NHL19Native` in Explorer.
  3. Right-click the `user` folder → *Send to* → *Compressed (zipped) folder*. If it's large, the
     `cache` folder inside can be left out: it's only the shader cache.
  4. Attach the zip to the issue.

Please don't attach game files or anything from the `out` folder: they're made from the game and
can't be shared. Saves are fine to share: they hold only your progress and settings.

## Version history

### v0.2.1 (2026-11-11)

Fixes for the game modes' saves, a crash fix, and a quicker way out of the game.

**Game**
- No more low-storage warning when starting Franchise, Season, Playoffs, Tournaments, NHL Threes or
  Be a Pro: the mode's save is now created and the game is told realistic save sizes
- Fixed a crash in Be a GM (a graphics memory error when depth and stencil buffers are paired)
- **Hold ○ for 2 seconds to quit:** a "Quit NHL 19?" box appears on any screen; ✕ (or Enter) quits,
  ○ (or Esc) goes back to the game. A short press of ○ still goes back a screen as usual.
  `BB_HOLD_CIRCLE_QUIT=0` turns it off

**Launcher**
- Logs now go to a `logs` folder beside `NHL19.exe`, so they're easy to find when reporting a
  problem (or to `%LOCALAPPDATA%\NHL19Native\logs` if that folder can't be written, for example
  under Program Files)

**Under the hood**
- Saves are given sizes that add up, as on a PS4 (the space they use, or the space the game asked
  for, whichever is larger) instead of 1 GiB each; the game totals its saves against its own
  budget, which the 1 GiB answers overran
- Opening a mode whose save doesn't exist yet now creates it, and the save dialog's answer matches
  the question the game asked (Yes / OK)
- `FindDepthTarget` no longer keeps a reference into the image list across an insert that can move it
- More save and storage details in the log (mount mode, blocks asked, save sizes)

**Known issues**
- Main menu hub tiles are still empty (EAWebKit isn't loaded yet)
- A slight jitter at the bottom of the jerseys in team select
- Modes other than Play Now and NHL Threes are lightly tested; one local player only; no online play

### v0.2.0 (2026-11-10)

Fixes for the players' looks, and a stability fix for every part of the game.

**Game**
- Bearded players drawn correctly: no more dark streak from the chin down to the skates in team
  select
- Calmer jerseys than in v0.1.0 (a slight jitter at the hem in team select remains)
- Fewer random glitches and crashes: the game's short-lived stack data is now safe from Windows'
  exception handling (details below)

**Under the hood**
- PS4 code may keep data in the 128 bytes just below its stack pointer (the "red zone"). Windows
  writes its exception information right there whenever an instruction faults, and the port's GPU
  memory tracking makes the game fault thousands of times a minute on purpose. Each of those could
  silently overwrite the game's data. At start-up the port now moves the game's red-zone accesses
  64 KiB lower, out of reach (about 9,500 of them). Code that switches stacks, such as libc's, is
  left alone. `BB_REDZONE_SHIFT=0` turns this off
- Precise readbacks (`BB_READBACKS=2`) are now the default, in the launcher too: the game reads the
  GPU's results for faces fresh. Before the red-zone fix, these extra faults made the jerseys shake,
  which is why v0.1.0 used 1
- Builds from a clean checkout are no longer marked `-dirty` (line endings and a file-name clash
  fixed)

**Known issues**
- Main menu hub tiles are still empty (EAWebKit isn't loaded yet)
- A slight jitter at the bottom of the jerseys in team select
- Modes other than Play Now are untested; one local player only; no online play

### v0.1.0 (2026-11-08)

First test release.

**Game**
- Runs the PS4 version (CUSA11126, update 1.60) natively on Windows: intro videos, menus and Play
  Now matches
- 60 FPS throughout, including matches
- Graphics settings for NHL 19: the 3D scene drawn (no black players or rink), calm jerseys
- AMD-only SSE4a instructions emulated, so the game runs on Intel CPUs
- Network mode without a server, so the game gets past its offline start-up crash
- No USB keyboard reported to the game (it asked for one at start-up)

**Launcher**
- NHL 19's own launcher: serial and update check, NHL 19 graphics defaults, data in
  `%LOCALAPPDATA%\NHL19Native`
- Output 720p / 1080p, FSR 1.0 upscaling, FPS limit, presentation mode, fullscreen

**Known issues**
- Main menu hub tiles are empty (the game's web engine, EAWebKit, isn't loaded yet)
- Modes other than Play Now are untested
- One local player only; no online play
- Some stutter the first time each scene's shaders are built

## Credits

This port is built on the work of:

- **[GNSB](https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS)**'s UFC 3 port, which this project
  is a fork of
- **SacredFire** (T3CHNOLOG1C): the loader and launcher the UFC 3 port grew from
- **bbport** (deadinside28): the original native PS4 loader approach
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the Vulkan graphics backend and much PS4 knowledge
- SDL3, Dear ImGui, Vulkan, Zydis, xxHash, fmt, sirit, LibAtrac9, miniz, AMD FidelityFX FSR and
  others: see [`licenses/`](licenses/)

## Licence

This project is free software, licensed under the **GNU General Public License v2.0 or later**: see
[`LICENSE`](LICENSE). Third-party components keep their own licences, listed in [`licenses/`](licenses/).

## Legal

This is a fan project and is not affiliated with, endorsed by or connected to Electronic Arts, EA
SPORTS, the National Hockey League, the NHL Players' Association or Sony Interactive Entertainment.
*NHL*, *EA SPORTS* and *PlayStation* are trademarks of their respective owners. The project contains
no copyrighted game code or assets; it only runs a copy of the game you dumped yourself from a
console you own.
