#!/usr/bin/env python3
"""Static recompiler driver.

Usage:
  recomp.py coverage <nso> [--jobs N]
  recomp.py emit     <nso> --roots <off>[,<off>...] --out <file.c> [--depth N]
  recomp.py emit     <nso> --sample N --out <file.c>
  recomp.py image    <nso> --out <file.bin>
  recomp.py build    <nso> --out <dir> [--jobs N] [--per-file INSNS]
  recomp.py compile  <dir> [--jobs N]
  recomp.py link     --dirs <dir>[,<dir>...] --out <exe>
  recomp.py all      <exefs dir> --out <build dir> [--jobs N] [--generate-only]
  recomp.py check    <exefs dir>
  recomp.py wrap-sig <nso> <address>

The game is version 13.0.5 (tools/game_version.py); its modules are this folder's dump/exefs
and dump/nro (the fighter modules).

coverage  lift every function and report what the lifter cannot translate yet
emit      write C for the given functions and everything they call directly
image     write the module image with base relocations applied for its load address
build     write C for a whole module, split across many files, plus its loader tables
compile   compile a build directory to objects (incremental; reports per-file failures)
link      compile the host runtime and link module build directories into an executable
all       image + build + compile every module of an ExeFS, then link; a module whose C is
          current (same build id, address and tools: modcache.py) is not generated again;
          --generate-only stops before compiling (the Linux build compiles the C itself:
          tools/linux/build_linux.py; so does the macOS one, tools/mac/build_mac.py, and on
          macOS all always stops there)
check     list each module's imports that no module exports and the runtime does not implement
wrap-sig  print the code signature of a guest function, for an HLE_WRAP (see hle_wraps)

Each module is recompiled for a fixed load address (see MODULES); a function is named after
its guest address, so names are unique across modules. A module function whose exported
name has a native implementation - `HLE(name)` in runtime/*.c - is emitted as a forwarder to
it, so the replacement is reached by every caller, including direct calls inside the module.
"""
import bisect
import mmap
import os
import re
import struct
import sys
import time
from collections import Counter
from multiprocessing import Pool

# this folder, for the modules below: a Python with a ._pth file (the embeddable one the installer
# sets up when the PC has none) does not put a script's own folder on the path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modcache  # noqa: E402
import progress  # noqa: E402
from funcs import discover  # noqa: E402
from lift import Lifter  # noqa: E402
from nso import Module  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Load addresses, in the order the console's loader places (and searches) the modules.
MODULES = {"main": 0x7100000000, "subsdk0": 0x7108000000, "sdk": 0x7109000000}
# Modules the game loads at run time (NROs). The runtime saves each one it is handed that is
# not compiled in yet as dump/nro/<build id>.nro; each is then given a load address of its
# own for good, recorded in dump/nro/bases.txt, so that rebuilding never moves a module
# (modcache.place).
NRO_DIR = os.path.join(ROOT, "dump", "nro")
_state = {}


def module_key(path):
    return os.path.splitext(os.path.basename(path))[0]


def nro_modules():
    """name -> base for every harvested NRO, naming and placing new arrivals."""
    manifest = os.path.join(NRO_DIR, "bases.txt")
    rows = modcache.read_bases(manifest)
    known = {r[3]: r for r in rows}
    added = False
    for f in sorted(os.listdir(NRO_DIR)) if os.path.isdir(NRO_DIR) else []:
        path = os.path.join(NRO_DIR, f)
        if not f.endswith(".nro") or module_key(f) in {r[0] for r in rows}:
            continue
        mod = Module(path)
        if mod.build_id in known:   # saved again under its id before it was compiled in
            os.remove(path)
            continue
        name = re.sub(r"\W", "_", module_key(mod.module_name().replace("\\", "/")).lower()) or "nro"
        if not name[0].isalpha():
            name = "nro_" + name
        base = modcache.place(name, mod.mem_end, rows)
        while name in MODULES or name in {r[0] for r in rows}:
            name += "_"
        row = [name, f"{base:x}", f"{base + mod.mem_end:x}", mod.build_id]
        rows.append(row)
        known[mod.build_id] = row
        os.replace(path, os.path.join(NRO_DIR, name + ".nro"))
        print(f"new NRO {name}: {mod.mem_end:,} bytes at {base:#x}")
        added = True
    if added:
        with open(manifest, "w", newline="\n") as f:
            f.writelines(" ".join(r) + "\n" for r in rows)
    return {r[0]: int(r[1], 16) for r in rows}


