# WindowSmash on a Mac

[← README](../README.md) · [Setup guide](BUILDING.md) · [Playing](PLAYING.md) · [Multiplayer](MULTIPLAYER.md) · [Technical guide](TECHNICAL.md)

The same recompiled game as the Windows build, as a native program for Macs with Apple silicon, built
on your Mac from your own copy of the game. It is **experimental**, and newer than the Windows and
Linux versions.

- The game draws with **Metal**, through a renderer of its own that is new: the game's shaders are
  translated to Metal's shading language and compiled the first time each one is used.
- The window, controllers and sound use **SDL3**.
- `Install WindowSmash.command` sets it all up and builds the game, as `Install WindowSmash.bat` does
  on Windows.

Any game version should work. On a Mac, 13.0.5 has been checked through its opening and menus, at
60 fps; full matches haven't been yet. The Windows build has been tested with 11.0.0 and 13.0.5.

> [!IMPORTANT]
> WindowSmash doesn't include the game. You need your own copy of Super Smash Bros. Ultimate, dumped
> from your own Nintendo Switch, with your console's keys and firmware. A built game holds the game's
> code and data: it's for your own Mac, not for sharing.

## Contents

- [What you need](#what-you-need)
- [Install and build](#install-and-build)
- [What the installer does](#what-the-installer-does)
- [Playing](#playing)
- [Controls](#controls)
- [What macOS asks](#what-macos-asks)
- [Where things are](#where-things-are)
- [If something goes wrong](#if-something-goes-wrong)
- [Building it by hand](#building-it-by-hand)
- [For developers](#for-developers)
- [Project status](#project-status)

## What you need

| | Requirement |
| :--- | :--- |
| **Mac** | One with Apple silicon: M1 or newer. Intel Macs can't run it. |
| **macOS** | macOS 14 Sonoma or newer; on macOS 14, version 14.5 or newer, for the Command Line Tools the build needs. |
| **Memory** | 8 GB at least; 16 GB or more builds much faster. |
| **Free space** | About 40 GB on the disk WindowSmash goes on, plus room for your game files (about 17 GB). |
| **Your game files** | The game (its `.nca` files, or an `.nsp` or `.xci`), with its update and DLC if you have them; your keys (`prod.keys` and `title.keys`); and your firmware's `.nca` files. Ryujinx's and yuzu's forks' own folders work as they are. |
| **Apple's Command Line Tools** | Apple's free compiler, version 16 or newer (with the macOS 15 SDK or newer: an older SDK lacks parts of Metal the game uses). The installer has macOS install them when they are missing; Xcode isn't needed. |
| **Time** | About 15 minutes of your own time, plus one to a few hours while the Mac builds the game. |

## Install and build

1. **Download WindowSmash.** On the [GitHub page](https://github.com/SNO2207/WindowSmash/tree/macos-port), click
   **Code**, then **Download ZIP**. Double-click the ZIP in your Downloads folder to unpack it, then move
   the folder to your home folder (the one with your name, in Finder's **Go** menu) and call it
   `WindowSmash`. Keep it out of Desktop, Documents and Downloads: iCloud Drive may try to upload the
   build (around 30 GB), and macOS asks for permission for those folders.
2. **Double-click `Install WindowSmash.command`** in that folder. The first time, macOS refuses to open
   it, because it was downloaded and isn't signed by a developer Apple knows: it says
   *"Install WindowSmash.command" Not Opened*. Click **Done**, then:
   1. Open **System Settings → Privacy & Security** and scroll down to **Security**.
   2. Next to *"Install WindowSmash.command" was blocked to protect your Mac*, click **Open Anyway**,
      and confirm with your password or Touch ID.
   3. Click **Open** in the box that follows.

   macOS asks this only once: the installer takes the download mark off WindowSmash's other files. (On
   macOS 14 the message says it is *from an unidentified developer*: Control-click the file, choose
   **Open**, then **Open** again.)
3. **A Terminal window opens** and shows each step as it goes; everything it shows is also kept in
   `install.log`. If Apple's Command Line Tools are missing, a window offers to install them: click
   **Install**, then **Agree**. They take 5 to 30 minutes; the installer waits.
4. **Choose your folders.** A window lists the folders it found for your game files, keys and firmware,
   with a line under each saying what is in it (✓ when it's fine, ✗ with what's missing). Click
   **Change…** to pick another folder, your online name, or the options; then **Set up and build**.
   The folder window shows hidden folders too, so Ryujinx's (`~/Library/Application Support/Ryujinx`)
   can be picked.
5. **Wait for the build.** The first one translates and compiles the whole game: one to a few hours,
   depending on the Mac. The Mac stays awake meanwhile, and you can keep using it. If the build stops,
   double-click `Install WindowSmash.command` again: it carries on where it stopped. It's finished
   when the window says **Done**.
6. **Play:** open **Play SSBU** from the Desktop or Spotlight, or from the Applications folder in your
   home folder (`~/Applications`, not the one in Finder's sidebar: in Finder, **Go → Home**, then
   **Applications**), or double-click `Play SSBU.command` in the WindowSmash folder.

<details>
<summary><strong>Without Open Anyway: from Terminal</strong></summary>

<br>

macOS only checks programs you open from Finder. To start the installer from Terminal instead, open
**Terminal** (in Applications → Utilities), type `bash ` (with a space), drag `Install
WindowSmash.command` from Finder into the window, and press <kbd>Return</kbd>.

Or download and start everything with one command. This puts WindowSmash in `~/WindowSmash`:

```bash
mkdir -p ~/WindowSmash && curl -fL https://codeload.github.com/SNO2207/WindowSmash/tar.gz/refs/heads/macos-port | tar -xz -C ~/WindowSmash --strip-components 1 && bash ~/WindowSmash/"Install WindowSmash.command"
```

</details>

The installer's options, for a Terminal window in the WindowSmash folder
(`./"Install WindowSmash.command" --help`):

| Option | What it does |
| :--- | :--- |
| `--game`, `--keys`, `--firmware` | Name a folder instead of choosing it. |
| `--online-name <name>` | The name other players see (up to 32 characters). |
| `--no-wizard` | Ask in the Terminal window instead of in dialogs. |
| `--yes` | Take the folders found or named, without asking. |
| `--jobs <n>` | How many files to compile at a time. The default goes by the memory: 4 under 16 GB, 8 with 16 GB, 16 with 32 GB or more. |
| `--skip-build` | Everything but compiling the game. |
| `--no-update` | Don't look for a newer WindowSmash. |
| `--dry-run` | Say what each step would do, and do nothing. |

**To update WindowSmash**, double-click `Install WindowSmash.command` again. It downloads the newest
WindowSmash if there is one, and builds again what the update changed: minutes, not hours. Your game
files, settings, save and online name stay as they are.

## What the installer does

Each step is skipped when it is already done, so running it again never repeats work.

| Step | What it does |
| :--- | :--- |
| 1. Update | Asks GitHub whether a newer WindowSmash is out than the one in your folder, and if so replaces the files that changed, then carries on as the new version. A folder made with Git is updated with `git pull` instead, when it is on the `macos-port` branch with no changes of its own. |
| 2. This Mac | Checks for Apple silicon (and that Terminal isn't running under Rosetta), macOS 14 or newer, the memory and the free space, and warns when the folder is in iCloud Drive, Downloads, or on a disk that isn't APFS. |
| 3. Command Line Tools | Has macOS install Apple's Command Line Tools (the compiler, the macOS SDK and Git) when they are missing, then builds and runs a test program with them, and checks that their macOS SDK is 15 or newer. |
| 4. Python | Makes a Python of WindowSmash's own in `deps/venv`, with the packages the tools use. When the Mac has no Python 3.10 or newer, `Install WindowSmash.command` first puts one into `deps/python` (a [python-build-standalone](https://github.com/astral-sh/python-build-standalone) Python 3.14, checked against its SHA-256). Nothing else on the Mac changes. |
| 5. SDL3 | Copies `SDL3.framework` out of SDL's own release (`SDL3-<version>.dmg`) into `deps/SDL3`, checked against the SHA-256 the release lists. Then it builds and runs a test program as the game is built (`tools/build_game.py --check`), so a missing piece shows before the questions. |
| 6. Your files | Finds your keys, firmware and game in Ryujinx's folders (and the folders Ryujinx lists its games in) and in those of yuzu's forks (Eden, Citron, Sudachi, Suyu, Torzu), and asks for the folders, your online name and the options. |
| 7. Build | Takes the program, fonts, data, DLC and fighters out of your files, then builds `build-mac/WindowSmash.app`, with the Mac kept awake. |
| 8. Launchers | Puts **Play SSBU** in `~/Applications`, with a link to it on the Desktop. |
| 9. Firewall | Only when the macOS firewall is on and you left that option ticked: lets the game take incoming connections, for hosting a private room. macOS asks for your password. With the firewall off (macOS's default) nothing is needed. |

## Playing

Every launcher starts the game from the WindowSmash folder. The ones ending in `.command` open a
Terminal window that shows the game's log as it goes; closing that window leaves the game running.

| Launcher | On Windows | What it does |
| :--- | :--- | :--- |
| **Play SSBU** (`~/Applications`, Desktop) | the desktop shortcut | The game, online, through the lobby in `online_server.txt` ([Multiplayer](MULTIPLAYER.md)). It asks its questions in small windows and shows no log. |
| `Play SSBU.command` | `Play SSBU.bat` | The same, with the log in Terminal. |
| `launchers/Play SSBU Offline.command` | `Play SSBU Offline.bat` | The game, offline. |
| `launchers/Host SSBU Netplay.command` | `Host SSBU Netplay.bat` | Hosts a private room, and shows the address to give the others. |
| `launchers/Join SSBU Netplay.command` | `Join SSBU Netplay.bat` | Joins a host's private room. |
| `launchers/Unlock All Fighters.command` | `Unlock All Fighters.bat` | Unlocks every base-game fighter on your save ([DLC and unlocks](DLC-AND-UNLOCKS.md)). Close the game first. |
| `launchers/Build WindowSmash.command` | `Build WindowSmash.bat` | Builds the game from your files, when the installer has set up the tools. |

- The name other players see online is the one you gave the installer. Without one, the game picks
  one, such as `Player1234`. To change it, open the [settings](PLAYING.md#settings) and go to
  **Online**.
- With [more than one game version](VERSIONS.md) built, the launchers ask which one to start. The one
  you picked last is the default. Every player in a room needs the same game version.
- There is no **Prepare Shaders** on a Mac yet: each shader is compiled when it is first needed, so a
  new effect can show a moment late, or stutter, the first time.
- `controllers.ini` and `display.ini` in the WindowSmash folder work as on Windows
  ([Playing](PLAYING.md#controllers)). In `display.ini`, the window's width and height are points,
  as macOS sizes windows: on a Retina display each point is 2×2 pixels. `borderless` and
  `fullscreen` are both macOS's full screen, in a space of its own. `render_scale` isn't supported
  on a Mac yet: the game always draws at 1920 × 1080 and is fitted into the window.
- **Settings:** press <kbd>Esc</kbd> in the game, or a controller's **Home** button, as on Windows
  ([Settings](PLAYING.md#settings)). The Mac has no **Render scale** or **Graphics card** row, and
  **Open controllers.ini** opens the file in your text editor.
- When the game asks for text, such as a name, a small window opens over the game for you to type
  it in.

## Controls

The same as on Windows ([Playing](PLAYING.md#controls)): plug in a controller and press a button, and
each pad becomes the next free player, up to eight. The keyboard plays as player 1.

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | <kbd>Z</kbd> / <kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd> |
| L / R | <kbd>A</kbd> / <kbd>S</kbd> |
| ZL / ZR | <kbd>Q</kbd> / <kbd>W</kbd> |
| + / − | <kbd>Return</kbd> / <kbd>Delete</kbd> |
| Right stick | <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> |

On a Mac keyboard the top row controls the brightness, volume and so on, and the F keys need
<kbd>fn</kbd>. The <kbd>⌘ Command</kbd> + number keys do the same without it:

| Key | Or | What it does |
| :--- | :--- | :--- |
| <kbd>Esc</kbd> | | Opens or closes the [settings](PLAYING.md#settings). |
| <kbd>F1</kbd> | <kbd>⌘</kbd>+<kbd>1</kbd> | Shows or hides the overlay: frame rate and, online, the connection. |
| <kbd>F2</kbd> | <kbd>⌘</kbd>+<kbd>2</kbd> | Shows more details, including the connected pads and the host's address. |
| <kbd>F3</kbd> | <kbd>⌘</kbd>+<kbd>3</kbd> | Takes a screenshot (into `screenshots/`). |
| <kbd>F4</kbd> to <kbd>F6</kbd> | <kbd>⌘</kbd>+<kbd>4</kbd> to <kbd>6</kbd> | The keys for troubleshooting, as on Windows ([Playing](PLAYING.md#controls)). |
| <kbd>⌥ Option</kbd>+<kbd>Return</kbd> or <kbd>⌘</kbd>+<kbd>Ctrl</kbd>+<kbd>F</kbd> | <kbd>F11</kbd> | Switches between a window and full screen. (macOS keeps <kbd>F11</kbd> for showing the desktop unless you change that in System Settings → Keyboard → Keyboard Shortcuts.) |

## What macOS asks

macOS asks before a program uses some of the Mac's features, once for each. These are the ones
WindowSmash may meet:

| What macOS asks | When | What to do |
| :--- | :--- | :--- |
| *"Install WindowSmash.command" Not Opened* | The first double-click on the installer. | **Open Anyway**, as in [Install and build](#install-and-build). |
| *"WindowSmash" would like to find and connect to devices on your local network* | The first time the game goes online. | **Allow.** The game then can open your router's port for a private room (UPnP), and connect straight to players on your own network. The online lobby works either way. Changed your mind? System Settings → Privacy & Security → Local Network. |
| *… would like to access files in your Desktop / Documents / Downloads folder*, or *on a removable volume* | The game reads its data where your game files are; Terminal makes the Desktop link. | **Allow**, or keep your game files elsewhere, for example in a folder in your home folder. |
| *… would like to receive keystrokes from any application* (Input Monitoring) | Some controllers are read through macOS's input devices, and macOS may ask for that. | **Allow** for those controllers; the keyboard and most pads work without it. System Settings → Privacy & Security → Input Monitoring. |
| *Do you want the application "WindowSmash.app" to accept incoming network connections?* | Hosting a private room with the macOS firewall on. | **Allow**, or let the installer add the rule (its firewall option). |

## Where things are

All of these are in the WindowSmash folder, unless the table says otherwise:

| What | Where |
| :--- | :--- |
| The game | `build-mac/WindowSmash.app`; another game version has its own, for example `v13.0.5/build-mac/`. |
| Its log | `build/play.log` (for another version, `v13.0.5/build/play.log`). |
| Your save | `save/`. Back it up now and then, with the game closed. |
| Screenshots | `screenshots/` |
| Crash and freeze reports | `reports/`, and macOS's own in `~/Library/Logs/DiagnosticReports` (`ssbu-*.ips`). |
| Settings | `controllers.ini`, `display.ini`, `online_server.txt`, `online_name.txt`, `online_direct.txt`. |
| The installer's log | `install.log` |
| WindowSmash's own tools | `deps/`: `venv` (its Python), `python` (when the Mac had none), `SDL3`. |
| The generated C and objects | `build/`, `build-mac/obj/` (around 30 GB). |
| Play SSBU | `~/Applications/Play SSBU.app`, and a link on the Desktop. It starts the game from this folder; if you move the folder, double-click `Install WindowSmash.command` in its new place to make Play SSBU again. |

> [!TIP]
> Time Machine backs up the build too. To leave out what can be made again, run
> `tmutil addexclusion build build-mac deps` in Terminal in the WindowSmash folder (keep `save/` in
> the backup).

## If something goes wrong

The installer stops with a message saying what went wrong and what to do; `install.log` keeps
everything it showed.

| What you see | What to do |
| :--- | :--- |
| *Not Opened* or *cannot be opened because Apple cannot check it* | **Open Anyway** in System Settings → Privacy & Security ([Install and build](#install-and-build)), or start it from Terminal. |
| *could not be executed because you do not have appropriate access privileges* | The ZIP was unpacked by a program that dropped the files' permissions. Unpack it again with Finder (a double-click), or start it from Terminal with `bash "Install WindowSmash.command"`. |
| *needs a Mac with Apple silicon* | WindowSmash builds for Apple silicon only: an Intel Mac can't run it. |
| The Command Line Tools' window doesn't appear, or their install fails | Install them from System Settings → General → Software Update, or download *Command Line Tools for Xcode* from [Apple's developer site](https://developer.apple.com/download/all/), then run the installer again. The window may open behind Terminal; if you close it, the installer says so and stops. |
| *the game needs the macOS 15 SDK or newer* / *cannot compile the game's runtime* | The Command Line Tools are too old. Update them in System Settings → General → Software Update (on macOS 14, update macOS to 14.5 or newer first), or download *Command Line Tools for Xcode* 16 or newer from [Apple's developer site](https://developer.apple.com/download/all/). |
| *Xcode's licence has not been accepted yet* | With Xcode installed and chosen as the developer tools, its licence must be accepted once: open Xcode, or run `sudo xcodebuild -license` in Terminal. |
| The build stops after a long time, the Mac very slow | It may have run out of memory: run the installer again with fewer jobs, in Terminal: `./"Install WindowSmash.command" --jobs 4`. |
| *The game data is not where it was when the game was built* | The game reads its data in place. Connect the disk it is on again, or run the installer to set it up from where it is now. |
| *The game is not built yet* / *No game version is built here yet* | Run `Install WindowSmash.command`. |
| The game window doesn't appear, or closes at once | Look at the end of `build/play.log` and at the newest file in `reports/`. Play SSBU says when the game stopped with an error. |
| No sound, or a controller does nothing | Check the log's `[audio]` and `[input]` lines. Press <kbd>F2</kbd> to see the pads the game knows. |
| Hosting a room: nobody can join | Allow the game on the local network (above), and through the firewall if it is on. The [multiplayer guide](MULTIPLAYER.md#which-address-to-give) says which address to give. |
| The log says *macOS keeps the game off your local network* | The game was not allowed on the local network: allow WindowSmash in System Settings → Privacy & Security → Local Network, then start the game again. |

When you report a problem, include the end of `build/play.log`, the report from `reports/`, your Mac's
chip and macOS version, and the game version ([Getting help](../README.md#getting-help)).

## Building it by hand

`launchers/Build WindowSmash.command` builds the game with what the installer set up. By hand, in
Terminal in the WindowSmash folder:

1. **The tools:** Apple's Command Line Tools 16 or newer (`xcode-select --install`), and a Python 3.10
   or newer, running as arm64, of WindowSmash's own. The `python3` that comes with the Command Line
   Tools (`/usr/bin/python3`) is Python 3.9: too old. Use another, such as Homebrew's
   (`brew install python@3.13`, then `python3.13` below), or the one `Install WindowSmash.command`
   downloads into `deps/python` when the Mac has none (`deps/python/bin/python3`).

   ```bash
   python3.13 -m venv deps/venv
   deps/venv/bin/python -m pip install -r requirements.txt
   ```

2. **SDL3:** from SDL's release `SDL3-<version>.dmg`, copy `SDL3.xcframework/macos-arm64_x86_64/SDL3.framework`
   into `deps/SDL3/` with `ditto`, which keeps its links.
3. **The build**, with your own folders:

   ```bash
   deps/venv/bin/python tools/build_game.py --game ~/MyDump --keys ~/MyKeys --firmware ~/MyFirmware
   ```

`tools/build_game.py` prepares the game tree as on Windows ([setup guide](BUILDING.md)), then generates
the game's C (`tools/recomp.py all --generate-only`) and compiles and links it with Apple's clang
(`tools/mac/build_mac.py`) into `build-mac/WindowSmash.app`, signed for this Mac. The installer itself is
`tools/mac/install_mac.py`; the launchers share `tools/mac/launch.sh`.

## For developers

- **The build:** `tools/mac/build_mac.py [--port <tree>] [--out <dir>] [--jobs N] [--runtime-only]
  [--link-only] [--replace]` compiles the C that `tools/recomp.py all --generate-only` wrote into
  `<tree>/build/` (on a Mac, `recomp.py all` only generates it), with the runtime, and links
  `<tree>/build-mac/WindowSmash.app` (the program is `Contents/MacOS/ssbu`). It compiles again a
  file whose source changed, a game object whose C is not the file it was compiled from
  (`obj/game_sources.tsv`: another game version's module can lend a tree older C), and everything
  when the compile flags change; `--replace` starts the game's objects over. Beside the app are
  `obj/` (the objects), `gen/` (the files the build makes), `codecs/` (libopus and libvpx;
  `tools/codec_libs.py --out <dir>` builds them alone) and `ssbu.syms`, which names the program's
  functions for the profiler (<kbd>F6</kbd>) and the hitch log. `tools/build_game.py --check` checks
  the tools: it builds and runs a small C, C++ and Objective-C program with SDL3, Metal and the
  codecs' headers.
- **Tests:** besides the Python ones (`tests/test_mac_build.py`, `test_mac_installer.py`,
  `test_lift_macos.py`), test programs whose first lines say how to build them:
  `tools/mac/shimtest.c` (the Windows API layer: memory, the write watch, fibers, waits, the
  profiler; with the loader and kernel, a made-up game's start and crashes), `tools/mac/msltest.m`
  (the shader translator's Metal output, run on the GPU), `tools/mac/gfxtest.m` (the Metal renderer,
  off screen), which `tests/test_mac_build.py` builds and runs, and `tools/mac/boottest/` (the whole
  program on a small made-up game, started as the launchers start it, its window shown a moment:
  `deps/venv/bin/python tools/mac/boottest/boottest.py`, which builds it in
  `~/Library/Caches/WindowSmash/boottest`: kept out of the WindowSmash folder, where the launchers
  would offer it as a game version).
- **Debugging:** the variables of the [Technical guide](TECHNICAL.md), and the Metal renderer's own
  forms: `SSBU_DUMP_MSL=<folder>` (or `SSBU_DUMP_HLSL`) saves each shader as compiled, as
  `<name>.metal`; `SSBU_GFX_PS_FILE` takes Metal source with a `fragment ... ps_main`;
  `SSBU_GPU_TIMES` logs each command buffer's GPU time (Apple's GPUs don't time single draws);
  `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` turn on Metal's own checks. After a crash, the
  shaders that were being compiled are in `dump/shadercrash/*.metal`. The shader cache keeps
  `m<version>_*.metal` files, next to the Windows build's `v<version>_*.dxbc` ones.

## Project status

| Area | Where it stands |
| :--- | :--- |
| **Overall** | Experimental, and the newest of the three versions. Expect problems that Windows doesn't have. |
| **Game versions** | **13.0.5** runs its opening movie, the new-save screens, Challenges, stage select and fighter select at 60 fps (on an M5 Pro). Full matches haven't been checked yet, and other versions haven't been tried on a Mac. |
| **Graphics** | A new Metal renderer. Shaders are compiled the first time each is used; there is no ahead-of-time preparation yet. |
| **Controllers, sound** | Through SDL3, as on Windows. Testing with real pads has been limited. |
| **Multiplayer** | The same online lobby and private rooms as on Windows and Linux. Playing between a Mac and the other two hasn't been tested yet. |
| **Macs** | Apple silicon with macOS 14.5 or newer. Intel Macs aren't supported. |
