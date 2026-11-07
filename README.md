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
| Player jerseys (cloth) | Calm with the default settings |
| Saves | Profile save loads; not tested much further |
| Controller | Working (one local player for now) |
| Local multiplayer (several controllers) | Not yet |
| Online play | Not available: EA's NHL 19 servers are gone (see [Online](#online)) |
| Trophies | Recorded locally |
| Intel CPUs | Working: the game's AMD-only SSE4a instructions are emulated |

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

Settings, saves, the shader cache and logs live in `%LOCALAPPDATA%\NHL19Native` (paste that into
the Explorer address bar). Set `BB_DATA_DIR=.` before starting the launcher to keep everything
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

## Release layout

```
NHL19Native\
  NHL19.exe             start here (the launcher)
  bin\                  sacredfire-game.exe (the loader), bb-gpu-capabilities.exe and their DLLs
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
| `BB_READBACKS` / `BB_READBACK_LINEAR` | 1 / 1 | Reads of GPU-written memory. Without the linear ones, the players and rink render black |
| `BB_WRITE_WINDOW` / `BB_WRITE_STEP` | 0 / 0 | UFC 3's write handling. On, the players' jerseys shake |
| `BB_PREUPLOAD` | 1 | Background upload of rewritten memory to the GPU |
| `BB_FLIP_RATE` | 0 | 0: 60 FPS throughout. `-1`: the game's own choice (30 FPS in matches, as on a PS4) |
| `BB_ONLINE_SERVER` | 127.0.0.1 | The network mode the game needs to start (no server behind it) |
| `BB_SSE4A_LOG=1` | off | Logs each emulated AMD-only instruction |
| `BB_LOG_UNRESOLVED=1` | off | Logs every unhandled system call (very large logs) |
| `BB_FRAME_STATS=1` | off | Frame timing in the log |

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
- the log of that session: the newest file in `%LOCALAPPDATA%\NHL19Native\logs\`, or `out/run.txt`
  when running from source
- **your saves**, so the bug can be reproduced:
  1. Close the game.
  2. Open `%LOCALAPPDATA%\NHL19Native` in Explorer.
  3. Right-click the `user` folder → *Send to* → *Compressed (zipped) folder*. If it's large, the
     `cache` folder inside can be left out: it's only the shader cache.
  4. Attach the zip to the issue.

Please don't attach game files or anything from the `out` folder: they're made from the game and
can't be shared. Saves are fine to share: they hold only your progress and settings.

## Version history

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
