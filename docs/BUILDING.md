# Setup guide

[← README](../README.md) · [Playing](PLAYING.md) · [Multiplayer](MULTIPLAYER.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md) · [13.0.5 update](UPDATES.md)

This guide takes you from a fresh download to playing. No programming knowledge is needed.

> [!IMPORTANT]
> WindowSmash doesn't include the game. You need your own copy of Super Smash Bros. Ultimate, dumped from your own Nintendo Switch, together with your console's keys and firmware. Your copy must be **version 13.0.5**, the game's last update ([which copies count](UPDATES.md#what-counts-as-1305)). This guide starts once you have those files.

> [!NOTE]
> **On a Mac** with Apple silicon, `Install WindowSmash.command` does what this guide's installer does: see [WindowSmash on a Mac](MACOS.md).

## Install in one go

**What you need:** a Windows 10 or 11 PC (64-bit, a processor with AVX2, 16 GB of RAM is comfortable, about 60 GB free) and your own game files: the game as dumped from your Switch, with its **13.0.5 update** (WindowSmash needs version 13.0.5), your console's keys (`prod.keys` and `title.keys`) and its firmware. If you have played the game in **yuzu or Ryujinx on this PC, updated to 13.0.5, you already have all three**, and the installer finds them by itself. Otherwise, put the game (its `.xci`, `.nsp` or `.nca` files), its 13.0.5 update `.nsp` and your DLC `.nsp` files in one folder; [Part 2](#part-2-gather-your-game-files) below says what the files are.

1. On **<https://github.com/SNO2207/WindowSmash>**, click the green **Code** button, then **Download ZIP**. Extract the ZIP somewhere with room, for example to `C:\WindowSmash`. Keep it out of the Desktop and Documents folders: OneDrive may try to upload them, and the finished build is around 30 GB.
2. Double-click **`Install WindowSmash.bat`** in that folder. Windows asks whether to run it (**The publisher could not be verified**): click **Run**. On some PCs a blue **Windows protected your PC** box comes instead: click **More info**, then **Run anyway**. Windows asks this about anything downloaded that isn't signed; the installer takes that mark off WindowSmash's other files, so it asks only this once.
3. A window opens with the folders it found for your game files, keys and firmware, and a line under each saying what is in it. Change a folder with **Browse…** if it is wrong or empty. If the game isn't version 13.0.5, the line under it says so in red: put the 13.0.5 update `.nsp` in the game folder and click **Check files**. Type the online name other players will see, and click **Set up and build**.
4. Wait. The installer gets what is missing (Python, a compiler, SDL3), prepares your game files, and builds the game: **30 minutes to a few hours**, depending on your PC. You can keep using the PC meanwhile. If it stops, run `Install WindowSmash.bat` again: it carries on where it stopped.
5. Play: double-click **`Play SSBU.bat`**, or the **Play SSBU** shortcut it put on the desktop.

<details>
<summary><strong>What the installer does</strong></summary>

<br>

Each step is skipped when it is already done, so running it again never repeats work:

