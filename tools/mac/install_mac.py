#!/usr/bin/env python3
"""WindowSmash on a Mac in one go: what Install WindowSmash.command runs (the macOS counterpart of
Install WindowSmash.bat, tools/install_windowsmash.ps1, and of tools/linux/install_linux.py, whose
platform-neutral helpers it uses: the update, the downloads, the folder checks).

Usage: Install WindowSmash.command [--game <dir>] [--keys <dir>] [--firmware <dir>] [--online-name <name>]
                                   [--jobs N] [--skip-build] [--no-update] [--no-wizard] [--yes] [--dry-run]

  1. update     the newest WindowSmash from GitHub (the BRANCH below): a git checkout of it is
                pulled; a ZIP download gets the files of the newer commit (tools/source_version.txt
                says which commit it is), then this installer starts again as the new version
  2. this Mac   Apple silicon (not under Rosetta), macOS 14 or newer, the memory, the free space,
                and whether the folder is somewhere awkward (iCloud Drive, Downloads, a disk that
                is not APFS)
  3. tools      Apple's Command Line Tools (clang, the macOS SDK, git): when they are missing,
                macOS's own window installs them (xcode-select --install) and this waits
  4. Python     a Python of WindowSmash's own in deps/venv, with requirements.txt
  5. SDL3       deps/SDL3/SDL3.framework, out of the newest SDL3-<version>.dmg of SDL's releases
                on GitHub, checked against the SHA-256 the release lists (window, pads, sound);
                then tools/build_game.py --check tries everything the build needs
  6. your files the folders with the game, the keys and the firmware (found in Ryujinx's and in
                yuzu's forks' folders), the name other players see online, and the options: in
                dialogs, or with --no-wizard in this window
  7. build      tools/build_game.py with those folders, kept awake (caffeinate): the game's
                program, fonts, data, DLC and fighters, then build-mac/WindowSmash.app, which
                compiles the whole game the first time
  8. launchers  Play SSBU in ~/Applications (and on the Desktop): it starts the game
  9. firewall   only when the Application Firewall is on: a rule letting the game take incoming
                connections (hosting a private room), with an administrator's password

A step whose result is there already is skipped, so running it again carries on where it stopped.
--yes takes the folders found (or given) without asking; --dry-run says what each step would do.
"""
import argparse
import hashlib
import json
import os
import platform
import plistlib
import re
import shlex
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "linux"))
import install_linux as il  # noqa: E402  (the update, download and folder helpers)

# Updates come from this branch: the macOS port's own until it is merged into main (then: "main").
# SSBU_UPDATE_BRANCH names another, for testing one.
BRANCH = os.environ.get("SSBU_UPDATE_BRANCH") or "macos-port"
DEPS = il.DEPS
VENV = il.VENV
DOWNLOADS = os.path.join(DEPS, "downloads")
SDL_DIR = os.path.join(DEPS, "SDL3")
FRAMEWORK = os.path.join(SDL_DIR, "SDL3.framework")
APP = os.path.join(ROOT, "build-mac", "WindowSmash.app")   # the game (tools/mac/build_mac.py)
STEPS = 9
MIN_MACOS = (14, 0)
MIN_SDK = (15, 0)   # (the oldest macOS SDK the runtime compiles with: tools/mac/toolchain.py's MIN_SDK)
NEED_DISK_GB = 40   # (the C and the objects: 8 GB; the program: 2; the update's data: 4; a game to decrypt first: 17)
MIN_RAM_GB = 8
SDL_RELEASES = "https://api.github.com/repos/libsdl-org/SDL/releases?per_page=30"
SDL_FALLBACK = {"version": "3.4.18", "name": "SDL3-3.4.18.dmg",   # when GitHub's API cannot be read
                "url": "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.dmg",
                "sha256": "c1adf3da91108dbc3e4205da5dacebc203085d283fa89e86eb2c889abd03a853"}
SDL_IN_DMG = os.path.join("SDL3.xcframework", "macos-arm64_x86_64", "SDL3.framework")
FIREWALL = "/usr/libexec/ApplicationFirewall/socketfilterfw"
LAUNCHER_NAME = "Play SSBU"
LAUNCHER_ID = "io.github.sno2207.windowsmash.play"
ICON = os.path.join(ROOT, "docs", "assets", "windowsmash.ico")
LAUNCHERS = ["Install WindowSmash.command", "Play SSBU.command", "launchers/Play SSBU Offline.command",
             "launchers/Host SSBU Netplay.command", "launchers/Join SSBU Netplay.command",
             "launchers/Unlock All Fighters.command", "launchers/Build WindowSmash.command",
             "tools/mac/launch.sh"]
EMULATORS = ("yuzu", "eden", "sudachi", "citron", "suyu", "torzu")   # yuzu and the forks with its layout
SSBU_TITLES = "01006a800016"   # (Super Smash Bros. Ultimate's title ids: the game's, its update's and DLC's)
NAME_MAX = 32   # (as the Windows setup window's box)
TITLE = "WindowSmash setup"
OPTION_LAUNCHER = "Put Play SSBU in ~/Applications (your own Applications folder) and on the Desktop"
OPTION_FIREWALL = "Let the game through the Application Firewall (asks for your password)"

Stop = il.Stop
step, info, would, read_text = il.step, il.info, il.would, il.read_text


class NoDialogs(Exception):
    """osascript could not show a window (no screen to show it on: over ssh, say)."""


# ---- what the steps decide by (tested in tests/test_mac_installer.py) ------------------------------
def macos_version(text):
    """(major, minor) from sw_vers -productVersion's answer ("14.6.1"), or None."""
    m = re.match(r"\s*(\d+)(?:\.(\d+))?", text or "")
    return (int(m.group(1)), int(m.group(2) or 0)) if m else None


def mac_folder_candidates(home, app_support, data_home, config_home, game_dirs=()):
    """Where the emulators keep keys, firmware and games on a Mac, most likely first: Ryujinx (and
    Ryubing, which keeps its folder name) in Application Support, with the folders it plays games
    from (game_dirs: Ryujinx plays a game from where it is, rather than installing it); yuzu and
    the forks with its layout, in Application Support or ~/.local/share; Ryujinx's older
    ~/.config/Ryujinx; ~/.switch for keys."""
    keys, firmware, game = [], [], []
    ryujinx = os.path.join(app_support, "Ryujinx")
    keys.append(os.path.join(ryujinx, "system"))
    firmware.append(os.path.join(ryujinx, "bis", "system", "Contents", "registered"))
    game += list(game_dirs)
    game.append(os.path.join(ryujinx, "bis", "user", "Contents", "registered"))
    game.append(os.path.join(ryujinx, "sdcard", "Nintendo", "Contents", "registered"))
    for d in [os.path.join(top, name) for top in (app_support, data_home) for name in EMULATORS]:
        keys.append(os.path.join(d, "keys"))
        firmware.append(os.path.join(d, "nand", "system", "Contents", "registered"))
        game.append(os.path.join(d, "nand", "user", "Contents", "registered"))
        game.append(os.path.join(d, "sdmc", "Nintendo", "Contents", "registered"))
    old = os.path.join(config_home, "Ryujinx")
    keys.append(os.path.join(old, "system"))
    firmware.append(os.path.join(old, "bis", "system", "Contents", "registered"))
    game.append(os.path.join(old, "bis", "user", "Contents", "registered"))
    keys.append(os.path.join(home, ".switch"))
    return {"keys": keys, "firmware": firmware, "game": game}


