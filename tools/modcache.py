"""What tools/recomp.py keeps between builds: NRO load addresses, the stamp that says a module's
build dir is current, and the check that every module's imports resolve.

NRO load addresses: dump/nro/bases.txt, "<name> <base> <end> <build id>", one row per fighter
module (NRO) the game has, each placed for good (place) so that rebuilding never moves one.
Modules must not share a 16 MB dispatch slot (runtime/host.c SLOT_SHIFT), and NROs live
between subsdk/sdk's slots and the heap: [0x7200000000, 0x7400000000), 512 slots.

Module build dirs: each holds a stamp, module.txt: "<build id> <base> <name> <tools
fingerprint>", written once its C is generated and compiled (by a build that compiles it
elsewhere - Linux, macOS: recomp.py all --generate-only - once it is generated). With the same
build id, base and tools fingerprint (the generator's sources and the runtime's native-function
set) the C is not generated again. recomp.py replaces a file rather than writing into it
(write_if_changed, compile_dir): a file can be a hard link that an earlier WindowSmash, which
shared build dirs between game versions, made to another folder's.
"""
import hashlib
import os
import re

TOOLS = os.path.dirname(os.path.abspath(__file__))
NRO_BASE, NRO_LIMIT, SLOT = 0x7200000000, 0x7400000000, 0x1000000
# the sources whose output is the generated C (a change in any of them generates it again)
GENERATOR = ("recomp.py", "lift.py", "lift_simd.py", "a64.py", "funcs.py", "eh.py", "nso.py", "regcache.py")


def replace_file(path, text):
    """Write a file as a new one (never through a hard link to another folder's)."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


# ---- NRO load addresses -------------------------------------------------------------
def read_bases(path):
    """[name, base, end, build id] rows of dump/nro/bases.txt."""
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [l.split() for l in f if len(l.split()) == 4]


def slots(base, end):
    return set(range(base // SLOT, (end - 1) // SLOT + 1))


def place(name, size, rows):
    """The base for a new NRO among this folder's NROs (`rows`: bases.txt rows): past all of
    them, else the first free slots."""
    used = set()
    for r in rows:
        used |= slots(int(r[1], 16), int(r[2], 16))
    top = max([NRO_BASE] + [int(r[2], 16) for r in rows])
    base = (top + SLOT - 1) // SLOT * SLOT
    if base + size > NRO_LIMIT:
        base = next((b for b in range(NRO_BASE, NRO_LIMIT - size + 1, SLOT) if not slots(b, b + size) & used), None)
        if base is None:
            raise SystemExit(f"no room left for {name} ({size:,} bytes) among the game's modules")
    return base


# ---- module build dirs ---------------------------------------------------------------
def fingerprint(extra):
    """The tools' fingerprint: the generator's sources and `extra` (the runtime's set of
    native functions and wraps, which change what is generated)."""
    h = hashlib.sha256()
    for n in GENERATOR:
        with open(os.path.join(TOOLS, n), "rb") as f:
            h.update(n.encode() + b"\0" + f.read().replace(b"\r\n", b"\n") + b"\0")
    h.update(extra.encode())
    return h.hexdigest()[:16]


TAIL = re.compile(r'const RtModule rt_module_(\w+) = \{\s*"\w+", 0x([0-9A-F]+)ull,.*?"([0-9A-F]+)",\s*\};', re.S)


def build_info(d):
    """(build id, base, name, tools fingerprint or None) of a module build dir, or None:
    from its stamp, or for a dir made before stamps, from the end of its g_module.c."""
    try:
        with open(os.path.join(d, "module.txt")) as f:
            p = f.read().split()
        return p[0], int(p[1], 16), p[2], p[3]
    except (OSError, IndexError, ValueError):
        pass
    try:
        with open(os.path.join(d, "g_module.c"), "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 4096))
            m = TAIL.search(f.read().decode("utf-8", "replace"))
    except OSError:
        return None
    return (m.group(3), int(m.group(2), 16), m.group(1), None) if m else None


def write_stamp(d, build_id, base, name, fp):
    replace_file(os.path.join(d, "module.txt"), f"{build_id} {base:x} {name} {fp}\n")


def clear_stamp(d):
    try:
        os.remove(os.path.join(d, "module.txt"))
    except OSError:
        pass


# ---- imports ---------------------------------------------------------------------------
def unresolved_imports(mods, native):
    """name -> (strong, weak) symbols the module refers to (by relocation) that no module
    exports and the runtime does not implement (`native`): what would stay unbound when the
    game runs (runtime/host.c resolve). mods: name -> nso.Module."""
    exported = set()
    for m in mods.values():
        exported |= {s[0] for s in m.exports}
    out = {}
    for name, m in mods.items():
        refs = {r[2] for r in m.relocs if r[1] != 1027 and r[2]}
        strong, weak = set(), set()
        for i in refs:
            sname, _v, _s, info, shndx = m.symbols[i]
            if not sname or shndx or sname in exported or sname in native:
                continue
            (weak if info >> 4 == 2 else strong).add(sname)
        out[name] = (sorted(strong), sorted(weak))
    return out


def report_imports(mods, native, path):
    """Print and write (to `path`) every module's unresolved imports; returns how many strong ones."""
    res = unresolved_imports(mods, native)
    lines = ["# symbols a module refers to that no module exports and the runtime does not implement (HLE):",
             "# <module> strong|weak <symbol>   (strong ones trap when called; weak ones read as 0)"]
    total_s = total_w = 0
    for name in mods:
        strong, weak = res[name]
        total_s += len(strong)
        total_w += len(weak)
        lines += [f"{name} strong {s}" for s in strong] + [f"{name} weak {s}" for s in weak]
        if strong:
            more = f" (+{len(strong) - 3} more)" if len(strong) > 3 else ""
            print(f"  {name}: {len(strong)} unresolved import(s): {', '.join(strong[:3])}{more}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    replace_file(path, "\n".join(lines) + "\n")
    print(f"imports: {total_s} strong and {total_w} weak symbols unresolved across {len(mods)} modules (listed in {path})")
    return total_s
