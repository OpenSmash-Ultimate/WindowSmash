#!/usr/bin/env python3
"""Minimal reader for the game's data.arc: file lookup by path, extraction.

Usage:
  arc.py info [--romfs-cfg <romfs.cfg>]
  arc.py get  <arc path>... --out <dir> [--romfs-cfg <romfs.cfg>]
  arc.py nro  [--out <dir>] [--romfs-cfg <romfs.cfg>]

data.arc is read in place inside the game's RomFS, found through romfs.cfg (default
dump/modules/romfs.cfg, the file the runtime reads; plain or layered, see nca.RomfsImage).
Paths are the archive's own, e.g. "prebuilt:/nro/release/lua2cpp_mario.nro"; `get` writes each
file under its base name. `nro` extracts every module in prebuilt:/nro/release/ that is not
compiled in yet (not in dump/nro/bases.txt) into dump/nro, where the next `recomp.py all` picks
it up - the same place the runtime saves an NRO the game hands over (runtime/hle_ro.c). The
archive stores only hashes of names, so `nro` knows a module by the names in NRO_NAMES.

Layout (smash-arc's naming): a header, then a zstd-compressed file system table; a path's
hash40 (crc32 of the lowercase path | length << 32) picks a bucket, a binary search in the
bucket gives the path, and path -> file info index -> file info -> info-to-data -> folder
offset + file data locate the bytes. The table's header holds the game version (data_version:
13.0.5's is 0x0D0005, tools/game_version.py). Early releases of the game (the original 1.0.0,
and others before the table's version field) store the table in other forms and layouts
(table_form); those are not 13.0.5's, and are refused.
"""
import argparse
import os
import struct
import sys
import zlib
try:
    from compression.zstd import ZstdError, decompress as zstd_decompress   # Python 3.14
except ImportError:                                                          # 3.10 to 3.13: the zstandard package
    import zstandard
    from zstandard import ZstdError

    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nca import RomfsImage, romfs_entries  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NRO_DIR = "prebuilt:/nro/release/"
# Every module in 13.0.5's NRO_DIR (98): the fighters, plus common, item, element (shared by
# Pyra/Mythra), koopag (Giga Bowser), ptrainer, and the Mii enemies of Spirits/Classic.
NRO_NAMES = ("bayonetta brave buddy captain chrom cloud common daisy dedede demon diddy dolly donkey duckhunt "
             "edge eflame element elight falco fox gamewatch ganon gaogaen gekkouga ike inkling item jack kamui "
             "ken kirby koopa koopag koopajr krool link littlemac lucario lucas lucina luigi mario mariod marth "
             "master metaknight mewtwo miienemyf miienemyg miienemys miifighter miigunner miiswordsman murabito "
             "nana ness pacman packun palutena peach pfushigisou pichu pickel pikachu pikmin pit pitb plizardon "
             "popo ptrainer purin pzenigame reflet richter ridley robot rockman rosetta roy ryu samus samusd "
             "sheik shizue shulk simon snake sonic szerosuit tantan toonlink trail wario wiifit wolf yoshi "
             "younglink zelda").split()
MAGIC = 0xABCDEF9876543210
LOW40 = (1 << 40) - 1
# the file system table's forms (ArcCross's rules: github.com/Ploaj/ArcCross, ArcCross/ARC.cs)
ZSTD_TABLE = 0x10             # the table header's first field when the table is zstd-compressed
V1_FILE_DATA = 0x8824AF68     # file data starting before this: the original release's layout (1.0.0)
V3_TABLE_SIZE = 0x2992DD4     # a table this long or longer has the version field (13.0.5's is 0x31EA858)
NOT_13 = ("This game data is not version 13.0.5's ({}): WindowSmash needs version 13.0.5 of the game. Put the "
          "game's 13.0.5 update (an .nsp file) next to the game and set it up again.")
VERSION_FIELD = 60            # the table's version field (Arc: after the counts and the region count)
DAMAGED = "This copy of the game looks damaged or incomplete ({}). Copy the game's files again."