def ryujinx_game_dirs(text):
    """The folders Ryujinx's Config.json lists its games in (game_dirs)."""
    try:
        dirs = json.loads(text).get("game_dirs") or []
    except (ValueError, AttributeError):
        return []
    return [d for d in dirs if isinstance(d, str) and d]


def smash_first(folders, names_in):
    """The folders, those whose files look like Super Smash Bros. Ultimate's first (a name with
    "smash" or one of its title ids in it); names_in(folder) lists a folder's file names."""
    def looks(folder):
        return any("smash" in n.lower() or SSBU_TITLES in n.lower() for n in names_in(folder))
    hit = [f for f in folders if looks(f)]
    return hit + [f for f in folders if f not in hit]


def select_sdl3_asset(releases):
    """The newest release (not a prerelease or draft) with an SDL3-3.x.y.dmg, as GitHub's API
    lists them (the Windows installer's Select-Sdl3Asset, for the Mac's package): its version, the
    DMG's URL and name, and the SHA-256 the API gives for it (digest), if any; or None."""
    best = None
    for r in releases or []:
        if not isinstance(r, dict) or r.get("prerelease") or r.get("draft"):
            continue
        for a in r.get("assets") or []:
            m = re.match(r"^SDL3-(3\.\d+\.\d+)\.dmg$", a.get("name") or "")
            if not m:
                continue
            v = tuple(int(x) for x in m.group(1).split("."))
            if best is None or v > best["key"]:
                d = re.match(r"^sha256:([0-9a-fA-F]{64})$", a.get("digest") or "")
                best = {"key": v, "version": m.group(1), "url": a.get("browser_download_url"), "name": a["name"],
                        "sha256": d.group(1).lower() if d else None}
    return best


def firewall_state(text):
    """The Application Firewall's state from socketfilterfw --getglobalstate: off, on, or all
    (on, and blocking every incoming connection)."""
    m = re.search(r"State = (\d+)", text or "")
    if m:
        return {0: "off", 1: "on"}.get(int(m.group(1)), "all")
    return "on" if "enabled" in (text or "").lower() else "off"


def mount_types(text):
    """[(mount point, filesystem)] from mount's lines: /dev/disk3s5 on /System/Volumes/Data (apfs, local, ...)."""
    out = []
    for line in (text or "").splitlines():
        m = re.match(r"^.+? on (.+) \(([^,()]+)[,)]", line)
        if m:
            out.append((m.group(1), m.group(2).strip()))
    return out


def inside(path, folder):
    path, folder = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(folder))
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def location_warnings(root, home, exists=os.path.exists, filesystem=None):
    """What is awkward about where this folder is (the warnings to show; nothing stops)."""
    out = []
    cloud = os.path.join(home, "Library", "Mobile Documents")
    for name in ("Desktop", "Documents"):
        if inside(root, os.path.join(home, name)) and exists(os.path.join(cloud, "com~apple~CloudDocs", name)):
            out.append(f"this folder is in {name}, which iCloud Drive keeps in sync: it would upload the build too "
                       "(around 30 GB). Better: move WindowSmash to your home folder (for example ~/WindowSmash) and "
                       "run it there.")
    if inside(root, cloud):
        out.append("this folder is in iCloud Drive, which would upload the build too (around 30 GB). Better: move "
                   "WindowSmash to your home folder (for example ~/WindowSmash) and run it there.")
    if inside(root, os.path.join(home, "Downloads")):
        out.append("this folder is in Downloads: macOS asks the game (and Terminal) for access to that folder, and "
                   "cleaning it up would take WindowSmash with it. Better: your home folder, for example ~/WindowSmash.")
    if filesystem and filesystem.lower() not in ("apfs", "hfs"):
        out.append(f"this disk is formatted {filesystem}, not APFS: the build relies on hard links, file modes and "
                   "case, which it may not keep. An APFS disk (the Mac's own) is better.")
    return out


def clean_name(name):
    """An online name as the game takes it: one line, at most NAME_MAX characters."""
    return " ".join((name or "").split())[:NAME_MAX]


def osascript_command(script, args):
    """How a dialog script runs: its text on osascript's standard input, every string it shows as
    an argument of its own (argv in the script), so nothing is ever put into the script itself."""
    return ["osascript", "-", *[str(a) for a in args]], script


def ico_png(data):
    """The biggest PNG picture in an .ico file (docs/assets/windowsmash.ico holds a 256-pixel one), or None."""
    if len(data) < 6:
        return None
    _reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    best, size = None, -1
    for i in range(count if kind == 1 else 0):
        if 6 + 16 * (i + 1) > len(data):
            break
        w, h, _c, _r, _p, _b, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        png = data[offset:offset + length]
        if png[:8] == b"\x89PNG\r\n\x1a\n" and (w or 256) > size:
            best, size = png, w or 256
    return best


def launcher_info(bundle_id=LAUNCHER_ID, name=LAUNCHER_NAME):
    """Info.plist of the Play SSBU app: a script (Contents/MacOS/play) that starts the game, with no
    Dock icon of its own (LSUIElement: the game has its own)."""
    return {"CFBundleDevelopmentRegion": "en", "CFBundleDisplayName": name, "CFBundleExecutable": "play",
            "CFBundleIconFile": "windowsmash", "CFBundleIdentifier": bundle_id, "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": name, "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
            "LSApplicationCategoryType": "public.app-category.action-games", "LSMinimumSystemVersion": "%d.%d" % MIN_MACOS,
            "LSUIElement": True, "NSHighResolutionCapable": True}


def launcher_script(root):
    """Contents/MacOS/play: tools/mac/launch.sh of this folder, asking in dialogs instead of in a
    Terminal window (--gui); or, when the folder has gone, a message saying so."""
    return f"""#!/bin/bash
# {LAUNCHER_NAME}: starts WindowSmash's game, online, from the folder below, as Play SSBU.command does but
# asking in dialogs. Made by Install WindowSmash.command; running that again makes this anew.
root={shlex.quote(root)}
if [ ! -f "$root/tools/mac/launch.sh" ]; then
    osascript - "WindowSmash is not where it was" "{LAUNCHER_NAME} starts the game from $root, which is not there \\
any more. Double-click Install WindowSmash.command in WindowSmash's folder to make {LAUNCHER_NAME} again." <<'EOF'
on run argv
    activate
    display alert (item 1 of argv) message (item 2 of argv) as critical
end run
EOF
    exit 1
fi
exec /bin/bash "$root/tools/mac/launch.sh" --gui play
"""