- **Updates WindowSmash first:** it asks GitHub whether a newer WindowSmash is out than the one in your folder, and if so downloads it and replaces the files that changed, then carries on with the new version. Your game files, settings, save and builds stay as they are, and so does a lobby code you added to `online_server.txt`. A folder made with Git is updated with `git pull` instead, when it is on the `main` branch with no changes of its own.
- **Checks the PC:** 64-bit Windows 10 or 11, a processor with AVX2, the installed RAM, and the free space on the drive. A failed check stops with a message saying what is missing.
- **Python:** uses the Python you have if it is 3.10 or newer, otherwise puts a copy of Python 3.14 into `deps\python`. Nothing else on the PC changes: no PATH, no file associations.
- **The compiler:** if none works yet, it first downloads one that needs no Visual Studio: LLVM's clang from LLVM's releases on GitHub, and Microsoft's C runtime and Windows SDK from Microsoft's own servers, into `deps\llvm` and `deps\msvc` (about 570 MB to download, 550 MB on disk, no administrator rights). Microsoft's parts are under Microsoft's license terms: the installer shows where to read them and asks whether you accept them before it downloads anything ([what it downloads](#the-compiler-without-visual-studio)). If that download or the check after it fails, it installs the free Visual Studio Build Tools with the C++ workload and Clang instead, through winget, which is part of Windows: about 4 GB to download, with a progress window. If you already have Visual Studio without them, it adds them to that one instead (Windows asks for administrator rights first). Without winget, it prints the link and the two boxes to tick.
- **SDL3** for PlayStation, Switch Pro and other controllers, into `deps\SDL3`, from SDL's own releases, checked against the SHA-256 the release lists.
- **Music and movies:** nothing to download. The codecs (libopus and libvpx) are built from the sources in `third_party` as part of the game build.
- **Your folders:** suggests yuzu's and Ryujinx's (keys, firmware and installed games), shows what it finds, and remembers your answers. The game's program, data and DLC are taken from there, even when they are still encrypted as the Switch stores them, or when the 13.0.5 update sits next to the game. A game that isn't 13.0.5 gets a red line saying what to add.
- **Afterwards:** writes `online_name.txt`, puts a shortcut on the desktop, adds a Windows Firewall rule for the game if you leave that ticked (it asks for administrator rights once), runs the build ([`launchers\Build WindowSmash.bat`](#shortcut-build-windowsmashbat)'s steps), and if ticked starts `launchers\Prepare Shaders.bat` in a window of its own.
- Everything it prints also goes to `install.log` in the WindowSmash folder.

Options for a PowerShell window: `tools\install_windowsmash.ps1 -DryRun` says what each step would do without doing it; `-NoWizard` asks in the window instead; `-Game`, `-Keys` and `-Firmware` give the folders; `-NoUpdate` skips the update check; `-BuildTools` installs the Visual Studio Build Tools for the compiler instead of downloading it into `deps`.

</details>

Prefer to do it yourself, or want to know what each step does? The rest of this page is the same setup by hand.

<details>
<summary><strong>By hand: the setup step by step</strong></summary>

<br>

**How long it takes:** about an hour, plus 30 minutes to a few hours while your PC builds the game. You copy each command from this page and paste it into PowerShell.

**Want it done for you?** After step 5, [`launchers\Build WindowSmash.bat`](#shortcut-build-windowsmashbat) does steps 7–14 in one go. **Comfortable with a command line?** Skip to the [quick version](#quick-version).

## Contents

| Part | Steps |
| :--- | :--- |
| [**Part 1: Check your PC**](#part-1-check-your-pc) | [1. Check your PC](#step-1-check-your-pc) |
| [**Part 2: Gather your game files**](#part-2-gather-your-game-files) | [2. Put your game files in three folders](#step-2-put-your-game-files-in-three-folders) |
| [**Part 3: Install the tools**](#part-3-install-the-tools) | [3. Python](#step-3-install-python) · [4. The compiler](#step-4-install-the-compiler) |
| [**Part 4: Get WindowSmash**](#part-4-get-windowsmash) | [5. Download](#step-5-download-windowsmash) · [Shortcut: one-click build](#shortcut-build-windowsmashbat) · [6. Open PowerShell](#step-6-open-powershell-in-the-windowsmash-folder) · [7. Install the helpers](#step-7-install-windowsmashs-helpers) |
| [**Part 5: Prepare the game**](#part-5-prepare-the-game) | [8. Program](#step-8-copy-the-games-program) · [9. Fonts](#step-9-take-the-fonts-out-of-your-firmware) · [10. Game data](#step-10-connect-your-game-data) · [11. DLC](#step-11-optional-turn-on-your-dlc) · [12. More controllers](#step-12-optional-support-more-kinds-of-controllers) · [13. Fighters](#step-13-take-out-the-fighters) |
| [**Part 6: Build and play**](#part-6-build-and-play) | [14. Build](#step-14-build-the-game) · [15. Play](#step-15-play) · [16. Shaders](#step-16-optional-prepare-the-shaders) |
| [**Afterwards**](#afterwards) | [Update WindowSmash](#update-windowsmash) · [If something goes wrong](#if-something-goes-wrong) · [Quick version](#quick-version) · [Reference](#reference) |

---

## Part 1: Check your PC

### Step 1: Check your PC

Open **Settings → System → About** and check:

| Line | What you need |
| :--- | :--- |
| **System type** | **64-bit operating system**. |
| **Installed RAM** | 16 GB is comfortable. Note the number: [step 14](#step-14-build-the-game) uses it. |
| **Processor** | One with AVX2: an Intel Core i3, i5, i7 or i9 with a model number of 4000 or higher (for example *i5-4590* or *i7-12700*), an Intel Core Ultra, or any AMD Ryzen. Most Pentium and Celeron processors don't have it. |

You also need **about 60 GB free on drive C:** for WindowSmash, plus about 17 GB wherever your game files go, and a graphics card or chip with DirectX 11 and an up-to-date driver.

---

## Part 2: Gather your game files

You need these files from your own Switch:

| What | What it looks like | Needed? |
| :--- | :--- | :--- |
| **The game's program**, version 13.0.5 | One `.nca` file, usually the biggest one of the 13.0.5 update (or of the game, if your dump has 13.0.5 merged in). Its name is a long string of letters and numbers. | Yes |
| **The game's data**, with the 13.0.5 update applied and unencrypted | One big file of about 17 GB, usually called `romfs.bin` | Yes |
| **Your console's keys** | `prod.keys` and `title.keys` | Yes |
| **Your console's firmware** | A folder with a few hundred `.nca` files | Yes, for the fonts |
| **DLC** you own | `.nsp` files | Optional |

> [!NOTE]
> **WindowSmash needs version 13.0.5 of the game**, its last update, and no other. The program and the data must both be 13.0.5's: the build checks both and stops with any other version. One version for everyone means anyone can play with anyone, and every bug is fixed in the same game. [Which copies count](UPDATES.md#what-counts-as-1305).

> [!TIP]
> **With `Install WindowSmash.bat` or `launchers\Build WindowSmash.bat` you don't need `romfs.bin`.** Put the game (a game card dump `.xci`, an `.nsp`, or its `.nca` files; or point at the folder yuzu or Ryujinx installed it in), its 13.0.5 update's `.nsp`, and your DLC `.nsp` files (loose, or in a `dlc` folder) into one folder. They find the program, decrypt what needs it, and apply the update over the game themselves. Decrypting the game needs about 15 GB more space and the update about 4 GB; both take a few minutes. The game as first released (version 1.0.0) is the best base for the update: it applies exactly. Over a game with an earlier update merged in, the update is rebuilt and checked ([how](UPDATES.md#how-the-installer-applies-it)).

<details>
<summary><strong>What do "update applied" and "unencrypted" mean?</strong></summary>

<br>

The game's data comes in two parts: the game as it was first sold, and the 13.0.5 update that changes it. WindowSmash needs the two already combined into one file. It reads that file directly while you play, so it has to be in its plain, unencrypted form.

Tools that unpack Switch games can make this file. For example, hactool's `--basenca` option combines the base game with the 13.0.5 update's Program NCA, and its `--romfs` option writes the result as `romfs.bin`. If your copy of the game already has 13.0.5 merged in, its Program NCA is the program file, and the data is inside it as well.

Have the game's data without the update, and the update's `.nsp`? WindowSmash can apply it for you: see [doing it by hand](UPDATES.md#doing-it-by-hand).

If your program `.nca` is itself already decrypted, you can use it in place of `romfs.bin`: see [step 10](#step-10-connect-your-game-data).

</details>

### Step 2: Put your game files in three folders

This guide keeps your game files in three folders. Create them and copy your files in:

| Folder | What goes in it |
| :--- | :--- |
| `D:\MyDump` | The program file, renamed to **`program.nca`**. The game data, renamed to **`romfs.bin`**. Optional: a folder named **`dlc`** with your DLC `.nsp` files. |
| `D:\MyKeys` | `prod.keys` and `title.keys`. |
| `D:\MyFirmware` | All the firmware's `.nca` files. |

- **No D: drive?** Use **C:** instead, and change `D:\` to `C:\` in every command in this guide.
- **Not sure which `.nca` is the program?** Copy all of the game's `.nca` files into `D:\MyDump` for now; [step 8](#step-8-copy-the-games-program) shows how to find it.

> [!TIP]
> Windows hides file endings such as `.nca` by default. To see them, open File Explorer and turn on **View → Show → File name extensions** (on Windows 10: **View → File name extensions**). Then you can make sure the file is `program.nca`, not `program.nca.nca`.

> [!WARNING]
> Don't move, rename or delete `romfs.bin` once you're done. The game reads it every time it starts.

---

## Part 3: Install the tools

WindowSmash needs two free tools: **Python**, which runs its helper programs, and a **compiler**, which builds the game.

### Step 3: Install Python

1. Go to **<https://www.python.org/downloads/windows/>**, find the newest **Python 3.14** release (for example *3.14.8*), and download its **Windows installer (64-bit)**. (Any Python from 3.10 on works; 3.14 is the one the installer uses.)
2. Run it. On the first screen, tick **Add python.exe to PATH**, then click **Install Now**.

To check, open PowerShell (Start menu → type `powershell`) and run:

```powershell
python --version
```

It should answer `Python 3.` followed by a version of 3.10 or higher.

> [!NOTE]
> If python.org offers only the **Python install manager**, install that, then run `py install 3.14` in PowerShell, open a new PowerShell window, and check again.

### Step 4: Install the compiler

The compiler turns the game into a Windows program. **You don't need to install anything for it now:** WindowSmash downloads its own, which needs no Visual Studio (about 570 MB, no administrator rights). [The compiler without Visual Studio](#the-compiler-without-visual-studio) says what it downloads, and from where.

- **With [`launchers\Build WindowSmash.bat`](#shortcut-build-windowsmashbat)**, it does this by itself when your PC has no compiler. At the start it shows where to read Microsoft's license terms for Microsoft's part of it and asks whether you accept them: type `yes` and press <kbd>Enter</kbd>. It then downloads the compiler while it prepares your game files, so the build can start straight away.
- **Step by step**, once you have done [step 7](#step-7-install-windowsmashs-helpers), run this in the same PowerShell window. It asks the same question, downloads for a minute or a few, and ends with `Done`:

  ```powershell
  python tools\msvc_deps.py install
  ```

**Or use Visual Studio**, if you have it or prefer it: Microsoft's free toolbox for building programs (several gigabytes). You won't need to open it: WindowSmash only uses the tools it installs. WindowSmash uses it when the downloaded compiler isn't there.

1. Go to **<https://visualstudio.microsoft.com/downloads/>** and download **Community** (*Free download*).
2. Run it. On the **Workloads** tab, tick **Desktop development with C++**.
3. On the right, under **Installation details**, also tick **C++ Clang tools for Windows**.
4. Click **Install**. It downloads several gigabytes. Restart the PC if it asks you to.

> [!TIP]
> Missed the Clang tick? Open **Visual Studio Installer** from the Start menu, click **Modify**, tick **C++ Clang tools for Windows**, and click **Modify** again.

---

## Part 4: Get WindowSmash

### Step 5: Download WindowSmash

1. On **<https://github.com/SNO2207/WindowSmash>**, click the green **Code** button, then **Download ZIP**.
2. Before extracting, right-click `WindowSmash-main.zip`, choose **Properties**, tick **Unblock** at the bottom if it's there, and click **OK**. This stops Windows from warning about every file inside.
3. Extract the ZIP to `C:\` (right-click → **Extract All…**), then rename the folder `C:\WindowSmash-main` to **`C:\WindowSmash`**.

> [!TIP]
> Keep WindowSmash out of the Desktop and Documents folders: OneDrive may try to upload them, and the finished build is around 30 GB.

<details>
<summary><strong>Prefer Git?</strong></summary>

<br>

With [Git for Windows](https://git-scm.com/download/win) installed, this does the same and makes updating easier later (`git pull`):

```powershell
git clone https://github.com/SNO2207/WindowSmash.git C:\WindowSmash
```

</details>

### Shortcut: Build WindowSmash.bat

Once steps 1–5 are done, **`launchers\Build WindowSmash.bat`** can do steps 7 to 14 for you in one go:

1. If you want PlayStation, Switch Pro and other controllers, do [step 12](#step-12-optional-support-more-kinds-of-controllers) first.
2. Double-click **`launchers\Build WindowSmash.bat`** in `C:\WindowSmash`.
3. It asks for the folders with your game files, keys and firmware. Press <kbd>Enter</kbd> to accept `D:\MyDump`, `D:\MyKeys` and `D:\MyFirmware`, or type your own. It remembers the answers for next time.
4. If your PC has no compiler, it asks whether you accept Microsoft's license terms for the compiler's Microsoft part ([step 4](#step-4-install-the-compiler)): type `yes` and press <kbd>Enter</kbd>. It downloads the compiler meanwhile.
5. It finds the program file by itself (no need to rename it) and checks that it is 13.0.5's, copies the fonts, connects your `romfs.bin` (or a decrypted program `.nca`), turns on your DLC if `D:\MyDump\dlc` exists, takes out the fighters, and builds with a `--jobs` number that fits your RAM.
6. If it stops with a message, fix that ([If something goes wrong](#if-something-goes-wrong) can help) and run it again: it skips whatever is already done.

When it's finished, go to [step 15](#step-15-play). Steps 6–14 below do the same by hand.

### Step 6: Open PowerShell in the WindowSmash folder

Open `C:\WindowSmash` in File Explorer, click the **address bar** at the top, type `powershell` and press <kbd>Enter</kbd>. A PowerShell window opens that is already in the right folder: its last line reads `PS C:\WindowSmash>`. Use this window for the rest of the guide.

**Running the commands:** copy each command from this page, paste it into PowerShell with <kbd>Ctrl</kbd>+<kbd>V</kbd>, and press <kbd>Enter</kbd>. Wait until the `PS C:\WindowSmash>` line comes back before the next one. If a command prints an error, look it up in [If something goes wrong](#if-something-goes-wrong) before going on.

### Step 7: Install WindowSmash's helpers

```powershell
python -m pip install -r requirements.txt
```

It ends with `Successfully installed` and a name starting with `pycryptodomex`, or with `Requirement already satisfied`.

---

## Part 5: Prepare the game

### Step 8: Copy the game's program

If you've already renamed the program file to `program.nca`, skip to the second command.

**Not sure which `.nca` is the program?** This lists every `.nca` file in `D:\MyDump` with what it contains:

```powershell
Get-ChildItem D:\MyDump\*.nca | ForEach-Object { "$($_.Name)  $(python tools/nca.py info $_.FullName --keys D:\MyKeys | Select-Object -First 1)" }
```

The program is the line that says `type=Program`. If two lines say it, choose the one whose `title_id` ends in `800`: that's the update's program. Rename that file to `program.nca`. It must be the 13.0.5 update's program (or the game's, for a dump with 13.0.5 merged in): the build stops with another version's.

**Copy the program** into WindowSmash:

```powershell
python tools/nca.py exefs "D:\MyDump\program.nca" --keys "D:\MyKeys" --out dump/exefs
```

It lists five files with their sizes, among them `main`, `sdk` and `subsdk0`.

### Step 9: Take the fonts out of your firmware

The game uses the Switch's own fonts, which are inside your firmware. This goes through every firmware file and copies the fonts out (about a minute):

```powershell
Get-ChildItem D:\MyFirmware\*.nca | ForEach-Object { python tools/nca.py romfs-get $_.FullName --keys D:\MyKeys --match .bfttf --flat --out D:\MyDump\fonts 2>$null }
```

Check that `D:\MyDump\fonts` now has these six files (others may be there too, which is fine):

```text
nintendo_udsg-r_std_003.bfttf
nintendo_udsg-r_org_zh-cn_003.bfttf
nintendo_udsg-r_ext_zh-cn_003.bfttf
nintendo_udjxh-db_zh-tw_003.bfttf
nintendo_udsg-r_ko_003.bfttf
nintendo_ext_003.bfttf
```

### Step 10: Connect your game data

Double-click **`launchers\Configure Game Data.bat`** in `C:\WindowSmash`. (If **Windows protected your PC** appears, click **More info**, then **Run anyway**.) Answer its questions:

| It asks for | Answer |
| :--- | :--- |
| The path to your romfs.bin | `D:\MyDump\romfs.bin` |
| The byte offset | Press <kbd>Enter</kbd>. |
| The folder with your fonts | `D:\MyDump\fonts` |
| An aoc.cfg | Press <kbd>Enter</kbd>; [step 11](#step-11-optional-turn-on-your-dlc) handles DLC. |

It should finish with **Game-data setup complete**. Don't start the game yet: it isn't built.

If it says `No unencrypted RomFS header at this offset`, your `romfs.bin` is still encrypted or isn't the game's data (see [Part 2](#part-2-gather-your-game-files)).

<details>
<summary><strong>Using a decrypted program <code>.nca</code> instead of <code>romfs.bin</code></strong></summary>

<br>

Skip `launchers\Configure Game Data.bat` and run these two commands instead. The first points WindowSmash at the data inside your program file; the second copies the fonts.

```powershell
python tools/nca.py romfs-cfg "D:\MyDump\program.nca" --keys "D:\MyKeys" --out dump/modules/romfs.cfg
```

```powershell
New-Item -ItemType Directory -Force dump\fonts | Out-Null; Copy-Item D:\MyDump\fonts\*.bfttf dump\fonts
```

If the first command says `the RomFS section is encrypted`, the file isn't decrypted, and you need a `romfs.bin` after all.

</details>

### Step 11: (Optional) Turn on your DLC

With your DLC `.nsp` files in `D:\MyDump\dlc`, run:

```powershell
python tools/nca.py aoc-cfg "D:\MyDump\dlc" --keys "D:\MyKeys" --out dump/modules/aoc.cfg
```

It answers `wrote dump/modules/aoc.cfg:` and how many add-ons it found. 13.0.5 knows every DLC fighter, Kazuya and Sora included. More in the [DLC guide](DLC-AND-UNLOCKS.md).

### Step 12: (Optional) Support more kinds of controllers

Xbox controllers and the keyboard work without this. PlayStation, Switch Pro and most other controllers need the free **SDL3** library:

1. On **<https://github.com/libsdl-org/SDL/releases>**, find the newest release whose number starts with **3** and download **`SDL3-devel-…-VC.zip`** from its **Assets** (the `-VC.zip`, not `-mingw`).
2. Extract it into a new folder `C:\WindowSmash\deps`.
3. Rename the extracted folder (named like `SDL3-3.2.24`) to **`SDL3`**, so that `C:\WindowSmash\deps\SDL3` holds `include`, `lib` and `LICENSE.txt`.

Do this before step 14. If you add it later, run [step 14](#step-14-build-the-game) again; it's quick the second time.

### Step 13: Take out the fighters

Most of the fighters' code is packed inside the game data. This unpacks it:

```powershell
python tools/arc.py nro
```

It lists about a hundred `.nro` files: one per fighter, plus some extras.

---

## Part 6: Build and play

### Step 14: Build the game

This is the long step: your PC translates the whole game into a Windows program. Pick the command that fits the **Installed RAM** from [step 1](#step-1-check-your-pc):

**Less than 16 GB:**

```powershell
python tools/recomp.py all dump/exefs --out build --jobs 4
```

**16 GB:**

```powershell
python tools/recomp.py all dump/exefs --out build --jobs 8
```

**32 GB or more:**

```powershell
python tools/recomp.py all dump/exefs --out build --jobs 16
```

- It takes **30 minutes to a few hours**, and prints a lot while it works. The PC can be slow meanwhile. A bar on the last line shows how far it has got and the time left (once the first files are compiled); the window's title shows it too.
- If it's interrupted (a restart, the PC going to sleep), run the same command again: it carries on where it stopped.
- It's finished when a line says `linked … objects into build\ssbu.exe` and the `PS C:\WindowSmash>` line comes back.

> [!TIP]
> Set the PC not to sleep while plugged in (**Settings → System → Power**) until the build is done.

### Step 15: Play

Double-click **`Play SSBU.bat`** in `C:\WindowSmash`.

- The first time, it asks for **your online name**, the name other players see. Press <kbd>Enter</kbd> to keep the suggested one.
- If Windows Security asks whether to allow `ssbu.exe` on networks, allow it; it's needed for playing online.
- A small window called **SSBU log** opens minimized next to the game. Leave it: it closes with the game.
- The first start takes a little longer, and some effects can stutter the first time they appear (see step 16).

> [!TIP]
> For a desktop shortcut, right-click `Play SSBU.bat` → **Show more options** (Windows 11) → **Send to → Desktop (create shortcut)**.

**What next?**

- Controls and settings: the [playing guide](PLAYING.md). `launchers\Play SSBU Offline.bat` starts the game without going online.
- Playing with friends: the [multiplayer guide](MULTIPLAYER.md).
- Unlocking every fighter on your save: [DLC and unlocks](DLC-AND-UNLOCKS.md#unlock-every-base-game-fighter).

### Step 16: (Optional) Prepare the shaders

The game's graphics programs (*shaders*) are prepared the first time each one is needed, which is why a new effect can stutter once. To prepare them all in advance, double-click **`launchers\Prepare Shaders.bat`**. It runs at low priority, so you can keep using the PC, and shows a progress bar with the time left: about half an hour on a 16-thread processor, an hour or two on a 4- to 8-thread one. You can close it at any time and start it again later; it keeps what it has done.

</details>

---

## Afterwards

### Update WindowSmash

Double-click **`Install WindowSmash.bat`** again. It downloads the newest WindowSmash from GitHub if there is one, replaces the files that changed, and builds again, which takes minutes rather than hours. Your game files, settings, save and a lobby code in `online_server.txt` stay as they are.

**Built from another game version before?** WindowSmash used to build other game versions too, most often 11.0.0. Such a folder is set up again as 13.0.5 the next time `Install WindowSmash.bat` runs, once your game folder has the 13.0.5 update: the game's files are prepared again and the game is built again, while your save, settings and prepared shaders stay. [A folder built from another version](UPDATES.md#a-folder-built-from-another-version) has the details, including what to do with an old `v13.0.5` folder.

By hand: download the ZIP again, unblock it as in [step 5](#step-5-download-windowsmash), and copy the contents of its `WindowSmash-main` folder over `C:\WindowSmash`, replacing the files. Then build again ([step 14](#step-14-build-the-game), or double-click `launchers\Build WindowSmash.bat`). With Git, run `git pull` instead of downloading.

### If something goes wrong

| What you see | What to do |
| :--- | :--- |
| `Install WindowSmash.bat` stops at a check | It says what the PC lacks: 64-bit Windows 10 or 11, a processor with AVX2, 8 GB of RAM, or free space (about 60 GB, less what is built already). Free some space or move WindowSmash to a drive with room, then run it again. |
| `Install WindowSmash.bat` says winget is not available | The compiler download didn't work either (the message above it says why). Install the compiler by hand: download the **Build Tools for Visual Studio** from <https://visualstudio.microsoft.com/visual-cpp-build-tools/>, tick **Desktop development with C++** and, under Installation details, **C++ Clang tools for Windows**, install, then run `Install WindowSmash.bat` again. |
| `Microsoft's license terms were not accepted` | The compiler needs Microsoft's C runtime and Windows SDK, and those come under Microsoft's license terms, whichever way you get them. To accept them, run `Install WindowSmash.bat` again and type `yes` when it asks. |
| The installer's window says a folder has a problem | The line under the folder says what: no `prod.keys`, no `.nca` files, no program among the game files. Point it at the right folder ([Part 2](#part-2-gather-your-game-files)) and click **Check files**. |
| `WindowSmash needs version 13.0.5` (in the installer's window, or when the build stops at its step 4 or 6) | Your game files are another version of the game: the game without its 13.0.5 update, or with another update. Put the game's 13.0.5 update `.nsp` in the game folder, next to the game (with yuzu or Ryujinx, install the update there). If the installer's window said it, click **Check files** and go on; if the build said it, run `Install WindowSmash.bat` again. A `romfs.bin` you made yourself must be made again with the 13.0.5 update applied. |
| `… set up from version 11.0.0 of the game` (or another version) | The folder was built by an older WindowSmash, from another game version. With the 13.0.5 update in your game folder, the installer sets the folder up again as 13.0.5 and builds again; your save, settings and prepared shaders stay. Without the update, it stops before changing anything and says what to add. See [a folder built from another version](UPDATES.md#a-folder-built-from-another-version). |
| `This WindowSmash was built from another version of the game`, when the game starts | The game was built from a version other than 13.0.5. Put the 13.0.5 update in your game folder and run `Install WindowSmash.bat` again: it sets the folder up again, keeping your save and settings. |
| `is the game's update, whose data is a patch over the game's own` | Your folder has the update but not the game itself. Put the game's own (base) program `.nca` next to it; from yuzu or Ryujinx, the installed-content folder has both. |
| `python` is not recognized, or *Python was not found* | Python isn't installed, or **Add python.exe to PATH** wasn't ticked. Install it again ([step 3](#step-3-install-python)), then open a new PowerShell window. Or double-click `Install WindowSmash.bat`, which brings its own. |
| `No module named 'Cryptodome'` or `No module named 'zstandard'` | Run [step 7](#step-7-install-windowsmashs-helpers) (with the same Python as the failing command). |
| **Windows protected your PC**, or **The publisher could not be verified** | Click **More info**, then **Run anyway** (or **Run**). Unblocking the ZIP first ([step 5](#step-5-download-windowsmash)) prevents it; `Install WindowSmash.bat` unblocks the rest of the folder itself. |
| `Cannot find path 'D:\MyDump…'` or `No such file or directory` | A file or folder isn't where the command expects it. Check the names from [step 2](#step-2-put-your-game-files-in-three-folders), including the ending (`program.nca`). No D: drive? Change `D:\` to `C:\` in the command. |
| `bad header_key?`, `missing title key` or `missing key_area_key` | Your keys are missing or too old. Check that `prod.keys` and `title.keys` are in `D:\MyKeys`, dumped from your Switch after its latest system update. |
| `not a PFS0 … wrong key?` in step 8 | That `.nca` probably isn't the program. Find the right one with the first command of [step 8](#step-8-copy-the-games-program). |
| `No unencrypted RomFS header` in step 10 | `romfs.bin` is still encrypted, or isn't the game's data. See [Part 2](#part-2-gather-your-game-files). |
| `Missing required font` in step 10 | Not every font was found. Check that `D:\MyFirmware` holds all your firmware's `.nca` files, then run [step 9](#step-9-take-the-fonts-out-of-your-firmware) again. |
| Step 13 fails with `romfs.cfg` or `data.arc` in the message | The game data isn't connected yet. Do [step 10](#step-10-connect-your-game-data) first. |
| `The system cannot find the file specified`, or `clang-cl` in an error, when the build starts | No compiler was found. Get one ([step 4](#step-4-install-the-compiler)); with Visual Studio, check the Clang tick (the tip at the end of that step). `python tools\build_game.py --check` tries the compiler and says which one it found. |
| The build says `FAILED`, or the PC runs out of memory | Run the build again with a smaller `--jobs` number, such as `--jobs 4`. |
| `The game isn't built here yet` when starting the game | The build hasn't finished. Run [step 14](#step-14-build-the-game) again. |
| The game closes, or stays black | Look at the end of `C:\WindowSmash\build\play.log` and in the `reports` folder. [Getting help](../README.md#getting-help) says what to send. |
| No music, or movies don't play | The build was made before the codecs came with WindowSmash. Download the newest WindowSmash and build again ([Update WindowSmash](#update-windowsmash)); see [Audio and video codecs](#audio-and-video-codecs). |
| A controller does nothing | Press <kbd>F2</kbd> in the game to see which pads it found. Anything other than an Xbox pad needs [step 12](#step-12-optional-support-more-kinds-of-controllers), then step 14 again. |
| Effects stutter the first time | That's shaders being prepared. [Step 16](#step-16-optional-prepare-the-shaders) does them all ahead of time. |

### Quick version

The example paths are your game files in `D:\MyDump`, your keys in `D:\MyKeys` and your firmware in `D:\MyFirmware`; `python` must be 3.10 or newer. (`launchers\Build WindowSmash.bat` does the same from `python -m pip` on, and takes an emulator's installed-content folder as the game folder.)

```powershell
git clone https://github.com/SNO2207/WindowSmash.git
cd WindowSmash
python -m pip install -r requirements.txt
python tools/msvc_deps.py install    # the compiler (asks about Microsoft's license terms); not needed with Visual Studio's Clang tools
python tools/nca.py exefs "D:\MyDump\program.nca" --keys "D:\MyKeys" --out dump/exefs
Get-ChildItem D:\MyFirmware\*.nca | ForEach-Object { python tools/nca.py romfs-get $_.FullName --keys D:\MyKeys --match .bfttf --flat --out D:\MyDump\fonts 2>$null }
& ".\launchers\Configure Game Data.bat"    # your unencrypted romfs.bin with 13.0.5 applied, and D:\MyDump\fonts
python tools/nca.py aoc-cfg "D:\MyDump\dlc" --keys "D:\MyKeys" --out dump/modules/aoc.cfg    # optional: DLC
python tools/arc.py nro
python tools/recomp.py all dump/exefs --out build --jobs 16
& ".\Play SSBU.bat"
```

---

## Reference

### Requirements

| Item | Requirement |
| :--- | :--- |
| Operating system | Windows 10 or 11, 64-bit. |
| CPU | x64 with AVX2, FMA and F16C. |
| Graphics | A Direct3D 11 GPU and a working driver. |
| Python | **3.10 or newer**, with the packages from `requirements.txt` (`pycryptodomex`, and `zstandard` on Pythons before 3.14, which has `compression.zstd` built in). `Install WindowSmash.bat` puts a copy of 3.14 into `deps/python` when there is none. |
| C toolchain | `clang-cl` and `lld-link` with Microsoft's C runtime and a Windows SDK: either `deps/llvm` with `deps/msvc` from `tools/msvc_deps.py` ([without Visual Studio](#the-compiler-without-visual-studio)), or Visual Studio 2022 or newer, or its Build Tools, with **Desktop development with C++** (which includes a Windows SDK) and **C++ Clang tools for Windows**. `tools/cc.py` takes `deps` when both folders are there, else finds Visual Studio's `clang-cl`; set `SSBU_CLANG` to use another (it wins over both). |
| Game inputs | Version 13.0.5 only: its ExeFS (from the 13.0.5 update's Program NCA), an unencrypted RomFS with the 13.0.5 update applied (or a layered one from `tools/update.py`), and six firmware fonts. Keys for reading packages. `tools/build_game.py` checks main's build ID (step 4) and data.arc's version (step 6). |
| Disk space | About 17 GB for the game data, which stays where it is, and around 30 GB for `build/`. |

To keep WindowSmash's Python packages apart from others, use a virtual environment:

```powershell
py -3.14 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

### Folder layout

```text
WindowSmash/
  Install WindowSmash.bat     sets everything up and builds the game
  Play SSBU.bat               starts the game
  launchers/                  the other launchers: offline play, private rooms, shaders, unlocks, building
  docs/                       these guides
  runtime/                    the Windows runtime (source)
  tools/                      the recompiler and helper tools (source); tools/dev/ holds the developers' diagnostics
  dump/
    exefs/                    main, sdk, subsdk0, rtld, main.npdm (step 8)
    modules/
      romfs.cfg               where the RomFS is: a file path, then a byte offset (step 10)
      aoc.cfg                 optional: installed DLC (step 11)
    nro/                      fighter and item modules (step 13); bases.txt after the build
    fonts/                    the six firmware fonts (step 10)
    shadercache/              compiled shaders, made while playing
  deps/
    SDL3/                     optional: SDL3's Windows development package (step 12)
    python/                   optional: the Python Install WindowSmash.bat set up
    llvm/, msvc/              optional: the compiler without Visual Studio (step 4)
  third_party/                the music and movie codecs' sources (libopus, libvpx)
  build/                      generated C, objects, ssbu.exe, play.log (step 14)
    codecs/                   the codec libraries, built from third_party
  save/                       your save, made when you first play
  version.txt                 the game version, 13.0.5 (written by tools/build_game.py)
```

`dump/`, `deps/`, `build/`, `save/`, `version.txt`, keys and packages are ignored by Git.

### Other ways to point at the RomFS

`dump/modules/romfs.cfg` holds two lines: the path of the file with the RomFS, then the byte at which the RomFS starts in it.

```text
D:\MyDump\romfs.bin
0
```

- **A plain RomFS image** (`romfs.bin`) starts at 0. `launchers\Configure Game Data.bat` checks its header before writing the file.
- **A decrypted NCA** has its RomFS further in. `python tools/nca.py romfs-cfg <nca> --keys <dir> --out dump/modules/romfs.cfg` finds the offset and writes the file. It refuses an NCA whose RomFS section is encrypted.
- **The 13.0.5 update over the game's own data** uses a layered RomFS made by `tools/update.py`: see [the 13.0.5 update](UPDATES.md#how-the-update-is-applied).

The program and the data must both be 13.0.5's.

### Fonts from one firmware archive

Step 9 tries every firmware file. To read one archive (the shared-font system archives hold the fonts):

```powershell
python tools/nca.py romfs-get "D:\MyFirmware\font-archive.nca" --keys "D:\MyKeys" --match .bfttf --flat --out dump/fonts
```

### Audio and video codecs

The game's music is Opus, and its movies are VP9. Their decoders, libopus and libvpx's VP9 decoder, come with WindowSmash as source in `third_party/` and are compiled into the game by the build (`tools/codec_libs.py`, a few seconds the first time): nothing to install. [`third_party/README.md`](../third_party/README.md) says which upstream versions and files they are.

To link your own builds of them instead, set these before the build command, in the same PowerShell window:

- **libopus:** list your static Windows x64 `.lib` files in `SSBU_OPUS_LIBS`, separated by semicolons:

  ```powershell
  $env:SSBU_OPUS_LIBS = 'D:\Codecs\opus.lib;D:\Codecs\opus_sse41.lib'
  ```

- **libvpx:** set `SSBU_VPX_DIR` to a folder with `include/` and `lib/Win64/Release/libvpx.lib`:

  ```powershell
  $env:SSBU_VPX_DIR = 'D:\Codecs\libvpx'
  ```

[`tools/cc.py`](../tools/cc.py) has the exact link settings.

### SDL3 somewhere else

Instead of `deps/SDL3`, set `SSBU_SDL3_DIR` to the extracted `SDL3-<version>` folder before building:

```powershell
$env:SSBU_SDL3_DIR = 'D:\Libraries\SDL3-3.x.y'
```

The runtime is compiled against SDL3's headers only and loads `SDL3.dll` when it starts; the link step copies the DLL next to `build/ssbu.exe`. Without SDL3, the build still works, and XInput (Xbox) pads 1–4 are players 1–4. SDL3 is under the zlib license ([third-party notices](../NOTICE.md)).

### The compiler without Visual Studio

`python tools\msvc_deps.py install` (which `Install WindowSmash.bat` and `launchers\Build WindowSmash.bat` run when no compiler works yet; the latter downloads while it prepares the game files) sets up two folders. It needs no administrator rights and installs nothing into Windows:

| Folder | What it holds | Where it comes from | Download | On disk |
| :--- | :--- | :--- | ---: | ---: |
| `deps\llvm` | LLVM's `clang-cl` (the compiler), `lld-link` (the linker), `llvm-lib` and clang's own headers | LLVM's official release on GitHub (<https://github.com/llvm/llvm-project/releases>), the one `tools/msvc_deps.py` names, checked against its SHA-256 | 276 MB | 226 MB |
| `deps\msvc` | Microsoft's C runtime (the static one the game uses, with its headers) and the Windows SDK's headers and 64-bit libraries | Microsoft's servers (`download.visualstudio.microsoft.com`): the packages Visual Studio's own installer uses, as Visual Studio's channel list (<https://aka.ms/vs/18/stable/channel>) names them, each checked against the SHA-256 the list gives | 294 MB | 319 MB |

For comparison, the Visual Studio Build Tools with the C++ workload and Clang are about 4 GB to download and 12 GB on disk, by Microsoft's own figures in that list.

**Microsoft's license.** The C runtime and the Windows SDK are Microsoft's software, under Microsoft's license terms for the Visual Studio Build Tools: <https://go.microsoft.com/fwlink/?LinkId=2327714> (the address the channel list gives). They aren't part of WindowSmash or covered by its license, and WindowSmash never includes them: they come straight from Microsoft to your PC, for building the game there. Before it downloads anything of Microsoft's, `msvc_deps.py` shows that address and asks whether you accept the terms; the only thing it reads before your answer is the channel list, to find the address. LLVM is free software under the Apache License 2.0 with LLVM exceptions.

How it works:

- Of Microsoft's packages it keeps what the build uses, laid out as clang-cl's `/winsysroot` option expects: `VC\Tools\MSVC\<version>\include` and `lib\x64` (`libcmt`, `libvcruntime`, `oldnames`), and `Windows Kits\10\Include\<version>\ucrt`, `um`, `shared` with `Lib\<version>\ucrt\x64` and `um\x64`. The SDK's `.msi` files are unpacked with `msiexec /a`, an administrative install, which only extracts their files.
- The C runtime and the SDK are the versions the Build Tools would install: the ones Visual Studio's current release recommends.
- `tools/cc.py` builds with `deps\llvm` and `/winsysroot deps\msvc` whenever both folders are there (each has a `windowsmash.json`, written last, that records versions, sources and sizes), before any Visual Studio. `SSBU_CLANG` still wins over both. `python tools\build_game.py --check` says which compiler it used.
- `python tools\msvc_deps.py plan` lists what it would download, without downloading; `status` shows what is set up and its sizes; `remove` deletes both folders, after which the build looks for Visual Studio again.
- Unpacking LLVM's archive takes about 1.2 GB of memory for a few seconds. The downloads go to `deps\downloads` and are deleted afterwards.

### What the build does

`tools/recomp.py all` translates each module (`main`, `subsdk0`, `sdk` and the fighter modules) to C, makes the relocation images and registries, compiles everything with `clang-cl`, compiles the runtime, and links `build/ssbu.exe`. Builds after the first are incremental. The first build gives each fighter module an address and records it in `dump/nro/bases.txt`; keep that file with your build, and don't copy it from another build.

Before compiling, it lists imported symbols that nothing provides in `build/imports.unresolved.txt`; a *strong* one there is the first sign that the game needs something the runtime doesn't have yet ([the imports check](TECHNICAL.md#the-imports-check)). It also checks that each of the runtime's hooks into the game's code finds the 13.0.5 function it expects, and stops if one doesn't ([code tied to the game version](TECHNICAL.md#code-tied-to-the-game-version)).

Close the game before building again: the link step replaces `ssbu.exe`. A successful build doesn't prove that every mode works.

If the log says the runtime *harvested* a fighter module at run time, the module was missing from `dump/nro/`: extract or collect it and build again. If the runtime traps, keep the matching part of the log and note the build revision. The linker map in `build/` helps to read host stack frames.

### Tests

`tests/requirements-dev.txt` adds the optional test tools. The [technical guide](TECHNICAL.md#verification-and-debugging) covers the source-only tests and the game-backed differential checks.
