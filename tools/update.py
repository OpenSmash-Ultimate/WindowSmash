#!/usr/bin/env python3
"""Apply the game's 13.0.5 update (a Patch NSP) over the game's own data.

Usage:
  update.py apply  <update> --base <base romfs.cfg> --keys <dir>
  update.py verify <update> --keys <dir>

<update> is the update's Patch NSP, an NSP holding the game and its update together (the update's
Program NCA is the one whose RomFS is a patch; --inner <name> names it), or its Program NCA on its
own (a .nca file, or the 00 file of one an emulator installed).

`apply` fills dump/ (what tools/build_game.py step 6 runs):
  exefs/                        the update's ExeFS (main, sdk, subsdk0, rtld, main.npdm)
  modules/romfs_patch.bin       the update's own RomFS data, decrypted
  modules/romfs.layers          where each piece of the updated RomFS comes from: the base's
                                file, romfs_patch.bin, or zeros (nca.write_layers)
  modules/romfs.cfg             "layered" and those files: what the runtime and tools read
  modules/romfs.unverified.txt  only if some 16 KB blocks could not be confirmed (see below)
`verify` reads the updated RomFS back through romfs.cfg and checks every block's hash.

An update's RomFS is a patch (BKTR) over the RomFS of the original release: a map of the new
image whose pieces are either the update's own data or bytes of the original base at given
offsets (in the base NCA's RomFS section). When the base is that original release, unpacked
(its RomFS section stored plain, which is what the runtime reads in place), the patch applies
exactly: every base piece is those bytes of the base's file. That is tried first - blocks
sampled across the image are checked against the update's hashes - and the whole result is
verified at the end like any other.

Otherwise the base need not be the original release: when it is an image with an earlier
update merged in (for example 11.0.0 rebuilt into one plain NCA), the patch's base offsets point
nowhere useful. What remains true is that every base piece the patch asks for is data of the
original release, and the merged base still holds that data - moved, file by file, folder by
folder. So the pieces are found again:

  1. data.arc's file tables (both versions) give every file's place in the new image and in the
     base; a new file inside a base piece takes its bytes from the same file in the base
     (same path, or else any base file of the same sizes), music and movies likewise from the
     stream tables, and the padding between files from its neighbours or zeros;
  2. every 16 KB block of the new image has a SHA-256 in the update's hash tree (IVFC level 5),
     so each choice is checked; a block that does not match is solved again with more
     candidates: the shift of the nearest confirmed data in the original release's own order
     (the patch also borrows runs of bytes from other files of the original), the same bytes
     elsewhere in the new image, zeros - per piece, then several pieces at once.

With 13.0.5 over a merged 11.0.0 base this confirmed all but 8 of 1,074,277 blocks (128 KB, inside
a few models' material and texture files whose original-release bytes are no longer in the
merged base); those keep the best guess and are listed in romfs.unverified.txt.

The hash tree is part of the patched image too, so an update could reuse the original release's
hashes for blocks that are the same as in that release. 13.0.5 carries a hash of its own for
every block; an update that doesn't is not 13.0.5's, and is refused (Patch.hash_level).
"""
import argparse
import bisect
import hashlib
import itertools
import os
import struct
import sys
import time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arc import Arc, data_arc_offset  # noqa: E402
from game_version import needs  # noqa: E402
from nca import LAYER_ZERO, Nca, RomfsImage, copy_out, nsp_ncas, pfs0_entries, write_layers  # noqa: E402

ZERO = "zero"        # a piece of zeros
DUMP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dump")
NODE = 0x4000        # bucket tree node size


def log(msg):
    print(msg, flush=True)


# ---- the update: its Program NCA, the patch tables, the hash tree ----------------------
def bucket_tree(table, entry_size, fmt):
    """Entries of a BKTR bucket tree (one level of offset nodes), and the end offset."""
    _, sets, end = struct.unpack_from("<IIQ", table, 0)
    if sets > (NODE - 0x10) // 8:
        raise SystemExit("patch table with more than one level of offset nodes: not supported")
    out = []
    for k in range(sets):
        o = NODE * (1 + k)
        _, n, _ = struct.unpack_from("<IIQ", table, o)
        out += [struct.unpack_from(fmt, table, o + 0x10 + j * entry_size) for j in range(n)]
    return out, end


