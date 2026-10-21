#!/usr/bin/env python3
"""WindowSmash on Linux in one go: what install-windowsmash.sh runs (the Linux counterpart of
Install WindowSmash.bat, tools/install_windowsmash.ps1).

Usage: bash install-windowsmash.sh [--game <dir>] [--keys <dir>] [--firmware <dir>] [--online-name <name>]
                                   [--jobs N] [--skip-build] [--no-update] [--no-packages] [--yes] [--dry-run]

  1. update     the newest WindowSmash from GitHub: a git checkout of main is pulled; a ZIP
                download gets the files of the newer commit (tools/source_version.txt says which
                commit it is), then this installer starts again as the new version
  2. this PC    x86-64 with AVX2, the memory, the free space
  3. packages   from the distribution, through sudo (apt, dnf, pacman or zypper): clang, lld and
                llvm, bison and flex, SDL2, libopus and libvpx, Vulkan, zenity, Python's venv; and
                the headers the build needs, put back where a package is installed without them
  4. Python     a Python of WindowSmash's own in deps/venv, with requirements.txt and Pillow
  5. parts      DXVK 3.1.1 and its headers, vkd3d 2.1 and SPIRV-Headers, downloaded into
                deps/linux-toolchain (each checked against its SHA-256)
  6. your files the folders with the game, the keys and the firmware (found in yuzu's, its forks'
                and Ryujinx's folders, then asked), and the name other players see online
  7. launchers  WindowSmash in the applications menu (play-ssbu.sh)
  8. build      tools/build_game.py with those folders: the game's program, fonts, data, DLC and
                fighters, then build-linux/ssbu, which compiles the whole game the first time

A step whose result is there already is skipped, so running it again carries on where it stopped.
--yes takes the folders found (or given) without asking; --dry-run says what each step would do.
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REPO_OWNER, REPO_NAME, BRANCH = "SNO2207", "WindowSmash", "main"
REPO_API = f"https://api.github.com/repos/{REPO_OWNER}/{REPO_NAME}"
SOURCE_VERSION = os.path.join("tools", "source_version.txt")   # "<commit> <date>": GitHub's ZIP fills it in
USER_AGENT = "WindowSmash-installer"
DEPS = os.path.join(ROOT, "deps")
VENV = os.path.join(DEPS, "venv")
TOOLCHAIN = os.path.join(DEPS, "linux-toolchain")
NEED_DISK_GB = 30   # (the C, the objects and the program: 7 GB; the update's data: 4; a game to decrypt first: 17)
MIN_RAM_GB = 7   # (8 GB machines report a little less)
PYTHON_EXTRA = ["pillow"]   # (the overlay font: tools/linux/make_overlay_font.py)
LAUNCHERS = ["install-windowsmash.sh", "play-ssbu.sh", "launchers/play-ssbu-offline.sh", "launchers/host-ssbu-netplay.sh",
             "launchers/join-ssbu-netplay.sh", "launchers/unlock-all-fighters.sh", "launchers/prepare-shaders.sh"]   # (as the .bat ones: two at the top)
# The third-party parts of the build (tools/linux/toolchain.py): (what, URL, SHA-256, file, where it goes,
# the archive's folder whose contents go there)
PARTS = [
    ("DXVK 3.1.1", "https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz",
     "506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7", "dxvk-native-3.1.1-steamrt-sniper.tar.gz",
     "dxvk-native", ""),
    ("DXVK 3.1.1's headers", "https://codeload.github.com/doitsujin/dxvk/tar.gz/refs/tags/v3.1.1",
     "ef4944134f6c39d0937f265391db2d08bca33cd3a2f0ee9133e1eb55f1cc657c", "dxvk-3.1.1-src.tar.gz",
     "dxvk-headers/include/native", "dxvk-3.1.1/include/native/"),
    ("mingw-directx-headers", "https://codeload.github.com/Joshua-Ashton/mingw-directx-headers/tar.gz/9df86f2341616ef1888ae59919feaa6d4fad693d",
     "9c1b8bbfd2d6c758fac4d93c2808a46bc1eee429fe656b4f21b29e02615aa8aa", "mingw-directx-headers-9df86f2.tar.gz",
     "dxvk-headers/include/native/directx", "mingw-directx-headers-9df86f2341616ef1888ae59919feaa6d4fad693d/"),
    ("vkd3d 2.1", "https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz",
     "7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a", "vkd3d-2.1.tar.xz",
     "src/vkd3d-2.1", "vkd3d-2.1/"),
    ("SPIRV-Headers", "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
     "5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9", "SPIRV-Headers-vulkan-sdk-1.4.363.0.tar.gz",
     "src/SPIRV-Headers-vulkan-sdk-1.4.363.0", "SPIRV-Headers-vulkan-sdk-1.4.363.0/"),
]
# What the build and the game need from the distribution, by package manager
PACKAGES = {
    "apt": ["clang", "lld", "llvm", "g++", "bison", "flex", "libsdl2-dev", "libopus-dev", "libvpx-dev", "libvulkan1",
            "mesa-vulkan-drivers", "vulkan-tools", "zenity", "python3-venv", "fontconfig", "fonts-dejavu-core"],
    "dnf": ["clang", "lld", "llvm", "gcc-c++", "bison", "flex", "SDL2-devel", "opus-devel", "libvpx-devel", "vulkan-loader",
            "mesa-vulkan-drivers", "vulkan-tools", "zenity", "python3", "fontconfig", "dejavu-sans-mono-fonts"],
    "pacman": ["clang", "lld", "llvm", "gcc", "bison", "flex", "sdl2", "opus", "libvpx", "vulkan-icd-loader",
               "vulkan-tools", "zenity", "python", "fontconfig", "ttf-dejavu"],
    "zypper": ["clang", "lld", "llvm", "gcc-c++", "bison", "flex", "SDL2-devel", "libopus-devel", "libvpx-devel",
               "libvulkan1", "vulkan-tools", "zenity", "python3", "fontconfig", "dejavu-fonts"],
}
# Those the build does without (the game needs a Vulkan driver, but which one is the graphics card's)
OPTIONAL = {"libvulkan1", "vulkan-loader", "vulkan-icd-loader", "mesa-vulkan-drivers", "vulkan-tools", "zenity",
            "fontconfig", "fonts-dejavu-core", "dejavu-sans-mono-fonts", "ttf-dejavu", "dejavu-fonts"}
INSTALL = {"apt": ["apt-get", "install", "-y"], "dnf": ["dnf", "install", "-y"],
           "pacman": ["pacman", "-S", "--needed", "--noconfirm"], "zypper": ["zypper", "--non-interactive", "install"],
           "rpm-ostree": ["rpm-ostree", "install", "--idempotent", "--allow-inactive"]}
PACKAGES["rpm-ostree"] = PACKAGES["dnf"]   # (an image-based Fedora: the same packages, layered)
# The headers the build compiles with, from /usr/include, and the package each comes with. A package
# manager can list a package as installed without them: SteamOS, for one, leaves every package's
# headers out of its system image.
INCLUDE = "/usr/include"
HEADERS = {
    "stdio.h": {"apt": "libc6-dev", "dnf": "glibc-devel", "pacman": "glibc", "zypper": "glibc-devel"},
    "linux/userfaultfd.h": {"apt": "linux-libc-dev", "dnf": "kernel-headers", "pacman": "linux-api-headers",
                            "zypper": "linux-glibc-devel"},
    "SDL2/SDL.h": {"apt": "libsdl2-dev", "dnf": "SDL2-devel", "pacman": "sdl2", "zypper": "SDL2-devel"},
    "opus/opus.h": {"apt": "libopus-dev", "dnf": "opus-devel", "pacman": "opus", "zypper": "libopus-devel"},
    "vpx/vpx_decoder.h": {"apt": "libvpx-dev", "dnf": "libvpx-devel", "pacman": "libvpx", "zypper": "libvpx-devel"},
}
REINSTALL = {"apt": ["apt-get", "install", "-y", "--reinstall"], "dnf": ["dnf", "reinstall", "-y"],
             "pacman": ["pacman", "-S", "--noconfirm"], "zypper": ["zypper", "--non-interactive", "install", "--force"]}


class Stop(Exception):
    """A step that cannot go on; the message says why and what to do."""


def step(text):
    print(f"\n{text}", flush=True)


def info(text):
    print(f"  {text}", flush=True)


def would(text):
    info(f"(dry run) would {text}")


# ---- what the steps decide by (tested in tests/test_linux_installer.py) --------------------------
def read_source_version(text):
    """The commit a source_version.txt names, in lower case, or None (a git checkout has the
    $Format placeholder instead)."""
    m = re.search(r"(?<![0-9a-fA-F])([0-9a-fA-F]{40})(?![0-9a-fA-F])", text or "")
    return m.group(1).lower() if m else None


def update_decision(status):
    """What to do, from GitHub's comparison of this copy's commit (the base) with the branch."""
    return {"identical": "current", "ahead": "update", "behind": "newer", "diverged": "diverged"}.get(status, "unknown")


def removed_paths(old, new):
    """The old version's files that the new one no longer has; never a path that could reach
    outside the folder."""
    keep = set(new)
    return [p for p in old if p not in keep and not p.startswith("/") and ".." not in p.split("/") and ":" not in p]


def lobby_line(lines):
    """The line of an online_server.txt the launchers read: (address, code, index), or None."""
    for i, line in enumerate(lines):
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        parts = t.split()
        return parts[0], (parts[1] if len(parts) > 1 else None), i
    return None


def merge_online_server(local, old, new):
    """online_server.txt after an update (as the Windows installer's Merge-OnlineServer). Unchanged by
    you: the new version's. You only added a lobby code to the old version's address: the new
    address with your code. Another address, or "off": yours. No old version to compare with: yours."""
    if not local:
        return new
    l, n = local.replace("\r\n", "\n"), new.replace("\r\n", "\n")
    if l.rstrip() == n.rstrip():
        return new
    if not old:
        return local
    o = old.replace("\r\n", "\n")
    if l.rstrip() == o.rstrip():
        return new
    mine, was = lobby_line(l.split("\n")), lobby_line(o.split("\n"))
    new_lines = n.split("\n")
    now = lobby_line(new_lines)
    if mine and was and now and mine[0] == was[0] and mine[0].lower() != "off":
        if not mine[1]:
            return new
        new_lines[now[2]] = f"{now[0]} {mine[1]}"
        return "\n".join(new_lines)
    return local


def os_release(text):
    """/etc/os-release's fields."""
    out = {}
    for line in text.splitlines():
        key, eq, value = line.partition("=")
        if eq:
            out[key.strip()] = value.strip().strip('"').strip("'")
    return out


def package_manager(fields, have=lambda tool: True, image_based=False):
    """The package manager for this distribution (apt, dnf, pacman or zypper), from os-release's
    ID and ID_LIKE, among those installed (have); None for one it does not know. On an image-based
    system booted by ostree (Fedora Silverblue and Kinoite, and the ones built on them: Bazzite,
    Bluefin, Aurora and others) the system's own folders are read-only: rpm-ostree adds packages."""
    if image_based and have("rpm-ostree"):
        return "rpm-ostree"
    ids = [fields.get("ID", "").lower()] + fields.get("ID_LIKE", "").lower().split()
    by_family = [("apt", {"debian", "ubuntu", "linuxmint", "pop", "elementary", "zorin", "kali", "raspbian"}),
                 ("dnf", {"fedora", "rhel", "centos", "rocky", "almalinux", "nobara", "ultramarine"}),
                 ("pacman", {"arch", "manjaro", "endeavouros", "cachyos", "garuda", "artix"}),
                 ("zypper", {"suse", "opensuse", "opensuse-tumbleweed", "opensuse-leap", "sles"})]
    for name in ids:
        for tool, family in by_family:
            if name in family and have(tool):
                return tool
    return None


def libstdcxx_package(gcc_versions, has_headers):
    """Debian and Ubuntu: clang uses the newest GCC it finds for its C++ library, and that GCC's
    libstdc++-<n>-dev may not be installed (when only its runtime came with something else): the
    package to add, or None."""
    versions = sorted((int(v) for v in gcc_versions if v.isdigit()), reverse=True)
    if versions and not has_headers(versions[0]):
        return f"libstdc++-{versions[0]}-dev"
    return None


def cpu_info(text):
    """(processor name, has AVX2 or None) from /proc/cpuinfo."""
    name, flags = None, None
    for line in text.splitlines():
        key, _, value = line.partition(":")
        key = key.strip()
        if key == "model name" and name is None:
            name = " ".join(value.split())
        elif key == "flags" and flags is None:
            flags = value.split()
    return name or "unknown processor", (None if flags is None else "avx2" in flags)


def mem_total_gb(text):
    m = re.search(r"^MemTotal:\s+(\d+)\s+kB", text, re.M)
    return int(m.group(1)) / (1 << 20) if m else None


def disk_need_gb(need, already):
    """How much free space the build still needs: the full amount, less what it has made already."""
    return max(0.0, need - already)


def folder_candidates(home, data_home, config_home, flatpak_dirs=()):
    """Where the emulators keep keys, firmware and installed games on Linux, most likely first:
    yuzu and the forks with its layout (in ~/.local/share, or a Flatpak's data folder), Ryujinx
    (~/.config/Ryujinx, or a Flatpak's), ~/.switch for keys. flatpak_dirs: the ~/.var/app folders."""
    keys, firmware, game = [], [], []
    yuzus = []
    for name in ("yuzu", "eden", "sudachi", "citron", "suyu"):
        yuzus.append(os.path.join(data_home, name))
        yuzus += [os.path.join(app, "data", name) for app in flatpak_dirs]
    for d in yuzus:
        keys.append(os.path.join(d, "keys"))
        firmware.append(os.path.join(d, "nand", "system", "Contents", "registered"))
        game.append(os.path.join(d, "nand", "user", "Contents", "registered"))
        game.append(os.path.join(d, "sdmc", "Nintendo", "Contents", "registered"))
    for d in [os.path.join(config_home, "Ryujinx")] + [os.path.join(app, "config", "Ryujinx") for app in flatpak_dirs]:
        keys.append(os.path.join(d, "system"))
        firmware.append(os.path.join(d, "bis", "system", "Contents", "registered"))
        game.append(os.path.join(d, "bis", "user", "Contents", "registered"))
        game.append(os.path.join(d, "sdcard", "Nintendo", "Contents", "registered"))
    keys.append(os.path.join(home, ".switch"))
    return {"keys": keys, "firmware": firmware, "game": game}


def desktop_quote(arg):
    """One argument of a .desktop file's Exec line (the Desktop Entry spec's quoting)."""
    if not re.search(r'[\s"\'\\><~|&;$*?#()`]', arg):
        return arg
    return '"' + re.sub(r'(["`$\\])', r"\\\1", arg) + '"'


def desktop_entry(root):
    """WindowSmash's entry in the applications menu: play-ssbu.sh in a terminal (it shows the log,
    and asks for the online name the first time)."""
    exec_line = " ".join(desktop_quote(a) for a in ("bash", posixpath.join(root, "play-ssbu.sh")))
    return ("[Desktop Entry]\nType=Application\nName=WindowSmash\nGenericName=Super Smash Bros. Ultimate\n"
            "Comment=Super Smash Bros. Ultimate, recompiled from your own copy\n"
            f"Exec={exec_line}\nPath={root}\nIcon={posixpath.join(root, 'docs', 'assets', 'windowsmash.ico')}\n"
            "Terminal=true\nCategories=Game;\n")


def strip_member(name, prefix):
    """An archive member's path inside the part's folder (prefix: the archive's folder that holds
    the part), or None for a member outside it."""
    while name.startswith("./"):
        name = name[2:]
    if not name.startswith(prefix):
        return None
    rest = name[len(prefix):].strip("/")
    return rest or None


def load_saved_folders(path):
    """The folders build_game.py remembers (dump/build_folders.txt)."""
    saved = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                key, _, value = line.strip().partition("=")
                if key in ("game", "keys", "firmware") and value:
                    saved[key] = value
    except OSError:
        pass
    return saved


def check_keys(folder):
    """(ok, what it says) for a keys folder."""
    if not folder or not os.path.isdir(folder):
        return False, "choose the folder with prod.keys and title.keys"
    if not os.path.isfile(os.path.join(folder, "prod.keys")):
        return False, "there is no prod.keys here"
    if not os.path.isfile(os.path.join(folder, "title.keys")):
        return True, "prod.keys found; no title.keys (needed for games bought from the eShop)"
    return True, "prod.keys and title.keys found"


def check_firmware(folder):
    if not folder or not os.path.isdir(folder):
        return False, "choose the folder with your firmware (a few hundred .nca files)"
    n = sum(1 for f in os.listdir(folder) if f.lower().endswith(".nca"))
    if not n:
        return False, "there are no .nca files here"
    return True, f"{n} .nca files (the game's fonts are taken out of them)"


# ---- step 1: the newest WindowSmash ---------------------------------------------------------------
def http(url, accept="*/*"):
    """(status, body bytes or None, error text or None): a 404 is an answer here, not an error."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read(), None
    except urllib.error.HTTPError as e:
        return e.code, None, f"HTTP {e.code}"
    except (urllib.error.URLError, OSError) as e:
        return 0, None, str(getattr(e, "reason", e))


def api(url):
    """GitHub's API: (data, None) or (None, (not found?, error text))."""
    code, body, err = http(url, "application/vnd.github+json")
    if 200 <= code < 300:
        try:
            return json.loads(body), None
        except ValueError:
            return None, (False, "an answer that is not JSON")
    return None, (code == 404, err or f"HTTP {code}")


def download(url, dest, what, sha256=None):
    """A file, shown as it comes; with sha256, checked (a mismatch is deleted and stops)."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    partial = dest + ".partial"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    t0, shown, h = time.time(), 0, hashlib.sha256()
    try:
        with urllib.request.urlopen(req, timeout=120) as r, open(partial, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            done = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                done += len(chunk)
                if time.time() - shown > 2:
                    shown = time.time()
                    info(f"{what}: {done / 2**20:.0f}" + (f" of {total / 2**20:.0f}" if total else "") + " MB")
    except (urllib.error.URLError, OSError) as e:
        raise Stop(f"Could not download {what} from {url} ({getattr(e, 'reason', e)}). Check the internet "
                   "connection, then run this again.")
    if sha256 and h.hexdigest() != sha256:
        os.remove(partial)
        raise Stop(f"The downloaded {what} does not match its SHA-256 ({h.hexdigest()}, expected {sha256}). Run this "
                   "again to download it afresh.")
    os.replace(partial, dest)
    info(f"{what}: {os.path.getsize(dest) / 2**20:.1f} MB in {time.time() - t0:.0f}s")


def local_commit():
    try:
        with open(os.path.join(ROOT, SOURCE_VERSION), encoding="utf-8") as f:
            return read_source_version(f.read())
    except OSError:
        return None


def update_git(dry):
    """A git checkout of main, with no changes of its own: git pull --ff-only. True when it changed."""
    if not shutil.which("git"):
        info("this folder is a git checkout, but git is not installed: not updating")
        return False

    def git(*args):
        return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True).stdout.strip()
    branch, head = git("rev-parse", "--abbrev-ref", "HEAD"), git("rev-parse", "HEAD")
    if branch != BRANCH:
        info(f"this is a git checkout of the branch {branch}, not {BRANCH}: not updating (update it with git)")
        return False
    if git("status", "--porcelain", "--untracked-files=no"):
        info("this git checkout has changes of its own: not updating (update it with git)")
        return False
    remote = (git("ls-remote", "origin", f"refs/heads/{BRANCH}").split() or [""])[0]
    if not remote:
        info("could not reach the git remote: going on with this version")
        return False
    if remote == head:
        info(f"up to date ({head[:7]}, git checkout)")
        return False
    if dry:
        would(f"run git pull --ff-only ({head[:7]} to {remote[:7]})")
        return False
    subprocess.run(["git", "-C", ROOT, "pull", "--ff-only", "origin", BRANCH])
    after = git("rev-parse", "HEAD")
    if after == head:
        info("git pull changed nothing (see above): going on with this version")
        return False
    info(f"updated with git: {head[:7]} to {after[:7]}")
    return True


def write_atomic(path, data):
    """A file replaced whole (a new file renamed over the old): a running script, such as
    install-windowsmash.sh itself, keeps reading the old one."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".new"
    with open(tmp, "wb") as f:
        f.write(data)
    if os.path.exists(path):
        shutil.copymode(path, tmp)
    os.replace(tmp, path)


def update_from_zip(old, new, new_date):
    """This folder's WindowSmash files replaced by those of commit `new` (GitHub's ZIP of it): only
    files whose content changed are written; the old commit's files the new one dropped are removed.
    Nothing else is touched: dump/, build/, save/, deps/ and the settings are not in the ZIP."""
    work = os.path.join(DEPS, "update")
    shutil.rmtree(work, ignore_errors=True)
    zip_path = os.path.join(work, f"WindowSmash-{new}.zip")
    download(f"https://codeload.github.com/{REPO_OWNER}/{REPO_NAME}/zip/{new}", zip_path, f"WindowSmash {new[:7]}")
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        tops = {n.split("/", 1)[0] for n in z.namelist()}
        if len(tops) != 1 or f"{next(iter(tops))}/tools/recomp.py" not in names:
            raise Stop("The downloaded WindowSmash does not look complete; nothing was changed. Run this again.")
        top = next(iter(tops)) + "/"
        files = {n[len(top):]: n for n in names if n.startswith(top) and n != top}
        tree, _err = api(f"{REPO_API}/git/trees/{old}?recursive=1")
        removed = []
        if tree and not tree.get("truncated"):
            removed = removed_paths([e["path"] for e in tree["tree"] if e.get("type") == "blob"], files)
        else:
            info("the old version's file list could not be read: files the new version dropped stay")
        _code, old_lobby, _e = http(f"https://raw.githubusercontent.com/{REPO_OWNER}/{REPO_NAME}/{old}/online_server.txt")
        changed = 0
        for rel, member in files.items():
            to = os.path.join(ROOT, rel)
            data = z.read(member)
            mode = (z.getinfo(member).external_attr >> 16) & 0o777
            if rel == "online_server.txt" and os.path.isfile(to):
                with open(to, encoding="utf-8", errors="replace", newline="") as f:
                    mine = f.read()
                theirs = data.decode("utf-8", "replace")
                merged = merge_online_server(mine, old_lobby.decode("utf-8", "replace") if old_lobby else None, theirs)
                if merged != theirs:
                    info("online_server.txt: kept your lobby settings")
                if merged != mine:
                    write_atomic(to, merged.encode("utf-8"))
                    changed += 1
                continue
            if os.path.isfile(to):
                with open(to, "rb") as f:
                    if f.read() == data:
                        continue
            write_atomic(to, data)
            if mode & 0o111 or rel.endswith(".sh"):
                os.chmod(to, 0o755)
            changed += 1
    gone = 0
    for rel in removed:
        p = os.path.join(ROOT, rel)
        if os.path.isfile(p):
            os.remove(p)
            gone += 1
    # last: until this is written, a stopped update is simply done again next time
    write_atomic(os.path.join(ROOT, SOURCE_VERSION), f"{new} {new_date}\n".encode())
    shutil.rmtree(work, ignore_errors=True)
    info(f"updated: {changed} file(s) replaced, {gone} removed; your game files, settings, save and builds are as they were")
    return True


def step_update(a):
    step("[1/8] Get the newest WindowSmash")
    if a.no_update:
        info("not checking (--no-update)")
        return False
    if os.path.isdir(os.path.join(ROOT, ".git")):
        return update_git(a.dry_run)
    local = local_commit()
    if not local:
        info(f"this folder does not say which version it is ({SOURCE_VERSION} names no commit): not updating.")
        info(f"To get the newest, download the ZIP again from https://github.com/{REPO_OWNER}/{REPO_NAME}")
        return False
    head, err = api(f"{REPO_API}/commits/{BRANCH}")
    if not head:
        info(f"could not ask GitHub for a newer version ({err[1]}); going on with this one")
        return False
    sha, date = head["sha"], head["commit"]["committer"]["date"][:10]
    if sha == local:
        info(f"up to date ({local[:7]}, {date})")
        return False
    cmp, err = api(f"{REPO_API}/compare/{local}...{sha}")
    if not cmp:
        info(f"this copy ({local[:7]}) is not on GitHub: not updating" if err[0] else
             f"could not ask GitHub for a newer version ({err[1]}); going on with this one")
        return False
    decision = update_decision(cmp.get("status"))
    if decision != "update":
        info({"current": f"up to date ({local[:7]})", "newer": f"this copy ({local[:7]}) is newer than GitHub's {BRANCH}: not updating",
              "diverged": f"this copy ({local[:7]}) comes from another branch than {BRANCH}: not updating"}.get(
            decision, f"GitHub's answer ({cmp.get('status')}) was not understood: not updating"))
        return False
    n = int(cmp.get("ahead_by", 0))
    info(f"a newer WindowSmash is on GitHub: {n} new commit{'s' if n != 1 else ''} ({local[:7]} to {sha[:7]}, {date})")
    titles = [c["commit"]["message"].split("\n")[0] for c in cmp.get("commits", [])]
    titles = [t for t in reversed(titles) if not re.match(r"Merge (pull request|branch) ", t)]
    for t in titles[:8]:
        info(f"  - {t}")
    if len(titles) > 8:
        info(f"  ... and {len(titles) - 8} more")
    if a.dry_run:
        would("download it and replace this folder's WindowSmash files (your game files, settings, save and builds stay)")
        return False
    return update_from_zip(local, sha, date)


# ---- step 2: this PC --------------------------------------------------------------------------------
def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def folder_gb(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total / 2**30


def step_check_pc():
    step("[2/8] Check this PC")
    if os.geteuid() == 0:
        raise Stop("Run this as your own user, not as root (not with sudo): it asks for sudo itself when it installs "
                   "packages, and everything else belongs to you.")
    fields = os_release(read_text("/etc/os-release"))
    info(f"{fields.get('PRETTY_NAME', 'Linux')}, kernel {platform.release()}, {platform.machine()}")
    if platform.machine() not in ("x86_64", "AMD64"):
        raise Stop(f"WindowSmash needs an x86-64 (64-bit Intel or AMD) PC; this one is {platform.machine()}.")
    name, avx2 = cpu_info(read_text("/proc/cpuinfo"))
    if avx2 is False:
        raise Stop(f"The processor ({name}) has no AVX2, which the game needs: WindowSmash cannot run on it. (In a "
                   "virtual machine, the host's processor must have it and pass it through.)")
    info(f"{name} - has AVX2" if avx2 else f"{name} - whether it has AVX2 is unknown; the game only starts if it does")
    ram = mem_total_gb(read_text("/proc/meminfo"))
    if ram is not None and ram < MIN_RAM_GB:
        raise Stop(f"This PC has {ram:.1f} GB of RAM; WindowSmash needs at least 8 GB (16 GB is comfortable).")
    if ram is not None:
        info(f"{ram:.0f} GB of RAM" + (": enough, but the build will be slow (16 GB is comfortable)" if ram < 15 else ""))
    already = sum(folder_gb(os.path.join(ROOT, d)) for d in ("dump", "build", "build-linux") if os.path.isdir(os.path.join(ROOT, d)))
    free = shutil.disk_usage(ROOT).free / 2**30
    need = disk_need_gb(NEED_DISK_GB, already)
    info(f"{free:,.0f} GB free here, {need:,.0f} GB still needed for the build")
    if free < need:
        raise Stop(f"Only {free:,.0f} GB are free in {ROOT}; the build needs about {need:,.0f} GB more. Free some space, "
                   "or move WindowSmash to a disk with room.")
    libc = platform.libc_ver()
    if libc[0]:
        info(f"{libc[0]} {libc[1]}")


# ---- step 3: the distribution's packages --------------------------------------------------------------
def installed(manager, pkg):
    """Whether a package (or, for dnf, zypper, rpm-ostree and pacman, something that provides it) is
    installed - in the running system (rpm-ostree: not one layered for the next start)."""
    if manager == "apt":
        r = subprocess.run(["dpkg-query", "-W", "-f=${Status}", pkg], capture_output=True, text=True)
        return r.returncode == 0 and "install ok installed" in r.stdout
    if manager in ("dnf", "zypper", "rpm-ostree"):
        return subprocess.run(["rpm", "-q", "--whatprovides", pkg], capture_output=True).returncode == 0
    if manager == "pacman":
        return subprocess.run(["pacman", "-T", pkg], capture_output=True).returncode == 0
    return False


def sudo_run(cmd, dry):
    full = ([] if os.geteuid() == 0 else ["sudo"]) + cmd
    info("$ " + " ".join(full))
    if dry:
        return True
    return subprocess.run(full).returncode == 0


def install_layered(missing, run):
    """rpm-ostree: the packages layered onto the system image and applied to the running one at once
    (--apply-live), so that no restart is needed; where that can't be done (an update waiting for
    the next start, say), the build's own for the next start. One name the repositories don't know
    fails a whole transaction: the second try leaves out those the build does without. Whether
    rpm-ostree took the build's packages (step_packages then sees whether they are there yet)."""
    needed = [p for p in missing if p not in OPTIONAL]
    for pkgs in [missing] + ([needed] if needed != missing else []):
        if pkgs and run([*INSTALL["rpm-ostree"], "--apply-live", *pkgs]):
            return True
    return bool(needed) and run([*INSTALL["rpm-ostree"], *needed])


def still_missing(wanted, is_installed):
    """The build's packages that are not installed (the ones it does without left out)."""
    return [p for p in wanted if p not in OPTIONAL and not is_installed(p)]


def missing_headers(manager, exists):
    """The build's headers that are not in /usr/include, each with the package it comes with (None
    for a package manager without a list here)."""
    names = "dnf" if manager == "rpm-ostree" else manager
    return [(h, by.get(names)) for h, by in HEADERS.items() if not exists(posixpath.join(INCLUDE, h))]


def header_fix(manager, pkgs, is_installed):
    """The commands that put back the packages whose headers are missing: reinstalled where the
    package manager has them (the files went after), installed where it doesn't."""
    there = [p for p in pkgs if is_installed(p)]
    rest = [p for p in pkgs if p not in there]
    return ([[*REINSTALL[manager], *there]] if there else []) + ([[*INSTALL[manager], *rest]] if rest else [])


def read_only(path="/usr"):
    """Whether the system's folders are mounted read-only (SteamOS's are, for one)."""
    try:
        return bool(os.statvfs(path).f_flag & os.ST_RDONLY)
    except (AttributeError, OSError):
        return False


def need_writable_system():
    if read_only():
        raise Stop("The system's folders are read-only, so packages can't be added or put back. Make them writable "
                   "(on SteamOS: sudo steamos-readonly disable), then run bash install-windowsmash.sh again.")


def debian_cxx_headers(manager, dry):
    """libstdc++-<n>-dev for the GCC clang picks, when it is missing (libstdcxx_package)."""
    gcc = "/usr/lib/gcc/x86_64-linux-gnu"
    versions = os.listdir(gcc) if os.path.isdir(gcc) else []
    pkg = libstdcxx_package(versions, lambda v: os.path.isdir(f"/usr/include/c++/{v}") or
                            os.path.isdir(f"/usr/include/x86_64-linux-gnu/c++/{v}"))
    if pkg and not installed(manager, pkg):
        info(f"clang uses GCC {pkg.split('-')[1]}'s C++ library: adding its headers")
        if not sudo_run([*INSTALL[manager], pkg], dry):
            raise Stop(f"{pkg} could not be installed (see above). Install it, then run this again.")


def vulkan_ready():
    """The Vulkan loader, and a driver for it."""
    import ctypes.util
    if not ctypes.util.find_library("vulkan") and not any(
            os.path.exists(os.path.join(d, "libvulkan.so.1")) for d in ("/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib")):
        return False
    if os.environ.get("VK_ICD_FILENAMES") or os.environ.get("VK_DRIVER_FILES"):
        return True
    return any(glob.glob(os.path.join(d, "*.json")) for d in ("/usr/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d",
                                                              "/etc/vulkan/icd.d"))


def step_headers(manager, a):
    """The build's headers, there on disk: the package manager can list a package as installed without
    them (HEADERS), and the build stops at the first one missing. Their packages are put back."""
    gone = missing_headers(manager, os.path.exists)
    if not gone:
        return
    pkgs = list(dict.fromkeys(p for _, p in gone if p))
    names = ", ".join(h for h, _ in gone)
    if manager in REINSTALL and not a.no_packages:
        info(f"missing from {INCLUDE}, although the build needs them: {names}. Putting back {', '.join(pkgs)}")
        if not a.dry_run:
            need_writable_system()
        for cmd in header_fix(manager, pkgs, lambda p: installed(manager, p)):
            sudo_run(cmd, a.dry_run)
        if a.dry_run:
            return
        gone = missing_headers(manager, os.path.exists)
        if gone:
            raise Stop(f"{', '.join(h for h, _ in gone)} still missing from {INCLUDE} after putting back "
                       f"{', '.join(pkgs)} (see above). Reinstall them yourself, then run this again.")
        info("there now")
        return
    if a.dry_run:
        info(f"missing from {INCLUDE}, although the build needs them: {names}")
        return
    if manager == "rpm-ostree":
        raise Stop(f"{names} not there yet (they come with {', '.join(pkgs)}). If rpm-ostree added them for the next "
                   "start, restart the PC, then run bash install-windowsmash.sh again.")
    what = (f"the package{'s' if len(pkgs) != 1 else ''} they come with: {', '.join(pkgs)}" if pkgs else
            "your distribution's packages for them (Debian calls them "
            f"{', '.join(dict.fromkeys(HEADERS[h]['apt'] for h, _ in gone))})")
    raise Stop(f"{names} missing from {INCLUDE}, and the build needs them. Install (or reinstall) {what}, then run "
               "this again with --no-packages.")


def step_packages(a):
    step("[3/8] Packages from your distribution")
    fields = os_release(read_text("/etc/os-release"))
    manager = package_manager(fields, lambda tool: shutil.which(INSTALL[tool][0]) is not None,
                              image_based=os.path.exists("/run/ostree-booted"))
    if not manager:
        info(f"{fields.get('PRETTY_NAME', 'this distribution')}: no package list for it here. Install these (or what "
             "your distribution calls them) yourself, then run this again with --no-packages:")
        info("  " + ", ".join(PACKAGES["apt"]))
        if not a.no_packages:
            raise Stop("Install the packages above, then run this again with --no-packages.")
        step_headers(None, a)
        return
    wanted = PACKAGES[manager]
    missing = [p for p in wanted if not installed(manager, p)]
    if not missing:
        info(f"installed ({manager}): {', '.join(wanted)}")
    elif a.no_packages:
        info(f"not installing (--no-packages); missing: {', '.join(missing)}")
    elif manager == "rpm-ostree":
        info(f"adding {', '.join(missing)} to the system with rpm-ostree (an image-based system); sudo asks for your "
             "password. This takes some minutes.")
        needed = [p for p in missing if p not in OPTIONAL]
        if not install_layered(missing, lambda cmd: sudo_run(cmd, a.dry_run)):
            if needed:
                raise Stop(f"rpm-ostree could not add {', '.join(needed)} (see above). If it says another transaction "
                           "is running (an update), wait for it, then run this again.")
            info(f"not added, and not needed for the build: {', '.join(missing)}")
    else:
        info(f"installing {', '.join(missing)} with {manager}; sudo asks for your password")
        if not a.dry_run:
            need_writable_system()
        if manager == "apt" and not sudo_run(["apt-get", "update"], a.dry_run):
            raise Stop("apt-get update failed (see above). Fix that, then run this again.")
        if not sudo_run([*INSTALL[manager], *missing], a.dry_run):
            # (one name this distribution does not know fails them all: each on its own, to say which)
            info("installing them one at a time, to see which one fails")
            failed = [p for p in missing if not sudo_run([*INSTALL[manager], p], a.dry_run)]
            needed = [p for p in failed if p not in OPTIONAL]
            if needed:
                raise Stop(f"These packages could not be installed (see above): {', '.join(needed)}. Install them (or "
                           "what your distribution calls them) yourself, then run this again with --no-packages.")
            if failed:
                info(f"not installed, and not needed for the build: {', '.join(failed)}")
    if manager == "apt" and not a.no_packages:
        debian_cxx_headers(manager, a.dry_run)
    if missing and not a.no_packages and not a.dry_run:   # (installed, as far as the package manager said: are they there?)
        still = still_missing(wanted, lambda p: installed(manager, p))
        if still and manager == "rpm-ostree":
            raise Stop("The packages are added to the system for its next start (they could not be applied to the "
                       "running one): restart the PC, then run bash install-windowsmash.sh again.")
        if still:
            raise Stop(f"These packages are still not installed: {', '.join(still)} (see above). Install them (or what "
                       "your distribution calls them) yourself, then run this again with --no-packages.")
    step_headers(manager, a)
    if not a.dry_run and not vulkan_ready():
        info("note: no Vulkan driver is installed. The game draws through Vulkan: install your graphics card's driver "
             "(Mesa's covers AMD and Intel, and has a software one, lavapipe; NVIDIA has its own)")


# ---- step 4: Python -------------------------------------------------------------------------------------
def venv_python():
    return os.path.join(VENV, "bin", "python")


def step_python(a):
    step("[4/8] Python")
    py = venv_python()
    if not os.path.isfile(py) or subprocess.run([py, "-c", "import sys"], capture_output=True).returncode:
        if a.dry_run:
            would(f"make a Python of WindowSmash's own in {os.path.relpath(VENV, ROOT)}, with requirements.txt and Pillow")
            return
        info(f"making a Python of WindowSmash's own in {os.path.relpath(VENV, ROOT)} (touches nothing else)")
        shutil.rmtree(VENV, ignore_errors=True)
        r = subprocess.run([sys.executable, "-m", "venv", VENV])
        if r.returncode:
            raise Stop("Python could not make its venv (see above). On Debian and Ubuntu that needs python3-venv: "
                       "run this again without --no-packages, or install it yourself.")
    version = subprocess.run([py, "-c", "import sys; print(sys.version.split()[0])"], capture_output=True, text=True).stdout.strip()
    info(f"Python {version}: {os.path.relpath(py, ROOT)}")
    need = subprocess.run([py, "-c", "import Cryptodome, PIL; import sys, importlib.util as u; sys.exit(0 if u.find_spec('zstandard') "
                          "or (sys.version_info >= (3, 14) and u.find_spec('compression.zstd')) else 1)"], capture_output=True)
    if need.returncode == 0:
        info("packages: installed")
        return
    if a.dry_run:
        would("install requirements.txt and Pillow there with pip")
        return
    info("installing the packages (requirements.txt, and Pillow for the overlay's font)")
    r = subprocess.run([py, "-m", "pip", "install", "--disable-pip-version-check", "-r", os.path.join(ROOT, "requirements.txt"),
                        *PYTHON_EXTRA])
    if r.returncode:
        raise Stop("pip could not install the packages (see above). Check the internet connection, then run this again.")


# ---- step 5: the build's third-party parts ------------------------------------------------------------
def extract_part(archive, dest, prefix):
    """The archive's files under `prefix` into dest (a folder made afresh), symbolic links kept."""
    tmp = dest + ".partial"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    with tarfile.open(archive) as t:
        members = []
        for m in t.getmembers():
            rel = strip_member(m.name, prefix)
            if rel is None or m.isdev():
                continue
            if m.islnk():   # (a hard link names another member: inside the part too)
                m.linkname = strip_member(m.linkname, prefix)
                if m.linkname is None:
                    continue
            m.name = rel
            members.append(m)
        if hasattr(tarfile, "data_filter"):
            t.extractall(tmp, members=members, filter="data")
        else:
            for m in members:   # (older Pythons: the same rule by hand)
                if m.name.startswith("/") or ".." in m.name.split("/") or (m.issym() and (m.linkname.startswith("/") or ".." in m.linkname.split("/"))):
                    raise Stop(f"{archive} has a file outside its folder ({m.name}): not used")
            t.extractall(tmp, members=members)
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    os.replace(tmp, dest)


def step_parts(a):
    step("[5/8] DXVK and vkd3d (deps/linux-toolchain)")
    downloads = os.path.join(TOOLCHAIN, "downloads")
    stamps = os.path.join(TOOLCHAIN, ".parts")
    redo = set()   # (a part inside another's folder - the directx headers in DXVK's - goes again after it)
    for what, url, sha, name, where, prefix in PARTS:
        dest = os.path.join(TOOLCHAIN, where)
        stamp = os.path.join(stamps, name + ".done")
        if name not in redo and read_text(stamp).strip() == sha and os.path.isdir(dest):
            info(f"{what}: there")
            continue
        if a.dry_run:
            would(f"download {what} ({url}) and unpack it into {os.path.relpath(dest, ROOT)}")
            continue
        archive = os.path.join(downloads, name)
        if not (os.path.isfile(archive) and hashlib.sha256(open(archive, "rb").read()).hexdigest() == sha):
            download(url, archive, what, sha)
        extract_part(archive, dest, prefix)
        os.makedirs(stamps, exist_ok=True)
        with open(stamp, "w") as f:
            f.write(sha + "\n")
        info(f"{what}: unpacked into {os.path.relpath(dest, ROOT)}")
        redo |= {p[3] for p in PARTS if p[4].startswith(where + "/")}


# ---- step 6: your files -----------------------------------------------------------------------------------
def detected_folders():
    """The folders to suggest: last time's, else the emulators', else nothing."""
    home = os.path.expanduser("~")
    c = folder_candidates(home, os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share"),
                          os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config"),
                          sorted(glob.glob(os.path.join(home, ".var", "app", "*"))))
    saved = load_saved_folders(os.path.join(ROOT, "dump", "build_folders.txt"))
    keys = saved.get("keys") or next((d for d in c["keys"] if os.path.isfile(os.path.join(d, "prod.keys"))), None)
    firmware = saved.get("firmware") or next((d for d in c["firmware"] if os.path.isdir(d)), None)
    game = saved.get("game") or next((d for d in c["game"] if os.path.isdir(d)), None)
    return {"game": game, "keys": keys, "firmware": firmware}


def scan_game(folder, keys):
    """(ok, what tools/gamefiles.py finds in the folder)."""
    r = subprocess.run([venv_python(), os.path.join(ROOT, "tools", "gamefiles.py"), folder, "--keys", keys, "--json"],
                       capture_output=True, text=True)
    try:
        scan = json.loads(r.stdout)
    except ValueError:
        return False, "could not read the folder: " + ((r.stdout + r.stderr).strip().splitlines() or ["?"])[-1]
    lines = list(scan.get("summary") or [])
    if os.path.isfile(os.path.join(folder, "romfs.bin")):
        lines.append("romfs.bin found (the game data, used as it is).")
    if scan.get("program"):
        return True, " ".join(lines)
    return False, " ".join(lines + [f"Problem: {scan.get('problem')}"])


def ask(label, default, interactive):
    if not interactive:
        return default
    try:
        answer = input(f"  {label} [{default or ''}]: ").strip().strip('"').strip("'")
    except EOFError:
        print()
        return default
    return os.path.expanduser(answer) if answer else default


def ask_folder(label, value, check, interactive):
    """A folder that passes `check` ((ok, text)), asked until it does (or given and checked once)."""
    while True:
        ok, text = check(value)
        info(f"{label}: {value or '(none)'} - {text}")
        if ok:
            return os.path.abspath(value)
        if not interactive:
            raise Stop(f"The {label} folder is not usable ({text}). Name it with --{label} (see bash install-windowsmash.sh --help).")
        value = ask(f"{label.capitalize()} folder", value, True)


def files_ready():
    """Whether the build has read everything it needs from the game files already (dump/: the
    program, fonts, game data, DLC; tools/build_game.py --check-ready): then they are not asked for
    again - an update only builds what changed."""
    return subprocess.run([venv_python(), os.path.join(ROOT, "tools", "build_game.py"), "--check-ready"],
                          cwd=ROOT, capture_output=True).returncode == 0


def step_files(a):
    step("[6/8] Your game files")
    if not (a.game or a.keys or a.firmware) and not a.dry_run and os.path.isfile(venv_python()) and files_ready():
        info("already read from your game files (dump/): not asking for them again. To build from other game files, "
             "run: bash install-windowsmash.sh --game <folder>")
        name_file = os.path.join(ROOT, "online_name.txt")
        name = a.online_name or read_text(name_file).strip()
        if name and name != read_text(name_file).strip():
            with open(name_file, "w", encoding="utf-8", newline="\n") as f:
                f.write(name + "\n")
        if name:
            info(f"online name: {name}")
        return {"game": None, "keys": None, "firmware": None, "name": name}
    found = detected_folders()
    for key in ("game", "keys", "firmware"):
        if getattr(a, key):
            found[key] = os.path.abspath(os.path.expanduser(getattr(a, key)))
    interactive = sys.stdin.isatty() and not a.yes
    if interactive:
        info("Press Enter to keep the folder in [brackets], or type another.")
    name_file = os.path.join(ROOT, "online_name.txt")
    default_name = a.online_name or read_text(name_file).strip() or f"Player{int.from_bytes(os.urandom(2), 'big') % 9000 + 1000}"
    if a.dry_run:
        for key in ("game", "keys", "firmware"):
            info(f"{key}: {found[key] or 'not found on this PC (to be asked)'}")
        would(f"ask for these folders and the online name ('{default_name}'), then build with the answers")
        return {**found, "name": default_name}
    if interactive and not a.keys:
        found["keys"] = ask("Keys folder (with prod.keys)", found["keys"], True)
    keys = ask_folder("keys", found["keys"], check_keys, interactive)
    if interactive and not a.firmware:
        found["firmware"] = ask("Firmware folder (.nca files)", found["firmware"], True)
    firmware = ask_folder("firmware", found["firmware"], check_firmware, interactive)
    if interactive and not a.game:
        found["game"] = ask("Game folder (the game's .nca, .nsp or .xci files, with its 13.0.5 update)", found["game"], True)
    game = ask_folder("game", found["game"], lambda p: (False, "choose the folder with your game files")
                      if not p or not os.path.isdir(p) else scan_game(p, keys), interactive)
    name = a.online_name or (ask("Online name (what other players see)", default_name, True) if interactive else default_name)
    if name != read_text(name_file).strip():
        with open(name_file, "w", encoding="utf-8", newline="\n") as f:
            f.write(name + "\n")
    info(f"online name: {name}")
    return {"game": game, "keys": keys, "firmware": firmware, "name": name}


# ---- step 7: launchers ----------------------------------------------------------------------------------
def step_launchers(a):
    step("[7/8] Launchers")
    for name in LAUNCHERS:
        path = os.path.join(ROOT, name)
        if os.path.isfile(path) and not a.dry_run:
            os.chmod(path, 0o755)
    apps = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "applications")
    entry = os.path.join(apps, "windowsmash.desktop")
    text = desktop_entry(ROOT)
    if read_text(entry) == text:
        info(f"applications menu: WindowSmash ({entry})")
    elif a.dry_run:
        would(f"add WindowSmash to the applications menu ({entry})")
    else:
        os.makedirs(apps, exist_ok=True)
        with open(entry, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.chmod(entry, 0o755)
        info(f"applications menu: WindowSmash ({entry})")
    info("in this folder: play-ssbu.sh (online); in launchers/: play-ssbu-offline.sh, host-ssbu-netplay.sh, "
         "join-ssbu-netplay.sh, unlock-all-fighters.sh, prepare-shaders.sh")
    firewall = None
    if shutil.which("ufw") and "Status: active" in subprocess.run(["ufw", "status"], capture_output=True, text=True).stdout:
        firewall = "sudo ufw allow 39420/udp"
    elif shutil.which("firewall-cmd") and subprocess.run(["firewall-cmd", "--state"], capture_output=True).returncode == 0:
        firewall = "sudo firewall-cmd --permanent --add-port=39420/udp && sudo firewall-cmd --reload"
    if firewall:
        info(f"a firewall is on: playing online works as it is; to host a private room, open UDP port 39420: {firewall}")


# ---- step 8: the build --------------------------------------------------------------------------------------
def step_build(a, files):
    step("[8/8] Build the game")
    cmd = [venv_python(), os.path.join(ROOT, "tools", "build_game.py")]
    for key in ("game", "keys", "firmware"):
        if files.get(key):   # (none when the game files were read already: files_ready)
            cmd += [f"--{key}", files[key]]
    if a.jobs:
        cmd += ["--jobs", str(a.jobs)]
    if a.skip_build:
        cmd.append("--prepare-only")
    if a.dry_run:
        would("run: " + " ".join(cmd))
        return False
    info("the program, fonts, data, DLC and fighters" + ("; not the build itself (--skip-build)" if a.skip_build else
         ", then the build: one to a few hours the first time"))
    env = {**os.environ, "SSBU_LINUX_TOOLCHAIN": TOOLCHAIN}
    if subprocess.run(cmd, cwd=ROOT, env=env).returncode:
        raise Stop("The build stopped (the message above says why). Fix that, then run bash install-windowsmash.sh again: it "
                   "carries on where it stopped.")
    return not a.skip_build


def main(argv=None):
    ap = argparse.ArgumentParser(prog="install-windowsmash.sh", description="Set up WindowSmash on Linux in one go.")
    ap.add_argument("--game", help="the folder with the game's files (found or asked)")
    ap.add_argument("--keys", help="the folder with prod.keys and title.keys (found or asked)")
    ap.add_argument("--firmware", help="the folder with the firmware's .nca files (found or asked)")
    ap.add_argument("--online-name", help="the name other players see online (asked; kept in online_name.txt)")
    ap.add_argument("--jobs", type=int, help="how many files to compile at a time (default: by the RAM)")
    ap.add_argument("--skip-build", action="store_true", help="everything but compiling the game")
    ap.add_argument("--no-update", action="store_true", help="don't look for a newer WindowSmash")
    ap.add_argument("--no-packages", action="store_true", help="don't install packages from the distribution")
    ap.add_argument("--yes", action="store_true", help="take the folders found or given, without asking")
    ap.add_argument("--dry-run", action="store_true", help="say what each step would do, do nothing")
    a = ap.parse_args(argv)
    print("WindowSmash setup (Linux)")
    print(f"  {ROOT}")
    if a.dry_run:
        print("  dry run: saying what each step would do, doing nothing")
    try:
        if step_update(a):
            step("Starting the updated installer")
            args = [x for x in (argv if argv is not None else sys.argv[1:]) if x != "--no-update"]
            os.execv(sys.executable, [sys.executable, "-u", os.path.join(ROOT, "tools", "linux", "install_linux.py"),
                                      *args, "--no-update"])
        step_check_pc()
        step_packages(a)
        step_python(a)
        if a.dry_run and not os.path.isfile(venv_python()):
            print("\n(dry run) the later steps need WindowSmash's Python; stopping here")
            return 0
        step_parts(a)
        files = step_files(a)
        step_launchers(a)
        built = step_build(a, files)
        print()
        print("Done. Start the game with ./play-ssbu.sh, or WindowSmash in your applications menu." if built else "Setup done.")
        if built:
            print("To make the game's shaders in advance (no hitch when an effect shows first): bash launchers/prepare-shaders.sh")
        return 0
    except Stop as e:
        print(f"\nERROR: {e}")
        return 1
    except KeyboardInterrupt:
        print("\nStopped. Run bash install-windowsmash.sh again to carry on from here.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
