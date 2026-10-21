#!/usr/bin/env python3
"""The macOS program end to end, on a small made-up game: tools/mac/build_mac.py builds the whole
runtime with three hand-written modules (main, subsdk0 and sdk: this folder's, in the form
tools/recomp.py writes a module's C) into the app, which is then started the ways a Mac starts it,
and each start's log is checked: the guest's address space reserved, the modules mapped at their
guest addresses and linked to each other and to the runtime's own functions, the window with its
Metal layer, the guest main thread through nn::init::Start (the SDK's start-up, the initialisers,
nninitStartup, nnMain), nnMain's checks of the kernel - memory, a second guest thread, waits,
sleeps, atomics, a guest setjmp the runtime's longjmp comes back to - and the process leaving
cleanly through svcExitProcess. No game files.

Usage: boottest.py [--tree <dir>] [--jobs N] [--only direct|open|launcher] [--show]
  --tree  the made-up game's folder (default: ~/Library/Caches/WindowSmash/boottest - not in the
          repository, whose own game it is not), laid out as a game tree: build/<module>/ (the C), dump/modules/ (the images), build-mac/
          (what build_mac.py makes: kept, so a second run only links)
  --show  print each start's log
The starts, each showing the game's window for a moment:
  direct    the app's program, <tree>/build-mac/WindowSmash.app/Contents/MacOS/ssbu <tree>/dump/modules,
            run in <tree>
  open      open -n -W -a <app>, with no arguments, as Finder starts it: the program finds the tree's
            dump/modules and the port's root (the nearest folder up with tools/recomp.py: here the
            tree) from where the app is; the log through SSBU_LOG
  launcher  launchers/Play SSBU Offline.command with SSBU_TREE=<tree>: open -n -W with SSBU_CWD and
            SSBU_LOG, as every launcher starts the game (tools/mac/launch.sh)
Exits 0 when the build and every start pass; a failed start's log is printed."""
import os
import re
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
MODULES = ("main", "subsdk0", "sdk")
# (outside the repository: its own dump/ and build/ are the real game's)
TREE = os.path.join(os.path.expanduser("~"), "Library", "Caches", "WindowSmash", "boottest")
# A string the guest code uses, at the address before it: "= 0x<address>ull; X(n) = <length>; /* "<text>" */"
STRING = re.compile(r'= 0x([0-9A-Fa-f]+)ull; X\(\d+\) = (\d+);\s*/\* "([^"]*)" \*/')
TABLE = re.compile(r'"(\w+)", 0x([0-9A-Fa-f]+)ull, 0x([0-9A-Fa-f]+)ull, 0x([0-9A-Fa-f]+)ull')


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write_if_changed(path, data):
    """(an unchanged file keeps its date: build_mac.py compiles again only what changed)"""
    mode = "wb" if isinstance(data, bytes) else "w"
    if os.path.exists(path):
        with open(path, "rb" if mode == "wb" else "r") as f:
            if f.read() == data:
                return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(data)


def strings(module):
    """[(address, text)] of the strings a module's code uses, from its C."""
    found = []
    for addr, length, text in STRING.findall(read(os.path.join(HERE, module, "g0000.c"))):
        if int(length) != len(text):
            raise SystemExit(f"{module}/g0000.c: the length given for \"{text}\" is {length}, not {len(text)}")
        found.append((int(addr, 16), text))
    return found


def image(module):
    """The module's image (<module>.reloc.bin): its name at +0x8000, its strings where its code reads them."""
    m = TABLE.search(read(os.path.join(HERE, module, "g_module.c")))
    name, base, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
    assert name == module, (name, module)
    img = bytearray(size)
    img[0x8000:0x8008] = module.encode().ljust(8, b"\0")
    used = set(range(0x8000, 0x8008))
    for addr, text in strings(module):
        at, data = addr - base, text.encode() + b"\0"
        if not 0x8000 <= at <= size - len(data) or used & set(range(at, at + len(data))):
            raise SystemExit(f"{module}: \"{text}\" at {addr:#x} is outside its data or over another string")
        img[at:at + len(data)] = data
        used |= set(range(at, at + len(data)))
    return bytes(img)


def make_tree(tree):
    for m in MODULES:
        out = os.path.join(tree, "build", m)
        code = read(os.path.join(HERE, m, "g0000.c"))
        write_if_changed(os.path.join(out, "g0000.c"), code)
        write_if_changed(os.path.join(out, "g_module.c"), read(os.path.join(HERE, m, "g_module.c")))
        statements = code.count(";\n")
        write_if_changed(os.path.join(out, "manifest.tsv"), f"g0000.c\t{statements}\t{statements}\n")
        write_if_changed(os.path.join(tree, "dump", "modules", m + ".reloc.bin"), image(m))
    romfs = os.path.join(tree, "dump", "romfs.bin")   # (the launchers check the game data is there; nothing reads it)
    write_if_changed(romfs, b"")
    write_if_changed(os.path.join(tree, "dump", "modules", "romfs.cfg"), romfs + "\n")
    write_if_changed(os.path.join(tree, "version.txt"), "1.0.0\n# tools/mac/boottest's made-up game\n")
    # (makes the tree the port's root too: the app started with no arguments works there, runtime/host.c)
    write_if_changed(os.path.join(tree, "tools", "recomp.py"), "# tools/mac/boottest: this folder is the port's root, for the game's start\n")


