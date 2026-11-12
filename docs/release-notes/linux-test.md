**A test build for Linux: not tested by the developer.** The port's developer has no Linux PC, so
this build has only been compiled, never run. It may not start at all. If you try it, please report
what happened, even if it worked: see "Reporting" below.

It's the same NHL 19 port as the Windows v0.2.1 release, built for Linux: the same game fixes and
settings, hold ○ for 2 seconds to quit, and the AMD-only instruction emulation for Intel CPUs.
There's no launcher window yet: settings are in a text file.

**You need**
- Linux x86-64 (a desktop, or a Steam Deck in desktop mode) with Python 3 and an up-to-date
  Vulkan 1.3 driver (Mesa for AMD and Intel, the proprietary driver for NVIDIA)
- Your own PS4 dump of NHL 19: CUSA11126 (North America) with the 1.60 update

**Starting**
1. Download `NHL19Native-x86_64.AppImage`, make it executable (`chmod +x`, or Properties >
   Permissions) and run it.
2. The first time, choose your `CUSA11126` folder. A `CUSA11126-patch` folder beside it is merged in
   automatically. Without zenity or kdialog installed, start it from a terminal instead:
   `./NHL19Native-x86_64.AppImage /path/to/CUSA11126`
3. The first start prepares the game (a minute or so), and the first match builds the shader cache.

The tar.gz has the same contents, for systems without AppImage support: unpack it and run
`./nhl19.sh`.

Settings: `~/.local/share/NHL19Native/nhl19.conf`. Saves: `~/.local/share/NHL19Native/user`.
Logs: a `logs` folder beside the AppImage.

**Reporting**

Open an issue on GitHub with:
- your distribution, CPU, GPU and driver version (`vulkaninfo --summary` shows the driver)
- what happened: didn't start, an error message, crashed (where?), or played fine
- the newest `.log` file from the `logs` folder
- if it got in game: how the graphics, sound and controller worked

Please don't attach game files.

This release contains no game files: you need your own PS4 copy of NHL 19.