def hash40(path):
    s = path.lower().encode()
    return zlib.crc32(s) | (len(s) << 32)


def table_form(read):
    """How data.arc's file system table is stored, from `read(offset, size)` over data.arc:
    (form, layout, size). form: "zstd", or "uncompressed" or "raw" (early releases); layout:
    "v3" (with the version field: later releases, 13.0.5 among them), "v2" (an early
    release's, without it) or "v1" (the original release's, other structures altogether); size:
    the table's, uncompressed. Only zstd and v3 is read here (readable)."""
    hdr = struct.unpack("<7Q", read(0, 0x38))
    if hdr[0] != MAGIC:
        raise SystemExit(DAMAGED.format("data.arc does not begin as the game's does"))
    data_offset, decompressed, compressed, _ = struct.unpack("<4I", read(hdr[4], 16))
    if data_offset == ZSTD_TABLE:
        form, size = "zstd", decompressed
    elif data_offset > ZSTD_TABLE:
        form, size = "uncompressed", data_offset   # (the table itself, whose first field is its size)
    elif compressed:
        form, size = "raw", compressed
    else:   # (zeros: a copy cut short)
        raise SystemExit(DAMAGED.format("data.arc's file table is not there"))
    layout = "v1" if hdr[2] < V1_FILE_DATA else "v3" if size >= V3_TABLE_SIZE else "v2"
    return form, layout, size


def readable(form, layout):
    """Whether this reader reads a table of this form: 13.0.5's (zstd, with the version field)."""
    return form == "zstd" and layout == "v3"


def form_text(form, layout):
    """A table form for a person (what a bug report should quote)."""
    return f"data.arc: {form} file table, {layout} layout"


def zstd_head(data, size):
    """At least the first `size` bytes `data` decompresses to, of a zstd frame given in part."""
    try:
        from compression.zstd import ZstdDecompressor   # Python 3.14
        return ZstdDecompressor().decompress(data, max_length=size)
    except ImportError:
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)


def data_version(read):
    """data.arc's version field (0x0D0005 for 13.0.5), from `read(offset, size)` over data.arc,
    reading only the table's start; None for an early release's table, which has none."""
    form, layout, _size = table_form(read)
    if not readable(form, layout):
        return None
    hdr = struct.unpack("<7Q", read(0, 0x38))
    data_start, _decomp, comp, _ = struct.unpack("<4I", read(hdr[4], 16))
    try:
        head = zstd_head(read(hdr[4] + data_start, min(comp, 0x40000)), VERSION_FIELD + 4)
    except ZstdError as e:
        raise SystemExit(DAMAGED.format(f"data.arc's file table could not be decompressed: {e}"))
    if len(head) < VERSION_FIELD + 4:
        raise SystemExit(DAMAGED.format("data.arc's file table is cut short"))
    return struct.unpack_from("<I", head, VERSION_FIELD)[0]


def unzstd(data, what):
    """zstd_decompress; data that is not zstd is a damaged copy, said plainly."""
    try:
        return zstd_decompress(data)
    except ZstdError as e:
        raise SystemExit(DAMAGED.format(f"{what} could not be decompressed: {e}"))


def data_arc_offset(image):
    """Where data.arc starts in a RomFS image."""
    found = [off for path, off, _ in romfs_entries(image) if path == "data.arc"]
    if not found:
        raise SystemExit("no data.arc in the game's RomFS")
    return found[0]


def game_data_version(cfg):
    """data_version of data.arc in the RomFS a romfs.cfg reads (its files closed again)."""
    image = RomfsImage(cfg)
    try:
        base = data_arc_offset(image)
        return data_version(lambda off, size: image.read(base + off, size))
    finally:
        image.close()


