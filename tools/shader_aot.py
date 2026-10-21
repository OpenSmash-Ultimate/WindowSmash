#!/usr/bin/env python3
"""Compile the game's shaders ahead of time, into the renderer's disk cache.

Usage:
  shader_aot.py [--jobs N] [--dry]

The game's GPU programs are stored in data.arc as they are uploaded: in the effect files
(.eff), the shader databases (.nushdb) and nested archives (.arc); a few more are built into the
game's main executable (dump/modules/main.reloc.bin). This finds every program there (by its
header word: 256-byte boundaries + 0x30 in effects and archives, 8-byte ones in databases - half
of a database's programs start 8 bytes past a 16-byte one - and 16-byte ones in the executables),
keeps one copy of each, pairs each pixel program with the vertex programs of its file (a vertex
program's outputs are its pixel program's inputs), and has tools/shaderaot.c translate and
compile them all into dump/shadercache, exactly as the renderer would: a program's first
use in the game then loads its DXBC instead of compiling it.

The files are dated 2001-01-01 until the game first uses them, so the start's prewarm only
makes the shaders the game has used. Programs with indirect branches are left out (their key
depends on a draw's constant buffers), as are compute programs (on their launch's size).
--dry translates without compiling and compares the keys with the cache that is there.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import threading
import time
try:
    from compression.zstd import decompress as zstd_decompress   # Python 3.14
except ImportError:                                               # 3.10 to 3.13: the zstandard package
    import zstandard

    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arc as A    # noqa: E402
from progress import bar, time_left   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINUX = sys.platform.startswith("linux")   # (compiled with vkd3d-shader there, with fxc on Windows)
LOW40 = (1 << 40) - 1
MAX_PROGRAM = 0x40000   # as the renderer reads


def is_header(w):
    """A vertex (type 1, shader type 1) or pixel (type 2, shader type 5) program's first word:
    header version 3, SASS version 3."""
    t, v, st, sass = w & 0x1F, (w >> 5) & 0x1F, (w >> 10) & 0xF, (w >> 17) & 0xF
    return v == 3 and sass == 3 and ((t == 1 and st == 1) or (t == 2 and st == 5))


def containers(arc):
    """(extension, (offset, stored size, size, flags)) of every distinct .eff/.nushdb/.arc."""
    want = {A.hash40(e): e for e in ("eff", "nushdb", "arc")}
    seen, out = set(), []
    off, size, count = arc.tab["paths"]
    for i in range(count):
        full, ext = struct.unpack_from("<QQ", arc.t, off + i * size)
        e = want.get(ext & LOW40)
        if not e:
            continue
        try:
            info = arc._entry("info_indices", full >> 40, "<2I")[1]
            where = arc._data(arc._entry("infos", info, "<4I")[2])
        except struct.error:
            continue
        if where and where[0] not in seen:
            seen.add(where[0])
            out.append((e, where))
    return out


READ_TRIES = 3          # a read the disk fails is tried this many times (a flaky SD card can manage the next)
READ_GIVE_UP = 10       # this many files running that the disk can't read: it has stopped answering


class DiskGone(Exception):
    """The disk fails every read (taken out, unmounted, failing): no point going on."""


def read_retried(arc, off, size, pause=0.5):
    """arc.read, tried again after a disk's read error; the error once every try failed."""
    for attempt in range(READ_TRIES):
        try:
            return arc.read(off, size)
        except OSError:
            if attempt == READ_TRIES - 1:
                raise
            time.sleep(pause)


# the first two bytes of every word is_header takes (the second's top bits are flags: MRT, kills pixels)
HEADER_START = re.compile(rb"\x61[\x04\x44\x84\xc4]|\x62[\x14\x54\x94\xd4]")


def add_file(programs, ids, data, step, start):
    """The programs in one file's bytes (at start + a multiple of step) into programs/ids, each
    pixel program paired with every vertex program of the file: the one before it is right for
    only about 60 % of the cache's pairs, the file's for about 95 % (the game has only a few dozen
    distinct vertex output masks, so this stays small)."""
    mv = memoryview(data)
    at = [k for k in (m.start() for m in HEADER_START.finditer(data))
          if k >= start and (k - start) % step == 0 and k < len(data) - 0x60 and is_header(int.from_bytes(mv[k:k + 4], "little"))]
    here = ([], [])
    for j, k in enumerate(at):
        end = min(len(data), k + MAX_PROGRAM, at[j + 1] if j + 1 < len(at) else len(data))
        blob = bytes(mv[k:end])
        stage = 0 if blob[0] & 0x1F == 1 else 1
        key = hashlib.blake2b(blob, digest_size=16).digest()
        pid = ids.get(key)
        if pid is None:
            pid = ids[key] = len(programs)
            programs.append([stage, blob, set()])
        here[stage].append(pid)
    for pid in here[1]:
        programs[pid][2].update(here[0])


