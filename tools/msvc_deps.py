#!/usr/bin/env python3
"""The compiler without Visual Studio: deps\\llvm and deps\\msvc, which tools/cc.py prefers.

Usage:
  msvc_deps.py install [--accept-license] [--keep-downloads]
  msvc_deps.py plan        say what install would download and from where (reads Microsoft's list)
  msvc_deps.py status      say what is set up here
  msvc_deps.py remove      delete deps\\llvm and deps\\msvc (cc.py then looks for Visual Studio again)

deps\\llvm  clang-cl, lld-link and llvm-lib with clang's own headers, from LLVM's official release
           on GitHub: the version pinned below, checked against its SHA-256. LLVM is free software
           (Apache License 2.0 with LLVM exceptions).
deps\\msvc  Microsoft's C runtime (the static one the game links: libcmt, libvcruntime, oldnames,
           with its headers) and the Windows SDK's headers and x64 libraries (um, ucrt, shared),
           laid out as clang-cl's /winsysroot expects them:
             VC\\Tools\\MSVC\\<version>\\include, lib\\x64
             Windows Kits\\10\\Include\\<version>\\ucrt, um, shared; Lib\\<version>\\ucrt\\x64, um\\x64
           They are Microsoft's software. They come from Microsoft's own servers: the packages its
           Visual Studio installer uses, as Visual Studio's channel list (CHANNEL_URL) names them,
           each checked against the SHA-256 the list gives. Nothing of them is downloaded before you
           accept Microsoft's license terms (the Visual Studio Build Tools', at the address the list
           gives; only the list itself is read before that). The SDK's .msi files are unpacked with
           msiexec /a, an administrative install: files only, no administrator rights, nothing
           registered with Windows. WindowSmash does not include any of these files.
Neither needs administrator rights. Each folder's windowsmash.json says what it holds: versions,
where from, download and unpacked sizes; it is written last, so a folder without it is unfinished
and cc.py does not use it. Downloads go to deps\\downloads and are deleted afterwards: LLVM's
archive and Microsoft's packages at the same time, those DOWNLOAD_JOBS files at a time.

The setup is part of the build: Install WindowSmash.bat runs this (step 4) when no compiler works,
and tools/build_game.py (launchers\\Build WindowSmash.bat) asks agree() at its first step and runs
install() in the background while it prepares the game files.

Exit status: 0 done, 1 failed, 2 Microsoft's license was declined (nothing was downloaded).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)   # (for cc's test build, also under a Python that does not add it)
ROOT = os.path.dirname(TOOLS)
DEPS = os.path.join(ROOT, "deps")
LLVM_DIR = os.path.join(DEPS, "llvm")
MSVC_DIR = os.path.join(DEPS, "msvc")
DOWNLOADS = os.path.join(DEPS, "downloads")
RECORD = "windowsmash.json"   # in each folder, written last (cc.py reads it too)
USER_AGENT = "WindowSmash-msvc-deps"
DOWNLOAD_JOBS = 6

# ---- LLVM: one pinned release ------------------------------------------------------------------
LLVM_VERSION = "23.1.3"
LLVM_ASSET = f"clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.zst"
LLVM_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/{LLVM_ASSET}"
LLVM_SHA256 = "7e69dc07a4536b79ff24a651a2611990bf8eaab17ea560f1092707c4f31ba01d"   # (the release's listed digest)
LLVM_SIZE = 276320456
# what the build runs: the compiler, the linker, and the librarian codec_libs.py makes the codec
# libraries with; none needs a DLL of LLVM's (they import only Windows' own)
LLVM_PROGRAMS = ("bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-lib.exe")
LLVM_KEEP = re.compile(r"(bin/(clang-cl|lld-link|llvm-lib)\.exe"
                       r"|lib/clang/\d+/include/.+"                                  # its intrinsics headers
                       r"|lib/clang/\d+/lib/windows/clang_rt\.builtins-x86_64\.lib)")  # its helper functions

# ---- Microsoft: Visual Studio's channel list ----------------------------------------------------
CHANNEL_URL = "https://aka.ms/vs/18/stable/channel"   # Visual Studio 2026, the Build Tools the installer's other route installs
MANIFEST_ID = "Microsoft.VisualStudio.Manifests.VisualStudio"
LICENSE_PRODUCT = "Microsoft.VisualStudio.Product.BuildTools"
VC_TOOLS = "Microsoft.VisualCpp.Tools.HostX64.TargetX64"   # its dependency names the toolset version the Build Tools install
VC_PACKAGES = ("Microsoft.VC.{v}.CRT.Headers.base", "Microsoft.VC.{v}.CRT.x64.Desktop.base",
               "Microsoft.VC.{v}.CRT.x64.Store.base")   # (the last only for lib\x64\oldnames.lib, which the others lack)
VC_LIBS = ("libcmt.lib", "libvcruntime.lib", "oldnames.lib", "legacy_stdio_definitions.lib",
           "legacy_stdio_wide_specifiers.lib")   # (of the CRT package's hundreds of MB: the static release C runtime)
SDK_WORKLOAD = "Microsoft.VisualStudio.Workload.VCTools"   # its recommended Windows SDK is the one the Build Tools install
SDK_MSIS = (   # (what each holds: measured on SDK 10.0.26100's, each unpacked with msiexec /a)
    "Windows SDK Desktop Headers x86-x86_en-us.msi",                      # most of um\ (windows.h, d3d11.h, ...)
    "Windows SDK Desktop Headers x64-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",           # the rest of um\ (d3dcompiler.h, ...)
    "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",  # shared\ (basetsd.h, dxgi.h, ...)
    "Windows SDK OnecoreUap Headers x86-x86_en-us.msi",                   # more of shared\
    "Windows SDK OnecoreUap Headers x64-x86_en-us.msi",
    "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",              # kernel32, d3d11, dxgi, d3dcompiler, ...
    "Windows SDK Desktop Libs x64-x86_en-us.msi",                         # user32, gdi32, winmm, ...
    "Universal CRT Headers Libraries and Sources-x86_en-us.msi",          # the C library itself (ucrt)
)
SDK_KEEP = re.compile(r"Windows Kits/10/(Include/[^/]+/(ucrt|um|shared)/.+|Lib/[^/]+/um/x64/.+"
                      r"|Lib/[^/]+/ucrt/x64/(libucrt|ucrt)\.lib)", re.IGNORECASE)
# what a build needs from them, checked once they are unpacked (cc.link's libraries among them)
NEEDED_HEADERS = ("include/vcruntime.h", "include/stdarg.h")
NEEDED_SDK_HEADERS = ("ucrt/stdio.h", "um/windows.h", "um/d3d11.h", "um/d3dcompiler.h", "shared/dxgi.h", "um/winsock2.h")

LICENSE_NOTICE = """\
Microsoft's C runtime and Windows SDK are Microsoft's software. WindowSmash does not include them:
it downloads them from Microsoft's servers onto this PC, to build the game here, and only if you
accept Microsoft's license terms for them (those of the Visual Studio Build Tools):
    {url}