# ---- small helpers ---------------------------------------------------------------------------------
def output(cmd, **kw):
    """(exit code, what it printed), or (None, why) when it cannot run."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    except OSError as e:
        return None, str(e)
    return r.returncode, (r.stdout + r.stderr).strip()


def sysctl(name):
    code, text = output(["sysctl", "-in", name])
    return text if code == 0 else ""


def shown(path):
    """A path for messages: under the home folder as ~/..."""
    home = os.path.expanduser("~")
    return "~" + path[len(home):] if inside(path, home) and path != home else path


def venv_python():
    return il.venv_python()


def step_header(n, text):
    step(f"[{n}/{STEPS}] {text}")


def ssl_certificates(dry=False):
    """Python's downloads (GitHub's API, the update, SDL3) check each server's certificate against a
    list of trusted ones; a Python that has no such list of its own (some do not read macOS's) gets
    macOS's, exported into deps/ (a dry run: the temporary folder) for this run and the build's."""
    paths = ssl.get_default_verify_paths()
    if os.environ.get("SSL_CERT_FILE") or (paths.cafile and os.path.isfile(paths.cafile)) or (
            paths.capath and os.path.isdir(paths.capath) and os.listdir(paths.capath)):
        return
    r = subprocess.run(["security", "find-certificate", "-a", "-p", "/System/Library/Keychains/SystemRootCertificates.keychain"],
                       capture_output=True, text=True)
    if r.returncode == 0 and "BEGIN CERTIFICATE" in r.stdout:
        pem = os.path.join(tempfile.gettempdir() if dry else DEPS, "macos-root-certificates.pem")
        il.write_atomic(pem, r.stdout.encode())
        os.environ["SSL_CERT_FILE"] = pem


def typed_path(text):
    """A folder as typed, or dragged into Terminal (which escapes its spaces: My\\ Games)."""
    text = text.strip()
    try:
        words = shlex.split(text)
    except ValueError:
        words = []
    return os.path.expanduser(words[0] if len(words) == 1 else text.strip('"').strip("'"))


def ask(label, default, interactive):
    if not interactive:
        return default
    try:
        answer = input(f"  {label} [{default or ''}]: ")
    except EOFError:
        print()
        return default
    return typed_path(answer) if answer.strip() else default


# ---- dialogs (osascript: the strings travel as arguments, never inside the script) ------------------
DIALOG = """on run argv
    set {theText, theTitle, okButton, cancelButton} to items 1 thru 4 of argv
    set theButtons to items 5 thru -1 of argv
    activate
    try
        if cancelButton is "" then
            set r to display dialog theText with title theTitle buttons theButtons default button okButton with icon note
        else
            set r to display dialog theText with title theTitle buttons theButtons default button okButton cancel button cancelButton with icon note
        end if
        return "=" & button returned of r
    on error number -128
        return ""
    end try
end run
"""
ASK = """on run argv
    set {theText, theTitle, theAnswer} to items 1 thru 3 of argv
    activate
    try
        set r to display dialog theText with title theTitle default answer theAnswer buttons {"Cancel", "OK"} default button "OK" cancel button "Cancel" with icon note
        return "=" & text returned of r
    on error number -128
        return ""
    end try
end run
"""
CHOOSE = """on run argv
    set {thePrompt, theTitle, several} to items 1 thru 3 of argv
    set n to (item 4 of argv) as integer
    if n > 0 then
        set theDefaults to items 5 thru (4 + n) of argv
    else
        set theDefaults to {}
    end if
    set theItems to items (5 + n) thru -1 of argv
    activate
    if several is "1" then
        set r to choose from list theItems with prompt thePrompt with title theTitle default items theDefaults with multiple selections allowed and empty selection allowed
    else
        set r to choose from list theItems with prompt thePrompt with title theTitle default items theDefaults
    end if
    if r is false then return ""
    set AppleScript's text item delimiters to linefeed
    return "=" & (r as text)
end run
"""
FOLDER = """on run argv
    set {thePrompt, theStart} to items 1 thru 2 of argv
    activate
    try
        set f to choose folder with prompt thePrompt default location (theStart as POSIX file) with invisibles
        return "=" & POSIX path of f
    on error number -128
        return ""
    end try
end run
"""
ALERT = """on run argv
    set {theTitle, theMessage} to items 1 thru 2 of argv
    activate
    display alert theTitle message theMessage as warning buttons {"OK"} default button "OK"
    return "="
end run
"""
ADMIN = """on run argv
    set {tool, theApp, thePrompt} to items 1 thru 3 of argv
    try
        do shell script quoted form of tool & " --add " & quoted form of theApp & " && " & quoted form of tool & " --unblockapp " & quoted form of theApp with prompt thePrompt with administrator privileges
        return "="
    on error errText number errNum
        if errNum is -128 then return ""
        return "!" & errText
    end try
end run
"""


def osascript(script, *args):
    """What the script answered (after its "="), or None when the person cancelled; NoDialogs when
    osascript cannot show a window."""
    cmd, text = osascript_command(script, args)
    try:
        r = subprocess.run(cmd, input=text, capture_output=True, encoding="utf-8", errors="replace")
    except OSError as e:
        raise NoDialogs(str(e))
    out = r.stdout.rstrip("\n")
    if r.returncode or not (out == "" or out.startswith("=") or out.startswith("!")):
        raise NoDialogs((r.stderr.strip().splitlines() or [f"osascript exit code {r.returncode}"])[-1])
    if out.startswith("!"):
        raise NoDialogs(out[1:])
    return out[1:] if out.startswith("=") else None


def dialog(text, buttons, default, cancel=""):
    return osascript(DIALOG, text, TITLE, default, cancel, *buttons)


def ask_text(text, answer):
    return osascript(ASK, text, TITLE, answer)


def choose(prompt, items, defaults=(), several=False):
    """The items picked (a list), or None when cancelled."""
    defaults = [d for d in defaults if d in items]
    out = osascript(CHOOSE, prompt, TITLE, "1" if several else "0", len(defaults), *defaults, *items)
    return None if out is None else [x for x in out.split("\n") if x]


def choose_folder(prompt, start):
    """A folder picked in Finder's window, which opens at `start` (or the home folder) and shows hidden
    folders too: the emulators' are in ~/Library, which Finder hides."""
    start = start if start and os.path.isdir(start) else os.path.expanduser("~")
    out = osascript(FOLDER, prompt, start)
    return (out.rstrip("/") or "/") if out else None


def alert(title, message):
    osascript(ALERT, title, message)


# ---- step 1: the newest WindowSmash ------------------------------------------------------------------
def make_executable():
    """The launchers runnable by a double-click (a ZIP opened by something other than Archive
    Utility may not keep their modes, and the update keeps those of .sh files only)."""
    for name in LAUNCHERS:
        path = os.path.join(ROOT, name)
        if os.path.isfile(path) and os.stat(path).st_mode & 0o777 != 0o755:
            os.chmod(path, 0o755)


def step_update(a):
    step_header(1, "Get the newest WindowSmash")
    if os.path.isdir(os.path.join(ROOT, ".git")) and not a.no_update and not developer_dir():
        info("this folder is a git checkout, and git comes with the Command Line Tools (step 3): not updating this time")
        return False
    # (install_linux's step_update, for this branch and under this installer's heading: its own is [1/8])
    was = il.step, il.BRANCH
    il.step, il.BRANCH = (lambda text: None), BRANCH
    try:
        changed = il.step_update(a)
    finally:
        il.step, il.BRANCH = was
    if changed:
        make_executable()
    return changed


# ---- step 2: this Mac ---------------------------------------------------------------------------------
def filesystem_of(path):
    """The filesystem of the disk a folder is on (apfs, hfs, exfat, msdos, smbfs...), or None."""
    code, text = output(["mount"])
    if code != 0:
        return None
    try:
        dev = os.stat(path).st_dev
    except OSError:
        return None
    for point, kind in mount_types(text):
        try:
            if os.stat(point).st_dev == dev:
                return kind
        except OSError:
            continue
    return None


def step_check_mac():
    step_header(2, "Check this Mac")
    if os.geteuid() == 0:
        raise Stop("Run this as yourself, not as root (not with sudo): it asks for an administrator's password itself "
                   "when it needs one, and everything else belongs to you.")
    _code, product = output(["sw_vers", "-productVersion"])
    version = macos_version(product) or macos_version(platform.mac_ver()[0])
    chip = sysctl("machdep.cpu.brand_string") or "unknown processor"
    machine = os.uname().machine
    info(f"macOS {product or '?'}, {chip}, {machine}")
    if sysctl("sysctl.proc_translated") == "1":
        raise Stop("This runs under Rosetta (as an Intel program), so the build would be one too. Start Install "
                   "WindowSmash.command again from Finder, or in Terminal with: arch -arm64 /bin/bash \"Install WindowSmash.command\"")
    if machine != "arm64":
        raise Stop(f"WindowSmash needs a Mac with Apple silicon (M1 or newer); this one has {chip}.")
    if version and version < MIN_MACOS:
        raise Stop(f"WindowSmash needs macOS {MIN_MACOS[0]} (Sonoma) or newer; this Mac has macOS {product}. Update it in "
                   "System Settings > General > Software Update, then run this again.")
    try:
        ram = int(sysctl("hw.memsize")) / 2**30
    except ValueError:
        ram = None
    if ram is not None and ram < MIN_RAM_GB - 0.5:
        raise Stop(f"This Mac has {ram:.0f} GB of memory; WindowSmash needs at least {MIN_RAM_GB} GB (16 GB is comfortable).")
    if ram is not None:
        info(f"{ram:.0f} GB of memory" + (": enough, but the build will be slow (16 GB is comfortable)" if ram < 15.5 else ""))
    already = sum(il.folder_gb(os.path.join(ROOT, d)) for d in ("dump", "build", "build-mac")
                  if os.path.isdir(os.path.join(ROOT, d)))
    free = shutil.disk_usage(ROOT).free / 2**30
    need = il.disk_need_gb(NEED_DISK_GB, already)
    info(f"{free:,.0f} GB free here, {need:,.0f} GB still needed for the build")
    if free < need:
        raise Stop(f"Only {free:,.0f} GB are free on the disk with {ROOT}; the build needs about {need:,.0f} GB more. Free "
                   "some space (System Settings > General > Storage), or move WindowSmash to a disk with room.")
    for text in location_warnings(os.path.realpath(ROOT), os.path.realpath(os.path.expanduser("~")),
                                  filesystem=filesystem_of(ROOT)):
        info("note: " + text)


# ---- step 3: Apple's Command Line Tools ---------------------------------------------------------------
CHECK_CXX = """#include <cstdio>
int main(int argc, char **) {
    try { if (argc > 99) throw argc; } catch (int) { return 1; }
    std::puts("ok");
    return 0;
}
"""


def developer_dir():
    """The developer tools' folder xcode-select names, when it has clang (asking xcrun before that
    would open macOS's install window), or None."""
    code, path = output(["xcode-select", "-p"])
    if code != 0 or not path or not os.path.isdir(path):
        return None
    if not any(os.path.isfile(os.path.join(path, *p)) for p in (("usr", "bin", "clang"),
               ("Toolchains", "XcodeDefault.xctoolchain", "usr", "bin", "clang"))):
        return None
    return path


def try_compiler():
    """(None, what it says) when xcrun's clang builds and runs a C++ test program for this Mac, as
    the game is built; else (the problem, the compiler's output)."""
    with tempfile.TemporaryDirectory(prefix="windowsmash-check-") as tmp:
        exe = os.path.join(tmp, "check")
        try:
            r = subprocess.run(["xcrun", "clang", "-arch", "arm64", "-mmacosx-version-min=%d.%d" % MIN_MACOS, "-x", "c++",
                                "-std=gnu++20", "-", "-o", exe, "-lc++"], input=CHECK_CXX, capture_output=True, text=True)
        except OSError as e:
            return "xcrun did not run", str(e)
        said = (r.stdout + r.stderr).strip()
        if r.returncode:
            return ("license" if re.search(r"licen[cs]e", said, re.I) else "build"), said
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode or r.stdout.strip() != "ok":
            return "run", (r.stdout + r.stderr).strip()
    _c, version = output(["xcrun", "clang", "--version"])
    _c, sdk = output(["xcrun", "--show-sdk-version"])
    sdk_version = macos_version(sdk)
    if sdk_version and sdk_version < MIN_SDK:
        return "sdk", sdk
    return None, f"{(version.splitlines() or ['clang'])[0]}, macOS SDK {sdk or '?'}"


CLT_INSTALLER = "/System/Library/CoreServices/Install Command Line Developer Tools.app"   # (xcode-select --install's window)


def clt_installer_open(app=CLT_INSTALLER):
    """Whether macOS's window installing the Command Line Tools is still open (it stays until its
    Done, through the download and the install), or None when that cannot be told (no such app
    where it was: another macOS)."""
    if not os.path.isdir(app):
        return None
    code, _said = output(["pgrep", "-f", app])
    return None if code is None or code > 1 else code == 0


def wait_for_tools(minutes=60, grace=15, gone_for=10):
    """Until the Command Line Tools' installer is done (it runs on its own, in its window). When
    its window has gone (Cancel, Not Now, or closed) and nothing was installed, nothing will be:
    that is said rather than waited out. Waits `grace` seconds first, for the window to open."""
    t0, said, gone = time.time(), time.time(), None
    while not developer_dir():
        now = time.time()
        if now - t0 > minutes * 60:
            raise Stop("The Command Line Tools are not installed yet. When macOS's window says the software was installed, "
                       "run Install WindowSmash.command again; it carries on from here.")
        if now - t0 > grace and clt_installer_open() is False:
            gone = gone or now
            if now - gone >= gone_for and not developer_dir():
                raise Stop("macOS's window installing the Command Line Tools was closed (Cancel or Not Now?) before they were "
                           "installed. Run Install WindowSmash.command again to open it again, or install them from System "
                           "Settings > General > Software Update.")
        else:
            gone = None
        if now - said > 60:
            said = now
            info(f"still waiting for the Command Line Tools ({(now - t0) / 60:.0f} minutes so far; Ctrl+C stops this)")
        time.sleep(5)


def step_tools(a):
    step_header(3, "Apple's Command Line Tools (the compiler)")
    if not developer_dir():
        if a.dry_run:
            would("ask macOS to install Apple's Command Line Tools (xcode-select --install) and wait for them")
            return
        info("the Command Line Tools (Apple's compiler, the macOS SDK and git) are not installed: macOS now opens a "
             "window to install them (it may open behind this one). Click Install, then Agree. It downloads about 1 GB: "
             "5 to 30 minutes. This waits (Ctrl+C stops it).")
        code, said = output(["xcode-select", "--install"])
        if code and not developer_dir():
            if "already installed" in said:
                raise Stop("macOS says the Command Line Tools are installed, but they cannot be found. Install them again "
                           "from System Settings > General > Software Update, or download Command Line Tools for Xcode "
                           "from https://developer.apple.com/download/all/ , then run this again.")
            info(f"(xcode-select: {said})")
        wait_for_tools()
        info("installed")
    problem, said = try_compiler()
    if problem == "license":
        raise Stop("Xcode's licence has not been accepted yet, so its compiler does not run. Open Xcode once and agree to "
                   "it, or type  sudo xcodebuild -license  in Terminal and read it to the end, then run this again.\n" + said[-800:])
    if problem == "sdk":
        raise Stop(f"The Command Line Tools here have the macOS {said} SDK, and the game needs the macOS {MIN_SDK[0]} SDK or "
                   "newer to be built. Update the Command Line Tools in System Settings > General > Software Update (on macOS "
                   "14, update macOS to 14.5 or newer first, then the tools), or install Command Line Tools for Xcode 16 or "
                   "newer from https://developer.apple.com/download/all/ , then run this again.")
    if problem:
        raise Stop("Apple's compiler could not build a test program (below). Reinstall the Command Line Tools (in Terminal: "
                   "xcode-select --install), then run this again.\n" + said[-1500:])
    info(f"{developer_dir()}: {said}")


# ---- step 4: Python ------------------------------------------------------------------------------------
# what the tools need of a Python: 3.10 or newer, running as arm64 (an Intel one would build under Rosetta)
PYTHON_OK = "import platform, sys; sys.exit(not (sys.version_info >= (3, 10) and platform.machine() == 'arm64'))"
PYTHON_IS = "import platform, sys; print(sys.version.split()[0], platform.machine())"


def python_problem(py):
    """None when the Python at `py` is one the tools can use; else what it is ("missing", or its
    version and architecture, e.g. "3.9.6 arm64")."""
    if not os.path.isfile(py):
        return "missing"
    try:
        if subprocess.run([py, "-c", PYTHON_OK], capture_output=True).returncode == 0:
            return None
        said = subprocess.run([py, "-c", PYTHON_IS], capture_output=True, text=True).stdout.strip()
    except OSError:
        said = ""
    return said or "does not run"


def step_python(a):
    step_header(4, "Python")
    py = venv_python()
    problem = python_problem(py)
    if problem:
        if a.dry_run:
            would(f"make a Python of WindowSmash's own in {os.path.relpath(VENV, ROOT)} from {sys.executable}, with requirements.txt")
            return
        if problem != "missing":   # (one made by hand from Apple's /usr/bin/python3 (3.9), say, or an Intel one)
            info(f"{os.path.relpath(VENV, ROOT)} is a Python the tools cannot use ({problem}; they need 3.10 or newer, as arm64): "
                 "making it again")
        info(f"making a Python of WindowSmash's own in {os.path.relpath(VENV, ROOT)}, from {sys.executable} (touches nothing else)")
        shutil.rmtree(VENV, ignore_errors=True)
        if subprocess.run([sys.executable, "-m", "venv", VENV]).returncode:
            raise Stop(f"Python ({sys.executable}) could not make WindowSmash's own in deps/venv (see above). Fix what it "
                       "says, then run this again.")
        problem = python_problem(py)
        if problem:
            raise Stop(f"The Python this runs with ({sys.executable}: {problem}) cannot run the tools: they need Python 3.10 or "
                       "newer, as arm64. Start Install WindowSmash.command from Finder: it finds or downloads one.")
    version = subprocess.run([py, "-c", "import sys; print(sys.version.split()[0])"], capture_output=True, text=True).stdout.strip()
    info(f"Python {version}: {os.path.relpath(py, ROOT)}")
    need = subprocess.run([py, "-c", "import Cryptodome; import sys, importlib.util as u; sys.exit(0 if u.find_spec('zstandard') "
                          "or (sys.version_info >= (3, 14) and u.find_spec('compression.zstd')) else 1)"], capture_output=True)
    if need.returncode == 0:
        info("packages: installed")
        return
    if a.dry_run:
        would("install requirements.txt there with pip")
        return
    info("installing the packages (requirements.txt)")
    if subprocess.run([py, "-m", "pip", "install", "--disable-pip-version-check", "-r",
                       os.path.join(ROOT, "requirements.txt")]).returncode:
        raise Stop("pip could not install the packages (see above). Check the internet connection, then run this again.")


# ---- step 5: SDL3 ----------------------------------------------------------------------------------------
def sdl3_version(framework):
    try:
        with open(os.path.join(framework, "Resources", "Info.plist"), "rb") as f:
            return plistlib.load(f).get("CFBundleShortVersionString")
    except (OSError, ValueError, plistlib.InvalidFileException):
        return None


def sdl3_ready(framework=FRAMEWORK):
    return os.path.isfile(os.path.join(framework, "Headers", "SDL_gamepad.h")) and os.path.isfile(os.path.join(framework, "SDL3"))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def install_sdl3_framework(dmg, dest):
    """SDL3.framework (the macOS one of the DMG's SDL3.xcframework, symbolic links and signature
    kept by ditto) and SDL's licence into dest, made afresh."""
    mount = tempfile.mkdtemp(prefix="windowsmash-sdl3-")
    code, said = output(["hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, dmg])
    if code != 0:
        os.rmdir(mount)
        raise Stop(f"{os.path.basename(dmg)} could not be opened (hdiutil: {said.splitlines()[-1] if said else code}). "
                   "Delete it and run this again to download it afresh.")
    tmp = dest + ".partial"
    try:
        src = os.path.join(mount, SDL_IN_DMG)
        if not os.path.isdir(src):
            raise Stop(f"{os.path.basename(dmg)} does not hold {SDL_IN_DMG} as expected.")
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        code, said = output(["ditto", src, os.path.join(tmp, "SDL3.framework")])
        if code != 0:
            raise Stop(f"SDL3.framework could not be copied out of {os.path.basename(dmg)} ({said}).")
        if os.path.isfile(os.path.join(mount, "LICENSE.txt")):
            shutil.copyfile(os.path.join(mount, "LICENSE.txt"), os.path.join(tmp, "LICENSE.txt"))
    finally:
        for force in ([], ["-force"]):
            if output(["hdiutil", "detach", mount, *force])[0] == 0:
                break
        try:
            os.rmdir(mount)
        except OSError:
            pass
    if not sdl3_ready(os.path.join(tmp, "SDL3.framework")):
        shutil.rmtree(tmp, ignore_errors=True)
        raise Stop(f"The SDL3.framework in {os.path.basename(dmg)} has no Headers/SDL_gamepad.h or SDL3 library.")
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)


def check_tools():
    """tools/build_game.py --check, now rather than after the questions: a test program built as the
    game is (C++ exceptions through C, SDL3, Metal), and run."""
    r = subprocess.run([venv_python(), os.path.join(ROOT, "tools", "build_game.py"), "--check"], cwd=ROOT,
                       capture_output=True, text=True)
    said = [line.strip() for line in (r.stdout + r.stderr).splitlines() if line.strip()]
    if r.returncode:
        problem = next((line[len("ERROR: "):] for line in said if line.startswith("ERROR: ")), said[-1] if said else "")
        raise Stop("The tools cannot build the game yet: " + problem)
    for line in said:   # (not its heading, nor the Python step 4 named)
        if not line.startswith(("[", "Python ")) and line != "The tools are ready.":
            info(line)


def step_sdl3(a):
    step_header(5, "SDL3 (the window, controllers and sound)")
    if sdl3_ready():
        v = sdl3_version(FRAMEWORK)
        info(f"deps/SDL3: already there{' (SDL ' + v + ')' if v else ''}")
        check_tools()
        return
    releases, err = il.api(SDL_RELEASES)
    asset = select_sdl3_asset(releases if isinstance(releases, list) else [])
    if not asset:
        if err:
            info(f"GitHub's release list could not be read ({err[1]}): taking SDL {SDL_FALLBACK['version']}")
        asset = SDL_FALLBACK
    if a.dry_run:
        would(f"download SDL3 {asset['version']} ({asset['url']}) and copy its SDL3.framework into deps/SDL3, checking its "
              f"SHA-256 ({asset['sha256'] or 'none listed'})")
        return
    dmg = os.path.join(DOWNLOADS, asset["name"])
    if not (os.path.isfile(dmg) and (not asset["sha256"] or sha256_of(dmg) == asset["sha256"])):
        il.download(asset["url"], dmg, f"SDL3 {asset['version']}", asset["sha256"])
    info("SHA-256 matches the release" if asset["sha256"] else "the release lists no SHA-256 for it; taking the download as it is")
    install_sdl3_framework(dmg, SDL_DIR)
    os.remove(dmg)
    info(f"deps/SDL3: SDL {sdl3_version(FRAMEWORK) or asset['version']}")
    check_tools()


# ---- step 6: your files ------------------------------------------------------------------------------------
def names_in(folder):
    try:
        return os.listdir(folder)
    except OSError:
        return []


def detected_folders():
    """The folders to suggest: last time's, else the emulators', else nothing."""
    home = os.path.expanduser("~")
    support = os.path.join(home, "Library", "Application Support")
    dirs = [d for d in ryujinx_game_dirs(read_text(os.path.join(support, "Ryujinx", "Config.json"))) if os.path.isdir(d)]
    c = mac_folder_candidates(home, support, os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share"),
                              os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config"), smash_first(dirs, names_in))
    saved = il.load_saved_folders(os.path.join(ROOT, "dump", "build_folders.txt"))
    keys = saved.get("keys") or next((d for d in c["keys"] if os.path.isfile(os.path.join(d, "prod.keys"))), None)
    firmware = saved.get("firmware") or next((d for d in c["firmware"] if os.path.isdir(d) and any(
        n.lower().endswith(".nca") for n in names_in(d))), None)
    game = saved.get("game") or next((d for d in c["game"] if os.path.isdir(d)), None)
    return {"game": game, "keys": keys, "firmware": firmware}


class Checks:
    """The folder checks, remembered (the game's reads its files: once per folder and keys)."""

    def __init__(self):
        self.games = {}

    def keys(self, folder):
        return il.check_keys(folder)

    def firmware(self, folder):
        return il.check_firmware(folder)

    def game(self, folder, keys):
        if not folder or not os.path.isdir(folder):
            return False, "choose the folder with your game files"
        if not self.keys(keys)[0]:
            return False, "the keys folder is needed to read the game files"
        if (folder, keys) not in self.games:
            self.games[folder, keys] = il.scan_game(folder, keys)
        return self.games[folder, keys]

    def all(self, f):
        return {"game": self.game(f["game"], f["keys"]), "keys": self.keys(f["keys"]), "firmware": self.firmware(f["firmware"])}


LABELS = {"game": "Game files", "keys": "Keys", "firmware": "Firmware"}
PROMPTS = {"game": "Choose the folder with your game files: the game (.nsp, .xci or .nca files), its 13.0.5 update and DLC.",
           "keys": "Choose the folder with prod.keys and title.keys.",
           "firmware": "Choose the folder with your firmware: a few hundred .nca files."}


def summary(f, checks, name, options, firewall):
    lines = ["WindowSmash builds the game from these folders. Choose Change… for any that is wrong or empty.", ""]
    for key in ("game", "keys", "firmware"):
        ok, text = checks[key]
        lines += [f"{LABELS[key]}: {shown(f[key]) if f[key] else '(none yet)'}", f"    {'✓' if ok else '✗'} {text}", ""]
    lines.append(f"Online name (what other players see): {name}")
    chosen = [OPTION_LAUNCHER] if options["launcher"] else []
    if firewall == "on" and options["firewall"]:
        chosen.append(OPTION_FIREWALL)
    lines.append("Options: " + ("; ".join(chosen) if chosen else "none"))
    return "\n".join(lines)


def wizard(found, name, options, firewall, checks):
    """The setup dialogs, as the Windows setup window: what was found and what is in it, Change… for
    a folder, the online name or the options, Set up and build once the three folders are usable.
    (answers, name, options); Stop when cancelled; NoDialogs when there is no screen for them."""
    f = dict(found)
    while True:
        c = checks.all(f)
        button = dialog(summary(f, c, name, options, firewall), ["Cancel", "Change…", "Set up and build"],
                        "Set up and build", "Cancel")
        if button is None:
            raise Stop("Cancelled. Run Install WindowSmash.command again when you are ready.")
        if button == "Set up and build":
            bad = [key for key in ("keys", "firmware", "game") if not c[key][0]]
            if not bad:
                return f, name, options
            alert("Not ready yet", "\n".join(f"{LABELS[k]}: {c[k][1]}" for k in bad) + "\n\nChoose Change… to pick the folder.")
            continue
        what = ["Game files", "Keys", "Firmware", "Online name", "Options"]
        picked = choose("What do you want to change?", what, [next((LABELS[k] for k in ("game", "keys", "firmware")
                                                                    if not c[k][0]), "Game files")])
        if not picked:
            continue
        key = {v: k for k, v in LABELS.items()}.get(picked[0])
        if key:
            folder = choose_folder(PROMPTS[key], f[key] or os.path.join(os.path.expanduser("~"), "Library", "Application Support"))
            if folder:
                f[key] = folder
        elif picked[0] == "Online name":
            answer = ask_text(f"The name other players see online (up to {NAME_MAX} characters; kept in online_name.txt):", name)
            if answer is not None and clean_name(answer):
                name = clean_name(answer)
        else:
            items = [OPTION_LAUNCHER] + ([OPTION_FIREWALL] if firewall == "on" else [])
            on = [OPTION_LAUNCHER] * options["launcher"] + [OPTION_FIREWALL] * options["firewall"]
            picked = choose("Tick what you want:", items, on, several=True)
            if picked is not None:
                options = {"launcher": OPTION_LAUNCHER in picked, "firewall": OPTION_FIREWALL in picked}


def ask_folder(label, key, value, check, interactive):
    """A folder that passes `check` ((ok, text)), asked until it does (or given and checked once)."""
    while True:
        ok, text = check(value)
        info(f"{label}: {value or '(none)'} - {text}")
        if ok:
            return os.path.abspath(value)
        if not interactive:
            raise Stop(f"The {label} folder is not usable ({text}). Name it with --{key} (see Install WindowSmash.command --help).")
        value = ask(f"{label.capitalize()} folder", value, True)


def yes_no(question, default, interactive):
    if not interactive:
        return default
    try:
        answer = input(f"  {question} [{'Y/n' if default else 'y/N'}]: ").strip().lower()
    except EOFError:
        print()
        return default
    return default if not answer else not answer.startswith("n")


def terminal_answers(a, found, name, options, firewall, checks, interactive):
    """The same questions in this window (--no-wizard, or no screen for dialogs), or none (--yes)."""
    if interactive:
        info("Press Return to keep the folder in [brackets], or type another (or drag the folder here).")
    if interactive and not a.keys:
        found["keys"] = ask("Keys folder (with prod.keys)", found["keys"], True)
    keys = ask_folder("keys", "keys", found["keys"], checks.keys, interactive)
    if interactive and not a.firmware:
        found["firmware"] = ask("Firmware folder (.nca files)", found["firmware"], True)
    firmware = ask_folder("firmware", "firmware", found["firmware"], checks.firmware, interactive)
    if interactive and not a.game:
        found["game"] = ask("Game folder (the game's .nca, .nsp or .xci files, with its 13.0.5 update)", found["game"], True)
    game = ask_folder("game", "game", found["game"], lambda p: checks.game(p, keys), interactive)
    if interactive and not a.online_name:
        try:
            typed = input(f"  Online name (what other players see) [{name}]: ")
        except EOFError:
            print()
            typed = ""
        name = clean_name(typed) or name
    options = {"launcher": yes_no(f"Put {LAUNCHER_NAME} in ~/Applications (your own Applications folder) and on the Desktop?", options["launcher"], interactive),
               "firewall": firewall == "on" and yes_no("Let the game through the Application Firewall (asks for your password)?",
                                                       options["firewall"], interactive)}
    return {"game": game, "keys": keys, "firmware": firmware}, name, options


def step_files(a, firewall):
    step_header(6, "Your game files")
    found = detected_folders()
    for key in ("game", "keys", "firmware"):
        if getattr(a, key):
            found[key] = os.path.abspath(os.path.expanduser(getattr(a, key)))
        info(f"{key}: {found[key] or 'not found on this Mac (to be chosen)'}")
    name_file = os.path.join(ROOT, "online_name.txt")
    name = clean_name(a.online_name or read_text(name_file).strip()) or f"Player{int.from_bytes(os.urandom(2), 'big') % 9000 + 1000}"
    # (--yes: no question that needs an answer, the administrator's password included)
    options = {"launcher": True, "firewall": firewall == "on" and not a.yes}
    checks = Checks()
    if a.dry_run:
        if found["game"] and found["keys"]:
            info("game folder check: " + checks.game(found["game"], found["keys"])[1])
        how = "ask in this window" if a.no_wizard else "open the setup dialogs"
        would(f"{how} with these folders and the online name '{name}', then build with the answers")
        return {**found, "name": name, **options}
    interactive = sys.stdin.isatty() and not a.yes
    answers = None
    if interactive and not a.no_wizard:
        try:
            answers = wizard(found, name, options, firewall, checks)
        except NoDialogs as e:
            info(f"the setup dialogs could not open ({e}); asking here instead")
    if answers is None:
        answers = terminal_answers(a, found, name, options, firewall, checks, interactive)
    folders, name, options = answers
    folders = {k: os.path.abspath(v) for k, v in folders.items()}
    if name != read_text(name_file).strip():
        with open(name_file, "w", encoding="utf-8", newline="\n") as f:
            f.write(name + "\n")
    for key in ("game", "keys", "firmware"):
        info(f"{key}: {folders[key]}")
    info(f"online name: {name}")
    return {**folders, "name": name, **options}


# ---- step 7: the build -------------------------------------------------------------------------------------
def build_command(a, files):
    cmd = [venv_python(), os.path.join(ROOT, "tools", "build_game.py")]
    for key in ("game", "keys", "firmware"):
        cmd += [f"--{key}", files[key] or f"<the {key} folder>"]   # (a dry run's may be still to be chosen)
    if a.jobs:
        cmd += ["--jobs", str(a.jobs)]
    if a.skip_build:
        cmd.append("--prepare-only")
    # (kept awake: a build of an hour or more is not stopped by the Mac going to sleep)
    return (["caffeinate", "-i"] if shutil.which("caffeinate") else []) + cmd


def step_build(a, files):
    step_header(7, "Build the game")
    cmd = build_command(a, files)
    if a.dry_run:
        would("run: " + shlex.join(cmd))
        return False
    info("the program, fonts, data, DLC and fighters" + ("; not the build itself (--skip-build)" if a.skip_build else
         ", then the build: one to a few hours the first time, minutes after an update. The Mac stays awake meanwhile; "
         "it can be used, but more slowly"))
    if subprocess.run(cmd, cwd=ROOT).returncode:
        raise Stop("The build stopped (the message above says why). Fix that, then run Install WindowSmash.command again: "
                   "it carries on where it stopped.")
    return not a.skip_build and os.path.isdir(APP)


# ---- step 8: launchers -----------------------------------------------------------------------------------
def make_icns(ico, out):
    """windowsmash.icns from the .ico's 256-pixel picture (sips scales it, iconutil packs it)."""
    with open(ico, "rb") as f:
        png = ico_png(f.read())
    if not png:
        return False
    with tempfile.TemporaryDirectory(prefix="windowsmash-icon-") as tmp:
        src = os.path.join(tmp, "icon.png")
        with open(src, "wb") as f:
            f.write(png)
        iconset = os.path.join(tmp, "windowsmash.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256):
            for scale in (1, 2):
                px = size * scale
                if px > 256:
                    continue
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                if output(["sips", "-z", str(px), str(px), src, "--out", os.path.join(iconset, name)])[0] != 0:
                    return False
        return output(["iconutil", "-c", "icns", iconset, "-o", out])[0] == 0


def launcher_icns():
    """The game's own icon (the build makes it), else made from docs/assets/windowsmash.ico."""
    built = os.path.join(APP, "Contents", "Resources", "windowsmash.icns")
    return built if os.path.isfile(built) else None


def read_bytes(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def make_launcher_app(app, root=ROOT, bundle_id=LAUNCHER_ID, icns=None):
    """The Play SSBU app, made whole beside it then moved into place (a running one keeps its old
    files); True when it changed. icns: its icon (else the game's, else made from the .ico)."""
    contents = os.path.join(app, "Contents")
    plist = plistlib.dumps(launcher_info(bundle_id), sort_keys=True)
    script = launcher_script(root).encode()
    icon = read_bytes(icns or launcher_icns() or "")
    old_icon = read_bytes(os.path.join(contents, "Resources", "windowsmash.icns"))
    if (read_bytes(os.path.join(contents, "Info.plist")) == plist and read_bytes(os.path.join(contents, "MacOS", "play")) == script
            and old_icon is not None and icon in (None, old_icon)):
        return False
    tmp = app + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(os.path.join(tmp, "Contents", "MacOS"))
    os.makedirs(os.path.join(tmp, "Contents", "Resources"))
    with open(os.path.join(tmp, "Contents", "Info.plist"), "wb") as f:
        f.write(plist)
    with open(os.path.join(tmp, "Contents", "PkgInfo"), "w") as f:
        f.write("APPL????")
    exe = os.path.join(tmp, "Contents", "MacOS", "play")
    with open(exe, "wb") as f:
        f.write(script)
    os.chmod(exe, 0o755)
    path = os.path.join(tmp, "Contents", "Resources", "windowsmash.icns")
    if icon is not None:
        with open(path, "wb") as f:
            f.write(icon)
    elif not make_icns(ICON, path):
        info(f"(its icon could not be made: {LAUNCHER_NAME} gets the plain one)")
    output(["codesign", "--force", "--sign", "-", tmp])   # (ad hoc: one identity for macOS's privacy settings)
    os.makedirs(os.path.dirname(app), exist_ok=True)
    if os.path.isdir(app) and not os.path.islink(app):
        shutil.rmtree(app)
    elif os.path.lexists(app):
        os.remove(app)
    os.replace(tmp, app)
    os.utime(app)   # (so Finder shows the new icon)
    return True


def step_launchers(a, files):
    step_header(8, "Launchers")
    if not a.dry_run:
        make_executable()
    info("in this folder: Play SSBU.command (online); in launchers/: Play SSBU Offline, Host SSBU Netplay, "
         "Join SSBU Netplay, Unlock All Fighters, Build WindowSmash (.command)")
    if not files.get("launcher"):
        info(f"{LAUNCHER_NAME} in ~/Applications: not wanted")
        return None
    app = os.path.join(os.path.expanduser("~"), "Applications", LAUNCHER_NAME + ".app")
    link = os.path.join(os.path.expanduser("~"), "Desktop", LAUNCHER_NAME + ".app")
    if a.dry_run:
        would(f"make {shown(app)}, which starts the game from this folder, and link it on the Desktop ({shown(link)})")
        return app
    changed = make_launcher_app(app)
    info(f"{LAUNCHER_NAME}: {shown(app)}" + ("" if changed else " (there already)"))
    try:
        if os.path.islink(link) and os.path.realpath(link) == os.path.realpath(app):
            info(f"on the Desktop: {shown(link)} (there already)")
        elif os.path.lexists(link):
            info(f"on the Desktop: something else is called {LAUNCHER_NAME} there already; left as it is")
        else:
            os.symlink(app, link)
            info(f"on the Desktop: {shown(link)}")
    except OSError as e:
        info(f"no link on the Desktop ({e.strerror}): {LAUNCHER_NAME} is in {shown(app)} (the Applications folder in your "
             "home folder: Finder > Go > Home; Spotlight finds it too)")
    return app


# ---- step 9: the Application Firewall ----------------------------------------------------------------------
def cdhash(app):
    code, text = output(["codesign", "-dvvv", app])
    m = re.search(r"^CDHash=([0-9a-f]+)", text or "", re.M) if code == 0 else None
    return m.group(1) if m else None


def step_firewall(a, files, state):
    step_header(9, "The Application Firewall")
    if state == "off":
        info("off (macOS's default): the game needs no rule")
        return
    if state == "all":
        info("it blocks every incoming connection: playing online and joining rooms work, but hosting a private room "
             "needs that turned off (System Settings > Network > Firewall > Options)")
        return
    if not files.get("firewall"):
        info("on; no rule wanted (macOS asks the first time the game takes connections, when you host a room)")
        return
    if not os.path.isdir(APP):
        info("on, but the game is not built yet: run this again after the build to add its rule")
        return
    stamp = os.path.join(ROOT, "build-mac", "firewall.txt")
    now = cdhash(APP)
    _code, listed = output([FIREWALL, "--listapps"])
    if now and read_text(stamp).strip() == now and APP in (listed or ""):
        info("on; the game is let through already")
        return
    if a.dry_run:
        would(f"let {os.path.relpath(APP, ROOT)} through the firewall (socketfilterfw --add, --unblockapp), with an "
              "administrator's password")
        return
    info("letting the game through: macOS now asks for an administrator's password")
    try:
        done = osascript(ADMIN, FIREWALL, APP, "WindowSmash wants to let its game take incoming connections "
                                               "(for hosting a private room).")
    except NoDialogs as e:
        info(f"that did not work ({e}). To do it in Terminal: sudo {FIREWALL} --add {shlex.quote(APP)} && sudo {FIREWALL} "
             f"--unblockapp {shlex.quote(APP)}")
        return
    if done is None:
        info("no password given: macOS asks the first time the game takes connections")
        return
    if now:
        with open(stamp, "w") as f:
            f.write(now + "\n")
    info("the game is let through (a rebuild changes its signature: running this again renews the rule)")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="Install WindowSmash.command", description="Set up WindowSmash on this Mac in one go.")
    ap.add_argument("--game", help="the folder with the game's files (found or asked)")
    ap.add_argument("--keys", help="the folder with prod.keys and title.keys (found or asked)")
    ap.add_argument("--firmware", help="the folder with the firmware's .nca files (found or asked)")
    ap.add_argument("--online-name", help="the name other players see online (asked; kept in online_name.txt)")
    ap.add_argument("--jobs", type=int, help="how many files to compile at a time (default: by the memory)")
    ap.add_argument("--skip-build", action="store_true", help="everything but compiling the game")
    ap.add_argument("--no-update", action="store_true", help="don't look for a newer WindowSmash")
    ap.add_argument("--no-wizard", action="store_true", help="ask in this window instead of in dialogs")
    ap.add_argument("--yes", action="store_true", help="take the folders found or given, without asking")
    ap.add_argument("--dry-run", action="store_true", help="say what each step would do, do nothing")
    a = ap.parse_args(argv)
    print("WindowSmash setup (macOS)")
    print(f"  {ROOT}")
    if a.dry_run:
        print("  dry run: saying what each step would do, doing nothing")
    try:
        ssl_certificates(a.dry_run)
        if step_update(a):
            step("Starting the updated installer")
            args = [x for x in (argv if argv is not None else sys.argv[1:]) if x != "--no-update"]
            os.execv(sys.executable, [sys.executable, "-u", os.path.join(ROOT, "tools", "mac", "install_mac.py"),
                                      *args, "--no-update"])
        step_check_mac()
        step_tools(a)
        step_python(a)
        if a.dry_run and not os.path.isfile(venv_python()):
            print("\n(dry run) the later steps need WindowSmash's Python; stopping here")
            return 0
        step_sdl3(a)
        _code, said = output([FIREWALL, "--getglobalstate"])
        firewall = firewall_state(said)
        files = step_files(a, firewall)
        built = step_build(a, files)
        app = step_launchers(a, files)
        step_firewall(a, files, firewall)
        print()
        if not built:
            print("Setup done." + (" Run Install WindowSmash.command again without --skip-build to build the game."
                                   if a.skip_build and not a.dry_run else ""))
            return 0
        desktop = app and os.path.islink(os.path.join(os.path.expanduser("~"), "Desktop", LAUNCHER_NAME + ".app"))
        print("Done. Start the game with " + (f"{LAUNCHER_NAME} (in ~/Applications, the Applications folder in your home folder; "
                                              "Spotlight finds it)" + (" or on the Desktop" if desktop else "")
                                              + ", or " if app else "") + "Play SSBU.command in this folder.")
        print(f"  the game: {os.path.relpath(APP, ROOT)}; its log: build/play.log; the other launchers are in launchers/")
        print("  the first time, macOS asks whether WindowSmash may find devices on your local network: Allow lets it "
              "open your router's port and play with players at home (docs/MACOS.md)")
        return 0
    except Stop as e:
        print(f"\nERROR: {e}")
        print("(everything above is also in install.log)")
        return 1
    except KeyboardInterrupt:
        print("\nStopped. Run Install WindowSmash.command again to carry on from here.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
