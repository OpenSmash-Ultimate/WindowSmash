"""arc.py's file system table: 13.0.5's (zstd, with the version field) reads, and its version is
told from the table's start (data_version); an early release's (uncompressed, raw, or an older
layout) is not 13.0.5's, and it and a damaged copy stop with a plain message, not a traceback.
Archives made up here, from the headers' rules."""
from pathlib import Path
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import arc  # noqa: E402
import gamefiles  # noqa: E402

NRO = arc.NRO_DIR + 'lua2cpp_mario.nro'
MODULE = b'NRO0 a fighter module ' * 50
FS = 0x100                       # where the file system table starts
FILE_DATA = 0x90000000           # a newer release's file section (past the original release's)
NONE = 0xFFFFFFFF


def compress(data):
    try:
        from compression.zstd import compress as c
    except ImportError:
        import zstandard
        c = zstandard.compress
    return c(data)


def table_v3(path, stored, size, flags, version=0x0D0005):
    """A table with the version field and one file, `path`, at the file section's start."""
    h = arc.hash40(path)
    t = bytearray(struct.pack('<11I', 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 0))   # (size), paths, info indices, folders,
    t += bytes(0x38 - len(t))                                             # folder offsets, child hashes, infos,
    t += struct.pack('<BBHIIIIIII', 0, 0, 0, version, 0, 0, 0, 0, 0, 0)   # info-to-datas, datas; no regions
    t += struct.pack('<4I', 0, 0, 0, 0)          # no quick dirs, no streams
    t += struct.pack('<4I', 1, 1, 0, 1)          # one hash group, one bucket: entry 0, one entry
    t += struct.pack('<Q', h)                    # hash -> path 0
    t += struct.pack('<Q', h) + bytes(24)        # path 0 -> info index 0
    t += struct.pack('<2I', 0, 0)                # info index 0 -> file info 0
    t += struct.pack('<Q', 0) + bytes(0x14)      # folder 0: the file section's start
    t += struct.pack('<4I', 0, 0, 0, 0)          # file info 0 -> info-to-data 0
    t += struct.pack('<3I', 0, 0, 0)             # info-to-data 0 -> folder 0, data 0
    t += struct.pack('<4I', 0, stored, size, flags)
    t += bytes(max(0, arc.V3_TABLE_SIZE - len(t)))   # (a table this long has the version field)
    struct.pack_into('<I', t, 0, len(t))
    return bytes(t)


def pieces(table_header, table=b'', file_data=FILE_DATA, files=b''):
    """data.arc as {offset: bytes}: its header, the table header and table, the files."""
    out = {0: struct.pack('<7Q', arc.MAGIC, 0x38, file_data, 0, FS, 0, 0), FS: table_header + table}
    if files:
        out[file_data] = files
    return out


def reader(parts):
    """read(offset, size) over pieces, zeros between them (a file section 2 GB in, unwritten)."""
    def read(off, size):
        out = bytearray(size)
        for at, data in parts.items():
            lo, hi = max(off, at), min(off + size, at + len(data))
            if lo < hi:
                out[lo - off:hi - off] = data[lo - at:hi - at]
        return bytes(out)
    return read


def flat(parts):
    return reader(parts)(0, max(at + len(data) for at, data in parts.items()))


def romfs(data_arc):
    """A RomFS image (nca.romfs_entries) holding data.arc, as a game's does."""
    head = struct.pack('<10Q', 0x50, 0x50, 0, 0x50, 0x18, 0x68, 0, 0x68, 0x28, 0x100)
    dirs = struct.pack('<6I', 0, NONE, NONE, 0, NONE, 0)
    files = struct.pack('<IIQQII', 0, NONE, 0, len(data_arc), NONE, 8) + b'data.arc'
    image = head + dirs + files
    return image + bytes(0x100 - len(image)) + data_arc


def readable_arc(version=0x0D0005):
    table = table_v3(NRO, len(compress(MODULE)), len(MODULE), 3, version)
    packed = compress(table)
    return pieces(struct.pack('<4I', 0x10, len(table), len(packed), len(packed) + 0x10), packed,
                  files=compress(MODULE))


def zstd_header(size=arc.V3_TABLE_SIZE):
    """A zstd table's header alone: all table_form reads."""
    return struct.pack('<4I', 0x10, size, 0x1000, 0x1010)


EARLY = {   # (form, layout): an early release's data.arc
    ('uncompressed', 'v2'): pieces(struct.pack('<4I', 0x400, 7, 3, 1) + bytes(0x3F0)),
    ('uncompressed', 'v3'): pieces(struct.pack('<4I', arc.V3_TABLE_SIZE + 0x40, 7, 3, 1)),
    ('zstd', 'v1'): pieces(zstd_header(), file_data=0x10000000),      # the original release (1.0.0)
    ('uncompressed', 'v1'): pieces(struct.pack('<4I', 0x400, 7, 3, 1), file_data=0x10000000),
    ('zstd', 'v2'): pieces(zstd_header(0x1000000)),
    ('raw', 'v2'): pieces(struct.pack('<4I', 0, 0, 0x400, 0)),
}


