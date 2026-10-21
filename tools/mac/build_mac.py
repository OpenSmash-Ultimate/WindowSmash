#!/usr/bin/env python3
"""Build the game for macOS on Apple silicon (arm64, macOS 14 or newer; the toolchain:
tools/mac/toolchain.py).

Usage: build_mac.py [--port <tree>] [--out <dir>] [--jobs N] [--runtime-only] [--link-only] [--replace]

The recompiled game is the C tools/recomp.py generates into <tree>/build/{main,subsdk0,sdk,nro/*}
(recomp.py all --generate-only); this compiles it, with the runtime, into <out> (default
<tree>/build-mac). <tree> is a game tree, a folder with dump/ and build/ at its top: the port's
own (the default), or another laid out the same way (tools/mac/boottest's made-up game). Files
with exception landing
sites (guest C++ try/catch) are compiled as C++ (see recomp.h). The runtime is runtime/*.c without
the Direct3D 11 renderer, the POSIX parts of the Linux platform layer (runtime/linux) and the
macOS one (runtime/macos: memory, fibers, window, the Metal renderer); it links against
SDL3.framework (deps/SDL3) and libopus and libvpx, built from third_party/ (tools/codec_libs.py).
The program is an app: <out>/WindowSmash.app, with SDL3.framework in it, signed (ad hoc) as a
whole; <out>/ssbu.syms names its functions for the profiler.
  <out>/obj/       the objects: rt/ (the runtime), main/, subsdk0/, sdk/, nro/<module>/
  <out>/gen/       the files made for the build: hle_table.c, wrap_missing.c, nro_table.c,
                   overlay_font.h (tools/mac/make_overlay_font.m), windowsmash.icns
  <out>/codecs/    libopus.a, libvpx.a
Incremental: a file is compiled again when its source is newer than its object, and a game object
also when its C is not the file it was compiled from (obj/game_sources.tsv records each one's size
and time: a module dir can hold older C than its objects, as one an earlier WindowSmash filled
with another folder's did) or the
game's compile command changed (the runtime's: obj/rt/cflags.txt). <out> holds one tree's game
(port.txt; a tree's own build-mac goes with it when the tree is moved or copied): build another
tree's into another <out>, or give --replace to start this one over (also this tree's own)."""
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                          # (toolchain, also under a Python that keeps
sys.path.insert(1, os.path.dirname(HERE))         # a script's folder off sys.path; codec_libs)
from toolchain import (AR, CFLAGS, CLANG, CODESIGN, CXXFLAGS, MIN_MACOS, NM, REPO, RUNTIME, SDL3_DIR,  # noqa: E402
                       SDL3_FRAMEWORK, TARGET, compile_each, run)


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


PORT = os.path.abspath(arg("--port") or REPO)
OUT = os.path.abspath(arg("--out") or os.path.join(PORT, "build-mac"))
JOBS = int(arg("--jobs", str(max(4, (os.cpu_count() or 8) - 2))))
OBJ = os.path.join(OUT, "obj")
GAME_SOURCES = os.path.join(OBJ, "game_sources.tsv")
GEN = os.path.join(OUT, "gen")
APP = os.path.join(OUT, "WindowSmash.app")
EXE = os.path.join(APP, "Contents", "MacOS", "ssbu")
BUNDLE_ID = "io.github.sno2207.windowsmash"
ICON = os.path.join(REPO, "docs", "assets", "windowsmash.ico")

RUNTIME_INCLUDES = ["-I", os.path.join(RUNTIME, "macos", "include"), "-I", os.path.join(RUNTIME, "linux", "include"),
                    "-I", RUNTIME, "-I", GEN, "-F", SDL3_DIR, "-I", os.path.join(REPO, "third_party", "libvpx")]
