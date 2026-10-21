"""tools/shader_aot.py's scan of data.arc for GPU programs. Made-up containers; no game files."""
from pathlib import Path
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shader_aot  # noqa: E402


class EmptyFilesTests(unittest.TestCase):
    def test_empty_files_are_not_unreadable(self):
        """data.arc has a few empty files marked compressed (the same 7 in 11.0.0 and 13.0.5): nothing
        to read in them. Prepare Shaders said "7 files could not be read" for them."""
        reads = []

        def read(off, n):
            reads.append(off)
            return b'\x28\xb5\x2f\xfd broken'[:n]   # (a zstd frame cut short)
        arc = types.SimpleNamespace(read=read)
        found = [('eff', (100, 0, 0, 3)), ('arc', (200, 0, 0, 3)), ('eff', (300, 8, 64, 3))]
        with mock.patch.object(shader_aot, 'containers', lambda a: found):
            programs, unread = shader_aot.find_programs(arc)
        self.assertEqual(unread, 1)   # (only the one whose data is broken)
        self.assertEqual(reads, [300])
        self.assertEqual(programs, [])


class ReadErrorTests(unittest.TestCase):
    """A Steam Deck tester's SD card failed a read (EIO) part-way through data.arc: Prepare Shaders
    stopped with a traceback. Now the file is tried again, else left out."""

    def arc(self, bad, fail_times=99):
        tries = {}

        def read(off, n):
            tries[off] = tries.get(off, 0) + 1
            if off in bad and tries[off] <= fail_times:
                raise OSError(5, 'Input/output error', '/media/card/base.romfs')
            return b''
        return types.SimpleNamespace(read=read), tries

    def scan(self, arc, offsets):
        found = [('eff', (off, 8, 8, 0)) for off in offsets]
        errors = []
        with mock.patch.object(shader_aot, 'containers', lambda a: found):
            programs, unread = shader_aot.find_programs(arc, errors, pause=0)
        return unread, errors

    def test_an_unreadable_file_is_left_out(self):
        arc, tries = self.arc({200})
        unread, errors = self.scan(arc, [100, 200, 300])
        self.assertEqual(unread, 1)
        self.assertEqual(tries, {100: 1, 200: shader_aot.READ_TRIES, 300: 1})
        self.assertEqual([e.filename for e in errors], ['/media/card/base.romfs'])
        text = shader_aot.read_error_text(errors, '/somewhere')
        self.assertIn('Input/output error, in /media/card/base.romfs', text)
        self.assertNotIn('copy WindowSmash made', text)

    def test_a_second_try_can_read_it(self):
        arc, tries = self.arc({200}, fail_times=1)
        unread, errors = self.scan(arc, [100, 200, 300])
        self.assertEqual((unread, errors), (0, []))
        self.assertEqual(tries[200], 2)

    def test_a_disk_that_stops_answering(self):
        offsets = list(range(0, 100 * 50, 100))
        arc, tries = self.arc(set(offsets[5:]))
        with self.assertRaises(shader_aot.DiskGone):
            self.scan(arc, offsets)
        self.assertEqual(len(tries), 5 + shader_aot.READ_GIVE_UP)   # (stopped there, not after all 50)

    def test_a_copy_of_its_own(self):
        tree = str(ROOT)
        e = OSError(5, 'Input/output error', str(ROOT / 'dump' / 'modules' / 'base.romfs'))
        self.assertIn('copy WindowSmash made', shader_aot.read_error_text([e], tree))


VS_WORD = 1 | 3 << 5 | 1 << 10 | 3 << 17   # a vertex program's first word (is_header's)
PS_WORD = 2 | 3 << 5 | 5 << 10 | 3 << 17   # a pixel program's
MRT, KILLS = 1 << 14, 1 << 15              # (flags most pixel programs have in that word)


def blob_with(size, programs):
    """size zero bytes with each (offset, first word) put in."""
    data = bytearray(size)
    for at, word in programs:
        data[at:at + 4] = word.to_bytes(4, 'little')
    return bytes(data)


class ScanTests(unittest.TestCase):
    """Where programs are looked for. In 11.0.0 the scan missed half of the shader databases'
    programs (they start 8 bytes past a 16-byte boundary), every program built into the game's
    executable, and with the first two bytes looked for, every pixel program with flags."""

    def scan(self, ext, data):
        arc = types.SimpleNamespace(read=lambda off, n: data[:n])
        with mock.patch.object(shader_aot, 'containers', lambda a: [(ext, (0, len(data), len(data), 0))]):
            programs, unread = shader_aot.find_programs(arc)
        return [(stage, len(blob)) for stage, blob, _pairs in programs]

    def test_databases_on_8_byte_boundaries(self):
        data = blob_with(0x400, [(0x10, VS_WORD), (0x108, PS_WORD | MRT), (0x204, VS_WORD)])
        self.assertEqual(self.scan('nushdb', data), [(0, 0xF8), (1, 0x2F8)])   # (not the one 4 bytes off)

    def test_effects_on_256_byte_boundaries_and_0x30(self):
        data = blob_with(0x400, [(0x30, VS_WORD), (0x130, PS_WORD | KILLS), (0x140, PS_WORD)])
        self.assertEqual(self.scan('eff', data), [(0, 0x100), (1, 0x2D0)])

    def test_every_header_word_is_looked_at(self):
        for word in range(1 << 16):
            word |= 3 << 17
            if shader_aot.is_header(word):
                self.assertTrue(shader_aot.HEADER_START.match(word.to_bytes(4, 'little')), hex(word))

    def test_programs_in_the_executables(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            modules = Path(d) / 'dump' / 'modules'
            modules.mkdir(parents=True)
            (modules / 'main.reloc.bin').write_bytes(blob_with(0x300, [(0x50, VS_WORD), (0x130, PS_WORD | MRT | KILLS), (0x208, VS_WORD)]))
            paths = shader_aot.module_images(d)
            self.assertEqual([Path(p).name for p in paths], ['main.reloc.bin'])
            arc = types.SimpleNamespace(read=lambda off, n: b'')
            with mock.patch.object(shader_aot, 'containers', lambda a: []):
                programs, _ = shader_aot.find_programs(arc, modules=paths)
        self.assertEqual([(stage, len(blob), sorted(pairs)) for stage, blob, pairs in programs], [(0, 0xE0, []), (1, 0x1D0, [0])])


class RomfsImageReadErrorTests(unittest.TestCase):
    def test_the_error_names_its_file(self):
        import nca
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'base.bin'
            src.write_bytes(bytes(64))
            layers = Path(d) / 'layers.bin'
            nca.write_layers(str(layers), 64, [(0, 0, 0)])
            cfg = Path(d) / 'romfs.cfg'
            cfg.write_text(f'layered\n{layers}\n{src}\n')
            image = nca.RomfsImage(str(cfg))
            self.assertEqual(image.read(8, 8), bytes(8))
            broken = mock.Mock()
            broken.read.side_effect = OSError(5, 'Input/output error')
            real, image.sources[0] = image.sources[0], broken
            with self.assertRaises(OSError) as caught:
                image.read(8, 8)
            self.assertEqual((caught.exception.errno, caught.exception.filename), (5, str(src)))
            real.close()


if __name__ == '__main__':
    unittest.main()