def expected(tree, app):
    """What a start's log must have, in this order (other lines may come between)."""
    sdk = [t for _a, t in strings("sdk")]
    main = [t for _a, t in strings("main") if "FAILED" not in t]
    lines = [r"\[boot\] " + re.escape(os.path.join(app, "Contents", "MacOS", "ssbu")) + r", built ",
             r"\[mem\] write watch: page protection",
             r"\[clock\] ",
             r"loaded 3 modules: 18 functions, 6 exported symbols, 8 symbol relocations; \d+ symbols bound to native "
             r"code, 0 left undefined \(0 NROs recompiled for loading later\)$",
             r"\[gfx\] window ready \(SDL cocoa video driver, ",
             r"\[boot\] nn::init::Start$",
             r"\[guest\] " + re.escape(sdk[0]) + "$",
             r"\[guest\] " + re.escape(sdk[1]) + "$",
             r"\[boot\] static constructors of sdk$",
             r"\[boot\] static constructors of subsdk0$",
             r"\[boot\] static constructors of main$",
             r"\[boot\] nninitStartup$",
             r"\[boot\] nnMain entered$"]
    lines += [r"\[guest\] " + re.escape(t) + "$" for t in main]
    return lines + [r"guest process exited$"]


def check(log, want):
    """None if the log has every line wanted, in order, and no stop or crash; else what is wrong."""
    lines = log.splitlines()
    bad = [l for l in lines if l.startswith("*** ") or "FAILED" in l]
    if bad:
        return f"the game stopped: {bad[0]}"
    at = 0
    for w in want:
        pattern = re.compile(w)
        while at < len(lines) and not pattern.match(lines[at]):
            at += 1
        if at == len(lines):
            return f"no line like {w!r} (after the ones before it)"
        at += 1
    return None


def stop_game(exe):
    """A start that did not end in time: its program stopped (open -W only waits for it)."""
    r = subprocess.run(["pgrep", "-f", exe], capture_output=True, text=True)
    for pid in r.stdout.split():
        try:
            os.kill(int(pid), signal.SIGKILL)
        except (OSError, ValueError):
            pass


def start(how, tree, app, exe):
    """Start the game one way: (log text, failure or None)."""
    build = os.path.join(tree, "build")
    os.makedirs(build, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SSBU_")}
    modules = os.path.join(tree, "dump", "modules")
    log = os.path.join(build, f"boottest_{how}.log")
    if os.path.exists(log):
        os.remove(log)
    status, problem = None, None
    try:
        if how == "direct":
            with open(log, "w") as f:
                status = subprocess.run([exe, modules, "--status", "30"], cwd=tree, env=env, stdout=f, stderr=subprocess.STDOUT,
                                        stdin=subprocess.DEVNULL, timeout=60).returncode
        elif how == "open":
            ini = os.path.join(tree, "display.ini")
            if os.path.exists(ini):
                os.remove(ini)
            status = subprocess.run(["open", "-n", "-W", "--env", f"SSBU_LOG={log}", "-a", app], env=env, stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=60).returncode
            if not os.path.exists(ini):   # (written where the game works when it is missing: the root found from the app)
                problem = f"no display.ini written in {tree}: the game did not work in the root it should have found"
        else:
            env.update(SSBU_TREE=tree, SSBU_DISPLAY=os.path.join(tree, "display.ini"))   # (not written into the repository)
            log = os.path.join(build, "play.log")
            r = subprocess.run(["/bin/bash", os.path.join(REPO, "launchers", "Play SSBU Offline.command")], env=env,
                               stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=90)
            status = r.returncode
            if "Starting the game, offline" not in r.stdout:
                problem = f"the launcher did not start the game:\n{r.stdout}{r.stderr}"
    except subprocess.TimeoutExpired:
        stop_game(exe)
        problem = "did not finish in time (stopped)"
    text = read(log) if os.path.exists(log) else ""
    if not problem and status:
        problem = f"exit status {status}"
    return text, problem or check(text, expected(tree, app))


def main():
    tree = os.path.abspath(arg("--tree") or TREE)
    jobs = arg("--jobs")
    only = arg("--only")
    hows = [only] if only else ["direct", "open", "launcher"]
    if any(h not in ("direct", "open", "launcher") for h in hows):
        raise SystemExit(__doc__)
    make_tree(tree)
    t0 = time.time()
    cmd = [sys.executable, os.path.join(REPO, "tools", "mac", "build_mac.py"), "--port", tree] + (["--jobs", jobs] if jobs else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        print(f"build: FAILED (exit {r.returncode})")
        return 1
    print(f"build: ok, {time.time() - t0:.0f} s ({r.stdout.strip().splitlines()[-1]})")
    app = os.path.join(tree, "build-mac", "WindowSmash.app")
    exe = os.path.join(app, "Contents", "MacOS", "ssbu")
    failed = 0
    for how in hows:
        t0 = time.time()
        text, problem = start(how, tree, app, exe)
        lines = len(text.splitlines())
        print(f"{how}: {'FAILED: ' + problem if problem else 'ok'} ({time.time() - t0:.1f} s, {lines} lines of log)")
        if problem or "--show" in sys.argv:
            print("".join("    " + l + "\n" for l in text.splitlines()))
        failed += problem is not None
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
