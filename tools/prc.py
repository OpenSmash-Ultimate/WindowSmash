#!/usr/bin/env python3
"""The game's param files (.prc, "paracobn"), and ARCropolis' patches of them (.prcx, .prcxml).

Usage:
  prc.py dump <file.prc>                 the params, as text
  prc.py patch <base.prc> <patch> <out>  <base.prc> with <patch> (.prcx or .prcxml) applied

A file: "paracobn", the hash table's size and the reference table's size (u32s), the hash table
(hash40s: every key and hash40 value), the reference table (strings, and each struct's keys: pairs
of a key's hash-table index and the value's offset from the struct, sorted by the key's hash40, the
values in the same order), then the root param - a struct.
A param is a type byte and its value: 1 bool, 2 i8, 3 u8, 4 i16, 5 u16, 6 i32, 7 u32, 8 float,
9 hash40 (a hash-table index), 10 string (a reference-table offset), 11 list (a count, then each
entry's offset from the list), 12 struct (a count, then its keys' reference-table offset).

A patch (prc-rs' format, which ARCropolis applies when it loads the file) holds only what changes:
a struct's keys - a new value, or a struct or list patched in turn; a list's entries by index (in a
.prcx, a list as long as its last changed entry, the entries before it that don't change "dummy"
params: a struct with no keys... see apply). Here, the patch is applied ahead of time, when mods are
applied (tools/mods.py)."""
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

BOOL, I8, U8, I16, U16, I32, U32, FLOAT, HASH, STRING, LIST, STRUCT = range(1, 13)
FIXED = {BOOL: "<?", I8: "<b", U8: "<B", I16: "<h", U16: "<H", I32: "<i", U32: "<I", FLOAT: "<f"}
NAMES = {BOOL: "bool", I8: "sbyte", U8: "byte", I16: "short", U16: "ushort", I32: "int", U32: "uint",
         FLOAT: "float", HASH: "hash40", STRING: "string", LIST: "list", STRUCT: "struct"}
MAGIC = b"paracobn"


class Error(Exception):
    pass


def hash40(text):
    s = text.lower().encode()
    return zlib.crc32(s) | (len(s) << 32)


class Struct(list):
    """[(key hash40, param)], in the file's order."""


class List(list):
    """[param]"""


def read(data):
    """The root struct of a .prc's bytes. A param: (type, value); a struct's value a Struct, a
    list's a List."""
    if data[:8] != MAGIC:
        raise Error("not a param file")
    hsize, rsize = struct.unpack_from("<2I", data, 8)
    hashes = [h for (h,) in struct.iter_unpack("<Q", data[0x10:0x10 + hsize])]
    ref = 0x10 + hsize
    params = ref + rsize

    def param(o):
        kind = data[o]
        if kind in FIXED:
            return kind, struct.unpack_from(FIXED[kind], data, o + 1)[0]
        if kind == HASH:
            return kind, hashes[struct.unpack_from("<I", data, o + 1)[0]]
        if kind == STRING:
            at = ref + struct.unpack_from("<I", data, o + 1)[0]
            return kind, data[at:data.index(b"\0", at)].decode("utf-8")
        if kind == LIST:
            n = struct.unpack_from("<I", data, o + 1)[0]
            return kind, List(param(o + off) for off in struct.unpack_from(f"<{n}I", data, o + 5))
        if kind == STRUCT:
            n, at = struct.unpack_from("<2I", data, o + 1)
            pairs = sorted(struct.iter_unpack("<Ii", data[ref + at:ref + at + n * 8]), key=lambda p: p[1])
            return kind, Struct((hashes[h], param(o + off)) for h, off in pairs)
        raise Error(f"unknown param type {kind} at {o:#x}")
    root = param(params)
    if root[0] != STRUCT:
        raise Error("the root param isn't a struct")
    return root


def write(root):
    """A .prc's bytes for a root struct param."""
    hashes, hash_at = [0], {0: 0}   # (the table starts with 0, as the game's files do)
    refs = bytearray()
    strings, keysets = {}, {}
    body = bytearray()

    def h(v):
        if v not in hash_at:
            hash_at[v] = len(hashes)
            hashes.append(v)
        return hash_at[v]

    def collect(p):   # every hash, in the order a file has them: keys and values as they come
        kind, v = p
        if kind == HASH:
            h(v)
        elif kind == LIST:
            for e in v:
                collect(e)
        elif kind == STRUCT:
            for k, e in sorted(v, key=lambda m: m[0]):
                h(k)
                collect(e)
    collect(root)

    def emit(p):
        kind, v = p
        o = len(body)
        body.append(kind)
        if kind in FIXED:
            body.extend(struct.pack(FIXED[kind], v))
        elif kind == HASH:
            body.extend(struct.pack("<I", hash_at[v]))
        elif kind == STRING:
            if v not in strings:
                strings[v] = None
            body.extend(struct.pack("<I", 0))
            pending.append((o + 1, ("s", v)))
        elif kind == LIST:
            body.extend(struct.pack("<I", len(v)))
            table = len(body)
            body.extend(b"\0" * 4 * len(v))
            for i, e in enumerate(v):
                struct.pack_into("<I", body, table + 4 * i, emit(e) - o)
        elif kind == STRUCT:
            body.extend(struct.pack("<2I", len(v), 0))
            offs = []
            for k, e in sorted(v, key=lambda m: m[0]):   # (by key, as the game's files: it looks keys up by hash)
                offs.append((hash_at[k], emit(e) - o))
            pending.append((o + 5, ("k", tuple(offs))))
        else:
            raise Error(f"unknown param type {kind}")
        return o
    pending = []
    emit(root)
    # the reference table: each struct's keys (one copy per distinct set), then the strings
    for at, (what, v) in pending:
        if what == "k" and v not in keysets:
            keysets[v] = len(refs)
            for pair in v:
                refs.extend(struct.pack("<Ii", *pair))
    for s in strings:
        strings[s] = len(refs)
        refs.extend(s.encode("utf-8") + b"\0")
    while len(refs) % 4:
        refs.append(0)
    for at, (what, v) in pending:
        struct.pack_into("<I", body, at, keysets[v] if what == "k" else strings[v])
    htab = b"".join(struct.pack("<Q", x) for x in hashes)
    return MAGIC + struct.pack("<2I", len(htab), len(refs)) + htab + bytes(refs) + bytes(body)


