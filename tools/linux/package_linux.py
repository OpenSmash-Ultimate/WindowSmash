#!/usr/bin/env python3
"""Put the Linux build together as a folder to copy into a Linux machine (your own: the folder
holds the game's code and data, so it is not for sharing).

Usage: package_linux.py [--port <dir>] [--build <build-linux dir>] [--out <dir>] [--tar] [--no-save] [--replace]

<out>/ssbu-linux/ (default <out>: dist-linux in this checkout) gets the program and its DXVK
libraries, the module images, fonts, the add-on content list and shader cache from the game
tree's dump/, a copy of its save (unless --no-save), the launchers (run.sh goes online: the
lobby is in online_server.txt), the guides and the unlock tool. <port>
is the tree the build was made from (its port.txt; see build_linux.py), and a package holds one
tree's game: package another tree's into another <out>, or give
--replace to start this one over - never one the game was played from. The game's RomFS stays
in the files it is read from on Windows (romfs.cfg: an NCA, or for an update the base's NCA and
the update's romfs_patch.bin, pieced together by the layer map the package carries); the
launchers find them, by name and size, on the Linux side.
--tar also writes ssbu-linux.tar beside it, which keeps the programs' executable bits."""
import os
import re
import shutil
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from toolchain import DXVK_LIBS, default_port, read_port_stamp, same_path, write_port_stamp   # noqa: E402


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


BUILD = os.path.abspath(arg("--build", os.path.join(REPO, "build-linux")))
PORT = os.path.abspath(arg("--port") or read_port_stamp(BUILD) or default_port())
OUT = os.path.abspath(arg("--out", os.path.join(REPO, "dist-linux")))
BUNDLE = os.path.join(OUT, "ssbu-linux")
EXECUTABLE = {"ssbu", "run.sh", "run-offline.sh", "host-netplay.sh", "join-netplay.sh", "install-deps.sh", "unlock-all-fighters.sh"}


def copy(src, dst):
    """Copy a file unless an identical-looking one (size and time) is there already."""
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return 0
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return os.path.getsize(src)


def copy_tree(src, dst, keep=lambda name: True, folders=False):
    """Copy the files `keep` takes; with `folders`, every folder too, empty ones included (a save has some)."""
    n = 0
    for root, _dirs, files in os.walk(src):
        if folders:
            os.makedirs(os.path.join(dst, os.path.relpath(root, src)), exist_ok=True)
        for f in files:
            if keep(f):
                n += copy(os.path.join(root, f), os.path.join(dst, os.path.relpath(root, src), f))
    return n


def shader_cache_version():
    with open(os.path.join(REPO, "runtime", "gpu_shader_cache.h"), encoding="utf-8") as f:
        return int(re.search(r"#define SHADER_CACHE_VERSION (\d+)", f.read()).group(1))


# A cache file dated before 2002 is the ahead-of-time compiler's (tools/shader_aot.py), not yet used
# by the game (runtime/gpu_shader_cache.h): left out, as the Windows kit leaves it out.
PLAYED_AFTER = 1009843200   # 2002-01-01 UTC


# The launchers' environment variables that name a RomFS file outright, by file name
ROMFS_FILE_VARS = {"romfs_patch.bin": "SSBU_ROMFS_PATCH"}


def romfs_files(cfg_path):
    """What the launchers need to set up the RomFS on Linux, from the Windows romfs.cfg (see
    runtime/hle_fs.c rom_open): (kind, layer map or None, NCA offset or "", files), each file
    (its variable, name, size, Windows folder). The files are found by name, so the names must
    differ."""
    with open(cfg_path) as f:
        lines = [line.strip() for line in f.read().splitlines()]
    if len(lines) < 2 or not lines[0]:
        raise SystemExit(f"{cfg_path}: not a romfs.cfg")
    if lines[0] == "layered":
        kind, layer_map, offset, paths = "layered", lines[1], "", [p for p in lines[2:] if p]
        with open(layer_map, "rb") as f:
            if f.read(8) != b"SSBUROM1":
                raise SystemExit(f"{layer_map}: not a layer map")
        if not 1 <= len(paths) <= 8:
            raise SystemExit(f"{cfg_path}: {len(paths)} sources (the runtime reads 1 to 8)")
    else:
        kind, layer_map, offset, paths = "plain", None, lines[1], lines[:1]
    files = []
    for path in paths:
        if not os.path.isfile(path):
            raise SystemExit(f"{cfg_path}: {path} is not there")
        folder, name = os.path.split(path.replace("/", "\\"))
        if name.lower() in (f[1].lower() for f in files):
            raise SystemExit(f"{cfg_path}: two sources named {name} (the launchers find them by name)")
        var = ROMFS_FILE_VARS.get(name) or ("SSBU_NCA" if name.lower().endswith(".nca") else None)
        if var is None or var in (f[0] for f in files):
            var = f"SSBU_ROMFS_{len(files)}"
        files.append((var, name, os.path.getsize(path), folder))
    return kind, layer_map, offset, files


def start_bundle():
    """A package holds one tree's game: one made from another tree is started over with
    --replace - but never one the game was played from (it has a save of its own then)."""
    if not os.path.isdir(BUNDLE):
        return
    packaged = read_port_stamp(os.path.join(BUNDLE, "dump"))
    if packaged is not None and same_path(packaged, PORT):
        return
    played = [p for p in ("build/play.log", "dump/modules/romfs.cfg") if os.path.exists(os.path.join(BUNDLE, p))]
    if played:
        raise SystemExit(f"{BUNDLE} has {packaged or 'another tree'}'s game and was played from ({played[0]}): "
                         f"move it away or give --out another folder")
    if "--replace" not in sys.argv:
        raise SystemExit(f"{BUNDLE} has {packaged or 'another tree'}'s game: give --out another folder, "
                         f"or --replace to start this one over")
    print(f"{BUNDLE}: had {packaged or 'another tree'}'s game; starting it over")
    shutil.rmtree(BUNDLE)