RUNTIME_DEFINES = ["-DSSBU_HAVE_OPUS", "-DSSBU_HAVE_VPX", "-DSSBU_HAVE_SDL3"]
# runtime/*.c as on Linux, but for Direct3D 11 and its overlay: the Metal renderer (runtime/macos) instead
# (its overlay draws the settings menu, gpu_menu.c, too)
RUNTIME_PATTERN = r"(host|kernel|gpu\w*|hle\w*|ipc\w*|net\w*|debug_watchdog)\.c"
RUNTIME_LEFT_OUT = ("gpu_d3d11.c", "gpu_overlay.c")
LINUX_PARTS = ("win32.c", "upnp.c", "eh.cpp")   # (runtime/linux: the Win32 calls on POSIX, UPnP, C++ landing sites)
MAC_SOURCES = (".c", ".m", ".cpp", ".S")
FRAMEWORKS = ("SDL3", "Metal", "QuartzCore", "AppKit", "Foundation", "CoreFoundation", "IOKit", "CoreGraphics")


def listing(folder, suffixes=None):
    """The names in a folder, sorted, without the ._* files macOS leaves beside each file on a
    volume that cannot hold its metadata (exFAT, SMB shares)."""
    if not os.path.isdir(folder):
        return []
    return sorted(n for n in os.listdir(folder) if not n.startswith("._") and (not suffixes or n.endswith(suffixes)))


def modules():
    """(name, generated C directory, object directory) for every module tools/recomp.py generated."""
    build = os.path.join(PORT, "build")
    found = [(n, os.path.join(build, n), os.path.join(OBJ, n)) for n in ("main", "subsdk0", "sdk")]
    nro = os.path.join(build, "nro")
    found += [(n, os.path.join(nro, n), os.path.join(OBJ, "nro", n)) for n in listing(nro)
              if os.path.exists(os.path.join(nro, n, "manifest.tsv"))]
    return [m for m in found if os.path.exists(os.path.join(m[1], "manifest.tsv"))]


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def read_port_stamp(folder):
    try:
        with open(os.path.join(folder, "port.txt")) as f:
            return f.read().strip() or None
    except OSError:
        return None


def write_port_stamp(folder, port):
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "port.txt"), "w", newline="\n") as f:
        f.write(port + "\n")


def use_tree():
    """<out>'s game objects must all be from one tree: of another tree's, one newer than this
    tree's source would be kept, and one this tree has no source for linked. So another tree's
    are only started over with --replace (which starts this tree's own over too). The tree's own
    build-mac (<tree>/build-mac) is its own wherever the tree was before: moved, renamed or
    copied, the tree took its C along with the objects compiled from it."""
    built = read_port_stamp(OUT)
    game = [os.path.join(OBJ, n) for n in ("main", "subsdk0", "sdk", "nro")]
    other = (built is None or not same_path(built, PORT)) and any(os.path.isdir(d) for d in game)
    if other and same_path(os.path.dirname(OUT), PORT):
        if built:
            print(f"{OUT}: this tree's own, built when it was at {built}")
        other = False
    if other and ("--replace" not in sys.argv or "--link-only" in sys.argv):
        raise SystemExit(f"{OUT} holds the game compiled from {built or 'another tree'}: build {PORT}'s into another "
                         f"--out, or give --replace (without --link-only) to start this one over")
    if (other or "--replace" in sys.argv) and "--link-only" not in sys.argv:
        print(f"{OUT} holds the game compiled from {built or 'another tree'}: compiling {PORT}'s from the start" if other
              else f"--replace: compiling {PORT}'s game from the start")
        for path in game + [APP]:
            shutil.rmtree(path, ignore_errors=True)
        for path in (os.path.join(OUT, "ssbu.syms"), os.path.join(OUT, "ssbu.filelist"), GAME_SOURCES):
            if os.path.exists(path):
                os.remove(path)
    write_port_stamp(OUT, PORT)


def needs(obj, *srcs):
    return not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(s) for s in srcs)


def has_landing_sites(path):
    with open(path, "rb") as f:
        return b"__try" in f.read()


