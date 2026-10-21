# The 13.0.5 update

[← README](../README.md) · [Setup guide](BUILDING.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md) · [Technical guide](TECHNICAL.md)

WindowSmash builds and plays one version of the game: **13.0.5**, its last update. Among other things, 13.0.5 adds **Kazuya and Sora** (Challenger Packs 10 and 11), so it has every DLC fighter.

One version for everyone means that anyone can play with anyone (the online lobby and Quickplay only put players of the same game together), and that a bug is found and fixed in one game rather than several.

**What has been checked on 13.0.5:** the game data (every block, see [below](#how-the-update-is-applied)), a build of `main` and all 98 fighter modules, and scripted runs through the menus. The title screen reads *Ver. 13.0.5*, the unlock sequence for the new content plays, and character select shows the full roster, Kazuya and Sora included. Matches haven't been checked as thoroughly.

## Contents

- [What counts as 13.0.5](#what-counts-as-1305)
- [How the installer applies it](#how-the-installer-applies-it)
- [A folder built from another version](#a-folder-built-from-another-version)
- [Doing it by hand](#doing-it-by-hand)
- [How the update is applied](#how-the-update-is-applied)

## What counts as 13.0.5

Any of these, from your own Switch:

- **The game and its 13.0.5 update** (an `.nsp`) in your game folder, side by side. The installer applies the update for you. The game as first released (1.0.0) is the best base: the update applies over it exactly. A game with an earlier update merged in works too ([how](#patching-a-merged-base)).
- **The game installed in yuzu or Ryujinx, with its 13.0.5 update installed.** The emulator's installed-content folder holds both.
- **A dump with 13.0.5 merged in already.**
- For the [step-by-step setup](BUILDING.md#part-2-gather-your-game-files): the 13.0.5 update's program `.nca`, and a `romfs.bin` with the 13.0.5 update applied.

Anything else is refused, with what to do:

| Where | What you see |
| :--- | :--- |
| The installer's window | The line under **Game files** turns red: *WindowSmash needs version 13.0.5, and this is …*, followed by what to add. **Set up and build** doesn't go on until the line is fixed: put the 13.0.5 update in the game folder and click **Check files**. |
| The build | Step 4 checks the game's program and step 6 its data. Another version stops the build with *WindowSmash needs version 13.0.5 …* and what to do. |
| The game | A game built from another version doesn't start: it says *This WindowSmash was built from another version of the game*, and what to do. |
| The online lobby | It turns away a game built from another version, such as one an older WindowSmash built from 11.0.0. The overlay (<kbd>F1</kbd>) says *Netplay: WindowSmash needs game 13.0.5: run the installer again*. |

The answer is the same each time: put the game's 13.0.5 update (an `.nsp` file) in your game folder, next to the game, and run `Install WindowSmash.bat` again. If you made `romfs.bin` by hand, make it again with the 13.0.5 update applied.

## How the installer applies it

When your game folder has the game and its 13.0.5 update side by side, the installer applies the update while it prepares your game files (`tools/build_game.py`, step 6):

1. It takes the game's program from the update and checks that it is 13.0.5's.
2. It reads the game's own data where it is, or first decrypts it into `dump\modules` (about 15 GB) when it is still encrypted as the Switch stores it.
3. `tools/update.py` lays the update over that data. The update's own data goes into `dump\modules\romfs_patch.bin` (about 3.6 GB), and `romfs.layers` maps where each piece of the updated data comes from. Nothing else is copied. This takes a few minutes.
4. Every 16 KB block of the result is checked against the update's own SHA-256 hashes.

- **Over the game as first released (1.0.0),** the update applies exactly, as it does on a Switch, and every block matches.
- **Over a game with an earlier update merged in** (for example 11.0.0 rebuilt into one file), the update is rebuilt over it ([how](#patching-a-merged-base)). With 13.0.5 over a merged 11.0.0, all but 8 of 1,074,277 blocks matched. Those 8 blocks (128 KB, in a few models' files, none of them in a fighter module) keep the best guess, and `dump\modules\romfs.unverified.txt` lists them.

Leave your game files where they are afterwards: the game reads its data from them.

## A folder built from another version

WindowSmash used to build other versions of the game too, and many WindowSmash folders were set up from 11.0.0. Such a folder is **set up again as 13.0.5** the next time you run the installer (`Install WindowSmash.bat`; on Linux `install-windowsmash.sh`, on a Mac `Install WindowSmash.command`), once your game folder has the 13.0.5 update:

1. Put the game's 13.0.5 update `.nsp` in your game folder, next to the game. (With yuzu or Ryujinx, install the update there.)
2. Run the installer. It says *This folder was set up from version 11.0.0 of the game. WindowSmash needs 13.0.5 now*, and sets the folder up again from your game folder.
3. It clears what your old game files made: the game's program, the game data's settings and the update's files, the DLC list, the fighters and the built game. A decrypted copy of the game, if the installer made one, stays, and the update is applied over it. Then it builds the game again, as 13.0.5.

**What stays:** your save, your settings (`controllers.ini`, `display.ini`, the `online_*.txt` files), your multiplayer identity (`netplay_id.txt`), the fonts, and the shaders already prepared. The first time 13.0.5 loads an 11.0.0 save, it upgrades it itself ([13.0.5's save](DLC-AND-UNLOCKS.md#1305s-save)).

Without the update in the game folder, the installer stops before it clears anything and says what to add. The game built before stays as it was until then, but the online lobby turns it away.

> [!TIP]
> **Had a `v13.0.5` folder?** The version tool WindowSmash had before put 13.0.5 in a folder of its own, `v13.0.5\`, with a save of its own. WindowSmash doesn't use that folder any more, and the installer says so when it sets your folder up again. To play on from the save in it, close the game and copy `v13.0.5\save` over the WindowSmash folder's `save\` (keep a copy of the old `save\` if you may want it back). Then `v13.0.5\` can be deleted.

## Doing it by hand

For the [step-by-step setup](BUILDING.md), when you have the game's data without the update (its `romfs.bin`, or a decrypted program `.nca`) and the update's `.nsp`, apply the update yourself, from the WindowSmash folder:

1. Do steps 1 to 9 of the setup guide. In [step 10](BUILDING.md#step-10-connect-your-game-data), connect the game's own data, without the update: its `romfs.bin`, or, with the decrypted `.nca` way, the game's own program `.nca` (not the update's).
2. Keep that configuration under another name, as the update's base:

   ```powershell
   Copy-Item dump\modules\romfs.cfg dump\modules\romfs_base.cfg
   ```

3. Apply the update (example paths):

   ```powershell
   python tools/update.py apply "D:\MyDump\update.nsp" --base dump/modules/romfs_base.cfg --keys "D:\MyKeys"
   ```

4. Go on with steps 11 to 14 (DLC, controllers, fighters, build).

- `apply` writes the update's program into `dump/exefs`, in place of the one from step 8, and the updated data's `dump/modules/romfs.cfg`.
- It takes a few minutes, and ends by reading the whole result back and checking every block. `python tools/update.py verify "D:\MyDump\update.nsp" --keys "D:\MyKeys"` repeats that check on its own.
- Don't move or delete the base's file or `romfs_patch.bin` afterwards: the game reads both.

## How the update is applied

### What it makes

| Where | What |
| :--- | :--- |
| `dump/exefs` | The update's program: `main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`. |
| `dump/modules` | The updated data: `romfs.cfg`, `romfs.layers` and `romfs_patch.bin` (the update's own data, about 3.6 GB), plus `romfs.unverified.txt` when some blocks couldn't be confirmed. The installer also keeps the base's configuration here (`romfs_base.cfg`), and its decrypted copy of the game when it made one. |

Git ignores `dump/`.

### Patching a merged base

An update's data is a patch over the data of the game's **original release**: a map of the new image whose pieces are either the update's own data or bytes of the original release at given places.

If your base is the original release, unpacked, those places are right. `update.py` checks that first (256 blocks across the image against the update's hashes), and then applies the patch exactly. If your base already has an earlier update merged in (for example 11.0.0 rebuilt into one plain file), they aren't, and applying the patch the usual way would give a corrupt game. So `update.py` finds the pieces again:

1. **Files.** data.arc's file tables, in both versions, give every file's place in the new image and in your base. A file the patch takes from the original release is found under its path in the base, or as another file of exactly the same size. Music and movies come from their own table. The padding between files is taken from its neighbours, or is zeros.
2. **Checking.** The update carries a SHA-256 hash for every 16 KB block of the new image, so every guess is checked. A block that fails is solved again with more candidates: the shift of the nearest confirmed data in the original release's own order, the same bytes elsewhere in the image, or zeros. First one piece at a time, then several.

With 13.0.5 over a merged 11.0.0 base, **1,074,269 of 1,074,277 blocks matched the update's hashes**. The other 8 blocks (128 KB) are inside a few models' material, texture and helper-bone files whose original-release bytes no longer exist in a merged base. They keep the best guess and are listed in `romfs.unverified.txt`. None of them is in a fighter module. With the original release's data as the base, the patch applies exactly.

13.0.5 carries a hash of its own for every block. An update that takes some of its hashes from the original release isn't 13.0.5's, and `update.py` refuses it.

### Reading it

The runtime reads the result in place (`runtime/hle_fs.c`). `romfs.cfg` starts with the word `layered`, then names the layer map and its sources: your base file and `romfs_patch.bin`. Nothing else is extracted, and the plain `romfs.cfg` format still works.
