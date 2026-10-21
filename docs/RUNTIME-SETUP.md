# Runtime packages

[← README](../README.md) · [Setup guide](BUILDING.md) · [Playing](PLAYING.md) · [Multiplayer](MULTIPLAYER.md)

A **runtime package** is a ZIP with a ready-built `ssbu.exe`, the module images it needs, the launchers, and the setup and unlock helpers. With one, you don't need Python or Visual Studio to play.

A package contains no game data, firmware fonts, keys, DLC list, save, debug maps, object files or shader caches. You still supply your own game data and fonts.

## Contents

- [What you need](#what-you-need)
- [First-time setup](#first-time-setup)
- [Updating a package](#updating-a-package)
- [After setup](#after-setup)

## What you need

- A Windows 10 or 11 PC, 64-bit, with a processor that has AVX2, FMA and F16C ([which ones](BUILDING.md#step-1-check-your-pc)).
- A graphics card or chip with DirectX 11 and a working driver.
- Your own copy of the game's data at **version 13.0.5**, the version every package is built for: with the 13.0.5 update applied and unencrypted, usually as `romfs.bin`. [Part 2 of the setup guide](BUILDING.md#part-2-gather-your-game-files) explains what that means.
- The six firmware font files (`.bfttf`). If you only have your firmware's `.nca` files, [step 9 of the setup guide](BUILDING.md#step-9-take-the-fonts-out-of-your-firmware) takes the fonts out; it needs Python and the WindowSmash source.
- A folder you can write to, such as `C:\WindowSmash` (not `Program Files`).

## First-time setup

1. On the release page, download **`WindowSmash-Windows-x64-runtime.zip`**, not the *Source code* archives GitHub adds to every release.
2. Right-click the ZIP, choose **Properties**, tick **Unblock** at the bottom if it's there, and click **OK**.
3. Right-click the ZIP, choose **Extract All…**, pick a folder such as `C:\WindowSmash`, and click **Extract**.
4. Open that folder and double-click **`Configure Game Data.bat`**. If **Windows protected your PC** appears, click **More info**, then **Run anyway**. Then answer its questions:

   | It asks for | Type |
   | :--- | :--- |
   | The path to your romfs.bin | The full path, for example `D:\MyDump\romfs.bin`. |
   | The byte offset | Press <kbd>Enter</kbd> for a plain `romfs.bin`. For a RomFS inside another unencrypted file, type the byte where it starts. |
   | The folder with your fonts | The folder holding the six `.bfttf` files, for example `D:\MyDump\fonts`. |
   | An aoc.cfg (optional) | Press <kbd>Enter</kbd> to skip. To use a DLC list you already have, type its path **without quote marks**. |

5. When it says **Game-data setup complete**, press any key, then double-click **`Play SSBU.bat`**.

The helper checks the RomFS header, the offset and the six fonts, copies the fonts into `dump\fonts\`, and writes `dump\modules\romfs.cfg`. An existing configuration is kept as a dated backup. It doesn't extract, decrypt or update anything, and the game reads the RomFS file in place, so keep it where it is.

These are the six fonts it looks for:

```text
nintendo_udsg-r_std_003.bfttf
nintendo_udsg-r_org_zh-cn_003.bfttf
nintendo_udsg-r_ext_zh-cn_003.bfttf
nintendo_udjxh-db_zh-tw_003.bfttf
nintendo_udsg-r_ko_003.bfttf
nintendo_ext_003.bfttf
```

## Updating a package

1. Close the game.
2. Extract the new package's contents into the folder that has `Play SSBU.bat`, and choose **Replace the files in the destination**.

The package contains no `romfs.cfg`, fonts, save or multiplayer identity, so your setup stays as it was. Run `Configure Game Data.bat` again only if your game data moved.

## After setup

- **Controls, controllers and the window:** the [playing guide](PLAYING.md). Pads other than Xbox controllers need `SDL3.dll` next to `build\ssbu.exe`; a package that includes it also includes its license, `SDL3-LICENSE.txt`.
- **Multiplayer:** every player needs the same package release. Every package is built from game version 13.0.5, as every WindowSmash is. See the [multiplayer guide](MULTIPLAYER.md).
- **Unlocking fighters:** after the first start, close the game and double-click `Unlock All Fighters.bat` ([DLC and unlocks](DLC-AND-UNLOCKS.md#unlock-every-base-game-fighter)).
- **Shaders:** shaders are prepared as you play, so a new effect can stutter once. `Prepare Shaders.bat` needs Python and Visual Studio, as for [building](BUILDING.md#step-16-optional-prepare-the-shaders).
- **Problems:** the log is `build\play.log`, and crashes or freezes write a report into `reports\` ([problem reports](PLAYING.md#problem-reports)).

This is an experimental runtime. Checking the archive and starting the game doesn't establish that every fighter, stage, mode or online match works.