def manifest_rows(src_dir):
    with open(os.path.join(src_dir, "manifest.tsv")) as f:
        rows = [line.split("\t") for line in f.read().splitlines() if line]
    return rows + [["g_module.c", "0", "0"]]


def game_objects(src_dir, out_dir):
    return [os.path.join(out_dir, fname[:-2] + ".o") for fname, _i, _b in manifest_rows(src_dir)]


def game_opt(fname, biggest, cxx):
    """The optimisation level a generated file is compiled at. Very large functions make the
    optimiser superlinear: those are built at a lower level (as on Windows). As C++ (cxx: a file
    with landing sites), a huge function full of them (lua2cpp item/fighter tables: up to 210k
    instructions, 41k sites) takes the optimiser hours even at -O1 - and seconds at -O0. Those are
    one-off set-up code; 38k-instruction ones still take 20 s at -O1."""
    if cxx and int(biggest) > 50000:
        return "-O0"
    return "-O1" if int(biggest) > 20000 or fname == "g_module.c" else "-O2"


def game_command():
    """What every game object is compiled with but its optimisation level (which its line in
    game_sources.tsv has): all of them again when it changes. Not the SDK's path, as the
    generated C hardly uses the SDK, and a switch between the Command Line Tools and an Xcode
    (another path) would compile hours of it again for nothing."""
    flags = list(CFLAGS)
    if "-isysroot" in flags[:-1]:
        flags[flags.index("-isysroot") + 1] = "<sdk>"
    return subprocess.list2cmdline([CLANG, *flags, *CXXFLAGS, "-I", RUNTIME])


def source_id(path):
    """What tells one version of a generated file from another: its size and time, to the second
    (as kept by a copy to a disk that keeps times to the second). A file of a module dir an earlier
    WindowSmash filled from another folder's (a hard link, or a copy with its times) can be older
    than the folder's objects, so being newer than the object is not enough."""
    st = os.stat(path)
    return f"{st.st_size}\t{int(st.st_mtime)}"


