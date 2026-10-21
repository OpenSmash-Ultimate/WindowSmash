#!/usr/bin/env python3
"""Minimal NCA3 reader: header decryption, section listing, ExeFS (PFS0) and RomFS access.

Usage:
  nca.py info      <file.nca> --keys <dir>
  nca.py exefs     <file.nca> --keys <dir> --out <dir>
  nca.py romfs-ls  <file.nca> --keys <dir>
  nca.py romfs-get <file.nca> --keys <dir> --out <dir> [--match <substr>] [--flat]
  nca.py romfs-cfg <file.nca> --keys <dir> --out <romfs.cfg>
  nca.py aoc-cfg   <dir> --keys <dir> --out <aoc.cfg> [--app <title id>]

aoc-cfg lists the add-on content (DLC) in a folder, from the metadata of each package, for the
runtime's nn::aoc: a registered-content folder (e.g. yuzu's nand/user/Contents/registered) or a
folder of DLC .nsp files, or both.

The romfs-* commands also take a RomFS image on its own (a romfs.bin another tool wrote) or a
romfs.cfg (the file the runtime reads, plain or layered - see RomfsImage) in place of the NCA;
those need no keys.

Key material is read from <dir>/prod.keys and <dir>/title.keys (and the tickets inside an NSP)
and is never printed.
"""
import argparse
import bisect
import os
import struct
import sys

from Cryptodome.Cipher import AES

MEDIA_UNIT = 0x200
CONTENT_TYPES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}
ENC_TYPES = {0: "Auto", 1: "None", 2: "XTS", 3: "CTR", 4: "BKTR"}
FS_TYPES = {0: "RomFS", 1: "PartitionFS"}


def load_keys(path):
    keys = {}
    if not os.path.exists(path):
        return keys
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            try:
                keys[k.strip().lower()] = bytes.fromhex(v.strip())
            except ValueError:
                pass
    return keys


def xts_decrypt(key32, data, sector0=0, sector_size=MEDIA_UNIT):
    """AES-128-XTS with Nintendo's big-endian sector tweak."""
    k1 = AES.new(key32[:16], AES.MODE_ECB)
    k2 = AES.new(key32[16:], AES.MODE_ECB)
    mask = (1 << 128) - 1
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = int.from_bytes(k2.encrypt((sector0 + s).to_bytes(16, "big")), "little")
        sec = data[s * sector_size:(s + 1) * sector_size]
        for b in range(0, sector_size, 16):
            c = int.from_bytes(sec[b:b + 16], "little") ^ t
            p = int.from_bytes(k1.decrypt(c.to_bytes(16, "little")), "little") ^ t
            out += p.to_bytes(16, "little")
            t <<= 1
            if t >> 128:
                t = (t & mask) ^ 0x87
    return bytes(out)


class Section:
    def __init__(self, nca, index, start, end, fsh):
        self.nca = nca
        self.index = index
        self.start = start
        self.end = end
        self.fs_type = fsh[0x02]
        self.hash_type = fsh[0x03]
        self.enc_type = fsh[0x04]
        self.ctr_hi = bytes(reversed(fsh[0x140:0x148]))
        if self.hash_type == 2:      # HierarchicalSha256 -> PFS0
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, 0x40)
        elif self.hash_type == 3:    # HierarchicalIntegrity (IVFC) -> RomFS in level 6
            self.data_off, self.data_size = struct.unpack_from("<QQ", fsh, 0x18 + 5 * 0x18)
        else:
            self.data_off, self.data_size = 0, end - start

    def read_raw(self, off, size, ctr_hi=None):
        """Read decrypted bytes at an offset relative to the section start. `ctr_hi` replaces
        the section's upper counter (an update's RomFS changes it per region: see update.py)."""
        if self.enc_type == 1 or self.nca.plaintext:
            self.nca.f.seek(self.nca.base + self.start + off)
            return self.nca.f.read(size)
        if self.enc_type not in (3, 4):
            raise NotImplementedError(f"encryption type {ENC_TYPES.get(self.enc_type, self.enc_type)}")
        absolute = self.start + off
        aligned = absolute & ~0xF
        pad = absolute - aligned
        length = (pad + size + 0xF) & ~0xF
        self.nca.f.seek(self.nca.base + aligned)
        iv = (ctr_hi or self.ctr_hi) + (aligned >> 4).to_bytes(8, "big")
        cipher = AES.new(self.nca.body_key, AES.MODE_CTR, nonce=b"", initial_value=int.from_bytes(iv, "big"))
        return cipher.decrypt(self.nca.f.read(length))[pad:pad + size]

    def read(self, off, size):
        """Read from the section's payload (the PFS0 / RomFS image)."""
        return self.read_raw(self.data_off + off, size)