class Arc:
    def __init__(self, source):
        """`source`: a romfs.cfg, or read(offset, size) over data.arc's own bytes."""
        if isinstance(source, str):
            image = RomfsImage(source)
            base = data_arc_offset(image)
            self.read = lambda off, size: image.read(base + off, size)
        else:
            self.read = source
        form, layout, _size = table_form(self.read)
        if not readable(form, layout):
            raise SystemExit(NOT_13.format(form_text(form, layout)))
        hdr = self.header = struct.unpack("<7Q", self.read(0, 0x38))
        self.file_section = hdr[2]
        data_start, decomp, comp, _ = struct.unpack("<4I", self.read(hdr[4], 16))
        t = self.t = unzstd(self.read(hdr[4] + data_start, comp), "data.arc's file table")
        if len(t) != decomp:
            raise SystemExit(DAMAGED.format("data.arc's file table did not decompress whole"))

        (_size, paths, info_indices, folders, offsets_1, child_hashes, infos, info_to_datas, datas,
         offsets_2, datas_2) = struct.unpack_from("<11I", t, 0)
        # the extra folders, infos, info-to-datas and datas: entries a patch added after each table's
        # own ones, which the table's last section (after datas, see mods.py) indexes; the counts
        # the game itself reads (f_710374f910)
        (self.regions, _, _, self.version, extra_folders, _, _, extra_infos, extra_2,
         extra_datas) = struct.unpack_from("<BBHIIIIIII", t, 56)
        o = 0x58 + self.regions * 12                 # header, then one 12-byte entry per region
        quick_dirs, stream_hashes, stream_indices, stream_datas = struct.unpack_from("<4I", t, o)
        o += 16 + quick_dirs * 12
        self.tab = {}   # name -> (offset, entry size, count)
        for name, count, size in (("stream_hash_to_entry", stream_hashes, 8), ("stream_entries", stream_hashes, 12),
                                  ("stream_indices", stream_indices, 4), ("stream_datas", stream_datas, 16)):
            self.tab[name] = (o, size, count)
            o += count * size
        groups, self.buckets = struct.unpack_from("<2I", t, o)
        self.bucket_table = o + 8
        o = self.bucket_table + self.buckets * 8
        for name, count, size in (("hash_to_path", groups, 8), ("paths", paths, 32), ("info_indices", info_indices, 8),
                                  ("dir_hash_to_info", folders, 8), ("dirs", folders, 0x34),
                                  ("folder_offsets", offsets_1 + offsets_2 + extra_folders, 0x1C),
                                  ("child_hashes", child_hashes, 8), ("infos", infos + datas_2 + extra_infos, 16),
                                  ("info_to_datas", info_to_datas + datas_2 + extra_2, 12),
                                  ("datas", datas + datas_2 + extra_datas, 16)):
            self.tab[name] = (o, size, count)
            o += count * size
        self.extra_at = o   # the extra section (patch-added files by hash), to the table's end

    def _entry(self, table, index, fmt):
        offset, size, _count = self.tab[table]
        return struct.unpack_from(fmt, self.t, offset + index * size)

    def _data(self, to_data):
        """(offset in data.arc, stored size, size, flags) of an info-to-data entry, or None."""
        try:
            folder, data, _ = self._entry("info_to_datas", to_data, "<3I")
            folder_offset = self._entry("folder_offsets", folder, "<Q")[0]
            in_folder, stored, size, flags = self._entry("datas", data, "<4I")
        except struct.error:   # an index past its table
            return None
        return self.file_section + folder_offset + in_folder * 4, stored, size, flags

    def files_by_path(self):
        """(path hash40, region) -> where its data is, for every path (each region's variant of
        a regional file)."""
        out = {}
        for i in range(self.tab["paths"][2]):
            full = self._entry("paths", i, "<Q")[0]
            try:
                info = self._entry("info_indices", full >> 40, "<2I")[1]
                to_data, flags = self._entry("infos", info, "<4I")[2:]
            except struct.error:
                continue
            # (a regional file: an empty entry, then one per region - the game reads to_data + 1 + its region)
            for r in range(self.regions) if flags & 0x8000 else (0,):
                where = self._data(to_data + 1 + r if flags & 0x8000 else to_data)
                if where:
                    out[(full & LOW40, r)] = where
        return out

    def reachable(self):
        """Where the data of every info-to-data entry is: everything a file can lead to."""
        out = []
        for i in range(self.tab["info_to_datas"][2]):
            where = self._data(i)
            if where:
                out.append(where)
        return out

    def streams(self):
        """(stream hash40, variant) -> (offset in data.arc, size): the stream: files (music,
        movies), which are stored uncompressed outside the file section."""
        out = {}
        nidx, ndat = self.tab["stream_indices"][2], self.tab["stream_datas"][2]
        for i in range(self.tab["stream_entries"][2]):
            full, flags = self._entry("stream_entries", i, "<QI")
            index = full >> 40
            for r in range((14 if flags & 1 else 5) if flags & 3 else 1):   # localized / regional variants
                if index + r >= nidx:
                    break
                d = self._entry("stream_indices", index + r, "<I")[0]
                if d < ndat:
                    size, offset = self._entry("stream_datas", d, "<QQ")
                    out[(full & LOW40, r)] = (offset, size)
        return out

    def locate(self, path):
        """(offset in data.arc, stored size, size, compressed) of a file, or None if absent."""
        h = hash40(path)
        start, count = struct.unpack_from("<2I", self.t, self.bucket_table + (h % self.buckets) * 8)
        lo, hi = start, start + count
        while lo < hi:
            mid = (lo + hi) // 2
            v = self._entry("hash_to_path", mid, "<Q")[0]
            if v & LOW40 == h:
                break
            lo, hi = (mid + 1, hi) if v & LOW40 < h else (lo, mid)
        else:
            return None
        full = self._entry("paths", v >> 40, "<Q")[0]
        info = self._entry("info_indices", full >> 40, "<2I")[1]
        _, _, to_data, flags = self._entry("infos", info, "<4I")
        where = self._data(to_data + 1 if flags & 0x8000 else to_data)   # (a regional file: its first region's)
        if where is None:
            return None
        offset, stored, size, flags = where
        return offset, stored, size, bool(flags & 3)

    def get(self, path):
        where = self.locate(path)
        if where is None:
            return None
        off, stored, size, compressed = where
        data = self.read(off, stored)
        if compressed:
            data = unzstd(data, path)
        if len(data) != size:
            raise SystemExit(f"{path}: {len(data)} bytes, the table says {size}")
        return data