def read_game_sources():
    """{object (relative to obj/): (level, c or c++, source_id)} of the game objects compiled with
    today's command, from game_sources.tsv (the last line of an object counts), and why there
    are none when the file says nothing about today's command (else None)."""
    try:
        with open(GAME_SOURCES, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return {}, "no record of the C its objects were compiled from"
    if lines[:1] != ["# " + game_command()]:
        return {}, "the compile command changed since its objects were compiled"
    found = {}
    for line in lines[1:]:
        p = line.split("\t")
        if len(p) == 5:
            found[p[0]] = (p[1], p[2], p[3] + "\t" + p[4])
    return found, None


def record_line(key, opt, lang, ident):
    return f"{key}\t{opt}\t{lang}\t{ident}\n"


compiled_from = {}   # object -> its line in game_sources.tsv, once compiled (game_jobs)


def record_compiled(job):
    """A game object compiled: its line added to game_sources.tsv (at once: a build stopped
    half-way keeps what it compiled)."""
    line = compiled_from.get(job[1])
    if line:
        with open(GAME_SOURCES, "a", encoding="utf-8", newline="\n") as f:
            f.write(line)


def game_jobs():
    recomp_h, module_h = os.path.join(RUNTIME, "recomp.h"), os.path.join(RUNTIME, "module.h")
    record, why = read_game_sources()
    kept = []
    jobs = []
    for _name, src_dir, out_dir in modules():
        rows = manifest_rows(src_dir)
        # objects (and C++ wrappers) of files the module no longer has: an older partitioning's
        keep = {fname[:-2] for fname, _i, _b in rows}
        for n in listing(out_dir, (".o", "_cxx.cpp")):
            if n.startswith("g") and re.sub(r"(_cxx\.cpp|\.o)$", "", n) not in keep:
                os.remove(os.path.join(out_dir, n))
        for fname, _insns, biggest in rows:
            src = os.path.join(src_dir, fname)
            obj = os.path.join(out_dir, fname[:-2] + ".o")
            key = os.path.relpath(obj, OBJ)
            ident = source_id(src)
            was = record.get(key)
            if was and was[2] == ident and was[0] == game_opt(fname, biggest, was[1] == "c++") and \
                    not needs(obj, src, recomp_h, *([module_h] if fname == "g_module.c" else [])):
                kept.append(record_line(key, *was))
                continue
            cxx = fname != "g_module.c" and has_landing_sites(src)
            opt = game_opt(fname, biggest, cxx)
            compiled_from[obj] = record_line(key, opt, "c++" if cxx else "c", ident)
            if why and os.path.exists(obj):
                print(f"game: {why}: compiling all of it again")
                why = None
            if cxx:
                wrapper = os.path.join(out_dir, fname[:-2] + "_cxx.cpp")
                os.makedirs(out_dir, exist_ok=True)
                text = '#include "recomp.h"\nextern "C" {\n#include "%s"\n}\n' % src
                if not os.path.exists(wrapper) or read(wrapper) != text:
                    with open(wrapper, "w") as f:
                        f.write(text)
                jobs.append((wrapper, obj, opt, ["-I", RUNTIME]))
            else:
                jobs.append((src, obj, opt, ["-I", RUNTIME]))
    os.makedirs(OBJ, exist_ok=True)
    with open(GAME_SOURCES, "w", encoding="utf-8", newline="\n") as f:   # (what is up to date; record_compiled adds the rest)
        f.write("# " + game_command() + "\n" + "".join(kept))
    return jobs


def hle_names():
    names = set()
    for n in listing(RUNTIME, ".c"):
        with open(os.path.join(RUNTIME, n), encoding="utf-8") as f:
            names |= set(re.findall(r"^HLE\((\w+)\)", f.read(), re.M))
    return sorted(names)


def hle_wraps():
    """The guest functions the runtime wraps (HLE_WRAP(<address>), as tools/recomp.py finds them)."""
    found = set()
    for n in listing(RUNTIME, ".c"):
        with open(os.path.join(RUNTIME, n), encoding="utf-8") as f:
            found |= {int(a, 16) for a in re.findall(r"^HLE_WRAP\(([0-9a-fA-F]+)\)", f.read(), re.M)}
    return sorted(found)


def read(path, mode="r"):
    with open(path, mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        return f.read()


def write_if_changed(path, text):
    if os.path.exists(path) and read(path) == text:
        return False
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def hle_table_text(names):
    return ('#include "host.h"\n\n' + "".join(f"GuestFn hle_{n};\n" for n in names)
            + "const HleEntry hle_table[] = {\n" + "".join(f'    {{"{n}", hle_{n}}},\n' for n in names)
            + f"    {{0, 0}}\n}};\nconst u32 hle_count = {len(names)};\n")


def wrap_missing_text(addresses):
    """A wrapped function no module of this build has (a made-up game's): the original its wrapper names
    (f_<address>_orig, which nothing reaches) is rt_wrap_missing. The Windows link says so with
    /alternatename (tools/recomp.py); here it is a weak stub a module's own replaces. Hidden, as
    everything is: an exported weak definition would have dyld look for others at every start."""
    return ('#include "host.h"\n\nvoid rt_wrap_missing(Cpu *c);\n'
            + "".join(f'__attribute__((weak, visibility("hidden"))) void f_{a:x}_orig(Cpu *c) {{ rt_wrap_missing(c); }}\n'
                      for a in addresses))


def nro_table_text(nros):
    return ('#include "module.h"\n\n' + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
            + "const RtModule *const rt_nro_modules[] = {" + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
            + f"const u32 rt_nro_count = {len(nros)};\n")


def overlay_font():
    """gen/overlay_font.h: the debug overlay's glyphs, drawn by Core Text (make_overlay_font.m)
    as the Windows build draws them with GDI."""
    os.makedirs(GEN, exist_ok=True)
    header = os.path.join(GEN, "overlay_font.h")
    tool_src = os.path.join(HERE, "make_overlay_font.m")
    if not needs(header, tool_src):
        return
    tool = os.path.join(GEN, "make_overlay_font")
    code, out = run([CLANG, *TARGET, "-O1", "-fobjc-arc", "-w", tool_src, "-o", tool,
                     "-framework", "CoreText", "-framework", "CoreGraphics", "-framework", "Foundation"])
    if not code:
        r = subprocess.run([tool, header], capture_output=True, text=True)
        code, out = r.returncode, r.stdout + r.stderr
        print(out.strip())
    if code:   # (only the debug overlay's text needs it: not worth stopping the build for)
        print(f"could not draw the overlay font: the overlay (F1, F2) and the settings menu show no text\n{out.strip()[:2000]}")
        with open(header, "w", newline="\n") as f:
            f.write("static const unsigned char overlay_font[%d] = {0};\n" % (24 * 16 * 48 * 6))


def ico_png(path, size=256):
    """The PNG picture of a size in a .ico file (Windows keeps the 256-pixel one as PNG), or None."""
    with open(path, "rb") as f:
        data = f.read()
    _reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    for i in range(count if kind == 1 else 0):
        w, h, _c, _r, _planes, _bpp, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        png = data[offset:offset + length]
        if (w or 256) == size and (h or 256) == size and png.startswith(b"\x89PNG\r\n\x1a\n"):
            return png
    return None


# the pictures an .icns holds (name in the .iconset, pixels): from 16 to 256, the .ico's largest
ICONSET = (("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
           ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256))


def app_icon():
    """gen/windowsmash.icns, the app's icon: docs/assets/windowsmash.ico's 256-pixel picture at
    each size (sips), made into an .icns (iconutil); made once. None without one (the app then
    has the system's icon)."""
    icns = os.path.join(GEN, "windowsmash.icns")
    if not needs(icns, ICON, os.path.abspath(__file__)):
        return icns
    png = ico_png(ICON)
    if not png:
        print(f"no 256-pixel picture in {ICON}: the app has no icon of its own")
        return None
    os.makedirs(GEN, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="windowsmash-icon-", dir=GEN) as tmp:
        src = os.path.join(tmp, "icon.png")
        with open(src, "wb") as f:
            f.write(png)
        iconset = os.path.join(tmp, "windowsmash.iconset")
        os.makedirs(iconset)
        for name, px in ICONSET:
            dst = os.path.join(iconset, name + ".png")
            if px == 256:
                shutil.copyfile(src, dst)
                continue
            r = subprocess.run(["sips", "-z", str(px), str(px), src, "--out", dst], capture_output=True, text=True)
            if r.returncode:
                print(f"sips could not scale the icon: the app has no icon of its own\n{(r.stdout + r.stderr)[:1000]}")
                return None
        partial = os.path.join(tmp, "windowsmash.icns")
        r = subprocess.run(["iconutil", "-c", "icns", iconset, "-o", partial], capture_output=True, text=True)
        if r.returncode:
            print(f"iconutil could not make the icon: the app has no icon of its own\n{(r.stdout + r.stderr)[:1000]}")
            return None
        os.replace(partial, icns)
    return icns


def object_names(sources, folder):
    """The object of each source in `folder`: <name>.o, or <name>_<extension>.o for sources of
    one name (fiber.S and fiber.c)."""
    stems = [os.path.splitext(os.path.basename(s))[0] for s in sources]
    out = []
    for src, stem in zip(sources, stems):
        ext = os.path.splitext(src)[1][1:]
        out.append(os.path.join(folder, (stem if stems.count(stem) == 1 else f"{stem}_{ext}") + ".o"))
    return out


def runtime_sources():
    """(source, object) of every runtime file the macOS program is made of."""
    rt_out = os.path.join(OBJ, "rt")
    own = [os.path.join(RUNTIME, n) for n in listing(RUNTIME)
           if re.fullmatch(RUNTIME_PATTERN, n) and n not in RUNTIME_LEFT_OUT]
    linux = [os.path.join(RUNTIME, "linux", n) for n in LINUX_PARTS]
    mac = [os.path.join(RUNTIME, "macos", n) for n in listing(os.path.join(RUNTIME, "macos"), MAC_SOURCES)]
    return list(zip(own + linux + mac, object_names(own, rt_out) + object_names(linux, os.path.join(rt_out, "linux"))
                    + object_names(mac, os.path.join(rt_out, "macos"))))


def runtime_headers():
    found = [os.path.join(RUNTIME, n) for n in listing(RUNTIME, ".h")]
    for folder in (os.path.join(RUNTIME, "linux", "include"), os.path.join(RUNTIME, "macos"),
                   os.path.join(RUNTIME, "macos", "include")):
        for dp, _dirs, files in os.walk(folder):
            found += [os.path.join(dp, n) for n in files if n.endswith(".h") and not n.startswith("._")]
    font = os.path.join(GEN, "overlay_font.h")
    return found + ([font] if os.path.exists(font) else [])


def generated_sources():
    """gen/hle_table.c, wrap_missing.c and nro_table.c, written when they change: (source, object, flags)."""
    os.makedirs(GEN, exist_ok=True)
    nros = [n for n, src, _o in modules() if os.path.basename(os.path.dirname(src)) == "nro"]
    out = []
    for name, text, flags in (("hle_table", hle_table_text(hle_names()), RUNTIME_INCLUDES + RUNTIME_DEFINES),
                              ("wrap_missing", wrap_missing_text(hle_wraps()), RUNTIME_INCLUDES + RUNTIME_DEFINES),
                              ("nro_table", nro_table_text(nros), ["-I", RUNTIME])):
        src = os.path.join(GEN, name + ".c")
        write_if_changed(src, text)
        out.append((src, os.path.join(OBJ, "rt", "gen", name + ".o"), flags))
    return out


def runtime_command():
    """What the runtime's objects were compiled with: all of them again when it changes."""
    return subprocess.list2cmdline([CLANG, *CFLAGS, *RUNTIME_INCLUDES, *RUNTIME_DEFINES]) + "\n"


def runtime_jobs():
    stamp = os.path.join(OBJ, "rt", "cflags.txt")
    try:
        with open(stamp, encoding="utf-8") as f:
            every = f.read() != runtime_command()
    except OSError:
        every = True
    headers = runtime_headers()
    jobs = []
    for src, obj in runtime_sources():
        if every or needs(obj, src, *headers):
            jobs.append((src, obj, "-O2", RUNTIME_INCLUDES + RUNTIME_DEFINES))
    for src, obj, flags in generated_sources():
        if every or needs(obj, src, *headers):
            jobs.append((src, obj, "-O1", flags))
    return jobs


def runtime_objects():
    return [obj for _src, obj in runtime_sources()] + [obj for _src, obj, _f in generated_sources()]


def compile_all(jobs, what, compiled=None):
    """Compile the jobs, biggest first; compiled(job) is called (here, one at a time) for each that compiled."""
    if not jobs:
        print(f"{what}: up to date")
        return True
    t0 = time.time()
    failures = []
    # biggest first: a few files take minutes, and started last they would hold up the end
    def source_size(job):   # (a C++ wrapper's size is that of the generated file it includes)
        src = job[0]
        if src.endswith("_cxx.cpp"):
            src = re.findall(r'#include "([^"]+)"', read(src))[-1]
        return os.path.getsize(src)
    jobs = sorted(jobs, key=source_size, reverse=True)
    with ThreadPoolExecutor(JOBS) as pool:
        futures = {pool.submit(compile_each, [job], 1): job for job in jobs}
        for done, fut in enumerate(as_completed(futures), 1):
            failed = fut.result()
            if failed and os.path.exists(futures[fut][1]):   # (an older object of it is not up to date)
                os.remove(futures[fut][1])
            elif not failed and compiled:
                compiled(futures[fut])
            failures += failed
            if done % 64 == 0 or done == len(jobs):
                print(f"{what}: {done}/{len(jobs)} compiled, {time.time() - t0:,.0f}s", flush=True)
    for src, err in failures[:8]:
        print(f"FAILED {src}:\n{err[:3000]}")
    if failures:
        print(f"{what}: {len(failures)} of {len(jobs)} files did not compile: "
              + ", ".join(os.path.relpath(src, REPO) if src.startswith(REPO) else os.path.basename(src) for src, _e in failures))
    return not failures


def codecs():
    import codec_libs
    missing = [n for n, ok in (("third_party/opus", codec_libs.have_opus()),
                               ("third_party/libvpx", codec_libs.have_vpx("config-arm64"))) if not ok]
    if missing:
        raise SystemExit(f"{' and '.join(missing)} not found: this checkout of WindowSmash is incomplete")
    return codec_libs.build_static(OUT, CLANG, CFLAGS, AR, jobs=JOBS, log=lambda m: print(m, flush=True))


def info_plist(version=None):
    """The app's Info.plist: what macOS knows it by (Game Mode, the network, the screen's resolution)."""
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "WindowSmash",
        "CFBundleExecutable": "ssbu",
        "CFBundleIconFile": "windowsmash",
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "WindowSmash",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version or "1.0",
        "CFBundleVersion": version or "1.0",
        "LSApplicationCategoryType": "public.app-category.action-games",
        "LSMinimumSystemVersion": MIN_MACOS,
        "LSSupportsGameMode": True,
        "NSHighResolutionCapable": True,
        "NSLocalNetworkUsageDescription": "WindowSmash finds the players of local and online matches on your network, "
                                          "and asks your router (UPnP) to let the other players reach you.",
    }


def tree_version():
    """The game version of the tree (version.txt's first line, e.g. 13.0.5), or None."""
    try:
        with open(os.path.join(PORT, "version.txt"), encoding="utf-8") as f:
            first = f.readline().strip()
    except OSError:
        return None
    return first if re.fullmatch(r"\d+(\.\d+){0,2}", first) else None


def copy_sdl3():
    """Contents/Frameworks/SDL3.framework: a copy of deps/SDL3's (ditto keeps its links), made
    again when that one changes."""
    dst = os.path.join(APP, "Contents", "Frameworks", "SDL3.framework")
    lib, copied = os.path.join(SDL3_FRAMEWORK, "SDL3"), os.path.join(dst, "SDL3")
    if os.path.isfile(copied) and os.path.getsize(copied) == os.path.getsize(lib) and \
            int(os.path.getmtime(copied)) == int(os.path.getmtime(lib)):
        return True
    shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    r = subprocess.run(["ditto", SDL3_FRAMEWORK, dst], capture_output=True, text=True)
    if r.returncode:
        print(f"could not copy {SDL3_FRAMEWORK} into the app:\n{(r.stdout + r.stderr)[:2000]}")
        return False
    return True


def bundle():
    """The rest of the app around the program: Info.plist, SDL3.framework, the icon; then the
    whole signed (ad hoc: no identity), as macOS runs nothing unsigned on Apple silicon."""
    contents = os.path.join(APP, "Contents")
    os.makedirs(os.path.join(contents, "Resources"), exist_ok=True)
    plist = plistlib.dumps(info_plist(tree_version()), fmt=plistlib.FMT_XML, sort_keys=True)
    path = os.path.join(contents, "Info.plist")
    if not os.path.exists(path) or read(path, "rb") != plist:
        with open(path, "wb") as f:
            f.write(plist)
    with open(os.path.join(contents, "PkgInfo"), "w", newline="") as f:
        f.write("APPL????")
    if not copy_sdl3():
        return False
    icns = app_icon()
    if icns:
        shutil.copyfile(icns, os.path.join(contents, "Resources", "windowsmash.icns"))
    r = subprocess.run([CODESIGN, "--force", "--deep", "--sign", "-", APP], capture_output=True, text=True)
    if r.returncode:
        print(f"could not sign {APP}:\n{(r.stdout + r.stderr)[:2000]}")
        return False
    os.utime(APP)   # (Finder and Launch Services read the app again)
    return True


def link():
    objs = runtime_objects()
    for _name, src_dir, out_dir in modules():
        objs += game_objects(src_dir, out_dir)
    absent = [o for o in objs if not os.path.isfile(o)]
    if absent:
        print(f"not compiled yet: {len(absent)} objects ({', '.join(os.path.relpath(o, OUT) for o in absent[:5])}"
              f"{', ...' if len(absent) > 5 else ''}): build without --link-only")
        return False
    try:
        libs = codecs()
    except RuntimeError as e:
        print(f"the codecs could not be built: {e}")
        return False
    filelist = os.path.join(OUT, "ssbu.filelist")   # (one path a line: the linker's own list, spaces and all)
    with open(filelist, "w", newline="\n") as f:
        f.write("".join(o + "\n" for o in objs))
    partial = os.path.join(OUT, "ssbu.partial")   # (moved into the app when linked: a running copy keeps its file)
    print(f"linking {len(objs)} objects into {APP}", flush=True)
    t0 = time.time()
    frameworks = [a for fw in FRAMEWORKS for a in ("-framework", fw)]
    code, out = run([CLANG, *TARGET, "-Xlinker", "-filelist", "-Xlinker", filelist, "-o", partial, *libs,
                     "-F", SDL3_DIR, *frameworks, "-lc++", "-Wl,-rpath,@executable_path/../Frameworks",
                     "-Wl,-no_deduplicate"])
    if code:
        errors = [l for l in out.splitlines() if "error" in l.lower() or "undefined" in l.lower()] or out.splitlines()
        print("link failed\n" + "\n".join(errors)[:6000])
        return False
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    os.replace(partial, EXE)
    with open(os.path.join(OUT, "ssbu.syms"), "w", newline="\n") as f:
        r = subprocess.run([NM, "-n", "--defined-only", EXE], stdout=f, stderr=subprocess.PIPE, text=True)
    if r.returncode:
        print(f"(no ssbu.syms: the profiler names no functions)\n{r.stderr[:1000]}")
    if not bundle():
        os.remove(EXE)   # (not a finished app: the next build makes it again, and no launcher starts it)
        return False
    print(f"linked {len(objs)} objects into {EXE} ({os.path.getsize(EXE) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.isfile(os.path.join(SDL3_FRAMEWORK, "Headers", "SDL.h")):
        raise SystemExit(f"SDL3.framework is not in {SDL3_DIR}: Install WindowSmash.command downloads it "
                         "(SSBU_SDL3_DIR names another folder holding one)")
    if not modules() and "--runtime-only" not in sys.argv:
        raise SystemExit(f"no recompiled game in {os.path.join(PORT, 'build')} (tools/recomp.py all --generate-only)")
    print(f"game tree: {PORT}")
    if "--runtime-only" not in sys.argv:
        use_tree()
    if "--link-only" not in sys.argv:
        overlay_font()
        compiled = compile_all(runtime_jobs(), "runtime")
        with open(os.path.join(OBJ, "rt", "cflags.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(runtime_command())   # (a file that did not compile has no object: it is tried again)
        if not compiled:
            return False
        if "--runtime-only" in sys.argv:
            return True
        if not compile_all(game_jobs(), "game", record_compiled):
            return False
    return link()


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