class Nca:
    def __init__(self, path, keydir, offset=0, title_keys=None):
        """An NCA file, or one inside a container (an NSP) at `offset`; `title_keys` adds
        rights id -> encrypted title key pairs (the container's tickets) to title.keys."""
        self.path = path
        self.base = offset
        self.f = open(path, "rb")
        self.f.seek(offset)
        prod = load_keys(os.path.join(keydir, "prod.keys"))
        title = load_keys(os.path.join(keydir, "title.keys"))
        title.update(title_keys or {})
        if "header_key" not in prod:
            raise SystemExit("prod.keys has no header_key")
        self.prod = prod
        raw = self.f.read(0xC00)
        # some dumps store the whole content already decrypted, header and sections alike
        self.plaintext = raw[0x200:0x204] == b"NCA3"
        hdr = raw if self.plaintext else xts_decrypt(prod["header_key"], raw)
        self.magic = hdr[0x200:0x204]
        if self.magic != b"NCA3":
            raise SystemExit(f"unsupported NCA magic {self.magic!r} (bad header_key?)")
        self.content_type = hdr[0x205]
        self.kaek_index = hdr[0x207]
        self.size, self.title_id = struct.unpack_from("<QQ", hdr, 0x208)
        self.sdk_version = struct.unpack_from("<I", hdr, 0x21C)[0]
        gen = max(hdr[0x206], hdr[0x220])
        self.key_gen = gen - 1 if gen else 0
        self.rights_id = hdr[0x230:0x240]
        self.has_rights = any(self.rights_id)

        if self.plaintext:
            # nothing to decrypt, so no key: a copy `decrypt` wrote keeps its rights id, and the
            # ticket with its title key stayed in the NSP it came from
            self.body_key = None
        elif self.has_rights:
            tk = title.get(self.rights_id.hex())
            kek = prod.get(f"titlekek_{self.key_gen:02x}")
            if tk is None or kek is None:
                raise SystemExit("missing title key or titlekek for this content")
            self.body_key = AES.new(kek, AES.MODE_ECB).decrypt(tk)
        else:
            name = ("application", "ocean", "system")[self.kaek_index]
            kak = prod.get(f"key_area_key_{name}_{self.key_gen:02x}")
            if kak is None:
                raise SystemExit(f"missing key_area_key_{name}_{self.key_gen:02x}")
            area = AES.new(kak, AES.MODE_ECB).decrypt(hdr[0x300:0x340])
            self.body_key = area[0x20:0x30]

        self.sections = []
        for i in range(4):
            s, e = struct.unpack_from("<II", hdr, 0x240 + i * 0x10)
            if not e:
                continue
            fsh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]
            sec = Section(self, i, s * MEDIA_UNIT, e * MEDIA_UNIT, fsh)
            sec.fs_header = fsh
            self.sections.append(sec)

    def section(self, fs_type):
        for s in self.sections:
            if s.fs_type == fs_type:
                return s
        return None


class PlainRomfs:
    """A RomFS image in a file of its own, read in place."""
    def __init__(self, path):
        self.f = open(path, "rb")
        self.start = self.data_off = 0
        self.enc_type = 1

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)


def is_plain_romfs(path):
    with open(path, "rb") as f:
        head = f.read(8)
    return len(head) == 8 and struct.unpack("<Q", head)[0] == 0x50


# ---- containers: NSP (a PFS0 of NCAs, tickets and certificates), XCI (HFS0 partitions) ---------
def partition_files(f, at, hfs):
    """[(name, offset in the file, size)] of the PFS0 (0x18-byte entries) or HFS0 (0x40-byte
    entries) partition at `at`; the data follows the header."""
    f.seek(at)
    head = f.read(0x10)
    magic, esz = (b"HFS0", 0x40) if hfs else (b"PFS0", 0x18)
    if head[:4] != magic:
        raise SystemExit(f"{f.name}: no {magic.decode()} partition at byte {at:#x}")
    count, strsz = struct.unpack_from("<II", head, 4)
    table = f.read(count * esz + strsz)
    strings = table[count * esz:]
    base = at + 0x10 + count * esz + strsz
    out = []
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", table, i * esz)
        out.append((strings[name_off:strings.index(b"\0", name_off)].decode(), base + off, size))
    return out


