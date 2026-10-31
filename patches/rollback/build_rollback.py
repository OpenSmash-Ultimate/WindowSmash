"""Links build\\ssbu_rollback.exe: WindowSmash's runtime + the nvfix patches + patches\\rollback\\src,
assembled in build\\rb_root\\runtime (runtime\\ itself is never modified), objects in build\\rt_rollback.

The rollback runtime wraps guest functions (HLE_WRAP), which changes the generated C of the module
they are in, so that module gets its own copy here: main is built in build\\main_rb (seeded once
from build\\main, so only the files the wraps change are compiled again). The other modules'
objects are shared with the normal build. See patches\\rollback\\README.md."""
import filecmp
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RUNTIME = os.path.join(ROOT, "runtime")
NVFIX = os.path.join(ROOT, "patches", "nvfix")
RB_ROOT = os.path.join(ROOT, "build", "rb_root")   # stands in for ROOT where the tools look for runtime\
SRC_DIR = os.path.join(RB_ROOT, "runtime")
MAIN, MAIN_RB = os.path.join(ROOT, "build", "main"), os.path.join(ROOT, "build", "main_rb")


def assemble():
    if os.path.isdir(SRC_DIR):
        shutil.rmtree(SRC_DIR)
    shutil.copytree(RUNTIME, SRC_DIR, ignore=shutil.ignore_patterns("*.orig"))
    # 1. the nvfix patches, when they were made for this runtime
    patched = [n for n in os.listdir(NVFIX) if n.endswith(".c.patched")] if os.path.isdir(NVFIX) else []
    if patched and all(filecmp.cmp(os.path.join(RUNTIME, n[:-8]), os.path.join(NVFIX, n[:-8] + ".orig"), shallow=False)
                       for n in patched):
        for n in patched:
            shutil.copy2(os.path.join(NVFIX, n), os.path.join(SRC_DIR, n[:-8]))
        print(f"[rb build] nvfix patches applied: {', '.join(n[:-8] for n in patched)}")
    else:
        print("[rb build] WARNING: nvfix patches missing or made for another runtime; building without them")
    # 2. this build's own files; a changed runtime file must still match the original it was made from
    base, src = os.path.join(HERE, "base"), os.path.join(HERE, "src")
    for n in sorted(os.listdir(src)):
        if os.path.exists(os.path.join(base, n)) and \
                not filecmp.cmp(os.path.join(RUNTIME, n), os.path.join(base, n), shallow=False):
            sys.exit(f"[rb build] runtime\\{n} changed since the rollback patch was made (base\\{n}): re-make the patch")
        shutil.copy2(os.path.join(src, n), os.path.join(SRC_DIR, n))
        print(f"[rb build] {'changed' if os.path.exists(os.path.join(base, n)) else 'added'}: {n}")


def point_tools_at_rb_root():
    """cc compiles the runtime from cc.RUNTIME (baked into CFLAGS at import); recomp's hle_names / hle_wraps
    scan ROOT\\runtime. All of them are pointed at the assembled tree."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import cc
    cc.CFLAGS = [SRC_DIR if a == cc.RUNTIME else a for a in cc.CFLAGS]
    cc.RUNTIME = SRC_DIR
    # a linker map beside the exe: crash and freeze reports name exe offsets, and this build has no PDB
    link = cc.link
    cc.link = lambda objs, exe, extra=(): link(objs, exe, [*extra, "/MAP:" + exe[:-4] + ".map"])
    import recomp
    for name in ("hle_names", "hle_wraps"):
        orig = getattr(recomp, name)

        def scan(orig=orig):
            root, recomp.ROOT = recomp.ROOT, RB_ROOT
            try:
                return orig()
            finally:
                recomp.ROOT = root
        setattr(recomp, name, scan)
    return recomp


def build_main(recomp):
    import modcache
    if not os.path.isdir(MAIN_RB):
        print("[rb build] first time: copying build\\main to build\\main_rb (1 GB)")
        shutil.copytree(MAIN, MAIN_RB)   # (copy2: the times stay, so unchanged objects stay up to date)
    nso = os.path.join(ROOT, "dump", "exefs", "main")
    build_id, base, fp = recomp.Module(nso).build_id, recomp.MODULES["main"], recomp.tools_fingerprint()
    if modcache.build_info(MAIN_RB) != (build_id, base, "main", fp):
        print("[rb build] generating main's C with the rollback wraps (a few minutes)")
        recomp.build(nso, MAIN_RB, 20, 40000)
    if not recomp.compile_dir(MAIN_RB, 16):
        sys.exit("[rb build] main_rb did not compile")
    modcache.write_stamp(MAIN_RB, build_id, base, "main", fp)


def main():
    with open(os.path.join(NVFIX, "dirs.txt"), encoding="utf-8") as f:
        dirs = [os.path.join(ROOT, d) for d in f.read().strip().split(",")]
    if not os.path.isdir(MAIN):
        sys.exit("Build WindowSmash first (Build WindowSmash.bat): the test build reuses its game modules.")
    assemble()
    recomp = point_tools_at_rb_root()
    build_main(recomp)
    dirs = [MAIN_RB if os.path.normpath(d) == MAIN else d for d in dirs]
    ok = recomp.link(dirs, os.path.join(ROOT, "build", "ssbu_rollback.exe"), os.path.join(ROOT, "build", "rt_rollback"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