def base_of(path):
    if arg("--base"):
        return int(arg("--base"), 16)
    key = module_key(path)
    if key not in MODULES and path.endswith(".nro"):
        MODULES.update(nro_modules())
    return MODULES[key]


def hle_names():
    """Exported names that have a native implementation in the runtime sources."""
    names = set()
    runtime = os.path.join(ROOT, "runtime")
    for name in os.listdir(runtime):
        if name.endswith(".c") and not name.startswith("._"):   # (not macOS's ._* files on exFAT)
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                names |= set(re.findall(r"^HLE\((\w+)\)", f.read(), re.M))
    return names


def hle_wraps():
    """Guest functions (not exported) wrapped natively: `HLE_WRAP(<address>)` in the runtime
    sources, address -> code signature or None. The function stays, as f_<address>_orig; its
    callers reach the native wrapper. The address is 13.0.5's; a comment `/* signature: <words> */`
    on the line before (`recomp.py wrap-sig` prints one) says what the function there begins with,
    which resolve_wraps checks. A build of modules that lack it (a made-up game's:
    tools/mac/boottest) links its f_<address>_orig to rt_wrap_missing (link)."""
    found = {}
    runtime = os.path.join(ROOT, "runtime")
    for name in os.listdir(runtime):
        if name.endswith(".c") and not name.startswith("._"):
            with open(os.path.join(runtime, name), encoding="utf-8") as f:
                for sig, a in re.findall(r"^(?:/\* signature: ([0-9a-f. ]+) \*/\r?\n)?HLE_WRAP\(([0-9a-fA-F]+)\)", f.read(), re.M):
                    found[int(a, 16)] = sig.split() or None
    return found