def main():
    exe = os.path.join(BUILD, "ssbu")
    if not os.path.exists(exe):
        raise SystemExit(f"no Linux build at {exe}: run tools/linux/build_linux.py first")
    built = read_port_stamp(BUILD)
    if built is None:
        print(f"note: {BUILD} does not say which game tree it was built from; packaging it with {PORT}'s data")
    elif not same_path(built, PORT):
        raise SystemExit(f"{BUILD} is the game of {built}, not {PORT}: build that tree first (build_linux.py --port)")
    dump = os.path.join(PORT, "dump")
    modules = os.path.join(dump, "modules")
    kind, layer_map, nca_offset, romfs = romfs_files(os.path.join(modules, "romfs.cfg"))
    start_bundle()
    os.makedirs(BUNDLE, exist_ok=True)
    copied = copy(exe, os.path.join(BUNDLE, "ssbu"))
    copied += copy(os.path.join(BUILD, "ssbu.syms"), os.path.join(BUNDLE, "ssbu.syms"))
    for so in ("libdxvk_d3d11.so.0", "libdxvk_dxgi.so.0"):
        copied += copy(os.path.join(DXVK_LIBS, so), os.path.join(BUNDLE, "lib", so))

    copied += copy_tree(modules, os.path.join(BUNDLE, "dump", "modules"), lambda f: f.endswith(".reloc.bin") or f == "aoc.cfg")
    # (the unlock tool reads which game version the save belongs to from main's build id)
    copied += copy(os.path.join(dump, "exefs", "main"), os.path.join(BUNDLE, "dump", "exefs", "main"))
    copied += copy_tree(os.path.join(dump, "fonts"), os.path.join(BUNDLE, "dump", "fonts"))
    version = shader_cache_version()   # (only the current translator's compiles: older ones are never read)
    cache = os.path.join(dump, "shadercache")   # (and only what the game has used)
    copied += copy_tree(cache, os.path.join(BUNDLE, "dump", "shadercache"),
                        lambda f: f.startswith(f"v{version}_") and os.path.getmtime(os.path.join(cache, f)) >= PLAYED_AFTER)
    os.makedirs(os.path.join(BUNDLE, "dump", "nro"), exist_ok=True)
    os.makedirs(os.path.join(BUNDLE, "build"), exist_ok=True)
    if "--no-save" not in sys.argv and os.path.isdir(os.path.join(PORT, "save")) and not os.path.isdir(os.path.join(BUNDLE, "save")):
        copied += copy_tree(os.path.join(PORT, "save"), os.path.join(BUNDLE, "save"), folders=True)
    if layer_map:   # (romfs.cfg itself the launchers write, with the Linux side's paths)
        copied += copy(layer_map, os.path.join(BUNDLE, "dump", "modules", "romfs.layers"))
    write_port_stamp(os.path.join(BUNDLE, "dump"), PORT)

    # the RomFS's files as the Windows side has them are what the launchers look for
    files_text = "\n".join("|".join(map(str, f)) for f in romfs)
    scripts = os.path.join(HERE, "bundle")
    for root, _dirs, files in os.walk(scripts):
        for f in files:
            src = os.path.join(root, f)
            dst = os.path.join(BUNDLE, os.path.relpath(src, scripts))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            text = open(src, encoding="utf-8").read()
            text = text.replace("@ROMFS_KIND@", kind).replace("@ROMFS_OFFSET@", nca_offset).replace("@ROMFS_FILES@", files_text)
            with open(dst, "w", encoding="utf-8", newline="\n") as out:
                out.write(text)
    # the guides, laid out as in the repository (the launchers' messages point at docs/...), and
    # online_server.txt: the online lobby run.sh joins
    for doc in ("LINUX.md", "MULTIPLAYER.md", "DLC-AND-UNLOCKS.md", "UPDATES.md"):
        copy(os.path.join(REPO, "docs", doc), os.path.join(BUNDLE, "docs", doc))
    if os.path.exists(os.path.join(REPO, "online_server.txt")):
        copy(os.path.join(REPO, "online_server.txt"), os.path.join(BUNDLE, "online_server.txt"))
    for tool in ("unlock_all.py", "ssbu_relay.py"):
        copy(os.path.join(REPO, "tools", tool), os.path.join(BUNDLE, "tools", tool))
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(BUNDLE) for f in fs)
    print(f"{BUNDLE}: {PORT}'s game, {total / 2**30:.2f} GB ({copied / 2**20:,.0f} MB copied now); the game data "
          f"stays in {', '.join(f'{name} ({size / 1e9:.1f} GB)' for _v, name, size, _f in romfs)}")

    if "--tar" in sys.argv:
        tar_path = os.path.join(OUT, "ssbu-linux.tar")

        def mode(info):
            name = os.path.basename(info.name)
            info.mode = 0o755 if info.isdir() or name in EXECUTABLE else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            return info
        with tarfile.open(tar_path + ".partial", "w") as t:
            t.add(BUNDLE, arcname="ssbu-linux", filter=mode)
        os.replace(tar_path + ".partial", tar_path)
        print(f"{tar_path}: {os.path.getsize(tar_path) / 2**30:.2f} GB")


if __name__ == "__main__":
    main()