def pick_patch_program(ncas, inner=None, where="the update"):
    """The update's Program NCA among a package's [(name, Nca)]: the one named `inner` if given;
    else the only Program NCA; of several (a package holding the game and its update together),
    the one whose RomFS is a patch (BKTR) - the base game's is a whole RomFS."""
    if inner:
        named = [n for name, n in ncas if name.lower() == inner.lower()]
        if not named:
            raise SystemExit(f"{where} holds no {inner}")
        return named[0]
    progs = [n for _name, n in ncas if n.content_type == 0]
    if len(progs) > 1:
        patches = [n for n in progs if n.section(0) is not None and n.section(0).enc_type == 4]
        progs = patches or progs
    if len(progs) != 1:
        raise SystemExit(f"{where}: expected one update Program NCA, found {len(progs)}")
    return progs[0]


class Patch:
    def __init__(self, nsp, keydir, inner=None):
        """The update in `nsp`: an update package, a package of the game with its update, or the
        update's Program NCA itself; `inner` names the Program NCA inside the package."""
        ncas = nsp_ncas(nsp, keydir)   # (a package, or the program .nca itself)
        self.nca = pick_patch_program(ncas, inner, nsp)
        self.exefs = self.nca.section(1)
        sec = self.sec = self.nca.section(0)
        if sec is None or sec.enc_type != 4:
            raise SystemExit("the update's RomFS is not a patch (BKTR) section")
        fsh = sec.fs_header
        ind_off, ind_size = struct.unpack_from("<QQ", fsh, 0x100)
        ctr_off, ctr_size = struct.unpack_from("<QQ", fsh, 0x120)
        generation, self.secure = struct.unpack_from("<II", fsh, 0x140)
        if fsh[0x110:0x114] != b"BKTR" or fsh[0x130:0x134] != b"BKTR":
            raise SystemExit("patch tables missing")
        self.ind, self.vend = bucket_tree(sec.read_raw(ind_off, ind_size), 0x14, "<QQi")
        self.ctr, self.cend = bucket_tree(sec.read_raw(ctr_off, ctr_size), 0x10, "<QIi")
        self.iv = [e[0] for e in self.ind]
        self.cv = [e[0] for e in self.ctr]
        self.data_end = ind_off              # the patch's own data comes before its tables
        ivfc = fsh[0x08:0x100]
        if ivfc[:4] != b"IVFC":
            raise SystemExit("no hash tree (IVFC) on the update's RomFS")
        self.levels = [struct.unpack_from("<QQI", ivfc, 0x10 + i * 0x18) for i in range(6)]   # (offset, size, block size log2)
        self.l6, self.l6size, blocklog = self.levels[5]
        self.block = 1 << blocklog
        self.blocks = (self.l6size + self.block - 1) // self.block
        self.hashes = self.hash_level(4)    # level 5: one SHA-256 per level-6 block

    def hash_level(self, i):
        """Hash level i + 1 (0 = level 1 ... 4 = level 5), which the update carries whole: one
        that takes some of it from the original release isn't 13.0.5's (which carries its own)."""
        off, size, _ = self.levels[i]
        out = bytearray()
        for _pv, n, st, ph in self.pieces(off, size):
            if st != 1:
                raise SystemExit(needs("this update takes some of its hashes from the original release, "
                                       "which the 13.0.5 update doesn't"))
            out += self.storage(ph, n)
        return bytes(out)

    def storage(self, off, size):
        """The patch's own data (section-relative), decrypted: each region has its own counter."""
        out = bytearray()
        while size:
            k = bisect.bisect_right(self.cv, off) - 1
            end = self.cv[k + 1] if k + 1 < len(self.cv) else self.cend
            n = min(size, end - off)
            out += self.sec.read_raw(off, n, struct.pack(">II", self.secure, self.ctr[k][2]))
            off += n
            size -= n
        return bytes(out)

    def pieces(self, v, size):
        """(virtual offset, length, storage, physical offset) covering [v, v + size)."""
        while size:
            k = bisect.bisect_right(self.iv, v) - 1
            end = self.iv[k + 1] if k + 1 < len(self.iv) else self.vend
            n = min(size, end - v)
            yield v, n, self.ind[k][2], self.ind[k][1] + (v - self.ind[k][0])
            v += n
            size -= n

    def vread(self, v, size, base=None):
        """The updated section's bytes; base(orig offset, n, virtual offset) supplies base pieces."""
        out = bytearray()
        for pv, n, st, ph in self.pieces(v, size):
            if st == 1:
                out += self.storage(ph, n)
            elif base:
                out += base(ph, n, pv)
            else:
                raise LookupError(f"bytes at {pv:#x} come from the original release (at {ph:#x})")
        return bytes(out)

    def block_hash(self, b):
        """Level-6 block b's SHA-256."""
        return self.hashes[b * 32:b * 32 + 32]


