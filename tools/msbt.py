#!/usr/bin/env python3
"""The game's text files (.msbt), and ARCropolis' patches of them (.xmsbt).

Usage:
  msbt.py dump <file.msbt>                 each label and its text
  msbt.py patch <base.msbt> <patch.xmsbt> <out>

An .msbt: "MsgStdBn", a byte-order mark, the text encoding (1: UTF-16), the version, the section
count and the file's size, then sections - a magic, the section's size, 8 bytes, its data, padded
to 16 bytes with 0xAB. LBL1: buckets of labels (a label is in bucket hash % bucket count; a label
names its text by index); ATR1: one attribute (of a fixed size) per text; TXT2: the texts (offsets,
then UTF-16 strings, each ending in a 0). Other sections are kept as they are.
An .xmsbt (UTF-16 XML): <xmsbt><entry label="..."><text>...</text></entry>...</xmsbt> - each
label's text replaced, or added with its label when the file hasn't got it."""
import struct
import sys
import xml.etree.ElementTree as ET

MAGIC = b"MsgStdBn"


class Error(Exception):
    pass


def label_hash(label, buckets):
    h = 0
    for ch in label.encode("utf-8"):
        h = (h * 0x492 + ch) & 0xFFFFFFFF
    return h % buckets


class Msbt:
    def __init__(self, data):
        if data[:8] != MAGIC:
            raise Error("not a text file (.msbt)")
        self.bom = data[8:10]
        if self.bom != b"\xff\xfe":
            raise Error("a big-endian .msbt")
        self.head = bytearray(data[:0x20])
        encoding = data[0xC]
        if encoding != 1:
            raise Error(f"an .msbt in text encoding {encoding} (only UTF-16 is read)")
        count = struct.unpack_from("<H", data, 0xE)[0]
        self.sections = []   # [magic, bytes]
        o = 0x20
        for _ in range(count):
            magic = bytes(data[o:o + 4])
            size = struct.unpack_from("<I", data, o + 4)[0]
            self.sections.append([magic, bytes(data[o + 0x10:o + 0x10 + size])])
            o += 0x10 + (size + 15) // 16 * 16
        self.buckets = 0
        self.labels = {}     # label -> text index
        self.order = []      # labels in file order, per bucket
        self.texts = []
        self.attr_size, self.attrs = 0, []
        for magic, body in self.sections:
            if magic == b"LBL1":
                self.buckets = struct.unpack_from("<I", body, 0)[0]
                for b in range(self.buckets):
                    n, at = struct.unpack_from("<2I", body, 4 + b * 8)
                    for _ in range(n):
                        ln = body[at]
                        label = body[at + 1:at + 1 + ln].decode("utf-8")
                        self.labels[label] = struct.unpack_from("<I", body, at + 1 + ln)[0]
                        self.order.append(label)
                        at += 1 + ln + 4
            elif magic == b"TXT2":
                n = struct.unpack_from("<I", body, 0)[0]
                offs = list(struct.unpack_from(f"<{n}I", body, 4)) + [len(body)]
                self.texts = [body[offs[i]:offs[i + 1]] for i in range(n)]   # (each with its 0 at the end)
            elif magic == b"ATR1":
                n, self.attr_size = struct.unpack_from("<2I", body, 0)
                self.attrs = [body[8 + i * self.attr_size:8 + (i + 1) * self.attr_size] for i in range(n)]
                self.attr_tail = body[8 + n * self.attr_size:]
        if not self.buckets:
            raise Error("an .msbt with no labels")

    def text(self, label):
        raw = self.texts[self.labels[label]]
        return raw.decode("utf-16-le").rstrip("\0")

    def set(self, label, text):
        raw = text.encode("utf-16-le") + b"\0\0"
        if label in self.labels:
            self.texts[self.labels[label]] = raw
            return
        self.labels[label] = len(self.texts)
        self.order.append(label)
        self.texts.append(raw)
        if self.attr_size:
            self.attrs.append(b"\0" * self.attr_size)

    def to_bytes(self):
        out = bytearray()
        for magic, body in self.sections:
            if magic == b"LBL1":
                per = [[] for _ in range(self.buckets)]
                for label in self.order:
                    per[label_hash(label, self.buckets)].append(label)
                table = bytearray(struct.pack("<I", self.buckets))
                names = bytearray()
                at = 4 + self.buckets * 8
                for labels in per:
                    table += struct.pack("<2I", len(labels), at + len(names))
                    for label in labels:
                        b = label.encode("utf-8")
                        names += bytes([len(b)]) + b + struct.pack("<I", self.labels[label])
                body = bytes(table + names)
            elif magic == b"TXT2":
                n = len(self.texts)
                at = 4 + 4 * n
                offs = []
                for t in self.texts:
                    offs.append(at)
                    at += len(t)
                body = struct.pack(f"<I{n}I", n, *offs) + b"".join(self.texts)
            elif magic == b"ATR1":
                body = struct.pack("<2I", len(self.attrs), self.attr_size) + b"".join(self.attrs) + self.attr_tail
            elif magic == b"TSY1":   # (one style index per text)
                n = len(self.texts)
                body = body[:4 * n] + b"\0" * max(0, 4 * n - len(body))
            out += magic + struct.pack("<I", len(body)) + b"\0" * 8 + body
            out += b"\xab" * (-len(body) % 16)
        head = bytearray(self.head)
        struct.pack_into("<H", head, 0xE, len(self.sections))
        struct.pack_into("<I", head, 0x12, 0x20 + len(out))
        return bytes(head + out)


def read_xmsbt(data):
    """[(label, text)] of an .xmsbt's bytes (UTF-16 with its byte-order mark, or UTF-8)."""
    if data[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = data.decode("utf-16")
    else:
        text = data.decode("utf-8-sig")
    text = text.replace('encoding="utf-16"', "").replace("encoding='utf-16'", "")   # (decoded already)
    root = ET.fromstring(text)
    out = []
    for e in root.iter("entry"):
        label = e.get("label")
        t = e.find("text")
        if label is not None and t is not None:
            out.append((label, t.text or ""))
    return out


def patch_bytes(base, patch):
    """`base` (an .msbt's bytes) with `patch` (an .xmsbt's) applied."""
    m = Msbt(base)
    for label, text in read_xmsbt(patch):
        m.set(label, text)
    return m.to_bytes()


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "dump":
        m = Msbt(open(sys.argv[2], "rb").read())
        for label in m.order:
            print(f"{label}\t{m.text(label)!r}")
    elif len(sys.argv) == 5 and sys.argv[1] == "patch":
        out = patch_bytes(open(sys.argv[2], "rb").read(), open(sys.argv[3], "rb").read())
        open(sys.argv[4], "wb").write(out)
    else:
        print(__doc__.split("\n\n")[0])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
