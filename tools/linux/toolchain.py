"""Toolchain for the Linux build: clang, targeting x86_64-linux-gnu.

On Windows it cross-compiles, with the Visual Studio clang and an Ubuntu 22.04 sysroot. On Linux
it builds natively, with the system's clang and lld and its headers and libraries
(install-windowsmash.sh installs them).

Everything third-party lives outside the repository: in this checkout's deps/linux-toolchain
(where install-windowsmash.sh puts it), else in linux-toolchain/ next to the checkout
(SSBU_LINUX_TOOLCHAIN overrides; docs/LINUX.md says where each part comes from):
  sysroot/        (cross-compiling only) Ubuntu 22.04 packages (glibc 2.35, libgcc, libstdc++,
                  SDL2, libopus, libvpx)
  dxvk-native/    DXVK's native Linux libraries (Direct3D 11 on Vulkan)
  dxvk-headers/   their headers (DXVK's include/native and mingw-directx-headers)
  src/vkd3d-*/    vkd3d source; build-vkd3d/ holds its generated parsers and libraries
Cross-compiled, linking against glibc 2.35 keeps the program runnable on Ubuntu 22.04 and
anything newer."""
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RUNTIME = os.path.join(REPO, "runtime")
HOST_LINUX = sys.platform.startswith("linux")   # building on Linux itself, not cross-compiling on Windows
EXE = "" if HOST_LINUX else ".exe"
OWN_TOOLCHAIN = os.path.join(REPO, "deps", "linux-toolchain")
TOOLCHAIN = os.environ.get("SSBU_LINUX_TOOLCHAIN") or (
    OWN_TOOLCHAIN if os.path.isdir(OWN_TOOLCHAIN) else os.path.join(os.path.dirname(REPO), "linux-toolchain"))
SYSROOT = "/" if HOST_LINUX else os.path.join(TOOLCHAIN, "sysroot")
DXVK_LIBS = os.path.join(TOOLCHAIN, "dxvk-native", "usr", "lib")
DXVK_INCLUDE = os.path.join(TOOLCHAIN, "dxvk-headers", "include", "native")
VKD3D_SRC = sorted(glob.glob(os.path.join(TOOLCHAIN, "src", "vkd3d-*")))[-1:] or [""]
VKD3D_SRC = VKD3D_SRC[0]
VKD3D_BUILD = os.path.join(TOOLCHAIN, "build-vkd3d")

# The game tree (dump/, build/, save/) a Linux build is made from unless --port names another:
# this checkout when it has game data of its own (dump/), otherwise a
# native-port checkout next to it (the Linux work in a worktree of its own). The build's folder
# and the package's dump/ record theirs in port.txt, so the two never mix trees.
PORT_DIR = REPO if os.path.isdir(os.path.join(REPO, "dump")) else os.path.join(os.path.dirname(REPO), "native-port")


def default_port():
    return PORT_DIR


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


LLVM_TOOLS = ("clang", "ld.lld", "llvm-ar", "llvm-nm")


def llvm_version(folder):
    """The LLVM major version in a folder's name (/usr/lib/llvm-18/bin: 18), else 0."""
    m = re.search(r"llvm-?(\d+)", folder)
    return int(m.group(1)) if m else 0


def pick_llvm_bin(candidates, has, version_of=llvm_version):
    """Of the folders that have every tool LLVM_TOOLS names (has(folder, tool)), the one with the
    newest LLVM (version_of(folder); the first of equals), or None. (Debian's newest is a
    /usr/lib/llvm-<n>; Arch's and Fedora's is /usr/bin, their numbered ones older copies.)"""
    best, best_version = None, -1
    for d in candidates:
        if d and all(has(d, t) for t in LLVM_TOOLS):
            v = version_of(d)
            if v > best_version:
                best, best_version = d, v
    return best