MODULES = ("main", "sdk", "subsdk0")


def module_images(tree):
    """The game's executables as loaded (<tree>/dump/modules/<name>.reloc.bin) that are there: some
    programs are built into main (the menus', the title's, the screen's own passes)."""
    paths = [os.path.join(tree, "dump", "modules", name + ".reloc.bin") for name in MODULES]
    return [p for p in paths if os.path.isfile(p)]


def find_programs(arc, read_errors=None, pause=0.5, modules=()):
    """Distinct programs: list of [stage, bytes, set of paired vertex program ids], and how many
    files could not be read. A file the disk can't read is left out (its shaders compile when the
    game first uses them, as without this tool); read_errors, a list, gets each such error.
    modules: executables' images, scanned too (their programs on 16-byte boundaries; one that
    isn't one - no end within MAX_PROGRAM - shaderaot leaves out)."""
    programs, ids = [], {}
    unread, failing = 0, 0
    for path in modules:
        with open(path, "rb") as f:
            add_file(programs, ids, f.read(), 16, 0)
    for ext, (off, stored, size, flags) in containers(arc):
        if not stored or not size:   # (an empty file - data.arc has a few, marked compressed: nothing in it)
            continue
        try:
            data = read_retried(arc, off, stored, pause)
            failing = 0
        except OSError as e:
            unread += 1
            failing += 1
            if read_errors is not None:
                read_errors.append(e)
            if failing >= READ_GIVE_UP:
                raise DiskGone(e) from e
            continue
        if flags & 3:
            try:
                data = zstd_decompress(data)
            except Exception:
                unread += 1
                continue
        step, start = (8, 0) if ext == "nushdb" else (256, 0x30)
        add_file(programs, ids, data, step, start)
    return programs, unread


def read_error_text(errors, tree=None):
    """What to say about the files the disk could not read."""
    files = list(dict.fromkeys(e.filename for e in errors if getattr(e, "filename", None)))
    why = errors[0].strerror or str(errors[0])
    lines = [f"The disk could not read {len(errors)} of the game's files ({why}"
             + (f", in {', '.join(files)}" if files else "") + "). Their shaders are left out: they compile when the game "
             "first uses them, as they would without this. The rest go ahead.",
             "A read error comes from the disk itself: most often an SD card or drive that is failing or not seated "
             "well, or a damaged copy of that file. The game can stop with the same error when it needs those files."]
    dump = os.path.join(os.path.abspath(tree or ROOT), "dump") + os.sep
    if any(os.path.abspath(f).startswith(dump) for f in files):
        lines.append("That file is a copy WindowSmash made: delete it and run the installer again to make it afresh.")
    if LINUX:
        lines.append("The system's own record of the error: sudo dmesg | grep -i error")
    return "\n".join(lines)


def write_pack(programs, path):
    with open(path, "wb") as f:
        f.write(b"SAOT" + struct.pack("<I", len(programs)))
        for stage, blob, pairs in programs:
            pairs = sorted(pairs)
            f.write(struct.pack(f"<3I{len(pairs)}I", stage, len(blob), len(pairs), *pairs))
            f.write(blob)


def build_tool(exe):
    sources = [os.path.join(ROOT, "tools", "shaderaot.c"), os.path.join(ROOT, "runtime", "gpu_shader.c")]
    headers = [os.path.join(ROOT, "runtime", n) for n in ("host.h", "gpu_shader.h", "gpu_shader_cache.h")]
    if LINUX:
        sources.append(os.path.join(ROOT, "runtime", "linux", "d3dcompile_vkd3d.c"))
    if os.path.exists(exe) and os.path.getmtime(exe) > max(os.path.getmtime(p) for p in sources + headers):
        return
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    if LINUX:
        build_tool_linux(exe, sources)
        return
    import cc
    r = subprocess.run([cc.CLANG, "/nologo", "/O2", *cc.LINK_FLAGS, *cc.CFLAGS, *sources, "/Fe" + exe, "/link", *cc.LINK_OPTIONS, "d3dcompiler.lib"],
                       capture_output=True, text=True, cwd=os.path.dirname(exe))
    if r.returncode:
        raise SystemExit("cannot build shaderaot:\n" + (r.stdout + r.stderr)[-3000:])