def extract_nros(arc, out):
    """Every module of NRO_DIR not in `out` yet (by name, or in its bases.txt) into `out`;
    returns how many were written."""
    manifest = os.path.join(out, "bases.txt")
    known = {l.split()[0] for l in open(manifest)} if os.path.exists(manifest) else set()
    os.makedirs(out, exist_ok=True)
    n = 0
    for name in NRO_NAMES:
        module = "lua2cpp_" + name
        if module in known or os.path.exists(os.path.join(out, module + ".nro")):
            continue
        data = arc.get(NRO_DIR + module + ".nro")
        if data is None:
            continue                              # a fighter newer than this data.arc
        with open(os.path.join(out, module + ".nro"), "wb") as f:
            f.write(data)
        print(f"  {module}.nro ({len(data)})")
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "get", "nro"])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--romfs-cfg")
    a = ap.parse_args()
    arc = Arc(a.romfs_cfg or os.path.join(ROOT, "dump", "modules", "romfs.cfg"))
    if a.cmd == "info":
        v = arc.version
        print(f"data.arc version {v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}, {arc.buckets} buckets")
    elif a.cmd == "get":
        if not a.out:
            raise SystemExit("get needs --out <dir>")
        os.makedirs(a.out, exist_ok=True)
        for path in a.paths:
            data = arc.get(path)
            if data is None:
                print(f"  {path}: not in the archive")
                continue
            with open(os.path.join(a.out, path.rsplit("/", 1)[-1]), "wb") as f:
                f.write(data)
            print(f"  {path} ({len(data)})")
    elif a.cmd == "nro":
        extract_nros(arc, a.out or os.path.join(ROOT, "dump", "nro"))


if __name__ == "__main__":
    main()
