"""Host toolchain wrapper: compiles generated C with clang-cl."""
import glob
import json
import os
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

import codec_libs   # (this folder, which every importer of cc puts on sys.path)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
DEPS_LLVM = os.path.join(ROOT, "deps", "llvm")   # tools/msvc_deps.py sets these two up
DEPS_MSVC = os.path.join(ROOT, "deps", "msvc")


def _deps_record(folder):
    """A deps folder's windowsmash.json (msvc_deps.py writes it last: no record, not finished)."""
    try:
        with open(os.path.join(folder, "windowsmash.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _find_toolchain():
    """(clang-cl, the /winsysroot folder or None, which toolchain that is, in words):
    SSBU_CLANG if set; else deps\\llvm's with deps\\msvc as the system root, when tools/msvc_deps.py
    has set both up (no Visual Studio needed); else Visual Studio's own (the "C++ Clang tools"
    component), newest edition first, else a standalone LLVM install, else whatever is on PATH.
    The last three find the C runtime and the Windows SDK themselves, through Visual Studio."""
    if os.environ.get("SSBU_CLANG"):
        return os.environ["SSBU_CLANG"], None, "SSBU_CLANG"
    llvm, msvc = _deps_record(DEPS_LLVM), _deps_record(DEPS_MSVC)
    clang = os.path.join(DEPS_LLVM, "bin", "clang-cl.exe")
    if llvm is not None and msvc is not None and os.path.isfile(clang):
        return clang, DEPS_MSVC, (f"deps\\llvm (LLVM {llvm.get('version')}) with deps\\msvc (MSVC {msvc.get('msvc')}, "
                                  f"Windows SDK {msvc.get('sdk')}): no Visual Studio")
    def vs_major(path):   # the folder is a year up to 2022 (2022 = 17), a major version since (18)
        name = path.split(os.sep + "Microsoft Visual Studio" + os.sep)[1].split(os.sep)[0]
        return {"2017": 15, "2019": 16, "2022": 17}.get(name, int(name) if name.isdigit() else 0)
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        found = sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")),
                       key=vs_major, reverse=True)
        if found:
            return found[0], None, "Visual Studio's C++ Clang tools"
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),):
        for clang in glob.glob(os.path.join(root, "LLVM", "bin", "clang-cl.exe")):
            return clang, None, "LLVM's installer (Program Files), with Visual Studio's C runtime and Windows SDK"
    return "clang-cl", None, "clang-cl on the PATH"


