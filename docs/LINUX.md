# WindowSmash on Linux

[← README](../README.md) · [Setup guide](BUILDING.md) · [Playing](PLAYING.md) · [Multiplayer](MULTIPLAYER.md) · [Technical guide](TECHNICAL.md)

The same recompiled game as the Windows build, as a native x86-64 Linux program. It is
**experimental**.

- The renderer is still the Direct3D 11 one, running on Vulkan through **DXVK**.
- Shaders are compiled by **vkd3d-shader** when they aren't already in the shader cache. The
  cache from the Windows build comes with this folder.
- The window, controllers and sound use **SDL2**.

There are two ways to get it:

- **Install it on Linux.** `install-windowsmash.sh` sets up WindowSmash on the Linux machine and
  builds the game there from your own files, as `Install WindowSmash.bat` does on Windows. See
  [Installing on Linux](#installing-on-linux).
- **Copy over a package from Windows.** A Windows PC that has built the game can make an
  `ssbu-linux` folder to copy to a Linux machine or virtual machine. See [Setting up](#setting-up).

As on Windows, the game must be version 13.0.5, its last update
([which copies count](UPDATES.md#what-counts-as-1305)).

> [!IMPORTANT]
> A built game holds the game's code and data. It's for your own machines, not for sharing.

## Contents

- [Installing on Linux](#installing-on-linux)
- [What you need](#what-you-need)
- [Setting up](#setting-up)
- [In a VMware virtual machine](#in-a-vmware-virtual-machine)
- [Controls](#controls)
- [Settings](#settings)
- [Other launchers](#other-launchers)
- [Saves](#saves)
- [If something goes wrong](#if-something-goes-wrong)
- [Building it](#building-it)

## Installing on Linux

`install-windowsmash.sh` does everything in one go: it gets the newest WindowSmash, installs what
the build needs, finds your game files, and builds the game. It needs:

- **An x86-64 PC with AVX2**, a Vulkan driver, and 8 GB of RAM or more (16 GB builds faster).
- **About 30 GB of free space**, and an internet connection.
- **Your password for `sudo`**, to install packages. It knows the package managers of Ubuntu,
  Debian, Linux Mint, Fedora, Arch, Manjaro and openSUSE, and the families built on them. On an
  image-based Fedora (Silverblue, Kinoite, Bazzite, Bluefin, Aurora and the like), whose system
  folders are read-only, it adds them with `rpm-ostree`, to the running system where it can. If
  they can only come with the next start, it says so: restart the PC and run the installer again.
  On SteamOS (a Steam Deck), make the system folders writable first with
  `sudo steamos-readonly disable`, and give the `deck` user a password with `passwd` if it has
  none. If pacman then says its keyring isn't set up, run `sudo pacman-key --init` and
  `sudo pacman-key --populate archlinux holo`. SteamOS leaves every package's header files out;
  the installer puts back the ones the build needs. A SteamOS update puts the system back as it
  was, so if something is missing after one, run the installer again.
- **Your own game files**: the game (its `.nca` files, or an `.nsp` or `.xci`), with its 13.0.5
  update (unless your dump has 13.0.5 merged in) and your DLC if you have it; your keys
  (`prod.keys` and `title.keys`); and your firmware's `.nca` files. An emulator's own folders work
  as they are, with the game updated to 13.0.5 there.

1. **Download WindowSmash.** On the [GitHub page](https://github.com/SNO2207/WindowSmash), click
   **Code > Download ZIP**, then unpack it in your home folder, for example as `~/WindowSmash`.
2. **Open a terminal in that folder.** Open the folder in your file manager, right-click an empty
   area, and choose **Open in Terminal**.
3. **Run the installer:**

   ```bash
   bash install-windowsmash.sh
   ```

4. **Answer its questions.** It asks for three folders, and fills in yuzu's, its forks' (Eden,
   Citron, Sudachi, Suyu) or Ryujinx's own folders when it finds them, Flatpak installs included.
   Press <kbd>Enter</kbd> to keep a folder in [brackets], or type another:

   | Folder | What's in it | For example |
   | :--- | :--- | :--- |
   | Keys | `prod.keys` and `title.keys` | `~/MyKeys` |
   | Firmware | your firmware's `.nca` files | `~/MyFirmware` |
   | Game | the game's files, with its 13.0.5 update and DLC | `~/MyDump` |

   It checks each one and says what it found, then asks for the name other players see online. A
   game that isn't version 13.0.5 is refused, with what to add.
5. **Wait for the build.** The first one compiles the whole game: from one to a few hours,
   depending on the processor and the memory. Later ones redo only what changed. If it stops, run
   `bash install-windowsmash.sh` again: it carries on where it stopped. Everything it says also
   goes to `install.log`. While it writes the game's code as C, then while it compiles it, a line
   now and then shows how far it has got and the time left. The time left appears once the first
   files are compiled. Most terminals also show it in the window's title.
6. **Play:** run `./play-ssbu.sh` in the folder, or choose **WindowSmash** in your applications
   menu.

The installer's steps:

| Step | What it does |
| :--- | :--- |
| 1. Update | Gets the newest WindowSmash from GitHub. Your game files, settings, save and build stay. |
| 2. This PC | Checks the processor, the memory and the free space. |
| 3. Packages | Installs clang, lld, bison, flex, SDL2, libopus, libvpx, the Vulkan loader and zenity from your distribution. |
| 4. Python | Makes a Python of WindowSmash's own in `deps/venv`, with the packages the tools use. |
| 5. DXVK and vkd3d | Downloads DXVK 3.1.1, vkd3d 2.1 and their headers into `deps/linux-toolchain`, each checked against its SHA-256. |
| 6. Your files | Asks for the three folders and the online name. |
| 7. Launchers | Adds WindowSmash to the applications menu. |
| 8. Build | Takes the program, fonts, data, DLC and fighters out of your files, then builds `build-linux/ssbu`. |

Running it again later updates WindowSmash and builds again what the update changed. Its options:

| Option | What it does |
| :--- | :--- |
| `--game`, `--keys`, `--firmware` | Name a folder instead of being asked. |
| `--yes` | Take the folders found or named, without asking. |
| `--online-name <name>` | The name other players see. |
| `--jobs <n>` | How many files to compile at a time. The default goes by the memory: 4 under 16 GB, 8 with 16 GB, 16 with 32 GB or more. |
| `--skip-build` | Everything but compiling the game. |
| `--no-update` | Don't look for a newer WindowSmash. |
| `--no-packages` | Don't install packages: for a distribution it doesn't know, install them yourself first. |
| `--dry-run` | Say what each step would do, and do nothing. |

The launchers work like the Windows ones: `play-ssbu.sh` at the top of the folder, the others in
`launchers/`.

| Launcher | On Windows | What it does |
| :--- | :--- | :--- |
| `play-ssbu.sh` | `Play SSBU.bat` | The game, online through the lobby in `online_server.txt` ([Multiplayer](MULTIPLAYER.md)). |
| `launchers/play-ssbu-offline.sh` | `Play SSBU Offline.bat` | The game, offline. |
| `launchers/host-ssbu-netplay.sh`, `launchers/join-ssbu-netplay.sh` | `Host/Join SSBU Netplay.bat` | A private room. |
| `launchers/unlock-all-fighters.sh` | `Unlock All Fighters.bat` | The unlock tool, on this folder's save. Run it with the game closed. |
| `launchers/prepare-shaders.sh` | `Prepare Shaders.bat` | Compiles all of the game's shaders in advance ([below](#shaders-and-first-time-stutter)). |

The game reads its data in place from your game folder, or from the decrypted copy in
`dump/modules` when the game had to be decrypted first. If your game folder is on a disk or shared
folder that isn't mounted, the launchers say so. The [Controls](#controls), [Settings](#settings) and
[If something goes wrong](#if-something-goes-wrong) sections below apply here too. The save is in
`save/`, and the log goes to `build/play.log`.

### Updating

Run `bash install-windowsmash.sh` again whenever you want the newest WindowSmash. It looks at
what changed and builds only that: your game files aren't asked for again (they are read once,
into `dump/`), and the game's code is compiled again only when the update changes it. An update
of the program's own parts (the graphics, the audio, the kernel: the files in `runtime/`) compiles
those files and links the program again, which takes minutes. An update that changes how the
game's code is translated (a performance change to the translator, say) compiles the game's code
again, once: about as long as the first build. To build from other
game files, name them: `bash install-windowsmash.sh --game <folder>`.

A folder an older WindowSmash built from another game version, such as 11.0.0, is set up again
as 13.0.5 once your game folder has the 13.0.5 update; your save and settings stay
([a folder built from another version](UPDATES.md#a-folder-built-from-another-version)).

### Shaders and first-time stutter

The game's graphics programs (*shaders*) are made the first time each one is needed, which is why
a new effect can show a moment late once. To make them all in advance, run
`bash launchers/prepare-shaders.sh` from a terminal (on a Steam Deck: in Desktop Mode). It shows a
progress bar with the time left, runs at low priority, and keeps the PC awake meanwhile. A first run
makes about 40,000 shaders: half an hour on a fast desktop, and probably an hour or two on
a Steam Deck. You can stop it with Ctrl+C and start it again later; what is done stays done. It
needs the game built first, and the game files where they were. It prepares the first step, the
shader programs themselves; DXVK and the graphics driver still make their own GPU code from them
as each shader is first used, and keep it for next time.

The rest of this guide is about the package from Windows, apart from those sections.

## What you need

These are for the package from Windows.

| | Requirement |
| :--- | :--- |
| **Linux** | 64-bit, with glibc 2.35 or newer: Ubuntu 22.04 or later, Debian 12 or later, Fedora 36 or later. |
| **Processor** | One with AVX2 (Intel Haswell / AMD Excavator or newer). Virtual machines normally pass it through. |
| **Graphics** | A Vulkan driver. Mesa's (`mesa-vulkan-drivers`) covers AMD, Intel and, without a GPU, the software renderer *lavapipe*. NVIDIA needs its proprietary driver. |
| **Memory** | About 6 GB for the game on top of the desktop, so 12 GB or more for the whole machine is comfortable. |
| **Game data** | The files the Windows build reads its RomFS from (its `romfs.cfg`): one NCA or RomFS image with 13.0.5 in it, or, when the Windows installer applied the 13.0.5 update itself, the game's own file and the update's `romfs_patch.bin`. |

## Setting up

Every command below runs in a terminal **inside this folder**. To open one, open the folder in
your file manager, right-click an empty area, and choose **Open in Terminal**. Paste a command
with <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd> and press <kbd>Enter</kbd>.

1. **Copy this folder (`ssbu-linux`) to the Linux machine.** If you have `ssbu-linux.tar`, unpack
   it there instead, because the tar keeps the programs executable:

   ```bash
   tar xf ssbu-linux.tar
   ```

   Copying the folder also works: the launchers fix the permissions.

2. **Install what the game needs from the system** (SDL2, Vulkan, zenity):

   ```bash
   bash install-deps.sh
   ```

3. **Make the game data reachable.** The game reads its RomFS in place, without extracting
   anything. When the Windows installer applied the 13.0.5 update over the game's own data, the
   layer map `dump/modules/romfs.layers`, which comes with this folder, pieces it together from
   two files: the game's file and `romfs_patch.bin`. Either:

   - copy those files into `ssbu-linux/game/`, or
   - in a **VMware** virtual machine, share the Windows folder that holds them instead (*VM >
     Settings > Options > Shared Folders*, set to *Always enabled*). It appears under
     `/mnt/hgfs/`, where the launchers look. If `/mnt/hgfs` is empty, mount it:

     ```bash
     sudo mkdir -p /mnt/hgfs && sudo vmhgfs-fuse .host:/ /mnt/hgfs -o allow_other
     ```

   The launchers find the files by their name and size (as they were on Windows when this
   folder was made) on their first run. They search this folder, `game/`, `/mnt/hgfs`,
   `/media`, `/mnt` and your home folder, then note where the files are in
   `dump/modules/romfs.cfg`. If a file moves, they look for it again. If one can't be found,
   they say its name and where it was on Windows.

   To point at the files yourself, which also skips searching a large shared folder, set
   `SSBU_NCA=/path/to/<the game's file>` and, when there are two files,
   `SSBU_ROMFS_PATCH=/path/to/romfs_patch.bin`. The launchers remember them after that:

   ```bash
   SSBU_ROMFS_PATCH=~/romfs_patch.bin bash run.sh
   ```

4. **Play:**

   ```bash
   bash run.sh
   ```

   The log goes to `build/play.log` and to the terminal.

`run.sh` plays online, as `Play SSBU.bat` does on Windows: the game joins the online lobby named
in `online_server.txt`, and Local Wireless lists every room that players in the same lobby
create, on Windows or Linux ([Multiplayer](MULTIPLAYER.md)). The first time, it asks for the name other
players see and keeps it in `online_name.txt`.

- To play offline, use `bash run-offline.sh`, or write `off` in `online_server.txt`. If the lobby
  doesn't answer, the game plays offline by itself after a few seconds.
- The word `off` in `online_direct.txt` keeps all of a room's traffic going through the lobby,
  as on Windows: no direct connections, so the other players don't learn your internet address.

## In a VMware virtual machine

- Turn on **Accelerate 3D graphics** (*VM > Settings > Display*).
- VMware gives Linux guests OpenGL but, as far as Mesa's drivers go, **no Vulkan GPU driver**. So
  DXVK runs on **lavapipe**, Mesa's software Vulkan renderer, which draws on the CPU, and the log
  says `[gfx] note: this is a software renderer`. Everything works, but the frame rate is low,
  especially in matches. A real GPU with a Vulkan driver should run at full speed, as the same
  renderer does on Windows, but that hasn't been measured on Linux yet.
- **More virtual CPU cores** help the software renderer most (*VM > Settings > Processors*). With
  4, menus run at only a few frames per second. Give the VM as many as the host can spare.
- **Enough memory:** 8 GB is tight with a desktop; 12 to 16 GB is better.
- To see what holds the frame rate back, press **F2**. *Present blocked … ms/frame* is the time
  the game waits each frame for the renderer to finish drawing, and *renderer … ms/frame*
  includes that wait. When nearly all of the renderer's time is the wait, it's the software
  renderer. In `top -H`, its threads are `llvmpipe-0`, `llvmpipe-1` and so on.
- `vulkaninfo --summary` shows which Vulkan device you have.
- Saving the VM's settings while *Shared Folders* is set to *Disabled* turns sharing off. Set it
  to *Always enabled*.
- **Multiplayer from the VM:** playing online (`run.sh`) and joining someone else's private room
  work with VMware's default NAT networking, because the game only sends to the server. Hosting a
  private room (`host-netplay.sh`) is different: other PCs can't reach a relay inside the VM.
  Either switch the VM's network adapter to *Bridged*, or forward UDP port 39420 in *Edit >
  Virtual Network Editor > VMnet8 > NAT Settings*.

## Controls

The same as on Windows.

**Gamepads:** up to 8 players on any pad SDL2 knows (Xbox, PlayStation, Switch Pro and most
others). Each pad that connects becomes the next free player, and a pad unplugged and plugged
back in gets its player back. Buttons go by position, as on a Switch pad: the bottom face button
is the console's B, and so on. **F2** lists the pads. A `gamecontrollerdb.txt` in this folder adds
SDL mappings for pads SDL doesn't know.

**Keyboard** (while the window has focus), on player 1 unless `controllers.ini` says otherwise:

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | Z / X / C / V |
| L / R | A / S |
| ZL / ZR | Q / W |
| + / − | Enter (not with Alt) / Backspace |
| Right stick | I / J / K / L |

| Key | What it does |
| :--- | :--- |
| F1 | Overlay |
| F2 | Details |
| F3 | Screenshot (into `screenshots/`) |
| F4 | Log one frame's drawing |
| F5 | Depth sampling on or off |
| F6 | 10-second profile (`build/profile_*.txt`) |
| F11 or Alt+Enter | Switch between a window and full screen |

When the game asks for text (a name, a Smash Tag), a `zenity` text box opens.

## Settings

Two files in this folder, each written with its defaults and explanations the first time the game
starts (delete one to get the defaults back). They mean the same as on Windows
([docs/PLAYING.md](PLAYING.md) in the source):

**`controllers.ini`:** the keyboard's player, whether pads work while the window isn't focused,
players kept for one pad, `layout = positional` or `labels`, dead zone, rumble strength, trigger
threshold and button remaps, for every pad or for one type (`[device ps5]`), name or GUID. The
log names each pad as it connects, with its type and GUID.

**`display.ini`:**

| Setting | On Linux |
| :--- | :--- |
| `window`, `width`, `height` | `borderless` and `fullscreen` are both the desktop's full screen on the window's monitor, which keeps its resolution. The picture stays 16:9, with black bars when the window has another shape. Unlike on Windows, the window isn't held to 16:9 while you resize it. |
| `pacing = clock` (default) | A steady 60 frames a second. DXVK presents each frame the moment it's ready; a compositing desktop shows it at its next refresh, and full screen without one may tear. To prevent that, start the game with `DXVK_CONFIG="dxgi.tearFree = True" ./run.sh`. |
| `pacing = vrr` | The same clock, for FreeSync / G-Sync monitors with VRR turned on in the desktop or the driver. Best with `window = fullscreen`. |
| `pacing = display` | Follows the monitor's refreshes on Windows; on Linux, it's the clock. |
| `latency`, `hide_cursor` | As on Windows: 1–3 frames the GPU may work ahead, and hiding the idle mouse pointer. |
| `render_scale` | As on Windows: draws the picture this many times bigger than 1920 × 1080 (`0.5`–`4`, or `auto` to match the screen at start). On a software Vulkan driver such as llvmpipe, keep `1`. |

**Shaders:** this folder carries the Windows build's compiled shaders that the game has used. Any
other shader is compiled the first time it's needed, so a new effect can show a moment late once.
The Windows build's start-up shader prewarm is off here: under DXVK it made a Vulkan pipeline for
every cached shader at once and held up the first frame (`SSBU_SHADER_PREWARM=1` turns it on).
The Windows build's ahead-of-time shaders (`launchers\Prepare Shaders.bat`) aren't in the package (about
1.5 GB).

## Other launchers

| Launcher | What it does |
| :--- | :--- |
| `run-offline.sh` | The game without going online, as `launchers\Play SSBU Offline.bat` on Windows. |
| `host-netplay.sh`, `join-netplay.sh` | A private room, as `launchers\Host SSBU Netplay.bat` and `launchers\Join SSBU Netplay.bat` on Windows ([Multiplayer](MULTIPLAYER.md)). |
| `unlock-all-fighters.sh` | The unlock tool, on this folder's save. Run it with the game closed. Like `launchers\Unlock All Fighters.bat`, it knows 13.0.5's save layout (it checks that this folder's game is 13.0.5) and changes nothing otherwise. |

## Saves

`save/` is a copy of the Windows build's save (its `save\` folder), taken when this folder was
made. The two don't sync: copy `save/` back to that folder on Windows to carry progress
back.

> [!WARNING]
> Unpacking a newer `ssbu-linux.tar` over this folder replaces `save/`, so copy it somewhere
> first. It also replaces `online_server.txt`; `online_name.txt` stays.

## If something goes wrong

| What you see | What to do |
| :--- | :--- |
| *Some libraries the game needs are not installed* | Run `bash install-deps.sh`. |
| *`SDL.h` file not found*, or another `.h` file, while building | The package is installed without its header files (SteamOS does this). Run `bash install-windowsmash.sh` again: it puts them back. |
| A black window, or `cannot create the window` / DXVK errors in the log | Run `vulkaninfo --summary`. If it lists no device, install `mesa-vulkan-drivers`. |
| No sound | SDL uses PulseAudio or PipeWire. Check the log for `[audio]` lines. |
| A crash | It writes `reports/crash_<date>_<time>.txt`: the guest function and host frames (`*** host exception`), also at the end of the log. Send it with `build/play.log`. When the port stops the game itself (something it can't do yet), it writes `reports/stopped_*.txt`. |
| A freeze (no new picture for 5 seconds) | It writes `reports/freeze_*.txt`, again at 30 and 120 seconds: every thread's stack and who holds the renderer's locks. `SSBU_WATCHDOG=0` turns that off. Windows also writes minidumps; Linux doesn't, so a core dump, if your system keeps them, is the closest thing. |
| Stutter | `SSBU_HITCH=40 ./run.sh` logs every frame over 40 ms with what the game's main, vsync and renderer threads spent it on (`[hitch]` lines in the log). |
| Suspected trouble with write tracking | `SSBU_WRITE_WATCH=protect` makes the renderer track CPU writes with page protection instead of the kernel's write tracking (`userfaultfd` + `PAGEMAP_SCAN`, Linux 6.7 and later). Use it only to rule that out. The log's first lines say which one is in use. |

## Building it

On Linux, `install-windowsmash.sh` builds it ([Installing on Linux](#installing-on-linux)). By hand:
`tools/build_game.py` prepares the game tree and then, on Linux, generates the game's C
(`tools/recomp.py all --generate-only`) and runs `build_vkd3d.py` and `build_linux.py` below with
the system's clang and lld. The third-party parts then live in `deps/linux-toolchain`, where the
installer puts them, and the overlay's font is drawn from the system's monospace font
(`tools/linux/make_overlay_font.py`, with Pillow).

On Windows, the Linux program is cross-compiled next to the Windows build. It compiles the same
generated C again, so build the game for Windows first (`tools/build_game.py`, or
`tools/recomp.py` by hand). You also need Visual Studio's Clang (`clang.exe` and `ld.lld.exe`, or name
their folder in `SSBU_LLVM_BIN`) and Python 3.

The third-party parts go in `linux-toolchain/`, next to the checkout (or in the folder named by
`SSBU_LINUX_TOOLCHAIN`):

| Folder | What goes there |
| :--- | :--- |
| `sysroot/` | These Ubuntu 22.04 (jammy) amd64 packages, each `.deb`'s data archive extracted into it: `libc6`, `libc6-dev`, `linux-libc-dev`, `libgcc-s1`, `libgcc-11-dev`, `libstdc++6`, `libsdl2-2.0-0`, `libsdl2-dev`, `libopus-dev`, `libvpx-dev`. Linking against 22.04's glibc keeps the program runnable on 22.04 and anything newer. |
| `dxvk-native/` | DXVK's native Linux build, version 3.1.1 (`dxvk-native-3.1.1-steamrt-sniper.tar.gz` from DXVK's releases), so that `dxvk-native/usr/lib` holds `libdxvk_d3d11.so.0` and `libdxvk_dxgi.so.0`. |
| `dxvk-headers/include/native/` | `include/native` from the DXVK 3.1.1 source, with mingw-directx-headers in its `directx/` folder (the commit DXVK 3.1.1 uses, `9df86f2`). |
| `src/vkd3d-2.1/` | The vkd3d 2.1 source. |
| `src/SPIRV-Headers-<version>/` | SPIRV-Headers, for example `vulkan-sdk-1.4.363.0`. |
| `winflexbison/` | win_flex_bison 2.5.25 (`win_bison.exe`, `win_flex.exe` and `data/`), for vkd3d's parsers. |
| `dxvk-win/dxvk-3.1.1/` | Optional: DXVK 3.1.1 for Windows, for `tools/linux/wintest.py`. |

Then, from the checkout, run these three in order:

```bash
python tools/linux/build_vkd3d.py linux
```

```bash
python tools/linux/build_linux.py --jobs 16
```

```bash
python tools/linux/package_linux.py --tar
```

| Command | What it does |
| :--- | :--- |
| `build_vkd3d.py` | Builds vkd3d-shader, once. |
| `build_linux.py` | Builds the checkout's game (its `dump/` and `build/`) into `build-linux/ssbu`, about 1.2 GB, with `ssbu.syms` for the profiler. `--port <folder>` builds another checkout's game instead; each output folder holds one game, so give that one its own `--out`. The first build compiles every game file again, about 25 minutes with 20 jobs; later builds only redo what changed. |
| `package_linux.py` | Makes `dist-linux/ssbu-linux` from it, and `ssbu-linux.tar` with `--tar`. `--build` and `--out` package another tree's build. |

To check the Linux renderer path without a Linux machine, `python tools/linux/wintest.py --vkd3d`
builds the Windows program with this runtime drawing through DXVK and compiling its shaders with
vkd3d-shader. `tools/linux/wintest_run.sh <program> <name>` runs it through the menus (no match)
with copies of the save and shader cache, and `tools/linux/compare_shots.py` compares two such
runs' captures.