Read them there first. Do you accept Microsoft's license terms?"""


class Failed(Exception):
    """What went wrong, said for the person running it."""


def log(msg=""):
    print(msg, flush=True)


# ---- what is kept, and where it goes (tested: tests/test_msvc_deps.py) -------------------------
def llvm_member(name):
    """Where an LLVM archive member goes under deps\\llvm ('/' separators), or None to skip it.
    The archive's top folder (clang+llvm-<version>-...) is dropped."""
    parts = name.replace("\\", "/").split("/")
    if len(parts) < 2 or any(p in ("", ".", "..") for p in parts[1:]) or ":" in name:
        return None
    rel = "/".join(parts[1:])
    return rel if LLVM_KEEP.fullmatch(rel) else None


def vc_member(name, folder):
    """Where a Visual Studio package's (.vsix, a zip) entry goes under deps\\msvc, or None: its
    Contents\\VC\\Tools\\MSVC\\<version>\\include and the VC_LIBS of lib\\x64, into the one toolset
    folder `folder` (the packages of one toolset can name slightly different build numbers)."""
    m = re.fullmatch(r"Contents/VC/Tools/MSVC/[^/]+/(include/.+|lib/x64/([^/]+))", name.replace("\\", "/"))
    if not m or ".." in m.group(1).split("/"):
        return None
    if m.group(2) is not None and m.group(2).lower() not in VC_LIBS:
        return None
    return f"VC/Tools/MSVC/{folder}/{m.group(1)}"