def clang_major(folder):
    """The clang major version of a folder's clang (its name says it, or clang -dumpversion), else 0."""
    v = llvm_version(folder)
    if v:
        return v
    try:
        out = subprocess.run([os.path.join(folder, "clang"), "-dumpversion"], capture_output=True, text=True, timeout=30).stdout
        return int(out.strip().split(".")[0])
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return 0


def _find_llvm():
    if os.environ.get("SSBU_LLVM_BIN"):
        return os.environ["SSBU_LLVM_BIN"]
    if HOST_LINUX:
        # the newest of the distribution's versioned LLVMs (Debian, Ubuntu: /usr/lib/llvm-<n>/bin;
        # Fedora, Arch: older ones in /usr/lib64/llvm<n>/bin, /usr/lib/llvm<n>/bin) and the folders on PATH
        versioned = sorted(set(glob.glob("/usr/lib/llvm-*/bin") + glob.glob("/usr/lib/llvm[0-9]*/bin") +
                               glob.glob("/usr/lib64/llvm*/bin")), key=llvm_version, reverse=True)
        found = pick_llvm_bin(versioned + os.environ.get("PATH", "").split(os.pathsep),
                              lambda d, t: os.access(os.path.join(d, t), os.X_OK), clang_major)
        if not found:
            raise SystemExit("clang, ld.lld, llvm-ar and llvm-nm were not found: install clang, lld and llvm "
                             "(install-windowsmash.sh does), or name their folder in SSBU_LLVM_BIN")
        return found
    roots = [os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")]
    found = []
    for root in roots:
        found += sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin")), reverse=True)
    found += [os.path.join(roots[0], "LLVM", "bin")]
    for d in found:
        if os.path.exists(os.path.join(d, "clang.exe")) and os.path.exists(os.path.join(d, "ld.lld.exe")):
            return d
    raise SystemExit("no clang + ld.lld found (set SSBU_LLVM_BIN)")


LLVM = _find_llvm()
CLANG = os.path.join(LLVM, "clang" + EXE)
LLVM_AR = os.path.join(LLVM, "llvm-ar" + EXE)
LLVM_NM = os.path.join(LLVM, "llvm-nm" + EXE)
# (on Linux, the system's own target and headers)
TARGET = [] if HOST_LINUX else ["--target=x86_64-linux-gnu", "--sysroot=" + SYSROOT]
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math (as on Windows).
# -fno-pic: the program is linked at a fixed address (non-PIE); its code is far over the size
# where PIC's extra indirections are free.
ARCH = ["-mavx2", "-mfma", "-mf16c", "-mbmi", "-mbmi2", "-mlzcnt", "-mpopcnt"]
CFLAGS = TARGET + ARCH + ["-fno-strict-aliasing", "-ffp-contract=off", "-fno-pic", "-fno-plt", "-w",
                          "-fasynchronous-unwind-tables", "-I", RUNTIME]


# (below normal priority: a build here should not get in the way of anything else on the machine)
_PRIORITY = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)


def run(cmd):
    if HOST_LINUX:
        r = subprocess.run(["nice", "-n", "10", *cmd], capture_output=True, text=True)
    else:
        r = subprocess.run(cmd, capture_output=True, text=True, creationflags=_PRIORITY)
    return r.returncode, r.stdout + r.stderr


def compile_each(jobs_list, jobs=16, extra=()):
    """Compile (src, obj, opt[, flags]) jobs in parallel; returns [(src, error text)] for failures."""
    def one(job):
        src, obj, opt = job[:3]
        flags = job[3] if len(job) > 3 else []
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        lang = ["-x", "c++", "-std=gnu++20", "-fexceptions"] if src.endswith((".cpp", ".cc")) or "c++" in flags else []
        flags = [f for f in flags if f != "c++"]
        code, out = run([CLANG, "-c", opt, *CFLAGS, *lang, *extra, *flags, src, "-o", obj])
        return (src, out) if code else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]