class TableFormTests(unittest.TestCase):
    def test_a_newer_release_reads(self):
        parts = readable_arc()
        self.assertEqual(arc.table_form(reader(parts))[:2], ('zstd', 'v3'))
        a = arc.Arc(reader(parts))
        self.assertEqual(a.version, 0x0D0005)
        self.assertEqual(a.get(NRO), MODULE)
        self.assertIsNone(a.get(arc.NRO_DIR + 'lua2cpp_luigi.nro'))
        with tempfile.TemporaryDirectory(prefix='arc-table-test-') as out, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(arc.extract_nros(a, out), 1)
            self.assertEqual(os.listdir(out), ['lua2cpp_mario.nro'])

    def test_early_releases_are_refused_plainly(self):
        for want, parts in EARLY.items():
            with self.subTest(want):
                self.assertEqual(arc.table_form(reader(parts))[:2], want)
                self.assertFalse(arc.readable(*want))
                self.assertIsNone(arc.data_version(reader(parts)))
                with self.assertRaises(SystemExit) as e:
                    arc.Arc(reader(parts))
                text = str(e.exception.code)
                self.assertIn("This game data is not version 13.0.5's", text)
                self.assertIn("Put the game's 13.0.5 update", text)
                self.assertIn(f'{want[0]} file table, {want[1]} layout', text)

    def test_data_version(self):
        self.assertEqual(arc.data_version(reader(readable_arc())), 0x0D0005)
        self.assertEqual(arc.data_version(reader(readable_arc(0x0B0000))), 0x0B0000)
        with self.assertRaises(SystemExit) as e:   # (a zstd table header with no table after it)
            arc.data_version(reader(pieces(zstd_header())))
        self.assertIn('damaged or incomplete', str(e.exception.code))

    def test_damaged_copies(self):
        packed = b'not a zstd frame' * 4    # (what a player's step 8 met: "Unknown frame descriptor")
        cases = {
            'not zstd': pieces(struct.pack('<4I', 0x10, arc.V3_TABLE_SIZE, len(packed), len(packed) + 0x10), packed),
            'zeros': pieces(bytes(16)),
            'magic': {**pieces(zstd_header()), 0: bytes(0x38)},
        }
        for name, parts in cases.items():
            with self.subTest(name), self.assertRaises(SystemExit) as e:
                arc.Arc(reader(parts))
            self.assertIn('damaged or incomplete', str(e.exception.code))
        table = table_v3(NRO, 64, len(MODULE), 3)   # (the table reads; the file's bytes are not zstd)
        packed = compress(table)
        a = arc.Arc(reader(pieces(struct.pack('<4I', 0x10, len(table), len(packed), 0), packed, files=b'x' * 64)))
        with self.assertRaises(SystemExit) as e:
            a.get(NRO)
        self.assertIn(f'{NRO} could not be decompressed', str(e.exception.code))

    def test_step_8_says_it_without_a_traceback(self):
        # tools/arc.py nro on an early release's game data, as the build's step 8 runs it
        with tempfile.TemporaryDirectory(prefix='arc-table-test-') as tmp:
            image = Path(tmp) / 'romfs.bin'
            image.write_bytes(romfs(flat(EARLY[('uncompressed', 'v1')])))
            cfg = Path(tmp) / 'romfs.cfg'
            cfg.write_text(f'{image}\n0\n')
            r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'arc.py'), 'nro', '--romfs-cfg', str(cfg),
                                '--out', str(Path(tmp) / 'nro')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("not version 13.0.5's", r.stderr)
            self.assertNotIn('Traceback', r.stderr)


class WizardTests(unittest.TestCase):
    """gamefiles: the game version the installer's window reads from a program's own data.arc."""

    def section(self, parts):
        image = romfs(flat(parts))
        return types.SimpleNamespace(read=lambda off, size: image[off:off + size])

    def test_data_arc_version(self):
        self.assertEqual(gamefiles.data_arc_version(self.section(readable_arc())), '13.0.5')
        self.assertEqual(gamefiles.data_arc_version(self.section(readable_arc(0x0B0000))), '11.0.0')
        self.assertEqual(gamefiles.data_arc_version(self.section(EARLY[('zstd', 'v1')])), '')   # (an early release)
        self.assertIsNone(gamefiles.data_arc_version(self.section(pieces(zstd_header()))))     # (damaged: not told)
        self.assertIsNone(gamefiles.data_arc_version(types.SimpleNamespace(read=lambda off, size: bytes(size))))


if __name__ == '__main__':
    unittest.main()