def vc_folder(names):
    """The toolset folder (VC\\Tools\\MSVC\\<this>) a package's entries use, or None."""
    for n in names:
        m = re.match(r"Contents/VC/Tools/MSVC/([0-9.]+)/", n.replace("\\", "/"))
        if m:
            return m.group(1)
    return None


def sdk_member(rel):
    """Whether a file msiexec unpacked (its path under the target folder, '/' separators) is kept."""
    return bool(SDK_KEEP.fullmatch(rel.replace("\\", "/")))


def channel_info(channel):
    """From Visual Studio's channel list: its version, the license's address (the Build Tools',
    English), and the package manifest's payload (url, sha256, size)."""
    items = {i.get("id"): i for i in channel.get("channelItems", [])}
    product = items.get(LICENSE_PRODUCT)
    manifest = items.get(MANIFEST_ID)
    if not product or not manifest or not manifest.get("payloads"):
        raise Failed("Microsoft's channel list does not name the Build Tools and the package manifest as expected.")
    license_url = None
    for r in product.get("localizedResources", []):
        if r.get("language", "").lower() == "en-us":
            license_url = r.get("license")
    if not license_url:
        raise Failed("Microsoft's channel list gives no license for the Build Tools.")
    return {"version": channel.get("info", {}).get("productDisplayVersion") or manifest.get("version"),
            "license": license_url, "manifest": manifest["payloads"][0]}


def index_packages(vsman):
    """The manifest's packages by id, in lower case (ids are not case-consistent), each a list of
    its entries (one per language for some)."""
    out = {}
    for p in vsman.get("packages", []):
        out.setdefault(p["id"].lower(), []).append(p)
    return out


def package(packages, pid):
    """A package's entry: the language-neutral one, else the English one; None if there is none."""
    entries = packages.get(pid.lower(), [])
    for p in entries:
        if not p.get("language") or p["language"].lower() == "neutral":
            return p
    for p in entries:
        if p["language"].lower() == "en-us":
            return p
    return None


def msvc_version(packages):
    """The toolset version the Build Tools install by default ("14.51"): the one the unversioned
    host-x64 tools package depends on; failing that, the newest versioned component."""
    tools = package(packages, VC_TOOLS)
    for dep in (tools or {}).get("dependencies", {}):
        m = re.fullmatch(r"microsoft\.vc\.([0-9.]+)\.tools\.hostx64\.targetx64\.base", dep.lower())
        if m:
            return m.group(1)
    found = []
    for pid in packages:
        m = re.fullmatch(r"microsoft\.visualstudio\.component\.vc\.(\d+\.\d+)(\.[0-9.]+)?\.x86\.x64", pid)
        if m:
            found.append(tuple(int(x) for x in m.group(1).split(".")))
    if not found:
        raise Failed("Microsoft's manifest names no C++ toolset.")
    return ".".join(map(str, max(found)))


def msvc_payloads(packages, version):
    """The CRT packages' files (headers, x64 desktop libraries) of a toolset version."""
    out = []
    for name in VC_PACKAGES:
        p = package(packages, name.format(v=version))
        if not p or not p.get("payloads"):
            raise Failed(f"Microsoft's manifest has no {name.format(v=version)}.")
        out += [dict(x, package=p["id"]) for x in p["payloads"]]
    return out


def sdk_package(packages):
    """The Windows SDK the C++ workload recommends (the Build Tools' default; else the newest):
    its package (the one with the .msi files) and version ("10.0.26100")."""
    numbers = []
    workload = package(packages, SDK_WORKLOAD) or {}
    for dep, how in workload.get("dependencies", {}).items():
        m = re.fullmatch(r"microsoft\.visualstudio\.component\.windows1[01]sdk\.(\d+)", dep.lower())
        if m and isinstance(how, dict) and how.get("type") == "Recommended":
            numbers.append(int(m.group(1)))
    if not numbers:
        for pid in packages:
            m = re.fullmatch(r"microsoft\.visualstudio\.component\.windows1[01]sdk\.(\d+)", pid)
            if m:
                numbers.append(int(m.group(1)))
    for n in sorted(numbers, reverse=True):
        for kind in ("windows11sdk", "windows10sdk"):
            component = package(packages, f"microsoft.visualstudio.component.{kind}.{n}")
            for dep in (component or {}).get("dependencies", {}):
                p = package(packages, dep)
                if p and any(x["fileName"].lower().endswith(".msi") for x in p.get("payloads", [])):
                    return p, f"10.0.{n}"
    raise Failed("Microsoft's manifest names no Windows SDK.")