def build_tool_linux(exe, sources):
    """On Linux the renderer's D3DCompile is vkd3d-shader (runtime/linux/d3dcompile_vkd3d.c): the tool
    is linked with it, so that it makes the same DXBC the game would."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "linux"))
    import toolchain as T
    vkd3d = os.path.join(T.VKD3D_BUILD, "linux", "libvkd3d-shader.a")
    if not os.path.isfile(vkd3d):
        print("the shader compiler (vkd3d-shader), once:", flush=True)
        if subprocess.run([sys.executable, os.path.join(ROOT, "tools", "linux", "build_vkd3d.py"), "linux"]).returncode:
            raise SystemExit("vkd3d-shader could not be built (see above)")
    includes = ["-I", os.path.join(ROOT, "runtime", "linux", "include"), "-I", os.path.join(T.DXVK_INCLUDE, "windows"),
                "-I", os.path.join(T.DXVK_INCLUDE, "directx"), "-I", os.path.join(T.SYSROOT, "usr", "include", "SDL2"),
                "-I", os.path.join(T.VKD3D_SRC, "include")]
    r = subprocess.run([T.CLANG, "-O2", *T.CFLAGS, "-D_GNU_SOURCE", "-D_REENTRANT", *includes, *sources, vkd3d, "-fuse-ld=lld",
                        "-no-pie", "-lm", "-lpthread", "-ldl", "-o", exe], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("cannot build shaderaot:\n" + (r.stdout + r.stderr)[-3000:])


def default_jobs():
    """One process per core but two (the PC stays usable), and as many as the RAM holds: each
    reads the whole pack of programs (about 0.4 GB) and translates them."""
    jobs = max(1, (os.cpu_count() or 2) - 2)
    if LINUX:
        try:
            gb = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
            jobs = min(jobs, max(1, int(gb // 2)))
        except (ValueError, OSError):
            pass
    return jobs


def low_priority():
    """Popen's arguments that run the compile below normal priority."""
    if LINUX:
        return {"preexec_fn": lambda: os.nice(10)}
    return {"creationflags": getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int, default=default_jobs())
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    tree = ROOT
    t0 = time.time()
    arc = A.Arc(os.path.join(tree, "dump", "modules", "romfs.cfg"))
    read_errors = []
    try:
        programs, unread = find_programs(arc, read_errors, modules=module_images(tree))
    except DiskGone as e:
        raise SystemExit(f"\nThe disk stopped answering: {READ_GIVE_UP} of the game's files running could not be read "
                         f"({e}). Check that it is still there (an SD card that came loose, a drive that went to sleep), "
                         "then run this again: what is done stays done.")
    nvs = sum(1 for p in programs if p[0] == 0)
    print(f"{len(programs)} distinct programs ({nvs} vertex, {len(programs) - nvs} pixel) in {time.time() - t0:.0f} s"
          + (f"; {unread} files could not be read" if unread else ""), flush=True)
    if read_errors:
        print(read_error_text(read_errors, tree), flush=True)
    work = os.path.join(tree, "dump", "aot")
    os.makedirs(work, exist_ok=True)
    pack = os.path.join(work, "programs.pack")
    write_pack(programs, pack)
    exe = os.path.join(ROOT, "build-linux", "shaderaot") if LINUX else os.path.join(ROOT, "build", "shaderaot.exe")
    build_tool(exe)
    cache = os.path.join(tree, "dump", "shadercache")
    os.makedirs(cache, exist_ok=True)
    jobs = 1 if a.dry else a.jobs
    procs = [subprocess.Popen([exe, pack, cache, str(i), str(jobs)] + (["--dry"] if a.dry else []),
                              stdout=subprocess.PIPE if a.dry else subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                              **low_priority())
             for i in range(jobs)]
    if a.dry:
        outs = [p.communicate() for p in procs]
        print("\n".join(line for _out, err in outs for line in err.splitlines() if line.startswith("part ")))
    else:
        print(f"compiling with {jobs} processes at low priority (the PC stays usable; stop it with " + ("Ctrl+C" if LINUX else "closing this window") + ": "
              "what is done stays done)", flush=True)
        parts = follow(procs, Progress(sys.stdout))
        print(summary(parts) or "\n".join(parts))
    if a.dry:
        keys = {tuple(l.split()) for l in outs[0][0].splitlines()}
        have = set()
        for name in os.listdir(cache):
            parts = name[:-5].split("_") if name.endswith(".dxbc") else []
            if len(parts) == 5 and parts[0] == "v" + str(cache_version()) and parts[1] in ("vs", "ps"):
                have.add(("vs", parts[2]) if parts[1] == "vs" else ("ps", parts[2], parts[3]))
        print(f"the cache has {len(have)} vertex/pixel keys; {len(have & keys)} of them are among the {len(keys)} found here")
    print(f"done in {time.time() - t0:.0f} s")