# ---- finding the base pieces again --------------------------------------------------------
class Seg:
    """A piece of the new level-6 image [v, e) (virtual offsets): kind 1 = the patch's own data
    at `off`; kind 0 = original-release data from `orig`, to be found in the base: `cands` are
    deltas (base file offset - v), ZERO, or ("p", patch data offset); `pick` is the choice."""
    __slots__ = ("v", "e", "kind", "off", "orig", "cands", "pick")

    def __init__(self, v, e, kind, off=0, orig=0, cands=None):
        self.v, self.e, self.kind, self.off, self.orig, self.cands, self.pick = v, e, kind, off, orig, cands or [], None

    def split(self, at):
        """Cut at `at`: this piece keeps [v, at), the one returned has [at, e)."""
        moved = lambda d: ("p", d[1] + (at - self.v)) if isinstance(d, tuple) else d  # noqa: E731
        t = Seg(at, self.e, self.kind, self.off + (at - self.v), self.orig + (at - self.v), [moved(d) for d in self.cands])
        t.pick = moved(self.pick)
        self.e = at
        return t


class Builder:
    def __init__(self, patch, base_cfg, keydir=None):
        self.p = patch
        img = RomfsImage(base_cfg)
        if img.entries is not None:
            raise SystemExit("the base's romfs.cfg is layered: give the base release's own romfs.cfg")
        self.base_path = os.path.abspath(img.f.name)
        self.bf = open(self.base_path, "rb")
        self.base_l6 = img.base                              # the base's RomFS image in its file
        self.base_arc = img.base + data_arc_offset(img)      # data.arc in the base's file
        self.l6 = patch.l6
        self.keydir = keydir

    def base_read(self, off, n):
        if off < 0:
            return b"\xfe" * n
        self.bf.seek(off)
        return self.bf.read(n)

    # 0. the original release itself
    def base_section(self):
        """Where the base's RomFS section starts in its file - what the patch's base offsets
        count from - or None if the base's file is not an NCA with its RomFS where romfs.cfg says."""
        if not self.keydir:
            return None
        try:
            nca = Nca(self.base_path, self.keydir)
        except (SystemExit, OSError, ValueError):
            return None
        sec = nca.section(0)
        if sec is None or sec.hash_type != 3 or nca.base + sec.start + sec.data_off != self.base_l6:
            return None
        return nca.base + sec.start

    def exact_segments(self, start):
        """The pieces as the patch says: base pieces are the base's bytes at their offsets."""
        p, segs = self.p, []
        for i, (v, ph, st) in enumerate(p.ind):
            end = p.iv[i + 1] if i + 1 < len(p.iv) else p.vend
            v0, v1 = max(v, p.l6), min(end, p.l6 + p.l6size)
            if v0 >= v1:
                continue
            if st == 1:
                segs.append(Seg(v0, v1, 1, off=ph + (v0 - v)))
                continue
            orig = ph + (v0 - v)
            s = Seg(v0, v1, 0, orig=orig, cands=[start + orig - v0])
            s.pick = s.cands[0]
            segs.append(s)
        self.segs, self.sv = segs, [s.v for s in segs]

    def is_original(self, start, samples=256):
        """Whether the base is the release the patch was made for: blocks with base pieces,
        sampled across the image, match the update's hashes when taken as the patch says."""
        self.exact_segments(start)
        blocks = sorted({(s.v - self.l6) // self.p.block for s in self.segs if s.kind == 0})   # (each piece's first)
        sample = blocks[::max(1, len(blocks) // samples)]
        return bool(sample) and all(self.ok(b, {}) for b in sample)

    # 1. the two data.arc's
    def tables(self):
        p = self.p
        # the RomFS header may be original-release data too: read that at the same place in the
        # base's image (verified later anyway)
        same_place = lambda ph, n, pv: self.base_read(self.base_l6 + (pv - p.l6), n)  # noqa: E731

        class Image:   # the new RomFS image (level 6), for romfs_entries
            start = data_off = 0
            enc_type = 1

            def read(_self, off, size):
                return p.vread(p.l6 + off, size, same_place)
        self.arc_v = p.l6 + data_arc_offset(Image())         # data.arc in the new image (virtual)
        # data.arc's header may itself be original-release data: until the base pieces are
        # found, read those at the same offset in the base's data.arc (verified later anyway)
        first = next(((v, ph) for v, n, st, ph in p.pieces(self.arc_v, 0x1000) if st == 0), None)
        orig_arc = first[1] - (first[0] - self.arc_v) if first else 0
        provisional = lambda ph, n, pv: self.base_read(self.base_arc + (ph - orig_arc), n)  # noqa: E731
        new = Arc(lambda off, n: p.vread(self.arc_v + off, n, provisional))
        old = Arc(lambda off, n: self.base_read(self.base_arc + off, n))
        v = new.version
        log(f"data.arc {v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF} over the base's {old.version >> 16}.{(old.version >> 8) & 0xFF}."
            f"{old.version & 0xFF}; reading their file tables")
        return new, old

    def extents(self, new, old):
        """virtual start -> [length, candidate base offsets], for every file of the new data.arc"""
        t0 = time.time()
        A, B = self.arc_v, self.base_arc
        f_new, f_old = new.files_by_path(), old.files_by_path()
        s_new, s_old = new.streams(), old.streams()
        r_new, r_old = set(new.reachable()), set(old.reachable())
        log(f"  tables read ({time.time() - t0:.0f}s)")
        by_size = defaultdict(set)
        for o, st, sz, _fl in r_old | set(f_old.values()):
            if st:
                by_size[(st, sz)].add(B + o)
        for o, sz in s_old.values():
            if sz:
                by_size[(sz, sz)].add(B + o)
        by_size = {k: sorted(v) for k, v in by_size.items()}
        firsts, keys = defaultdict(list), defaultdict(set)   # v -> same-path twins; v -> (stored, size)
        lengths = defaultdict(int)
        for k, (o, st, sz, _fl) in f_new.items():
            if st:
                t = f_old.get(k)
                if t and t[1] == st and B + t[0] not in firsts[A + o]:
                    firsts[A + o].append(B + t[0])
                keys[A + o].add((st, sz))
        for k, (o, sz) in s_new.items():
            if sz:
                t = s_old.get(k)
                if t and t[1] == sz and B + t[0] not in firsts[A + o]:
                    firsts[A + o].append(B + t[0])
                keys[A + o].add((sz, sz))
        for o, st, sz, _fl in r_new:
            if st:
                keys[A + o].add((st, sz))
        ext = {}
        for v, ks in keys.items():
            cands = list(firsts.get(v, ()))
            for kk in sorted(ks):
                cands += by_size.get(kk, ())
            ext[v] = [max(kk[0] for kk in ks), list(dict.fromkeys(cands))]
        self.ext = ext
        self.keys = defaultdict(list)       # (stored, size) -> new-image starts, for duplicates
        for v, ks in keys.items():
            for kk in ks:
                self.keys[kk].append(v)
        self.key_of = keys
        self.key_starts = sorted(keys)
        log(f"{len(f_new):,} paths, {len(s_new):,} streams; {len(ext):,} file extents in the new image ({time.time() - t0:.0f}s)")

    def segments(self):
        p, ext = self.p, self.ext
        starts = sorted(ext)
        segs = []
        for i, (v, ph, st) in enumerate(p.ind):
            end = p.iv[i + 1] if i + 1 < len(p.iv) else p.vend
            v0, v1 = max(v, p.l6), min(end, p.l6 + p.l6size)
            if v0 >= v1:
                continue
            if st == 1:
                segs.append(Seg(v0, v1, 1, off=ph + (v0 - v)))
                continue
            orig = ph + (v0 - v)
            j = bisect.bisect_left(starts, v0)
            while j > 0 and starts[j - 1] + ext[starts[j - 1]][0] > v0:
                j -= 1
            cur, parts = v0, []
            while j < len(starts) and starts[j] < v1:
                s0 = starts[j]
                length, cands = ext[s0]
                if s0 + length > cur:
                    a, b = max(cur, s0), min(v1, s0 + length)
                    if a > cur:
                        parts.append([cur, a, None])
                    parts.append([a, b, [c - s0 for c in cands]])
                    cur = b
                j += 1
            if cur < v1:
                parts.append([cur, v1, None])
            for q, (a, b, deltas) in enumerate(parts):
                if deltas is None:   # padding between files: as its neighbours, or zeros
                    deltas = []
                    for r in (q - 1, q + 1):
                        if 0 <= r < len(parts) and parts[r][2]:
                            deltas += [d for d in parts[r][2][:2] if d not in deltas]
                    deltas.append(ZERO)
                segs.append(Seg(a, b, 0, orig=orig + (a - v0), cands=deltas))
        self.segs = segs
        self.sv = [s.v for s in segs]
        log(f"{len(segs):,} pieces ({sum(s.e - s.v for s in segs if s.kind == 0) / 2**30:.1f} GiB from the original release)")

    # 2. checking against the hash tree
    def part(self, s, a, b, d):
        if s.kind == 1:
            return self.p.storage(s.off + (a - s.v), b - a)
        if d is None or d == ZERO:
            return bytes(b - a)
        if isinstance(d, tuple):
            return self.p.storage(d[1] + (a - s.v), b - a)
        return self.base_read(a + d, b - a)

    def in_block(self, b):
        v0 = self.l6 + b * self.p.block
        v1 = min(v0 + self.p.block, self.l6 + self.p.l6size)
        k = bisect.bisect_right(self.sv, v0) - 1
        out = []
        while k < len(self.segs) and self.segs[k].v < v1:
            out.append(k)
            k += 1
        return v0, v1, out

    def choice(self, k):
        s = self.segs[k]
        return s.pick if s.pick is not None else (s.cands[0] if s.cands else None)

    def ok(self, b, picks):
        """Whether block b matches its hash with these choices."""
        return self.digest(b, picks) == self.p.block_hash(b)

    def digest(self, b, picks):
        """Block b's SHA-256 with these choices (the others as they are)."""
        v0, v1, ks = self.in_block(b)
        h, n = hashlib.sha256(), 0
        for k in ks:
            s = self.segs[k]
            a, e = max(v0, s.v), min(v1, s.e)
            h.update(self.part(s, a, e, picks[k] if k in picks else self.choice(k)))
            n += e - a
        if n < self.p.block:
            h.update(bytes(self.p.block - n))
        return h.digest()

    def distinct(self, k, b, cands):
        """The candidates that give different bytes for piece k within block b."""
        v0, v1, _ = self.in_block(b)
        s = self.segs[k]
        a, e = max(v0, s.v), min(v1, s.e)
        seen, out = set(), []
        for d in cands:
            h = hashlib.sha256(self.part(s, a, e, d)).digest()
            if h not in seen:
                seen.add(h)
                out.append(d)
        return out

    def first_pass(self):
        """Every block with the first choices; a failing block tries the combinations of its
        undecided pieces. A piece is decided by the first block it is confirmed in."""
        decided, bad = set(), []
        t0 = time.time()
        for b in range(self.p.blocks):
            v0, v1, ks = self.in_block(b)
            open_ = [k for k in ks if self.segs[k].kind == 0 and k not in decided]
            if self.ok(b, {}):
                picks = {}
            else:
                opts = [self.distinct(k, b, self.segs[k].cands) or [None] for k in open_]
                space = 1
                for o in opts:
                    space *= len(o)
                picks = None
                if space <= 50000:
                    for combo in itertools.product(*opts):
                        if self.ok(b, dict(zip(open_, combo))):
                            picks = dict(zip(open_, combo))
                            break
                if picks is None:
                    bad.append(b)
                    continue
            for k in open_:
                self.segs[k].pick = picks.get(k, self.choice(k))
                decided.add(k)
            if b % 100000 == 0 and b:
                log(f"  {b:,} of {self.p.blocks:,} blocks, {len(bad):,} not yet confirmed ({time.time() - t0:.0f}s)")
        self.decided = decided
        return bad

    def solve(self, bad):
        """Each remaining block on its own (its pieces cut at the block's edges), with more
        candidates, until no block more can be confirmed."""
        cuts = set()
        for b in bad:
            v0, v1, _ = self.in_block(b)
            cuts |= {v0, v1}
        self.cut(cuts)
        segs = self.segs
        bad_set = set(bad)
        for rnd in range(1, 6):
            pts = sorted((s.orig, s.pick + s.v - s.orig) for s in segs
                         if s.kind == 0 and isinstance(s.pick, int) and (s.v - self.l6) // self.p.block not in bad_set)
            ps = [q[0] for q in pts]
            still = []
            for b in bad:
                if self.solve_block(b, pts, ps, bad_set):
                    bad_set.discard(b)
                else:
                    still.append(b)
            log(f"  round {rnd}: {len(bad) - len(still)} more confirmed, {len(still)} left")
            if len(still) == len(bad):
                break
            bad = still
        return bad

    def cut(self, cuts):
        """The pieces cut at these virtual offsets."""
        cuts, segs = sorted(cuts), []
        for s in self.segs:
            i = bisect.bisect_right(cuts, s.v)
            while i < len(cuts) and cuts[i] < s.e:
                segs.append(s)
                s = s.split(cuts[i])
                i += 1
            segs.append(s)
        self.segs, self.sv = segs, [s.v for s in segs]

    def nearby(self, s, pts, ps, w=8):
        """Deltas of the confirmed data nearest to s in the original release's own order."""
        i = bisect.bisect_right(ps, s.orig) - 1
        return [s.orig + pts[j][1] - s.v for j in range(i - w, i + w + 1) if 0 <= j < len(pts)]

    def duplicates(self, s, bad_set):
        """The same bytes elsewhere in the new image (a file of the same sizes), already confirmed."""
        out = []
        i = bisect.bisect_right(self.key_starts, s.v) - 1
        while i >= 0 and self.key_starts[i] > s.v - 0x400000 and len(out) < 40:
            start = self.key_starts[i]
            for kk in self.key_of[start]:
                if not (start <= s.v and s.e <= start + kk[0]):
                    continue
                for other in self.keys[kk]:
                    a = other + (s.v - start)
                    j = bisect.bisect_right(self.sv, a) - 1
                    if other == start or j < 0 or self.segs[j].e < a + (s.e - s.v):
                        continue
                    t = self.segs[j]
                    if (a - self.l6) // self.p.block in bad_set:
                        continue
                    if t.kind == 1:
                        d = ("p", t.off + (a - t.v))
                    elif isinstance(t.pick, int):
                        d = a + t.pick - s.v
                    else:
                        continue
                    if d not in out:
                        out.append(d)
            i -= 1
        return out

    def solve_block(self, b, pts, ps, bad_set):
        v0, v1, ks = self.in_block(b)
        base = [k for k in ks if self.segs[k].kind == 0]
        opts = {}
        for k in base:
            s = self.segs[k]
            c = ([s.pick] if s.pick is not None else []) + s.cands + self.nearby(s, pts, ps) + self.duplicates(s, bad_set) + [ZERO]
            opts[k] = self.distinct(k, b, list(dict.fromkeys(c, None)))
        cur = {k: opts[k][0] for k in base}

        def accept(picks):
            for k, d in picks.items():
                self.segs[k].pick = d
            return True
        if self.ok(b, cur):
            return accept(cur)
        # one shift for all of the block's pieces (neighbouring files moved together)
        shifts = []
        for k in base:
            s = self.segs[k]
            shifts += [s.v + d - s.orig for d in opts[k] if isinstance(d, int)]
        for dm in dict.fromkeys(shifts):
            p = {k: self.segs[k].orig + dm - self.segs[k].v for k in base}
            if self.ok(b, p):
                return accept(p)
        # one, two, then three pieces at a time (within a budget of tries per block)
        budget = 200000
        for r in (1, 2, 3):
            for combo in itertools.combinations(base, r):
                space = 1
                for k in combo:
                    space *= len(opts[k])
                if space > 20000:
                    continue
                for ds in itertools.product(*[opts[k] for k in combo]):
                    p = dict(cur)
                    p.update(zip(combo, ds))
                    if self.ok(b, p):
                        return accept(p)
                budget -= space
                if budget < 0:
                    return False
        return False

    # 3. the result
    def layers(self):
        """(image offset, source, source offset) pieces: 0 = the base's file, 1 = romfs_patch.bin."""
        out = []
        for s in self.segs:
            at = s.v - self.l6
            if s.kind == 1:
                src, off = 1, s.off
            elif isinstance(s.pick, tuple):
                src, off = 1, s.pick[1]
            elif isinstance(s.pick, int):
                src, off = 0, s.v + s.pick
            else:
                src, off = LAYER_ZERO, 0
            if out:
                pat, psrc, poff = out[-1]
                if psrc == src and (src == LAYER_ZERO or poff + (at - pat) == off):
                    continue
            out.append((at, src, off))
        return out


def unconfirmed(patch, digests):
    """The level-6 blocks whose SHA-256 in `digests` (every block's, one after another) isn't the
    one the update has for it."""
    return [b for b in range(patch.blocks) if digests[b * 32:b * 32 + 32] != patch.hashes[b * 32:b * 32 + 32]]


def verify(patch, cfg, report=None):
    """Check every block of the RomFS that `cfg` describes against the update's hash tree."""
    img = RomfsImage(cfg)
    digests, t0 = bytearray(), time.time()
    for b in range(patch.blocks):
        data = img.read(b * patch.block, patch.block)
        if len(data) < patch.block:
            data += bytes(patch.block - len(data))
        digests += hashlib.sha256(data).digest()
        if b % 200000 == 0 and b:
            log(f"  verified {b:,} of {patch.blocks:,} blocks ({time.time() - t0:.0f}s)")
    bad = unconfirmed(patch, bytes(digests))
    log(f"{patch.blocks - len(bad):,} of {patch.blocks:,} blocks match the update's hash tree; {len(bad)} do not")
    if report:
        if bad:
            with open(report, "w", newline="\n") as f:
                f.write("# 16 KB blocks of the updated RomFS whose bytes are unconfirmed: <block> <image offset>\n")
                f.writelines(f"{b} {b * patch.block:#x}\n" for b in bad)
        elif os.path.exists(report):
            os.remove(report)
    return bad


def write_patch_data(patch, path, end):
    """The patch's own data, decrypted, as one plain file (romfs_patch.bin)."""
    chunk, done, t0 = 64 << 20, 0, time.time()
    with open(path + ".partial", "wb") as f:
        while done < end:
            n = min(chunk, end - done)
            f.write(patch.storage(done, n))
            done += n
            if done % (1 << 30) < chunk:
                log(f"  {path}: {done / 2**30:.1f} of {end / 2**30:.1f} GiB ({time.time() - t0:.0f}s)")
    os.replace(path + ".partial", path)


def open_patch(nsp, keydir, inner=None):
    patch = Patch(nsp, keydir, inner)
    log(f"update: patch data {patch.data_end / 2**30:.2f} GiB, {len(patch.ind):,} patch pieces, "
        f"new RomFS {patch.l6size / 2**30:.2f} GiB in {patch.blocks:,} blocks")
    return patch


def apply_exefs(patch, dump):
    """The update's ExeFS into <dump>/exefs."""
    log("ExeFS:")
    for name, off, size in pfs0_entries(patch.exefs):
        log(f"  {name:<12} {size:>10}")
        copy_out(patch.exefs, off, size, os.path.join(dump, "exefs", name))


def apply_romfs(patch, base_cfg, keydir, dump):
    """The updated RomFS over the base's: <dump>/modules/romfs.cfg and its files.
    Returns the blocks left unconfirmed."""
    t0 = time.time()
    modules = os.path.join(dump, "modules")
    os.makedirs(modules, exist_ok=True)
    b = Builder(patch, base_cfg, keydir)
    start = b.base_section()
    if start is not None and b.is_original(start):
        log("the base is the release this update was made for: applying the patch as it is")
    else:
        log("the base is not the release this update was made for (an earlier update merged in?): "
            "finding the original release's pieces in it")
        new, old = b.tables()
        b.extents(new, old)
        b.segments()
        log("checking each block against the update's hash tree:")
        bad = b.first_pass()
        log(f"first pass: {patch.blocks - len(bad):,} of {patch.blocks:,} blocks confirmed ({time.time() - t0:.0f}s)")
        if bad:
            b.solve(bad)

    layers = b.layers()
    patch_end = max([s.off + (s.e - s.v) for s in b.segs if s.kind == 1] +
                    [s.pick[1] + (s.e - s.v) for s in b.segs if isinstance(s.pick, tuple)])
    bin_path = os.path.abspath(os.path.join(modules, "romfs_patch.bin"))
    map_path = os.path.abspath(os.path.join(modules, "romfs.layers"))
    log(f"writing {bin_path} ({patch_end / 2**30:.2f} GiB) and {len(layers):,} layer pieces")
    write_patch_data(patch, bin_path, patch_end)
    write_layers(map_path, patch.l6size, layers)
    cfg = os.path.join(modules, "romfs.cfg")
    with open(cfg, "w", newline="\n") as f:
        f.write(f"layered\n{map_path}\n{b.base_path}\n{bin_path}\n")
    log(f"wrote {cfg}; verifying the result as the runtime will read it:")
    bad = verify(patch, cfg, os.path.join(modules, "romfs.unverified.txt"))
    log(f"done in {time.time() - t0:.0f}s" + (f"; unconfirmed blocks listed in {modules}/romfs.unverified.txt" if bad else ""))
    return bad


def apply(a):
    patch = open_patch(a.nsp, a.keys, a.inner)
    apply_exefs(patch, a.dump)
    apply_romfs(patch, a.base, a.keys, a.dump)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["apply", "verify"])
    ap.add_argument("nsp", help="the update: a Patch NSP, or its Program NCA (a .nca file, or an installed one's 00 file)")
    ap.add_argument("--keys", required=True, help="folder with prod.keys (and title.keys)")
    ap.add_argument("--dump", default=DUMP, help=argparse.SUPPRESS)   # (where to write: tests)
    ap.add_argument("--base", help="apply: romfs.cfg of the installed base (e.g. dump/modules/romfs.cfg)")
    ap.add_argument("--inner", help="the update's Program NCA inside the package, by name (a package can hold the "
                                    "game and its update together; without it, the Program NCA with a patch RomFS)")
    a = ap.parse_args()
    if a.cmd == "apply":
        if not a.base:
            raise SystemExit("apply needs --base <the base's romfs.cfg>")
        apply(a)
    else:
        patch = Patch(a.nsp, a.keys, a.inner)
        modules = os.path.join(a.dump, "modules")
        sys.exit(1 if verify(patch, os.path.join(modules, "romfs.cfg"), os.path.join(modules, "romfs.unverified.txt")) else 0)


if __name__ == "__main__":
    main()