def payload_name(payload):
    """A payload's file name inside its package, with '\\' separators (the manifest writes some
    SDKs' with '/', others' with '\\')."""
    return payload["fileName"].replace("/", "\\")


def sdk_payloads(sdk, names=SDK_MSIS):
    """The payloads of the SDK's .msi files named."""
    by_name = {payload_name(x).lower(): x for x in sdk.get("payloads", [])}
    out = []
    for n in names:
        p = by_name.get(("Installers\\" + n).lower())
        if not p:
            raise Failed(f"the Windows SDK package has no {n}.")
        out.append(p)
    return out


def msi_cabs(msi, sdk):
    """The SDK payloads holding the .cab files an .msi's files are in: the cabinet names in its
    bytes (its Media table) that are files of the package."""
    by_name = {payload_name(x).lower(): x for x in sdk.get("payloads", [])}
    out = {}
    for m in re.finditer(rb"[0-9a-fA-F]{32}\.cab", msi):
        p = by_name.get(("Installers\\" + m.group().decode()).lower())
        if p:
            out[payload_name(p).lower()] = p
    return [out[k] for k in sorted(out)]


# ---- downloads --------------------------------------------------------------------------------
def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode("utf-8-sig"))


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class Progress:
    """Bytes as they arrive (from several threads), shown as one line per tenth of the total."""
    def __init__(self, label, total):
        self.label, self.total, self.done, self.shown = label, total, 0, 0
        self.lock = threading.Lock()

    def __call__(self, n):
        with self.lock:
            self.done += n
            tenth = min(10, self.done * 10 // self.total) if self.total else 0
            if tenth > self.shown:
                self.shown = tenth
                log(f"    {self.label}: {tenth * 10}% of {self.total / 1e6:.0f} MB")


def download(url, dest, sha256=None, size=None, what=None, progress=None):
    """url into dest (through dest.partial), checked against its size and SHA-256 if given; a
    file already at dest with the right SHA-256 is kept. Returns the bytes downloaded. progress(n)
    is told of the bytes as they come (a kept file's all at once; a failed try's taken back)."""
    if sha256 and os.path.isfile(dest) and sha256_of(dest) == sha256.lower():
        if progress:
            progress(os.path.getsize(dest))
        return 0
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    partial = dest + ".partial"
    if what:
        log(f"  downloading {what}" + (f" ({size / 1e6:.0f} MB)" if size else ""))
        log(f"    from {url}")
    for attempt in range(1, 4):
        got = 0
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            h = hashlib.sha256()
            with urllib.request.urlopen(req, timeout=120) as r, open(partial, "wb") as f:
                while True:
                    block = r.read(1 << 20)
                    if not block:
                        break
                    f.write(block)
                    h.update(block)
                    got += len(block)
                    if progress:
                        progress(len(block))
            # (the SHA-256 decides: Microsoft's manifest gives some .vsix files a size they don't
            # have, its SHA-256 for them being right; the size is for the progress then)
            if sha256 and h.hexdigest() != sha256.lower():
                raise Failed(f"{os.path.basename(dest)} does not match its SHA-256")
            if not sha256 and size and got != size:
                raise Failed(f"{os.path.basename(dest)}: {got} bytes downloaded, {size} expected")
            os.replace(partial, dest)
            return got
        except (OSError, Failed) as e:
            if progress:
                progress(-got)
            if attempt == 3:
                raise Failed(f"could not download {url}: {e}")
            log(f"    {e}; trying again")
            time.sleep(3 * attempt)


def download_all(items, label):
    """[(payload, local path)] downloaded DOWNLOAD_JOBS at a time (some forty files, most of them a
    few MB: one at a time, each one's connection and start would add up); returns the bytes downloaded."""
    progress = Progress(label, sum(p.get("size") or 0 for p, _dest in items))
    with ThreadPoolExecutor(DOWNLOAD_JOBS) as pool:
        return sum(pool.map(lambda item: download(item[0]["url"], item[1], item[0].get("sha256"), item[0].get("size"),
                                                  progress=progress), items))


# ---- the two folders ---------------------------------------------------------------------------
def read_record(folder):
    try:
        with open(os.path.join(folder, RECORD), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_record(folder, record):
    with open(os.path.join(folder, RECORD), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1)
        f.write("\n")


def disk_bytes(folder):
    return sum(os.path.getsize(os.path.join(d, n)) for d, _dirs, files in os.walk(folder) for n in files)


def fresh(folder):
    """An empty folder (removed first if there)."""
    if os.path.exists(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)
    return folder


def put_in_place(partial, final):
    if os.path.exists(final):
        shutil.rmtree(final)
    os.replace(partial, final)


def zstd_reader(path):
    """The decompressed bytes of a .zst file. LLVM packs its release with a 1 GB window (zstd
    --long), past the 128 MB decoders allow by default; unpacking takes about that much memory."""
    try:
        from compression import zstd   # Python 3.14 and newer
        return zstd.ZstdFile(path, options={zstd.DecompressionParameter.window_log_max: 31})
    except ImportError:
        import zstandard   # requirements.txt, before 3.14
        return zstandard.ZstdDecompressor(max_window_size=1 << 31).stream_reader(open(path, "rb"), read_across_frames=True,
                                                                                  closefd=True)


def install_llvm(keep_downloads=False):
    """deps\\llvm from the pinned release, unless it is there already. Returns its record."""
    record = read_record(LLVM_DIR)
    if record and record.get("version") == LLVM_VERSION:
        log(f"  deps\\llvm: LLVM {LLVM_VERSION}, already there")
        return record
    archive = os.path.join(DOWNLOADS, LLVM_ASSET)
    got = download(LLVM_URL, archive, LLVM_SHA256, LLVM_SIZE, f"LLVM {LLVM_VERSION} (clang) from GitHub",
                   progress=Progress("LLVM", LLVM_SIZE))
    log("    SHA-256 matches the release; unpacking clang-cl, lld-link, llvm-lib and clang's headers")
    partial = fresh(LLVM_DIR + ".partial")
    count = 0
    with zstd_reader(archive) as z, tarfile.open(fileobj=z, mode="r|") as tar:
        for m in tar:
            rel = llvm_member(m.name)
            if not rel or not m.isfile():
                continue
            dst = os.path.join(partial, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with tar.extractfile(m) as src, open(dst, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
            count += 1
    missing = [p for p in LLVM_PROGRAMS if not os.path.isfile(os.path.join(partial, *p.split("/")))]
    if not os.path.isdir(os.path.join(partial, "lib", "clang")):
        missing.append("lib/clang")
    if missing:
        raise Failed(f"the LLVM archive lacks {', '.join(missing)}")
    r = subprocess.run([os.path.join(partial, "bin", "clang-cl.exe"), "--version"], capture_output=True, text=True)
    if r.returncode or f"clang version {LLVM_VERSION}" not in r.stdout:
        raise Failed("the unpacked clang-cl does not run:\n" + (r.stdout + r.stderr)[-1500:])
    record = {"what": "LLVM (clang-cl, lld-link, llvm-lib, clang's headers)", "version": LLVM_VERSION,
              "url": LLVM_URL, "sha256": LLVM_SHA256, "license": "Apache-2.0 WITH LLVM-exception",
              "download_bytes": LLVM_SIZE, "downloaded_bytes": got, "files": count, "disk_bytes": disk_bytes(partial),
              "installed": datetime.datetime.now().isoformat(timespec="seconds")}
    write_record(partial, record)
    put_in_place(partial, LLVM_DIR)
    if not keep_downloads:
        os.remove(archive)
    log(f"  deps\\llvm: LLVM {LLVM_VERSION}, {count} files, {record['disk_bytes'] / 1e6:.0f} MB")
    return record


def read_channel():
    try:
        return channel_info(fetch_json(CHANNEL_URL))
    except (OSError, ValueError) as e:
        raise Failed(f"Microsoft's Visual Studio channel list ({CHANNEL_URL}) could not be read: {e}")


def ask_license(url):
    """Shows Microsoft's license notice and asks; True when the answer is yes."""
    log(LICENSE_NOTICE.format(url=url))
    log("Type yes and press Enter to accept (anything else: no):")
    try:
        answer = sys.stdin.readline()
    except (OSError, ValueError):
        answer = ""
    return answer.strip().lower() in ("yes", "y")


def msi_log_error(logfile):
    """The first "Error NNNN. ..." line of an msiexec log (UTF-16), or ''."""
    try:
        with open(logfile, encoding="utf-16", errors="replace") as f:
            for line in f:
                if re.match(r"Error \d+\.", line):
                    return line.strip()
    except OSError:
        pass
    return ""


def run_msiexec(msi, target, logfile):
    """msiexec's administrative install of an .msi: its files into target, as they lie in it.
    One installation runs at a time on Windows (1618): wait for another for up to two minutes."""
    line = f'msiexec.exe /a "{msi}" /qn /norestart TARGETDIR="{target}" /L*v "{logfile}"'
    for attempt in range(7):
        code = subprocess.run(line).returncode
        if code == 0:
            return
        if code != 1618:
            why = msi_log_error(logfile)
            raise Failed(f"msiexec could not unpack {os.path.basename(msi)} (code {code}{': ' + why if why else ''}; its log: {logfile})")
        time.sleep(20)
    raise Failed(f"another installation kept Windows Installer busy; {os.path.basename(msi)} was not unpacked")


def install_msvc(channel, keep_downloads=False):
    """deps\\msvc from Microsoft's packages (the license was accepted). Returns its record."""
    log(f"  reading Microsoft's package manifest (Visual Studio {channel['version']})")
    packages = index_packages(fetch_json(channel["manifest"]["url"]))
    version = msvc_version(packages)
    vc = msvc_payloads(packages, version)
    sdk, sdk_version = sdk_package(packages)
    msis = sdk_payloads(sdk)
    partial = fresh(MSVC_DIR + ".partial")
    work = fresh(os.path.join(DOWNLOADS, "msvc"))

    # the C runtime's .vsix packages (zips with Contents\VC\Tools\MSVC\<version>\...) and the Windows
    # SDK's .msi files, then the .cab files those name (msiexec /a reads them from beside the .msi)
    def local(p):
        return os.path.join(work, os.path.basename(payload_name(p)))
    first = vc + msis
    log(f"  downloading Microsoft's C runtime ({len(vc)} packages) and Windows SDK {sdk_version} ({len(msis)} .msi files) "
        f"from {first[0]['url'].split('/')[2]}")
    downloaded = download_all([(p, local(p)) for p in first], "the C runtime and the SDK's .msi files")
    cabs = {}
    for p in msis:
        with open(local(p), "rb") as f:
            for c in msi_cabs(f.read(), sdk):
                cabs[payload_name(c).lower()] = c
    cabs = list(cabs.values())
    downloaded += download_all([(c, local(c)) for c in cabs], f"the SDK's {len(cabs)} .cab files")
    files = [{"file": payload_name(p), "url": p["url"], "size": p.get("size"), "sha256": p.get("sha256")} for p in first + cabs]
    folder = unpack_vsix([local(p) for p in vc], partial)
    # (into a short folder: the SDK's deepest files are over 100 characters below it, and msiexec
    # cannot write past Windows' 260-character path limit, which a deep WindowSmash folder reaches)
    unpacked = tempfile.mkdtemp(prefix="windowsmash-sdk-")
    log("  unpacking them (msiexec /a: files only)")
    try:
        for p in msis:
            run_msiexec(local(p), unpacked, local(p) + ".log")
        sdk_folder = collect_sdk(unpacked, partial)
    finally:
        shutil.rmtree(unpacked, ignore_errors=True)
    check_msvc(partial, folder, sdk_folder)
    record = {"what": "Microsoft's C runtime and Windows SDK (x64)", "channel": CHANNEL_URL,
              "visual_studio": channel["version"], "msvc": folder, "sdk": sdk_folder,
              "license": channel["license"], "license_accepted": datetime.datetime.now().isoformat(timespec="seconds"),
              "download_bytes": sum(f["size"] or 0 for f in files), "downloaded_bytes": downloaded,
              "disk_bytes": disk_bytes(partial), "packages": files}
    write_record(partial, record)
    put_in_place(partial, MSVC_DIR)
    if not keep_downloads:
        shutil.rmtree(work, ignore_errors=True)
    log(f"  deps\\msvc: MSVC {folder}, Windows SDK {sdk_folder}, {record['disk_bytes'] / 1e6:.0f} MB "
        f"(downloaded {record['download_bytes'] / 1e6:.0f} MB)")
    return record


def unpack_vsix(paths, root):
    """The C runtime's .vsix files (zips) into root\\VC\\Tools\\MSVC\\<version>: the folder of the
    one with libraries, every package's files put in it. Returns that version."""
    folder = None
    for path in paths:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            folder = folder or vc_folder(n for n in names if "/lib/" in n)
    if not folder:
        raise Failed("Microsoft's C runtime packages hold no VC\\Tools\\MSVC libraries.")
    for path in paths:
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                rel = vc_member(n, folder)
                if rel:
                    out = os.path.join(root, *rel.split("/"))
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with z.open(n) as src, open(out, "wb") as f:
                        shutil.copyfileobj(src, f, 1 << 20)
    return folder


def collect_sdk(unpacked, root):
    """What msiexec unpacked into `unpacked` (Windows Kits\\10\\...) that the build uses, moved into
    root the same way. Returns the SDK's version folder ("10.0.26100.0"), or None."""
    sdk_folder = None
    for d, _dirs, names in os.walk(unpacked):
        for n in names:
            src = os.path.join(d, n)
            rel = os.path.relpath(src, unpacked).replace(os.sep, "/")
            if sdk_member(rel):
                out = os.path.join(root, *rel.split("/"))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                shutil.move(src, out)   # (from %TEMP%: maybe another drive)
                if rel.split("/")[2].lower() == "include":
                    sdk_folder = sdk_folder or rel.split("/")[3]
    return sdk_folder


def check_msvc(root, folder, sdk_folder):
    """The unpacked files have what a build needs, or Failed naming what is missing."""
    import cc   # (for the libraries its link names)
    vc = os.path.join(root, "VC", "Tools", "MSVC", folder)
    kits = os.path.join(root, "Windows Kits", "10")
    missing = [h for h in NEEDED_HEADERS if not os.path.isfile(os.path.join(vc, *h.split("/")))]
    missing += [lib for lib in ("libcmt.lib", "libvcruntime.lib", "oldnames.lib") if not os.path.isfile(os.path.join(vc, "lib", "x64", lib))]
    if not sdk_folder:
        missing.append("Windows Kits\\10")
    else:
        missing += [h for h in NEEDED_SDK_HEADERS if not os.path.isfile(os.path.join(kits, "Include", sdk_folder, *h.split("/")))]
        libs = set()
        for part in ("um", "ucrt"):
            d = os.path.join(kits, "Lib", sdk_folder, part, "x64")
            libs |= {n.lower() for n in os.listdir(d)} if os.path.isdir(d) else set()
        missing += [lib for lib in ("libucrt.lib", "kernel32.lib", "uuid.lib", *cc.SYSTEM_LIBS) if lib.lower() not in libs]
    if missing:
        raise Failed("Microsoft's packages lacked " + ", ".join(missing))


def test_build():
    """The check the installer runs (build_game.py --check: a test program built and linked as the
    game is, with every library cc.link names, then run), which must have taken deps\\llvm and
    deps\\msvc; otherwise both folders' records are removed, so that cc.py does not take them, and
    Failed says why. (SSBU_CLANG, which would win, is left out of its environment.)"""
    env = {k: v for k, v in os.environ.items() if k.upper() != "SSBU_CLANG"}
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "build_game.py"), "--check"],
                       capture_output=True, text=True, env=env, cwd=ROOT)
    out = r.stdout + r.stderr
    if r.returncode == 0 and "toolchain: deps\\llvm" in out:
        return
    for folder in (LLVM_DIR, MSVC_DIR):
        if os.path.exists(os.path.join(folder, RECORD)):
            os.replace(os.path.join(folder, RECORD), os.path.join(folder, "windowsmash-failed.json"))
    raise Failed("a test program did not build with deps\\llvm and deps\\msvc:\n" + out[-2000:])


# ---- the commands ------------------------------------------------------------------------------
def plan():
    log(f"LLVM {LLVM_VERSION} from GitHub: {LLVM_URL}")
    log(f"  {LLVM_SIZE / 1e6:.0f} MB, SHA-256 {LLVM_SHA256}; keeps clang-cl, lld-link, llvm-lib and clang's headers in deps\\llvm")
    channel = read_channel()
    log(f"Microsoft's C runtime and Windows SDK from Microsoft's servers (Visual Studio {channel['version']}'s packages: {CHANNEL_URL})")
    log(f"  only after you accept Microsoft's license terms: {channel['license']}")
    packages = index_packages(fetch_json(channel["manifest"]["url"]))
    version = msvc_version(packages)
    vc = msvc_payloads(packages, version)
    sdk, sdk_version = sdk_package(packages)
    msis = sdk_payloads(sdk)
    for p in vc:
        log(f"  {p['package']}: {(p.get('size') or 0) / 1e6:.1f} MB")
    log(f"  Windows SDK {sdk_version}: {len(msis)} .msi files ({sum(p.get('size') or 0 for p in msis) / 1e6:.1f} MB) "
        f"and the .cab files they name (their sizes are known once the .msi files are read)")


def status():
    for folder, name in ((LLVM_DIR, "deps\\llvm"), (MSVC_DIR, "deps\\msvc")):
        r = read_record(folder)
        if not r:
            log(f"{name}: not set up" + (" (unfinished)" if os.path.isdir(folder) else ""))
        elif folder == LLVM_DIR:
            log(f"{name}: LLVM {r['version']}, {r['disk_bytes'] / 1e6:.0f} MB on disk, {r['download_bytes'] / 1e6:.0f} MB downloaded"
                + ("" if r["version"] == LLVM_VERSION else f" (this version of WindowSmash takes {LLVM_VERSION})"))
        else:
            log(f"{name}: MSVC {r['msvc']}, Windows SDK {r['sdk']} (Visual Studio {r['visual_studio']}), "
                f"{r['disk_bytes'] / 1e6:.0f} MB on disk, {r['download_bytes'] / 1e6:.0f} MB downloaded; license {r['license']}")


def remove():
    for folder in (LLVM_DIR, MSVC_DIR, LLVM_DIR + ".partial", MSVC_DIR + ".partial", DOWNLOADS):
        if os.path.exists(folder):
            shutil.rmtree(folder)
            log(f"removed {os.path.relpath(folder, ROOT)}")


def agree(accept=False):
    """Microsoft's license, before anything of Microsoft's is downloaded: (yes or no, the channel
    list's info). (True, None) when deps\\msvc is there already: nothing to download or ask.
    `accept` (--accept-license) answers yes without asking."""
    if read_record(MSVC_DIR):
        return True, None
    channel = read_channel()
    if accept:
        log(f"Microsoft's license terms ({channel['license']}): accepted (--accept-license)")
        return True, channel
    return ask_license(channel["license"]), channel


def install(accept=False, keep_downloads=False, agreed=None):
    """Both folders, each unless it is there: LLVM from GitHub and Microsoft's packages at the same
    time, then the test build. `agreed`: agree()'s answer, when it was asked already (the build
    asks at its first step and downloads later). 0 done, 2 declined (nothing downloaded)."""
    ok, channel = agreed or agree(accept)
    if not ok:
        log("Not accepted: nothing was downloaded.")
        return 2
    t0 = time.time()
    with ThreadPoolExecutor(1) as other:
        llvm = other.submit(install_llvm, keep_downloads)
        msvc = install_msvc(channel, keep_downloads) if channel else read_record(MSVC_DIR)
        llvm = llvm.result()
    if not channel:
        log(f"  deps\\msvc: MSVC {msvc['msvc']}, Windows SDK {msvc['sdk']}, already there")
    if not keep_downloads:
        try:
            os.rmdir(DOWNLOADS)   # (emptied: each part removes its own downloads)
        except OSError:
            pass
    log("  trying them on a test program")
    test_build()
    log(f"Done in {time.time() - t0:.0f} s: {(llvm['disk_bytes'] + msvc['disk_bytes']) / 1e6:.0f} MB in deps\\llvm and deps\\msvc "
        f"(downloads: {(llvm['download_bytes'] + msvc['download_bytes']) / 1e6:.0f} MB).")
    return 0


def main():
    ap = argparse.ArgumentParser(description="The compiler without Visual Studio: deps\\llvm and deps\\msvc.")
    ap.add_argument("command", choices=("install", "plan", "status", "remove"))
    ap.add_argument("--accept-license", action="store_true",
                    help="accept Microsoft's license terms without asking (only if you have read and accept them)")
    ap.add_argument("--keep-downloads", action="store_true", help="keep what was downloaded in deps\\downloads")
    a = ap.parse_args()
    try:
        if a.command == "install":
            return install(a.accept_license, a.keep_downloads)
        {"plan": plan, "status": status, "remove": remove}[a.command]()
    except (Failed, OSError, ValueError) as e:   # (OSError: the network, the disk; ValueError: an answer that is not JSON)
        log(f"\nERROR: {e}")
        return 1
    except KeyboardInterrupt:
        log("\nStopped.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