# ---- progress: shaderaot's "progress <done> <of>" lines, from every part, as one bar ------------
def parse_progress(line):
    """(done, of) from a "progress <done> <of>" line, else None."""
    p = line.split()
    if len(p) == 3 and p[0] == "progress" and p[1].isdigit() and p[2].isdigit():
        return int(p[1]), int(p[2])
    return None


def progress_text(done, of, rate=None, width=30):
    """The bar, the share done, and the time left at `rate` shaders a second (once there is one)."""
    frac = min(1.0, done / of) if of else 1.0
    text = f"{bar(frac, width)} {frac:4.0%}  {done:,} of {of:,} shaders"
    if rate and 0 < done < of:
        text += ", " + time_left((of - done) / rate)
    return text


class Progress:
    """Shows the parts' combined progress: one line drawn over itself in a console, else a line
    every 5 % (for a log)."""

    WINDOW = 120   # seconds of recent pace the time left goes by (the pace picks up after the start)

    def __init__(self, out, console=None):
        self.out = out
        self.console = out.isatty() if console is None else console
        self.samples = []   # (time, shaders done), once every part has said how much it has
        self.ready_said = False
        self.shown = -1     # (the last 5 % step a log line was written for)
        self.last = None
        self.width = 0

    def rate(self, now, done):
        """Shaders a second over the last WINDOW seconds, once there are 10 seconds of them."""
        self.samples.append((now, done))
        while len(self.samples) > 2 and self.samples[1][0] <= now - self.WINDOW:
            self.samples.pop(0)
        t0, d0 = self.samples[0]
        return (done - d0) / (now - t0) if now - t0 >= 10 and done > d0 else None

    def __call__(self, state, now=None, final=False):
        now = time.time() if now is None else now
        if any(of is None for _done, of in state):
            if not self.ready_said:
                self.out.write("getting ready: translating the vertex programs...\n")
                self.out.flush()
                self.ready_said = True
            return
        done, of = sum(d for d, _o in state), sum(o for _d, o in state)
        text = progress_text(done, of, self.rate(now, done))
        if self.console:
            self.out.write("\r" + text.ljust(self.width) + ("\n" if final else ""))
            self.width = len(text)
            self.out.flush()
        else:
            step = int(100 * done / of) // 5 if of else 20
            if step > self.shown or (final and text != self.last):
                self.out.write(text + "\n")
                self.out.flush()
                self.shown, self.last = step, text


def follow(procs, show, every=1.0):
    """Waits for the parts, showing their progress as it comes; returns their last lines ("part ...")."""
    state = [(0, None)] * len(procs)
    lasts = []

    def read(i, p):
        for line in p.stderr:
            got = parse_progress(line)
            if got:
                state[i] = got
            elif line.startswith("part "):
                lasts.append(line.rstrip())
    readers = [threading.Thread(target=read, args=(i, p), daemon=True) for i, p in enumerate(procs)]
    for r in readers:
        r.start()
    while any(r.is_alive() for r in readers):
        show(state)
        time.sleep(every)
    for p in procs:
        p.wait()
    show([(d, o if o is not None else d) for d, o in state], final=True)
    return lasts


def summary(lines):
    """The parts' last lines added up into one, or None if they don't read as expected."""
    fields = ("keys", "compiled", "already cached", "with jump tables", "fxc failures", "not translated")
    total = dict.fromkeys(fields, 0)
    for line in lines:
        m = re.search(r": (\d+) keys; (\d+) compiled, (\d+) already cached, (\d+) with jump tables \(left out\), "
                      r"(\d+) fxc failures, (\d+) not translated", line)
        if not m:
            return None
        for k, v in zip(fields, m.groups()):
            total[k] += int(v)
    return (f"{total['keys']:,} shaders: {total['compiled']:,} compiled, {total['already cached']:,} already there, "
            f"{total['with jump tables']:,} left out (jump tables), {total['fxc failures']:,} that fxc could not compile, "
            f"{total['not translated']:,} not translated")


def cache_version():
    text = open(os.path.join(ROOT, "runtime", "gpu_shader_cache.h"), encoding="utf-8").read()
    return int(re.search(r"#define SHADER_CACHE_VERSION (\d+)", text).group(1))


if __name__ == "__main__":
    main()