def pfs0_files(path):
    """[(name, offset in the file, size)] of a PFS0 container such as an NSP."""
    with open(path, "rb") as f:
        return partition_files(f, 0, False)


def xci_files(path):
    """[(name, offset, size)] of an XCI's secure partition: the game's NCAs."""
    with open(path, "rb") as f:
        head = f.read(0x200)
        if head[0x100:0x104] != b"HEAD":
            raise SystemExit(f"{path}: not an XCI")
        root = struct.unpack_from("<Q", head, 0x130)[0]
        for name, off, _size in partition_files(f, root, True):   # (offsets count from the root header's end)
            if name == "secure":
                return partition_files(f, off, True)
    raise SystemExit(f"{path}: the XCI has no secure partition")


def container_kind(path):
    """"nsp", "xci" or "nca" by content, not name."""
    with open(path, "rb") as f:
        head = f.read(0x200)
    return "nsp" if head[:4] == b"PFS0" else "xci" if head[0x100:0x104] == b"HEAD" else "nca"


def container_files(path):
    """The NCAs a file holds: [(name, offset, size)] - itself when it is a bare .nca."""
    kind = container_kind(path)
    if kind == "nsp":
        return [e for e in pfs0_files(path) if e[0].endswith(".nca")]
    if kind == "xci":
        return [e for e in xci_files(path) if e[0].endswith(".nca")]
    return [(os.path.basename(path), 0, os.path.getsize(path))]


# signature type -> where a ticket's body starts (the type, the signature, its padding)
TICKET_SIG = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80, 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}


def nsp_title_keys(path):
    """rights id (hex) -> encrypted title key, from the common tickets in an NSP (a game card's
    NCAs need none: their keys are in their own key areas)."""
    keys = {}
    if container_kind(path) != "nsp":
        return keys
    with open(path, "rb") as f:
        for name, off, size in pfs0_files(path):
            if not name.endswith(".tik"):
                continue
            f.seek(off)
            t = f.read(size)
            body = TICKET_SIG.get(struct.unpack_from("<I", t)[0])
            if body is None or t[body + 0x141] != 0:   # personalized tickets need the console's key
                continue
            keys[t[body + 0x160:body + 0x170].hex()] = t[body + 0x40:body + 0x50]
    return keys


def container_ncas(path, keydir, suffix=".nca"):
    """The NCAs in an NSP or XCI (or the bare .nca itself) whose names end in `suffix`, opened in
    place with the container's own tickets. Ones under a key we lack are skipped."""
    tks = nsp_title_keys(path)
    out, first_error = [], None
    for name, off, _size in container_files(path):
        if not name.endswith(suffix):
            continue
        try:
            out.append((name, Nca(path, keydir, off, tks)))
        except SystemExit as e:   # (under a key we lack, or not an NCA at all)
            first_error = first_error or f"{name}: {e}"
    if not out and first_error:
        raise SystemExit(f"{path}: no readable NCA - {first_error}")
    return out


nsp_ncas = container_ncas


# ---- the RomFS as the runtime reads it: romfs.cfg -------------------------------------
LAYER_MAGIC = b"SSBUROM1"
LAYER_ZERO = 0xFFFFFFFF   # source of a piece that is all zeros


def write_layers(path, size, entries):
    """A layer map: the image is `size` bytes; entry (at, source, offset) covers [at, next at)
    with the bytes of source file `source` from `offset` on (LAYER_ZERO: zeros)."""
    with open(path, "wb") as f:
        f.write(LAYER_MAGIC + struct.pack("<QII", size, len(entries), 0))
        f.write(b"".join(struct.pack("<QQII", at, off, src, 0) for at, src, off in entries))


