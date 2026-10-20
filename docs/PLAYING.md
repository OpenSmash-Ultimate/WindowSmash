# Playing

[← README](../README.md) · [Setup guide](BUILDING.md) · [Multiplayer](MULTIPLAYER.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md)

Everything about playing once the game is built: how to start it, the controls, the settings, and where WindowSmash keeps your save, screenshots and problem reports.

## Contents

- [Starting the game](#starting-the-game)
- [Settings](#settings)
- [Controls](#controls)
- [Controllers](#controllers)
- [Window and display](#window-and-display)
- [Shaders and first-time stutter](#shaders-and-first-time-stutter)
- [Low frame rate](#low-frame-rate)
- [Where your files are](#where-your-files-are)
- [Problem reports](#problem-reports)

## Starting the game

Everything starts from a launcher. `Play SSBU.bat` and `Install WindowSmash.bat` are in the WindowSmash folder; the others are in its `launchers` folder. Double-click the one you want:

| Launcher | What it does |
| :--- | :--- |
| **`Play SSBU.bat`** | Starts the game and joins the online lobby, so Local Wireless shows everyone's rooms. If the lobby doesn't answer, the game plays offline. |
| **`launchers\Play SSBU Offline.bat`** | Starts the game without going online. |
| **`launchers\Host SSBU Netplay.bat`** | Starts the game as the host of a private room ([multiplayer guide](MULTIPLAYER.md#private-room)). |
| **`launchers\Join SSBU Netplay.bat`** | Starts the game and joins someone's private room. |
| **`launchers\Unlock All Fighters.bat`** | Unlocks every base-game fighter on your save. Close the game first ([DLC and unlocks](DLC-AND-UNLOCKS.md#unlock-every-base-game-fighter)). |
| **`launchers\Prepare Shaders.bat`** | Prepares all of the game's shaders ahead of time ([below](#shaders-and-first-time-stutter)). |
| **`Install WindowSmash.bat`** | Sets everything up and builds the game ([setup guide](BUILDING.md#install-in-one-go)). Run it again to get the newest WindowSmash and build it. |
| **`launchers\Build WindowSmash.bat`** | Builds the game from your files, when the tools are already installed ([setup guide](BUILDING.md#shortcut-build-windowsmashbat)). |
| **`launchers\Configure Game Data.bat`** | Points WindowSmash at your game data and fonts ([setup guide, step 10](BUILDING.md#step-10-connect-your-game-data)). |

With [more than one game version](VERSIONS.md) built, the launchers ask which one to start. The last one you picked is the default.

The first time you play online, the game picks a name for you, such as `Player1234`, and shows it at the bottom of the screen. To change it, open the [settings](#settings) and go to **Online**.

## Settings

Press <kbd>Esc</kbd> in the game to open the settings. On a controller, press its **Home** button (the PS or Xbox button), or hold **−** and **+** together for a second. Close the settings the same way, or with **B**.

| Page | What you can change |
| :--- | :--- |
| **Display** | Window or full screen, frame pacing, how many frames the graphics card works ahead, the mouse pointer, the frame rate in the corner, the window's size, the render scale, the graphics card. |
| **Controllers** | For each connected controller: which player it is, face buttons by position or by label, stick dead zone, rumble, how far a trigger is pulled for ZL / ZR, and which control is each button (**Buttons...**). For the keyboard: which player it adds to. Also whether controllers work while another window is in front. |
| **Online** | Public lobby, a private lobby code, your own server, or offline; your name; direct connections ([multiplayer guide](MULTIPLAYER.md)). |
| **Game** | Restart or quit the game, the keys, and the game version. |

- **Move** with the arrow keys or the D-pad. **Change** a setting with left and right. **Select** with <kbd>Enter</kbd> or **A**. **Switch pages** with <kbd>Tab</kbd>, the number keys <kbd>1</kbd>–<kbd>4</kbd>, or **L** / **R**.
- The game **keeps running** behind the settings, but it doesn't see your buttons or keys while they're open. In a match, pause first. In a netplay room, the other players keep playing.
- Every change is **saved at once**. Settings marked **now** take effect straight away. Settings marked **next start** take effect the next time the game starts: **Game → Restart the game** does that for you. A match in progress is lost when you restart.
- To type a name or a code, use the keyboard, or pick the letters with a controller: **A** types, **B** deletes, **+** finishes.

The settings are kept in the same files as always: `display.ini`, `controllers.ini` and the `online_*.txt` files in the WindowSmash folder. You can still edit those files by hand, and the settings keep your comments in them. Some things are only in the files, such as settings for a whole kind of controller ([below](#controllersini)).

## Controls

The keyboard plays as player 1, together with player 1's pad if there is one:

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | <kbd>Z</kbd> / <kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd> |
| L / R | <kbd>A</kbd> / <kbd>S</kbd> |
| ZL / ZR | <kbd>Q</kbd> / <kbd>W</kbd> |
| + / − | <kbd>Enter</kbd> / <kbd>Backspace</kbd> |
| Right stick | <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> |

| Key | What it does |
| :--- | :--- |
| <kbd>Esc</kbd> | Opens or closes the [settings](#settings). |
| <kbd>F1</kbd> | Shows or hides the overlay: frame rate and, online, the connection. |
| <kbd>F2</kbd> | Shows more details, including the connected pads. |
| <kbd>F3</kbd> | Takes a screenshot. |
| <kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> | Switches between a window and full screen. |

<details>
<summary><strong>Keys for troubleshooting</strong></summary>

<br>

| Key | What it does |
| :--- | :--- |
| <kbd>F4</kbd> | Writes one frame's drawing commands to the log. |
| <kbd>F5</kbd> | Turns depth sampling on or off. |
| <kbd>F6</kbd> | Records a 10-second profile into `build\profile_*.txt`. |

</details>

## Controllers

Up to **eight players** can use pads at once: Xbox, PlayStation, Switch Pro, and most other controllers.

- Each pad that connects becomes the **next free player**. A pad that's unplugged and plugged back in gets its player back.
- Buttons go by **position**, as on a Switch controller. On an Xbox controller, the right-hand button (**B**) is the Switch's **A**, and the bottom one (**A**) is the Switch's **B**. To use the printed letters instead, or to change any button, open the [settings](#settings) and go to **Controllers**.
- The settings' **Controllers** page, or <kbd>F2</kbd> in the game, shows which pads WindowSmash found and which player each one is.

> [!NOTE]
> PlayStation, Switch Pro and most other pads need `SDL3.dll` next to `build\ssbu.exe`. [Step 12 of the setup guide](BUILDING.md#step-12-optional-support-more-kinds-of-controllers) adds it. Without it, only Xbox (XInput) pads work, as players 1–4.

### controllers.ini

The first time the game starts, it writes **`controllers.ini`** into the WindowSmash folder. The [settings](#settings) change it for you; you can also open it in Notepad (**Controllers → Open controllers.ini** opens it). Every setting is explained inside the file. Changes made by hand apply the next time the game starts, and deleting the file brings back the defaults.

| Setting | What it does | Default |
| :--- | :--- | :--- |
| `keyboard_player` | Which player the keyboard adds to: 1–8, or 0 for no keyboard. | `1` |
| `background` | `1`: pads keep working while the game window isn't in front. `0`: only while it is. | `1` |
| `layout` | `positional`: buttons by position, as on a Switch pad. `labels`: by the letter printed on the pad (PlayStation pads stay positional). | `positional` |
| `deadzone` | How much of the stick's travel around the centre is ignored, 0–50 percent. | `0` |
| `rumble` | Rumble strength, 0–100 percent. `0` turns it off. | `100` |
| `trigger_threshold` | How far a trigger must be pulled to press ZL or ZR, 1–99 percent. | `25` |
| *remaps* | `<Switch button> = <pad control>`, for example `ZR = rightshoulder`. The file lists every name. | |

The file has these sections:

- **`[general]`** holds `keyboard_player` and `background`.
- **`[players]`** reserves a player for one pad, for example `2 = DualSense Wireless Controller` (the settings' **Player** row writes the pad's GUID there). The log names every pad as it connects, with its type and GUID.
- **`[device default]`** applies to every pad.
- **`[device ps5]`**, **`[device <name>]`** or **`[device <GUID>]`** applies to one kind of pad, or one pad. Types include `xbox360`, `xboxone`, `ps4`, `ps5`, `switchpro`, `joyconpair` and `gamecube`. The settings write a controller's changes under `[device <its name>]`, or under the section it already takes its settings from.

For example, to make every pad use the printed letters and turn rumble down on a PlayStation 5 pad:

```ini
[device default]
layout = labels

[device ps5]
rumble = 60
```

## Window and display

<kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> switches between a window and full screen at any time. The game draws its picture at 1920 × 1080 and fits it into the window, always at 16:9, with black bars when the window has another shape. `render_scale` (below) draws it bigger for a sharper picture on a bigger screen. Screenshots keep the whole picture at the size it was drawn.

The first start also writes **`display.ini`**, which sets how the game starts. The settings' **Display** page changes it too:

| Setting | Values | Default |
| :--- | :--- | :--- |
| `window` | `windowed`: a window that keeps 16:9 as you resize it. `borderless`: covers its whole monitor. `fullscreen`: borderless and always on top; the monitor keeps its resolution. | `windowed` |
| `width`, `height` | The window's inside size at 100% display scaling. | `1280`, `720` |
| `pacing` | `clock`: a steady 60 frames a second. `vrr`: for G-Sync or FreeSync monitors, each frame shown as soon as it's ready; best with `borderless`. `display`: follows a 60, 120 or 240 Hz monitor's own refresh. | `clock` |
| `latency` | How many frames the graphics card may work ahead, 1–3. Lower is more responsive. | `3` |
| `hide_cursor` | `1` hides the mouse pointer after two seconds without movement. | `1` |
| `overlay` | `1` shows the frame rate in the corner from the start (<kbd>F1</kbd> shows or hides it while playing). | `1` |
| `gpu` | Which graphics card draws the game. `auto`: the fastest; on a laptop with two, the NVIDIA or AMD one rather than the processor's own. Or part of a card's name, such as `nvidia`, `radeon` or `intel`. `build\play.log` lists the cards found (a `[gfx] graphics cards` line). | `auto` |
| `render_scale` | How many times bigger than 1920 × 1080 the game draws its picture, in each direction: a number from `0.5` to `4`, or `auto`. `auto` matches the picture to the screen when the game starts, for example 1.33 (2560 × 1440) on a 1440p monitor with `window = borderless`; it never goes below 1. A picture bigger than the window is shrunk to fit, which smooths edges (`2` on a 1440p monitor). Each step up costs the graphics card more: `2` draws four times the pixels and needs about 200 MB more graphics memory. | `1` |

`controllers.ini` and `display.ini` apply to every game version started from the same folder. A `display.ini` written by an older version has no `gpu` or `render_scale` line; the game then uses `auto` and `1`. Add the line yourself to use it, or delete `display.ini` to get a new one with every setting. `render_scale` is read when the game starts: change it, then start the game again.

## Shaders and first-time stutter

The game's graphics programs (*shaders*) are prepared the first time each one is used, so a new effect can show a moment late, or stutter, the first time you see it. WindowSmash keeps every prepared shader in `dump\shadercache\` and loads them in the background each time the game starts, so each stutter happens only once.

To prepare them all before playing, double-click **`launchers\Prepare Shaders.bat`**. It runs once per game version, at low priority so the PC stays usable, and shows a progress bar with the time left. A first run compiles about 40,000 shaders (for 13.0.5): about half an hour on a 16-thread processor, an hour or two on a 4- to 8-thread one. You can close it at any time and start it again later; what's done stays done, so a later run takes minutes. It needs Python and Visual Studio, as for [building](BUILDING.md).

## Low frame rate

If matches run slowly on a PC that should manage them (press <kbd>F1</kbd> for the frame rate), check these first:

- **A laptop is drawing with its processor's graphics.** With two graphics chips, the game takes the NVIDIA or AMD one by itself. Check the `[gfx] drawing with` line near the top of `build\play.log`, and see `gpu` in [`display.ini`](#window-and-display).
- **Prepare Shaders is still running.** It uses most of the processor; let it finish, or close it while you play.
- **NVIDIA's shader cache is off.** In NVIDIA Control Panel → *Manage 3D settings*, set **Shader Cache Size** to *Driver default* (not *Disabled*). Without it, the graphics driver redoes its part of every shader each time the game starts.
- **Windows uses a slow system timer.** Some "gaming tweaks" set Windows to time everything with an old, slow timer (the HPET). WindowSmash notices and works around it, and says so near the top of `build\play.log`: a `[clock]` line ending in *slow*. Other programs stay slower, though; to undo the tweak, run `bcdedit /deletevalue useplatformclock` in a Command Prompt opened as administrator, then restart the PC.

## Where your files are

All of these are in the WindowSmash folder:

| What | Where |
| :--- | :--- |
| Your save | `save\`. Another game version keeps its own, for example `v13.0.5\save\`. |
| Screenshots (<kbd>F3</kbd>) | `screenshots\` |
| The game's log | `build\play.log` |
| Crash and freeze reports | `reports\` |
| Controller and window settings | `controllers.ini`, `display.ini` |
| Your online name, lobby and direct connections | `online_name.txt`, `online_server.txt`, `online_direct.txt` |
| Your multiplayer identity | `netplay_id.txt`. Don't copy it to another PC: two PCs with the same one look like one player. |

> [!TIP]
> **Back up your save** now and then: copy the `save` folder somewhere safe while the game is closed. To restore it, close the game and copy it back.

## Problem reports

When the game crashes, or freezes with no new picture for five seconds, WindowSmash writes a report into the **`reports\`** folder:

| File | Written when |
| :--- | :--- |
| `crash_<time>.txt` | The game crashed. |
| `freeze_<time>.txt` | No new picture for 5 seconds (again at 30 and 120 seconds). |
| `stopped_<time>.txt` | The game stopped with an error message. |
| `<…>.dmp` | Next to a crash or freeze report: a minidump of the game's memory. |

When you report a problem, attach the `.txt` report and the end of `build\play.log`. Read them first: they can contain folder names, player names and internet addresses. **Share a `.dmp` file only privately**, with someone who asked for it, because it contains part of the game's memory. [Getting help](../README.md#getting-help) lists what else to include.

If the game doesn't start at all, check, in this order: the game data path ([step 10](BUILDING.md#step-10-connect-your-game-data)), that the game version matches, the six font files, that your processor has AVX2, and your graphics driver.

WindowSmash is experimental. That it starts doesn't mean every fighter, stage, mode or online match works.