CLANG, WINSYSROOT, TOOLCHAIN = _find_toolchain()
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math. (BASE_CFLAGS is
# also what the codec libraries are compiled with: same instruction set, same C runtime.) With
# deps\msvc, /winsysroot is where the C runtime's and the Windows SDK's headers and libraries are.
BASE_CFLAGS = ["/nologo", "/arch:AVX2", "-mfma", "-mf16c", "-fno-strict-aliasing", "/clang:-ffp-contract=off", "-w",
               *(["/winsysroot", WINSYSROOT] if WINSYSROOT else [])]
CFLAGS = [*BASE_CFLAGS, "/I", RUNTIME]
# Linking through clang-cl: with lld-link when it is next to clang-cl (else Visual Studio's link.exe),
# and LINK_OPTIONS after /link. With deps\msvc both are given the system root. clang-cl, given
# /winsysroot, hands lld-link the root's library folders (-libpath:). lld-link, given /winsysroot:,
# does not add the folders of a Visual Studio it finds on its own. Without the root, clang-cl hands
# lld-link no library folders on a PC without Visual Studio. lld-link would then add the root's
# folders only once an object file has told it the machine type, and clang-cl puts the libraries
# named after /link before the objects. So none of SYSTEM_LIBS was found (players' builds stopped
# at the link; on a PC with Visual Studio, clang-cl hands lld-link VS's folders instead, which
# hides this).
def link_flags(root):
    """(LINK_FLAGS, LINK_OPTIONS) for a system root, or for None."""
    lld = ["-fuse-ld=lld"] if os.path.exists(os.path.join(os.path.dirname(CLANG), "lld-link.exe")) else []
    return (lld + (["/winsysroot", root] if root else []), ["/winsysroot:" + root] if root else [])


LINK_FLAGS, LINK_OPTIONS = link_flags(WINSYSROOT)
# the Windows libraries the game links (all from the Windows SDK)
SYSTEM_LIBS = ("winmm.lib", "user32.lib", "gdi32.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "dxguid.lib",
               "ws2_32.lib", "ole32.lib", "oleaut32.lib")

# The audio and video codecs: libopus for the hwopus service (music) and libvpx for the video
# decode engine's VP9 (movies). Normally built from the sources under third_party/ by
# tools/codec_libs.py when link() runs. A developer can supply libraries instead: SSBU_OPUS_LIBS
# is a semicolon-separated list of Windows x64 .lib files, SSBU_VPX_DIR a folder with include/
# and lib/Win64/Release/libvpx.lib.
OPUS_LIBS = [p for p in os.environ.get("SSBU_OPUS_LIBS", "").split(";") if p]
if not all(os.path.exists(p) for p in OPUS_LIBS):
    OPUS_LIBS = []
VPX_DIR = os.environ.get("SSBU_VPX_DIR", "")
VPX_INCLUDE = os.path.join(VPX_DIR, "include")
VPX_LIBS = [os.path.join(VPX_DIR, r"lib\Win64\Release\libvpx.lib")] if VPX_DIR else []
if not all(os.path.exists(p) for p in VPX_LIBS) or not os.path.exists(os.path.join(VPX_INCLUDE, "vpx", "vpx_decoder.h")):
    VPX_LIBS = []
# (supplied libraries were built against the DLL C runtime; this program uses the static one)
EXTERNAL_LIBS = OPUS_LIBS + VPX_LIBS
BUILT_CODECS = []   # the libraries codec_libs builds for this link, from third_party/
if not OPUS_LIBS and codec_libs.have_opus():
    OPUS_LIBS, BUILT_CODECS = [codec_libs.OPUS_LIB], BUILT_CODECS + ["opus"]
if not VPX_LIBS and codec_libs.have_vpx():
    VPX_LIBS, VPX_INCLUDE, BUILT_CODECS = [codec_libs.VPX_LIB], codec_libs.VPX, BUILT_CODECS + ["libvpx"]
if OPUS_LIBS:
    CFLAGS.append("/DSSBU_HAVE_OPUS")
if VPX_LIBS:
    CFLAGS += ["/DSSBU_HAVE_VPX", "/I", VPX_INCLUDE]
# SDL3 for controllers (zlib licence): the runtime loads SDL3.dll at run time, so the build needs only the headers,
# and the link step copies the DLL beside the exe. SSBU_SDL3_DIR overrides where (include/, lib/x64/SDL3.dll).
SDL3_DIR = os.environ.get("SSBU_SDL3_DIR", os.path.join(ROOT, "deps", "SDL3"))
SDL3_DLL = os.path.join(SDL3_DIR, r"lib\x64\SDL3.dll")
if os.path.exists(os.path.join(SDL3_DIR, "include", "SDL3", "SDL_gamepad.h")) and os.path.exists(SDL3_DLL):
    CFLAGS += ["/DSSBU_HAVE_SDL3", "/I", os.path.join(SDL3_DIR, "include")]
else:
    SDL3_DLL = None
EXTRA_LIBS = OPUS_LIBS + VPX_LIBS + (["/NODEFAULTLIB:msvcrt.lib"] if EXTERNAL_LIBS else [])

# Compiler crashes. A player's build stopped on two files that clang-cl crashed on (access violations,
# one in the parser, one in the optimiser) where the very same clang-cl compiles them here: a PC short
# of memory, or one not quite stable, does that, and it rarely happens twice to the same file. So
# compile_each compiles a file the compiler crashed on again, on its own once the others are done, at
# these levels: its own, then /O1 (fewer optimiser passes; the giant functions are built at /O1 anyway).
# Never /Od: the C's huge functions would want huge stack frames. A crash is told from an error in the
# code by what clang prints (it exits with 1 either way): its crash handler's report, or LLVM's own fatal
# errors such as running out of memory; a clang-cl that crashes outright exits with the exception code.
RETRY = {"/O2": ("/O2", "/O1"), "/O1": ("/O1", "/O1")}
CRASH_SIGNS = ("PLEASE submit a bug report", "Stack dump:", "frontend command failed", "LLVM ERROR:")
# and without a bug report's files: after a crash clang would preprocess the file again, into %TEMP%
NO_CRASH_FILES = "-fno-crash-diagnostics"


def crashed(code, out):
    """Whether a failed compile (exit code, output) was the compiler crashing."""
    return code < 0 or code >= 0xC0000000 or any(sign in out for sign in CRASH_SIGNS)


def crash_cause(code, out):
    """A crash's cause in a few words, for the build's log."""
    if "LLVM ERROR: out of memory" in out:
        return "out of memory"
    found = re.search(r"Exception Code: (0x[0-9A-Fa-f]+)", out)
    return f"exception {found.group(1)}" if found else f"exception 0x{code:08X}" if code >= 0xC0000000 else "a crash"


def crash_report(out):
    """A crash's output without the compiler's command line and its raw stack frames."""
    return "\n".join(line for line in out.splitlines() if line.strip() and not re.match(r"\s*#\d+ 0x", line)
                     and "Program arguments:" not in line and not line.startswith("PLEASE submit"))


def compile_one(src, obj, opt="/O2"):
    r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"compile failed: {src}\n{(r.stdout + r.stderr)[:4000]}")
    return obj