def signature(mod, off, size, n=16):
    """The code signature of the function at module offset `off`: its first n instructions as
    hex words, with the nibbles that depend on where code and data lie ('.') - the immediates
    of branches, adr/adrp and literal loads, and the page offsets added to an adrp's result."""
    out, pages = [], set()
    for i in range(min(n, size // 4)):
        w = mod.u32(off + 4 * i)
        mask = 0xFFFFFFFF
        if w & 0x7C000000 == 0x14000000:                                  # b, bl
            mask = 0xFC000000
        elif w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000 or w & 0x3B000000 == 0x18000000:
            mask = 0xFF00001F                                             # b.cond, cbz/cbnz, ldr literal
        elif w & 0x7E000000 == 0x36000000:                                # tbz/tbnz
            mask = 0xFFF8001F
        elif w & 0x1F000000 == 0x10000000:                                # adr, adrp
            mask = 0x9F00001F
            if w >> 31:
                pages.add(w & 31)
        elif (w >> 5) & 31 in pages and (w & 0x7F800000 == 0x11000000 or w & 0x3B000000 == 0x39000000):
            mask = 0xFFC003FF                                             # add / load / store at an adrp page
        out.append("".join(f"{w >> s & 15:x}" if mask >> s & 15 == 15 else "." for s in range(28, -4, -4)))
    return out


def resolve_wraps(mod, base, sizes, name):
    """This module's wraps: {offset: ("wrap", wrapper)}. Each wrapped function must start at its
    address and begin as its signature says: the addresses are 13.0.5's, and one that isn't there
    (the runtime wrapping a function that moved) stops the build rather than leave a hook out."""
    forward = {}
    for a, sig in sorted(hle_wraps().items()):
        if max((b for b in MODULES.values() if b <= a), default=None) != base:
            continue   # another module's
        off = a - base
        if off not in sizes:
            raise SystemExit(f"{name}: HLE_WRAP({a:x}): no function starts there in this module "
                             "(the runtime's hooks are for game version 13.0.5)")
        if sig is not None and signature(mod, off, sizes[off], len(sig)) != sig:
            raise SystemExit(f"{name}: HLE_WRAP({a:x}): the function there does not begin as its signature says "
                             "(the runtime's hooks are for game version 13.0.5)")
        forward[off] = ("wrap", f"hle_wrap_{a:x}")
    return forward


def tools_fingerprint():
    """What the generated C depends on besides the module and its address (modcache)."""
    import lift
    wraps = sorted(hle_wraps().items())
    return modcache.fingerprint("\n".join(sorted(hle_names())) + "\n" + repr(wraps) + f"\nregcache {int(lift.REGCACHE)}")


def write_if_changed(path, text):
    """Leave an identical file untouched so its object stays up to date. A changed one is
    written as a new file (modcache.replace_file)."""
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return False
    modcache.replace_file(path, text)
    return True


def _init(image_path, entries, base, forward, setjmp=None, pads=None):
    f = open(image_path, "rb")
    _state["img"] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    _state["args"] = (entries, base, forward, setjmp, pads)


def unwind_jumpto(mod, found):
    """libunwind's Registers_arm64::jumpto: loads every register from the context it is given
    and returns into the frame being resumed. That cannot work as a host return, so it is
    replaced natively; found by its code (ldp x2, x3, [x0, #0x10] ... ldr x30, [x0, #0x100])."""
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(re.escape(struct.pack("<I", 0xA9410C02)), text):
        if m.start() % 4:
            continue
        a = mod.text.mem + m.start()
        if a in found and struct.pack("<I", 0xF940801E) in text[m.start():m.start() + 0x40]:
            return {a: "rt_unwind_jumpto"}
    return {}


SETJMP_NAMES = {"setjmp", "_setjmp", "__setjmp"}


def setjmp_targets(mod):
    """Where calls to setjmp land in this module: its own definitions and its import stubs."""
    from funcs import import_stubs
    targets = {v for n, v, _s, info, _x in mod.exports if n in SETJMP_NAMES}
    slots = {r[0]: mod.symbols[r[2]][0] for r in mod.relocs if r[2]}
    targets |= {stub for stub, slot in import_stubs(mod).items() if slots.get(slot) in SETJMP_NAMES}
    return targets


def _lifter():
    return Lifter(_state["img"], *_state["args"])


def _cover(funcs):
    lifter = _lifter()
    ok = ok_insns = insns = bad_insns = 0
    reasons = Counter()
    blocked = Counter()
    for start, size in funcs:
        _code, why, _callees = lifter.lift_function(start, size)
        insns += size // 4
        if why:
            bad_insns += sum(why.values())
            reasons.update(why)
            kinds = {r for r, _w in why}
            if len(kinds) == 1:
                blocked[next(iter(kinds))] += 1
        else:
            ok += 1
            ok_insns += size // 4
    return (ok, ok_insns, insns, bad_insns, reasons, blocked, lifter.tables, lifter.indirect_tail,
            lifter.table_like, lifter.unresolved, lifter.assumed)


def _emit(args):
    index, funcs, outdir = args
    lifter = _lifter()
    own = {a for a, _ in funcs}
    bodies, ext, bad = [], set(), 0
    for start, size in funcs:
        code, why, callees = lifter.lift_function(start, size)
        bad += sum(why.values())
        bodies.append(code)
        ext |= callees
    name = f"g{index:04d}.c"
    text = ('#include "recomp.h"\n\n' + "".join(f"GuestFn f_{lifter.base + a:x};\n" for a in sorted(ext | own))
            + "\n" + "\n\n".join(bodies) + "\n")
    write_if_changed(os.path.join(outdir, name), text)
    insns = sum(s for _, s in funcs) // 4
    return name, ext - own, insns, max(s for _, s in funcs) // 4, bad, lifter.unresolved


def load(path):
    """(module, {entry: sources}, [(entry, size)]) with every function that has code."""
    mod = Module(path)
    found, sizes = discover(mod)
    funcs = sorted((a, sizes[a]) for a, src in found.items() if "eh" in src or "plt" in src)
    # Hand-written assembly (system-call wrappers, memcpy, setjmp, ...) has no unwind entry.
    # Such a routine runs from its entry to the next known entry or described function,
    # less any trailing padding.
    starts = [a for a, _ in funcs]
    ends = [a + s for a, s in funcs]

    def described(a):
        k = bisect.bisect_right(starts, a) - 1
        return k >= 0 and a < ends[k]

    # Compiled code also reaches such routines by tail-jump alone; a `b` out of a described
    # function to somewhere no description covers is an entry too. (A `b` inside an assembly
    # routine is left alone: those are its own internal labels.)
    text = bytes(mod.image[mod.text.mem:mod.text.end])
    for m in re.finditer(rb"[\x14-\x17]", text[3::4]):
        src = mod.text.mem + m.start() * 4
        imm = struct.unpack_from("<I", text, m.start() * 4)[0] & 0x3FFFFFF
        target = src + (imm - 0x4000000 if imm & 0x2000000 else imm) * 4
        if mod.text.mem <= target < mod.text.end and target not in found and described(src) and not described(target):
            found[target] = {"b"}
    marks = sorted(set(found) | set(starts) | {mod.text.end})
    extra = []
    for a in sorted(found):
        if "eh" in found[a] or "plt" in found[a] or not mod.text.mem <= a < mod.text.end or a & 3:
            continue
        if described(a):
            continue   # an address inside a described function: a data pointer, not an entry
        end = marks[bisect.bisect_right(marks, a)]
        while end > a and mod.u32(end - 4) in (0, 0xD503201F, 0xE7FFDEFE):
            end -= 4
        if end > a:
            extra.append((a, end - a))
    return mod, found, sorted(funcs + extra)


def image_file(mod):
    build_dir = os.path.join(ROOT, "build")
    os.makedirs(build_dir, exist_ok=True)
    path = os.path.join(build_dir, f"image-{mod.build_id[:16]}.img")
    if not os.path.exists(path) or os.path.getsize(path) != len(mod.image):
        with open(path, "wb") as f:
            f.write(mod.image)
    return path


def coverage(path, jobs):
    import capstone
    mod, found, funcs = load(path)
    step = 400
    chunks = [funcs[i:i + step] for i in range(0, len(funcs), step)]
    ok = ok_insns = insns = bad = tables = tails = table_like = assumed = 0
    reasons, blocked, unresolved = Counter(), Counter(), []
    with Pool(jobs, _init, (image_file(mod), set(found), base_of(path), {})) as pool:
        for r in pool.imap_unordered(_cover, chunks):
            ok += r[0]; ok_insns += r[1]; insns += r[2]; bad += r[3]
            reasons.update(r[4]); blocked.update(r[5])
            tables += r[6]; tails += r[7]; table_like += r[8]; unresolved += r[9]; assumed += r[10]

    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    by_kind, by_mnem = Counter(), Counter()
    for (why, w), n in reasons.items():
        by_kind[why] += n
        d = next(cs.disasm_lite(struct.pack("<I", w), 0), None)
        by_mnem[(why, d[2] if d else "?")] += n

    print(f"functions lifted completely : {ok:,} / {len(funcs):,} ({ok * 100 / len(funcs):.1f}%)")
    print(f"instructions lifted         : {insns - bad:,} / {insns:,} ({(insns - bad) * 100 / insns:.2f}%)")
    print(f"instructions in clean funcs : {ok_insns:,} ({ok_insns * 100 / insns:.1f}%)")
    print(f"jump tables resolved        : {tables:,} of {table_like:,} table-style dispatches"
          f" ({assumed:,} by validation rather than proof)    br treated as indirect tail call: {tails:,}")
    if unresolved:
        print("unresolved table dispatches   : " + " ".join(f"{a:#x}" for a in sorted(unresolved)[:40]))
    if by_kind:
        print("\nunsupported instructions by reason:")
        for why, n in by_kind.most_common():
            print(f"  {n:>9,}  {why:<42} sole blocker of {blocked[why]:,} functions")
        print("\ntop unsupported mnemonics:")
        for i, ((why, m), n) in enumerate(by_mnem.most_common(48)):
            print(f"  {m:<9}{n:>8,}", end="\n" if i % 6 == 5 else "")
        print()


def emit(path, roots, out, depth, sample=0):
    mod, found, funcs = load(path)
    sizes = dict(funcs)
    lifter = Lifter(mod.image, set(found), base_of(path))
    base = lifter.base
    if sample:   # every k-th function that lifts cleanly, callees left as stubs
        clean = [a for a, size in funcs[::max(1, len(funcs) // (sample * 2))]
                 if not lifter.lift_function(a, size)[1]]
        roots, depth = clean[:sample], 0
    bodies, stubs, seen = {}, set(), set()
    frontier = list(roots)
    for _ in range(depth + 1):
        nxt = []
        for a in frontier:
            if a in seen:
                continue
            seen.add(a)
            if a not in sizes:
                stubs.add(a)
                continue
            code, why, callees = lifter.lift_function(a, sizes[a])
            if why:
                print(f"  f_{base + a:x}: {sum(why.values())} unsupported instruction(s) -> traps at runtime")
            bodies[a] = code
            nxt.extend(callees)
        frontier = nxt
    stubs |= {a for a in frontier if a not in seen}
    with open(out, "w") as f:
        f.write('#include "recomp.h"\n\n')
        for a in sorted(set(bodies) | stubs):
            f.write(f"void f_{base + a:x}(Cpu *c);\n")
        f.write("\n")
        for a in sorted(stubs):
            f.write(f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "function not recompiled"); }}\n')
        for a in sorted(bodies):
            f.write("\n" + bodies[a] + "\n")
        f.write("\nconst struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[] = {\n")
        for a in sorted(bodies):
            f.write(f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n")
        f.write(f"}};\nconst u32 rt_func_count = {len(bodies)};\n")
    print(f"wrote {out}: {len(bodies)} functions, {len(stubs)} stubs")


def image(path, out):
    mod, base = Module(path), base_of(path)
    img = bytearray(mod.image) + bytes(mod.mem_end - len(mod.image))
    n = 0
    for off, typ, _sym, addend in mod.relocs:
        if typ == 1027:
            struct.pack_into("<Q", img, off, base + addend)
            n += 1
    with open(out, "wb") as f:
        f.write(img)
    print(f"wrote {out}: {len(img):,} bytes, {n:,} base relocations applied for {base:#x}")


def module_table(mod, name, base, funcs, missing):
    """C source for a module's loader tables: functions, exports, and symbol relocations."""
    imports, relocs = {}, []
    for off, typ, sym, addend in mod.relocs:
        if typ == 1027:
            continue
        sname, _value, _size, info, _shndx = mod.symbols[sym]
        index = imports.setdefault(sname, (len(imports), info >> 4 == 2))[0]
        relocs.append((off, addend, index))
    defined = [a for a, _ in funcs]
    out = ['#include "module.h"\n\n']
    out += [f"GuestFn f_{base + a:x};\n" for a in defined]
    out += [f'void f_{base + a:x}(Cpu *c) {{ rt_trap(c, 0x{base + a:X}ull, "no code at call target"); }}\n'
            for a in sorted(missing)]
    out.append("\nstatic const RtFunc funcs[] = {\n")
    out += [f"    {{0x{base + a:X}ull, f_{base + a:x}}},\n" for a in defined]
    out.append("};\nstatic const RtSym exports[] = {\n")
    out += [f'    {{"{n}", 0x{base + v:X}ull}},\n' for n, v, _s, _i, _x in mod.exports]
    out.append("};\nstatic const char *const imports[] = {\n")
    out += [f'    "{n}",\n' for n in imports]
    out.append("    0\n};\nstatic const u8 import_weak[] = {" + ",".join(str(int(w)) for _i, w in imports.values()) + ",0};\n")
    out.append("static const RtReloc relocs[] = {\n")
    out += [f"    {{0x{o:X}ull, {a}, {i}}},\n" for o, a, i in relocs]
    out.append("    {0, 0, 0}\n};\n\n")
    out.append(f"const RtModule rt_module_{name} = {{\n"
               f'    "{name}", 0x{base:X}ull, 0x{mod.mem_end:X}ull, 0x{mod.text.end:X}ull, 0x{base + mod.dyn.get(12, 0):X}ull,\n'
               f"    funcs, {len(defined)}, exports, {len(mod.exports)}, imports, import_weak, {len(imports)}, "
               f'relocs, {len(relocs)}, "{mod.build_id}",\n}};\n')
    return "".join(out), len(imports), len(relocs)


def build(path, outdir, jobs, per_file):
    mod, found, funcs = load(path)
    name, base = module_key(path), base_of(path)
    os.makedirs(outdir, exist_ok=True)
    modcache.clear_stamp(outdir)   # (until generated and compiled again)
    hle = hle_names()
    forward = {v: f"hle_{n}" for n, v, _s, info, _x in mod.exports if n in hle and info & 0xF == 2}
    forward.update(unwind_jumpto(mod, found))
    forward.update(resolve_wraps(mod, base, dict(funcs), name))
    from eh import landing_pads
    pads = landing_pads(mod)
    chunks, cur, count = [], [], 0
    for a, size in funcs:   # a function bigger than the budget gets a file to itself
        if cur and count + size // 4 > per_file:
            chunks.append(cur)
            cur, count = [], 0
        cur.append((a, size))
        count += size // 4
    if cur:
        chunks.append(cur)
    defined = {a for a, _ in funcs}
    missing, manifest, bad, unresolved = set(), [], 0, []
    with Pool(jobs, _init, (image_file(mod), set(found), base, forward, setjmp_targets(mod), pads)) as pool:
        tasks = [(i, c, outdir) for i, c in enumerate(chunks)]
        for fname, ext, insns, biggest, b, unres in pool.imap_unordered(_emit, tasks):
            missing |= ext - defined
            manifest.append((fname, insns, biggest))
            bad += b
            unresolved += unres
    table, imports, relocs = module_table(mod, name, base, funcs, missing)
    write_if_changed(os.path.join(outdir, "g_module.c"), table)
    keep = {n for n, _i, _b in manifest} | {"g_module.c"}
    for old in os.listdir(outdir):   # drop files left over from a different partitioning
        if old.startswith("g") and old.endswith((".c", ".obj")) and os.path.splitext(old)[0] + ".c" not in keep:
            os.remove(os.path.join(outdir, old))
    write_if_changed(os.path.join(outdir, "manifest.tsv"), "".join(f"{n}\t{i}\t{b}\n" for n, i, b in sorted(manifest)))
    size = sum(os.path.getsize(os.path.join(outdir, n)) for n, _i, _b in manifest)
    print(f"{name}: {len(manifest)} files, {size / 1048576:,.0f} MB of C, {len(defined):,} functions, "
          f"{sum(i for _n, i, _b in manifest):,} instructions; {bad} untranslated, {len(missing)} calls to nowhere, "
          f"{len(unresolved)} unresolved jump tables, {len(forward)} replaced natively; "
          f"{imports:,} symbols referenced by {relocs:,} relocations")


def stale_files(outdir):
    """What compile_dir compiles, in order: (source, object, optimisation level, estimated
    seconds: progress.file_cost) of each file whose object is missing or older than it."""
    with open(os.path.join(outdir, "manifest.tsv")) as f:
        rows = [line.split("\t") for line in f.read().splitlines()]
    todo = []
    for name, insns, biggest in rows + [["g_module.c", "0", "0"]]:
        src = os.path.join(outdir, name)
        obj = src[:-2] + ".obj"
        if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(src):
            # very large functions make the optimiser superlinear; build those at a lower level
            todo.append((src, obj, "/O1" if int(biggest) > 20000 or name == "g_module.c" else "/O2",
                         progress.TABLE if name == "g_module.c" else progress.file_cost(int(insns), int(biggest))))
    return todo


def compile_dir(outdir, jobs, shown=None):
    """Compile a module's stale files; `shown`, a progress.Build, follows them."""
    import cc
    todo = stale_files(outdir)
    for _src, obj, _opt, _cost in todo:   # a new file, not one written through (it may be a hard link: modcache.replace_file)
        if os.path.exists(obj):
            os.remove(obj)
    t0 = time.time()
    if shown:
        shown.begin("compile", os.path.basename(outdir), [(src, cost) for src, _obj, _opt, cost in todo])
    failures = cc.compile_each([t[:3] for t in todo], jobs, *((shown.started, shown.finished) if shown else ()))
    if shown:
        shown.end("compile", os.path.basename(outdir))
    objs = [n for n in os.listdir(outdir) if n.endswith(".obj")]
    total = sum(os.path.getsize(os.path.join(outdir, n)) for n in objs)
    print(f"{os.path.basename(outdir)}: compiled {len(todo) - len(failures)} of {len(todo)} stale files in "
          f"{time.time() - t0:,.0f}s; {len(objs)} objects, {total / 1048576:,.0f} MB")
    for src, err in failures[:5]:
        print(f"FAILED {os.path.basename(src)}:\n{err[:1500]}")
    return not failures


def link(dirs, exe, rt_dir=None):
    """Compile the host runtime and link it with the given module build directories. The
    runtime's objects go to rt_dir (default: rt beside the exe)."""
    import cc
    t0 = time.time()
    rt_dir = rt_dir or os.path.join(os.path.dirname(exe), "rt")
    os.makedirs(rt_dir, exist_ok=True)
    headers = max(os.path.getmtime(os.path.join(cc.RUNTIME, n)) for n in os.listdir(cc.RUNTIME) if n.endswith(".h"))
    # the compile command the objects were built with: cc.CFLAGS change as SDL3, libopus or libvpx come and go
    stamp, flags = os.path.join(rt_dir, "cflags.txt"), " ".join([cc.CLANG, *cc.CFLAGS]) + "\n"
    old = ""
    if os.path.exists(stamp):
        with open(stamp, encoding="utf-8") as f:
            old = f.read()
    every = old != flags
    sources = [n for n in os.listdir(cc.RUNTIME) if re.fullmatch(r"(host|kernel|gpu\w*|hle\w*|ipc\w*|debug\w*|net\w*)\.c", n)]
    jobs = []
    for n in sources:
        src, obj = os.path.join(cc.RUNTIME, n), os.path.join(rt_dir, n[:-2] + ".obj")
        if every or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), headers):
            jobs.append((src, obj, "/O2"))
    # Native implementations by name, for symbols no module defines.
    names = sorted(hle_names())
    table = os.path.join(rt_dir, "hle_table.c")
    changed = write_if_changed(table, '#include "host.h"\n\n' + "".join(f"GuestFn hle_{n};\n" for n in names)
                               + "const HleEntry hle_table[] = {\n" + "".join(f'    {{"{n}", hle_{n}}},\n' for n in names)
                               + f"    {{0, 0}}\n}};\nconst u32 hle_count = {len(names)};\n")
    if changed or every or not os.path.exists(table[:-2] + ".obj"):
        jobs.append((table, table[:-2] + ".obj", "/O1"))
    # The run-time loadable modules (NROs) linked in, for nn::ro to find by build id.
    nros = [os.path.basename(os.path.normpath(d)) for d in dirs
            if os.path.basename(os.path.dirname(os.path.normpath(d))) == "nro"]
    nro_table = os.path.join(rt_dir, "nro_table.c")
    changed = write_if_changed(nro_table, '#include "module.h"\n\n'
                               + "".join(f"extern const RtModule rt_module_{n};\n" for n in nros)
                               + "const RtModule *const rt_nro_modules[] = {"
                               + "".join(f"&rt_module_{n}, " for n in nros) + "0};\n"
                               + f"const u32 rt_nro_count = {len(nros)};\n")
    if changed or every or not os.path.exists(nro_table[:-2] + ".obj"):
        jobs.append((nro_table, nro_table[:-2] + ".obj", "/O1"))
    failures = cc.compile_each(jobs, 4)
    for src, err in failures:
        print(f"FAILED {os.path.basename(src)}:\n{err[:4000]}")
    if failures:
        return False
    write_if_changed(stamp, flags)   # (only now: a failed compile is tried again)
    objs = [os.path.join(rt_dir, n[:-2] + ".obj") for n in sources] + [table[:-2] + ".obj", nro_table[:-2] + ".obj"]
    for d in dirs:
        objs += sorted(os.path.join(d, n) for n in os.listdir(d) if n.endswith(".obj"))
    # a wrap no module has (a made-up game's modules, tools/mac/boottest): its original is a stub nothing calls
    cc.link(objs, exe, [f"/alternatename:f_{a:x}_orig=rt_wrap_missing" for a in sorted(hle_wraps())])
    # the controllers' SDL3.dll, which the runtime loads from the exe's own folder
    if cc.SDL3_DLL:
        import shutil
        dll = os.path.join(os.path.dirname(exe), "SDL3.dll")
        if not os.path.exists(dll) or os.path.getsize(dll) != os.path.getsize(cc.SDL3_DLL) or \
                os.path.getmtime(dll) < os.path.getmtime(cc.SDL3_DLL):
            shutil.copy2(cc.SDL3_DLL, dll)
    print(f"linked {len(objs)} objects into {exe} ({os.path.getsize(exe) / 1048576:,.0f} MB) in {time.time() - t0:,.0f}s")
    return True


def module_paths(exefs):
    """name -> module file, for the ExeFS's modules and dump/nro's NROs (placing new NROs)."""
    nros = nro_modules()
    MODULES.update(nros)
    return {name: os.path.join(NRO_DIR, name + ".nro") if name in nros else os.path.join(exefs, name) for name in MODULES}


def check(exefs, mods=None):
    """The imports no module exports and the runtime does not implement, listed before
    anything is compiled. mods: the modules, if loaded already (name -> Module)."""
    mods = mods or {name: Module(path) for name, path in module_paths(exefs).items()}
    return modcache.report_imports(mods, hle_names(), os.path.join(ROOT, "build", "imports.unresolved.txt"))


def build_all(exefs, out, jobs, compile_c=True):
    """Every module's C first, then each module's compile, then the link: once the C is there,
    what is left to compile is known, and the progress bar's time left (progress.Build) can say
    how long it takes. compile_c False (--generate-only): the C only."""
    paths = module_paths(exefs)
    mods = {name: Module(path) for name, path in paths.items()}
    check(exefs, mods)
    fp = tools_fingerprint()
    targets = {name: os.path.join(out, "nro", name) if os.path.dirname(nso) == NRO_DIR else os.path.join(out, name)
               for name, nso in paths.items()}
    shown = progress.Build(min(jobs, 16), compile=compile_c, link=progress.LINK if compile_c else None)
    for name, mod in mods.items():
        current = modcache.build_info(targets[name]) == (mod.build_id, MODULES[name], name, fp)
        shown.module(name, (mod.text.end - mod.text.mem) // 4,
                     [(t[0], t[3]) for t in stale_files(targets[name])] if current else None)
    build_ids = {name: mod.build_id for name, mod in mods.items()}
    del mods   # (the compilers want the memory)
    exe = os.path.join(out, "ssbu.exe")
    with progress.Line(shown.status, title=f"building {os.path.basename(exe)}" if compile_c else "writing the game's C"):
        reused = 0
        for name, nso in paths.items():
            shown.begin("gen", name)
            target, build_id, base = targets[name], build_ids[name], MODULES[name]
            img = os.path.join(ROOT, "dump", "modules", name + ".reloc.bin")
            if not os.path.exists(img) or os.path.getmtime(img) < os.path.getmtime(nso):
                image(nso, img)
            if modcache.build_info(target) == (build_id, base, name, fp):
                print(f"{name}: C up to date (build id {build_id[:16]}, same tools)")
                reused += 1
            else:
                build(nso, target, jobs, 40000)
            if not compile_c:
                modcache.write_stamp(target, build_id, base, name, fp)
            shown.written(name, [(t[0], t[3]) for t in stale_files(target)])
            shown.end("gen", name)
        print(f"{reused} of {len(paths)} modules reused without generating their C again")
        if not compile_c:
            print(f"the C of {len(paths)} modules is in {out} (--generate-only: not compiled here)")
            return True
        for name in paths:
            if not compile_dir(targets[name], min(jobs, 16), shown):
                return False
            modcache.write_stamp(targets[name], build_ids[name], MODULES[name], name, fp)
        shown.begin("link")
        ok = link(list(targets.values()), exe)
        shown.end("link")
        return ok


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, nso = sys.argv[1], sys.argv[2]
    if cmd == "coverage":
        coverage(nso, int(arg("--jobs", "20")))
    elif cmd == "emit":
        roots = [int(r, 16) for r in arg("--roots", "").split(",") if r]
        emit(nso, roots, arg("--out"), int(arg("--depth", "8")), int(arg("--sample", "0")))
    elif cmd == "image":
        image(nso, arg("--out"))
    elif cmd == "build":
        build(nso, arg("--out"), int(arg("--jobs", "20")), int(arg("--per-file", "40000")))
    elif cmd in ("compile", "link") and sys.platform == "darwin":
        sys.exit(f"recomp.py {cmd} builds with clang-cl, for Windows: on macOS, tools/mac/build_mac.py compiles and links")
    elif cmd == "compile":
        sys.exit(0 if compile_dir(nso, int(arg("--jobs", "16"))) else 1)
    elif cmd == "link":
        sys.exit(0 if link(arg("--dirs").split(","), arg("--out"), arg("--rt", None)) else 1)
    elif cmd == "all":
        # (on macOS the C is compiled by tools/mac/build_mac.py, the compiling and linking here being clang-cl's)
        compile_c = "--generate-only" not in sys.argv and sys.platform != "darwin"
        sys.exit(0 if build_all(nso, arg("--out"), int(arg("--jobs", "20")), compile_c) else 1)
    elif cmd == "check":
        check(nso)
    elif cmd == "wrap-sig":
        m = Module(nso)
        a = int(sys.argv[3], 16)
        off = a - base_of(nso)
        sizes = dict(load(nso)[2])
        if off not in sizes:
            sys.exit(f"{a:x} is not a function start in {nso}")
        print(f"/* signature: {' '.join(signature(m, off, sizes[off]))} */")
    else:
        sys.exit(__doc__)
