// The game this launcher starts (launcher/imgui/nhl_launcher.cpp). The NHL 21 port keeps its own
// copy of this file; nothing else in the launcher is game-specific.
#pragma once

#define GAME_NAME "NHL 19"               // shown in the window and the messages
#define GAME_PRODUCT "NHL19Native"       // the data folder: %LOCALAPPDATA%\<this>, ~/.local/share/<this>
#define GAME_TITLE_ID "CUSA11126"        // the PS4 dump the port supports
#define GAME_REGION "North America"
#define GAME_UPDATE "01.60"              // its update (APP_VER in sce_sys/param.sfo)
#define GAME_STARTER_WINDOWS "NHL19.exe" // started with --play (the Windows launcher's game start)
#define GAME_STARTER_LINUX "nhl19.sh"    // started with --play (the Linux package's game start)