def read_layers(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != LAYER_MAGIC:
        raise SystemExit(f"{path}: not a layer map")
    size, count, _ = struct.unpack_from("<QII", data, 8)
    return size, [(at, src, off) for at, off, src, _ in struct.iter_unpack("<QQII", data[24:24 + count * 24])]


class RomfsImage:
    """The game's RomFS image, from the romfs.cfg the runtime reads (runtime/hle_fs.c):
      plain:   "<file>\\n<byte offset>\\n" - the image in place inside one file (the base NCA)
      layered: "layered\\n<layer map>\\n<source 0>\\n<source 1>...\\n" - the image pieced together
               from several files, as a layer map says (an update over its base: update.py)"""
    start = data_off = 0   # what romfs_entries expects of a section
    enc_type = 1

    def __init__(self, cfg):
        with open(cfg, encoding="utf-8-sig") as f:
            lines = [l.strip() for l in f.read().splitlines() if l.strip()]
        if lines and lines[0] == "layered":
            self.size, self.entries = read_layers(lines[1])
            self.at = [e[0] for e in self.entries]
            self.paths = lines[2:]
            self.sources = [open(p, "rb") for p in self.paths]
        else:
            self.entries = None
            self.paths = lines[:1]
            self.f = open(lines[0], "rb")
            self.base = int(lines[1])

    def close(self):
        for f in self.sources if self.entries is not None else [self.f]:
            f.close()

    def read(self, off, size):
        if self.entries is None:
            try:
                self.f.seek(self.base + off)
                return self.f.read(size)
            except OSError as e:   # (a disk's read error: named with its file, which the error itself doesn't say)
                raise OSError(e.errno, e.strerror, self.paths[0]) from e
        out = bytearray()
        k = bisect.bisect_right(self.at, off) - 1
        while size > 0 and 0 <= k < len(self.entries):
            at, src, soff = self.entries[k]
            end = self.entries[k + 1][0] if k + 1 < len(self.entries) else self.size
            n = min(size, end - off)
            if src == LAYER_ZERO:
                out += bytes(n)
            else:
                f = self.sources[src]
                try:
                    f.seek(soff + (off - at))
                    out += f.read(n)
                except OSError as e:
                    raise OSError(e.errno, e.strerror, self.paths[src]) from e
            off += n
            size -= n
            k += 1
        return bytes(out)


def pfs0_entries(sec):
    head = sec.read(0, 0x10)
    if head[:4] != b"PFS0":
        raise SystemExit(f"section {sec.index}: not a PFS0 ({head[:4]!r}) - wrong key?")
    count, strsz = struct.unpack_from("<II", head, 4)
    table = sec.read(0x10, count * 0x18 + strsz)
    strings = table[count * 0x18:]
    base = 0x10 + count * 0x18 + strsz
    for i in range(count):
        off, size, name_off = struct.unpack_from("<QQI", table, i * 0x18)
        name = strings[name_off:strings.index(b"\0", name_off)].decode()
        yield name, base + off, size


def romfs_entries(sec):
    hdr = sec.read(0, 0x50)
    (hsz, _dho, _dhs, dmo, dms, _fho, _fhs, fmo, fms, fdo) = struct.unpack("<10Q", hdr)
    if hsz != 0x50:
        raise SystemExit("section is not a RomFS image")
    dirs = sec.read(dmo, dms)
    files = sec.read(fmo, fms)
    none = 0xFFFFFFFF

    def walk(doff, prefix):
        _parent, _sib, child, ffile, _h, nlen = struct.unpack_from("<6I", dirs, doff)
        name = dirs[doff + 0x18:doff + 0x18 + nlen].decode("utf-8", "replace")
        path = prefix + name + ("/" if name else "")
        fo = ffile
        while fo != none:
            _fp, fsib, doff2, dsize, _fh, fnlen = struct.unpack_from("<IIQQII", files, fo)
            fname = files[fo + 0x20:fo + 0x20 + fnlen].decode("utf-8", "replace")
            yield path + fname, fdo + doff2, dsize
            fo = fsib
        co = child
        while co != none:
            yield from walk(co, path)
            co = struct.unpack_from("<I", dirs, co + 4)[0]

    yield from walk(0, "")


def copy_out(sec, off, size, dest, chunk=64 << 20):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    done = 0
    with open(dest, "wb") as o:
        while done < size:
            n = min(chunk, size - done)
            o.write(sec.read(off + done, n))
            done += n
            if size > chunk:
                print(f"\r  {dest}: {done * 100 // size}%", end="", file=sys.stderr)
    if size > chunk:
        print(file=sys.stderr)


def aoc_cfg(root, keydir, out, app):
    """Write "<index> <title id> ..." for each AddOnContent package of one application, from
    installed content (.nca files) and DLC packages (.nsp files) anywhere under `root`."""
    found = {}   # application id -> {index: line}

    def meta(nca):
        if nca.content_type != 1:
            return
        sec = nca.section(1)
        for name, off, size in pfs0_entries(sec):
            if not name.endswith(".cnmt"):
                continue
            cnmt = sec.read(off, size)
            title, version, kind = struct.unpack_from("<QIB", cnmt, 0)
            if kind != 0x82:   # AddOnContent; extended header { u64 application id; u32 required version }
                continue
            owner, required = struct.unpack_from("<QI", cnmt, 0x20)
            index = title - (owner + 0x1000)   # the index counts from the application's AOC base id
            line = f"{index} {title:016X} v{version >> 16} needs application v{required >> 16}\n"
            have = found.setdefault(owner, {}).get(index)
            if have is None or int(have.split()[2][1:]) < version >> 16:   # the newest version wins
                found[owner][index] = line

    for dp, _dirs, files in os.walk(root):
        for f in files:
            path = os.path.join(dp, f)
            try:
                if f.endswith(".nca"):
                    meta(Nca(path, keydir))
                elif f.endswith(".nsp"):
                    for _name, nca in nsp_ncas(path, keydir, ".cnmt.nca"):
                        meta(nca)
            except SystemExit:   # content under a title key we lack: not metadata
                continue
    if app is None:
        if len(found) > 1:
            raise SystemExit("add-on content of several applications: pick one with --app " +
                             " ".join(f"{a:016X}" for a in found))
        app = next(iter(found), None)
    entries = sorted(found.get(app, {}).items())
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", newline="\n") as f:
        f.write(f"# add-on content of {app or 0:016X}: <index> <title id> <version> (tools/nca.py aoc-cfg)\n")
        f.writelines(line for _, line in entries)
    print(f"wrote {out}: {len(entries)} add-on contents")


def decrypt_nca(nca, out, chunk=64 << 20):
    """Write the NCA decrypted - its header and every section as plain bytes - so that it can
    be read in place afterwards (romfs-cfg, the runtime, update.py's base). Resumes a cut-off
    copy (<out>.partial) at the chunk it stopped in. A patch (BKTR) section is refused: an
    update's RomFS is only meaningful applied over its base (update.py)."""
    nca.f.seek(nca.base)
    raw = nca.f.read(0xC00)
    header = raw if nca.plaintext else xts_decrypt(nca.prod["header_key"], raw)
    # the file as regions: the header, then each section where it lies, zeros in between
    regions, at = [(header, None, 0, 0xC00)], 0xC00
    for sec in sorted(nca.sections, key=lambda s: s.start):
        if sec.enc_type == 4:
            raise SystemExit(f"section {sec.index} is a patch (BKTR) over another NCA: not decryptable on its own")
        if sec.start > at:
            regions.append((None, None, 0, sec.start - at))
        regions.append((None, sec, 0, sec.end - sec.start))
        at = sec.end
    if nca.size > at:
        regions.append((None, None, 0, nca.size - at))
    partial = out + ".partial"
    done = os.path.getsize(partial) // chunk * chunk if os.path.exists(partial) else 0
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    with open(partial, "r+b" if done else "wb") as o:
        o.seek(done)
        o.truncate()
        pos, shown = 0, -1
        for data, sec, _off, length in regions:
            if pos + length <= done:
                pos += length
                continue
            skip = max(0, done - pos)
            for rel in range(skip, length, chunk):
                n = min(chunk, length - rel)
                if data is not None:
                    o.write(data[rel:rel + n])
                elif sec is None:
                    o.write(bytes(n))
                else:
                    o.write(sec.read_raw(rel, n))
                pct = (pos + rel + n) * 100 // nca.size
                if pct != shown:   # (once per percent: a log keeps every line)
                    print(f"\r  decrypted {pct}%", end="", file=sys.stderr)
                    shown = pct
            pos += length
    print(file=sys.stderr)
    os.replace(partial, out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "exefs", "romfs-ls", "romfs-get", "romfs-cfg", "aoc-cfg", "decrypt"])
    ap.add_argument("nca")
    ap.add_argument("--keys")
    ap.add_argument("--out")
    ap.add_argument("--match", default="")
    ap.add_argument("--flat", action="store_true", help="romfs-get: every file straight into --out, without its folders")
    ap.add_argument("--app", type=lambda s: int(s, 16), help="aoc-cfg: the application whose add-on content to list")
    ap.add_argument("--inner", help="the NCA of that name inside the package (an .nsp or .xci) given in place of an NCA")
    a = ap.parse_args()

    if a.cmd == "aoc-cfg":
        if not (a.keys and a.out):
            raise SystemExit("aoc-cfg needs --keys <dir> and --out <aoc.cfg>")
        return aoc_cfg(a.nca, a.keys, a.out, a.app)
    if a.inner:   # an NCA inside an NSP or XCI, read in place (with an NSP's own tickets)
        if not a.keys:
            raise SystemExit("--keys <dir> is needed to read an NCA")
        found = [n for name, n in nsp_ncas(a.nca, a.keys) if name.lower() == a.inner.lower()]
        if not found:
            raise SystemExit(f"{a.nca} holds no {a.inner}")
        nca = found[0]
    elif is_plain_romfs(a.nca) or a.nca.endswith(".cfg"):
        if a.cmd not in ("romfs-ls", "romfs-get"):
            raise SystemExit(f"{a.nca} is a RomFS image, not an NCA: only romfs-ls and romfs-get apply")
        romfs = RomfsImage(a.nca) if a.nca.endswith(".cfg") else PlainRomfs(a.nca)
        nca = type("PlainImage", (), {"section": lambda self, fs_type: romfs, "plaintext": True})()
    else:
        if not a.keys:
            raise SystemExit("--keys <dir> is needed to read an NCA")
        nca = Nca(a.nca, a.keys)
    if a.cmd == "info":
        print(f"type={CONTENT_TYPES.get(nca.content_type, nca.content_type)} title_id={nca.title_id:016X} "
              f"size={nca.size} key_gen={nca.key_gen} titlekey_crypto={nca.has_rights} "
              f"sdk={nca.sdk_version >> 24}.{(nca.sdk_version >> 16) & 0xFF}.{(nca.sdk_version >> 8) & 0xFF}")
        for s in nca.sections:
            print(f"  section {s.index}: {FS_TYPES.get(s.fs_type, s.fs_type):<12} enc={ENC_TYPES.get(s.enc_type, s.enc_type):<5} "
                  f"file=[{s.start:#x},{s.end:#x}) payload_off={s.data_off:#x} payload_size={s.data_size:#x}")
    elif a.cmd == "exefs":
        sec = nca.section(1)
        for name, off, size in pfs0_entries(sec):
            print(f"  {name:<12} {size:>10}")
            copy_out(sec, off, size, os.path.join(a.out, name))
    elif a.cmd == "romfs-ls":
        total = 0
        for path, _off, size in romfs_entries(nca.section(0)):
            total += size
            print(f"{size:>14}  {path}")
        print(f"total {total} bytes")
    elif a.cmd == "romfs-cfg":   # where the runtime finds the RomFS: "<nca path>\n<byte offset>\n"
        sec = nca.section(0)
        if sec.enc_type != 1 and not nca.plaintext:
            raise SystemExit("the RomFS section is encrypted; the runtime reads it in place only when it is not")
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        at = getattr(nca, "base", 0) + sec.start + sec.data_off   # (base: where the NCA starts in its package)
        with open(a.out, "w") as f:
            f.write(f"{os.path.abspath(a.nca)}\n{at}\n")
        print(f"wrote {a.out}: RomFS at byte {at:#x} of {a.nca}")
    elif a.cmd == "romfs-get":
        sec = nca.section(0)
        for path, off, size in romfs_entries(sec):
            if a.match in path:
                print(f"  {path} ({size})")
                copy_out(sec, off, size, os.path.join(a.out, os.path.basename(path) if a.flat else path.replace("/", os.sep)))
    elif a.cmd == "decrypt":   # the whole NCA as plain bytes: readable in place from then on
        if not a.out:
            raise SystemExit("decrypt needs --out <file>")
        if nca.plaintext:
            print(f"{a.nca} is already decrypted")
        else:
            decrypt_nca(nca, a.out)
            print(f"wrote {a.out}: {nca.size} bytes")


if __name__ == "__main__":
    main()