def compile_many(sources, jobs=16, opt="/O2"):
    """Compile each source next to itself; returns the object paths."""
    with ThreadPoolExecutor(jobs) as pool:
        return list(pool.map(lambda s: compile_one(s, os.path.splitext(s)[0] + ".obj", opt), sources))


def _compile(src, obj, opt):
    r = subprocess.run([CLANG, "/c", opt, *CFLAGS, NO_CRASH_FILES, src, "/Fo" + obj], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def compile_each(jobs_list, jobs=16, started=None, finished=None, log=print):
    """Compile (src, obj, opt) triples in parallel, in order; returns [(src, error text)] for
    failures. started(job) and finished(job, seconds), if given, are called (from the worker
    threads) as each one starts and ends. A file the compiler crashed on is compiled again once
    the others are done, on its own (RETRY); log() says so."""
    def one(job):
        src, obj, opt = job
        if started:
            started(job)
        t0 = time.time()
        code, out = _compile(src, obj, opt)
        if finished:
            finished(job, time.time() - t0)
        return (job, code, out) if code else None
    with ThreadPoolExecutor(jobs) as pool:
        failed = [f for f in pool.map(one, jobs_list) if f]
    return [f for f in (_compile_again(*f, log) for f in failed) if f]


def _compile_again(job, code, out, log):
    """A failed compile's (src, error text), or None if it was a crash and the file compiles now."""
    src, obj, opt = job
    name = os.path.basename(src)
    if not crashed(code, out):
        return src, out
    for again in RETRY.get(opt, (opt, opt)):
        log(f"the compiler crashed on {name} ({crash_cause(code, out)}): compiling it again on its own"
            + (f", at {again}" if again != opt else ""))
        code, out = _compile(src, obj, again)
        if not code:
            log(f"{name} compiled" + (f" (at {again}: a little less optimised, works the same)" if again != opt else ""))
            return None
        if not crashed(code, out):
            return src, out
    return src, (f"the compiler (clang-cl) crashed on this file {1 + len(RETRY.get(opt, (opt, opt)))} times. The same "
                 "file compiles on other PCs with the same compiler, so this PC most likely ran out of memory or is not "
                 "quite stable (overclocked, or overheating). Close other programs and run the build again: it carries "
                 "on where it stopped.\n" + crash_report(out))


def link_command(rsp, exe, options=(), libs=()):
    """clang-cl's command that links the objects listed in the response file `rsp` into `exe`, with
    SYSTEM_LIBS and `libs`; `options` go to the linker. (build_game.py's compiler check links its
    test program with it too, so that the check tries what the build does.)"""
    return [CLANG, "/nologo", *LINK_FLAGS, "@" + rsp, "/Fe" + exe, "/link", *LINK_OPTIONS, *options, *SYSTEM_LIBS, *libs]


def link(objs, exe, options=()):
    """Link objects into an executable; `options` go to the linker. The codec libraries built
    from third_party/ are made first if they are missing or stale (a few seconds)."""
    if BUILT_CODECS:
        codec_libs.build(BUILT_CODECS, log=lambda m: print(m, flush=True))
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    r = subprocess.run(link_command(rsp, exe, ["/HIGHENTROPYVA:NO", "/MAP:" + exe[:-4] + ".map", *options], EXTRA_LIBS),
                       capture_output=True, text=True)
    if r.returncode:
        lines = (r.stdout + r.stderr).splitlines()
        errors = [l for l in lines if "error" in l.lower()] or lines   # (the third-party libraries' CRT warnings drown them)
        raise RuntimeError("link failed\n" + "\n".join(errors)[:4000])
    return exe