# ---- patches ---------------------------------------------------------------------------------
def apply(base, patch):
    """`base` (a param) with `patch` applied (prc-rs' rules): a struct's keys replaced or patched
    in turn (a key the base hasn't got is added), a list's entries by index (a patch entry that is
    an empty struct leaves the base's as it is; entries past the base's end are added); any
    other value replaced."""
    kind, v = base
    pkind, pv = patch
    if kind == STRUCT and pkind == STRUCT:
        out = Struct(v)
        where = {k: i for i, (k, _) in enumerate(out)}
        for k, p in pv:
            if k in where:
                out[where[k]] = (k, apply(out[where[k]][1], p))
            else:
                where[k] = len(out)
                out.append((k, p))
        return STRUCT, out
    if kind == LIST and pkind == LIST:
        out = List(v)
        for i, p in enumerate(pv):
            if i < len(out):
                if not (p[0] == STRUCT and not p[1]):
                    out[i] = apply(out[i], p)
            else:
                out.append(p)
        return LIST, out
    return patch


def read_xml(text):
    """A .prcxml (prc-rs' XML, a patch or a whole file) as a param; a list entry with an "index"
    attribute patches that entry, the ones between left empty structs (unchanged)."""
    def key(e):
        k = e.get("hash")
        if k is None:
            return None
        return int(k, 16) if k.startswith("0x") else hash40(k)

    def param(e):
        tag = e.tag
        if tag == "struct":
            return STRUCT, Struct((key(c), param(c)) for c in e)
        if tag == "list":
            out = List()
            for c in e:
                i = c.get("index")
                i = int(i) if i is not None else len(out)
                while len(out) < i:
                    out.append((STRUCT, Struct()))
                if i < len(out):
                    out[i] = param(c)
                else:
                    out.append(param(c))
            return LIST, out
        text = (e.text or "").strip()
        for kind, name in NAMES.items():
            if name == tag:
                if kind == BOOL:
                    return kind, text.lower() == "true"
                if kind in (I8, U8, I16, U16, I32, U32):
                    return kind, int(text, 0)
                if kind == FLOAT:
                    return kind, float(text)
                if kind == HASH:
                    return kind, int(text, 16) if text.startswith("0x") else hash40(text)
                return kind, e.text or ""
        raise Error(f"unknown param <{tag}> in the patch")
    root = ET.fromstring(text)
    return param(root)


def patch_bytes(base, patch, xml=False):
    """`base` (a .prc's bytes) with `patch` (a .prcx's bytes, or a .prcxml's) applied."""
    p = read_xml(patch) if xml else read(patch)
    return write(apply(read(base), p))


def dump(p, names=None, depth=0, out=None):
    out = [] if out is None else out
    kind, v = p
    pad = "  " * depth
    if kind == STRUCT:
        for k, e in v:
            if e[0] in (STRUCT, LIST):
                out.append(f"{pad}{names.get(k, hex(k)) if names else hex(k)}: {NAMES[e[0]]}[{len(e[1])}]")
                dump(e, names, depth + 1, out)
            else:
                out.append(f"{pad}{names.get(k, hex(k)) if names else hex(k)} = {e[1] if e[0] != HASH else hex(e[1])}")
    elif kind == LIST:
        for i, e in enumerate(v):
            out.append(f"{pad}[{i}] {NAMES[e[0]]}" + (f"[{len(e[1])}]" if e[0] in (STRUCT, LIST) else f" = {e[1]}"))
            if e[0] in (STRUCT, LIST):
                dump(e, names, depth + 1, out)
    return out


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "dump":
        print("\n".join(dump(read(open(sys.argv[2], "rb").read()))))
    elif len(sys.argv) == 5 and sys.argv[1] == "patch":
        base = open(sys.argv[2], "rb").read()
        patch = open(sys.argv[3], "rb").read()
        out = patch_bytes(base, patch, xml=sys.argv[3].lower().endswith(".prcxml"))
        open(sys.argv[4], "wb").write(out)
    else:
        print(__doc__.split("\n\n")[0])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
