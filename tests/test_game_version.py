"""tools/game_version.py: WindowSmash builds and plays game version 13.0.5 only. What a game
folder's files say about the version (scan_problem: the installers' window), a main module's
build id, and the places that keep their own copy of 13.0.5's build id. No game files."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import game_version as gv  # noqa: E402
import modcache  # noqa: E402

MAIN_11 = '4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C'


def program(kind, **fields):
    return {'kind': kind, 'version': None, 'display_version': None, 'data_version': None, **fields}


class ScanTests(unittest.TestCase):
    def test_the_13_0_5_update(self):
        self.assertIsNone(gv.scan_problem(program('update', version=2031616, display_version='13.0.5')))
        self.assertIsNone(gv.scan_problem(program('update', version=2031616)))
        self.assertEqual(gv.program_version(program('update', version=2031616)), '13.0.5')

    def test_another_update(self):
        text = gv.scan_problem(program('update', version=30 << 16, display_version='13.0.4'))
        self.assertEqual(text, "WindowSmash needs version 13.0.5, and this is the game's update 13.0.4: put the 13.0.5 "
                               "update (.nsp) in this folder instead.")
        self.assertLessEqual(len(text), 130)   # (the installer's window has two lines for it, with "Problem: ")
        self.assertIn("update v12:", gv.scan_problem(program('update', version=12 << 16)))
        # (a title version that isn't 13.0.5's with a display version that says so: the title version decides)
        self.assertIn("update v14:", gv.scan_problem(program('update', version=14 << 16, display_version='13.0.5')))

    def test_a_game_without_the_update(self):
        text = gv.scan_problem(program('base', version=0, data_version='11.0.0', display_version='1.0.0'))
        self.assertEqual(text, 'WindowSmash needs version 13.0.5, and this is the game at 11.0.0 without its 13.0.5 '
                               'update: put that update (.nsp) in this folder.')   # (data.arc's version first)
        self.assertLessEqual(len(text), 130)
        self.assertIn('the game at 1.0.0 without', gv.scan_problem(program('base', data_version='', display_version='1.0.0')))
        self.assertIn('this is an early release of the game without', gv.scan_problem(program('base', data_version='')))
        self.assertIn('the game at 12.0.0 without', gv.scan_problem(program('base', display_version='12.0.0')))

    def test_a_dump_with_13_0_5_in_it(self):
        self.assertIsNone(gv.scan_problem(program('base', version=0, data_version='13.0.5', display_version='1.0.0')))

    def test_files_that_do_not_say(self):
        # (a renamed update without its metadata, a base whose data can't be read here: step 4 decides)
        self.assertIsNone(gv.scan_problem(program('update')))
        self.assertIsNone(gv.scan_problem(program('base')))
        self.assertIsNone(gv.scan_problem({'kind': 'base'}))

    def test_message_names_the_folder(self):
        self.assertEqual(gv.needs('the program in p.nca is version 11.0.0', 'D:\\MyDump'),
                         "WindowSmash needs version 13.0.5 of the game, and the program in p.nca is version 11.0.0. "
                         "Put the game's 13.0.5 update (an .nsp file) in D:\\MyDump, next to the game.")


class BuildIdTests(unittest.TestCase):
    def main(self, folder, build_id, magic=b'NSO0'):
        path = Path(folder) / 'main'
        head = bytearray(0x100)
        head[:4] = magic
        head[0x40:0x40 + len(build_id) // 2] = bytes.fromhex(build_id)
        path.write_bytes(head)
        return path

    def test_main_build_id(self):
        with tempfile.TemporaryDirectory(prefix='game-version-test-') as tmp:
            self.assertEqual(gv.main_build_id(self.main(tmp, gv.MAIN_BUILD_ID)), gv.MAIN_BUILD_ID)
            self.assertEqual(gv.main_build_id(self.main(tmp, MAIN_11)), MAIN_11)
            self.assertIsNone(gv.main_build_id(self.main(tmp, MAIN_11, magic=b'NRO0')))
            self.assertIsNone(gv.main_build_id(Path(tmp) / 'none'))
            (Path(tmp) / 'short').write_bytes(b'NSO0')
            self.assertIsNone(gv.main_build_id(Path(tmp) / 'short'))

    def test_names(self):
        self.assertEqual(gv.build_name(gv.MAIN_BUILD_ID.lower()), '13.0.5')
        self.assertEqual(gv.build_name(MAIN_11), '11.0.0')
        self.assertIsNone(gv.build_name('ABCD'))
        self.assertIsNone(gv.build_name(None))
        self.assertEqual(gv.version_text(gv.DATA_ARC_VERSION), '13.0.5')

    def test_every_copy_is_13_0_5s(self):
        """The places that can't import game_version keep the same build id."""
        want = gv.MAIN_BUILD_ID
        copies = {
            'runtime/host.c': r'#define REQUIRED_MAIN_BUILD "([0-9A-F]+)"',
            'tools/unlock_all.py': r'LAYOUTS = \{\s*"([0-9A-F]+)"',
            'tools/Unlock-All-Fighters.ps1': r"\$knownLayouts = @\{\s*'([0-9A-F]+)'",
            'tools/mac/boottest/main/g_module.c': r'relocs, 4, "([0-9A-F]+)"',
        }
        relay = (ROOT / 'tools' / 'ssbu_relay.py').read_text(encoding='utf-8')
        if want in relay or 'REQUIRED_BUILD' in relay:   # (the lobby's own check, where it has one)
            copies['tools/ssbu_relay.py'] = r'REQUIRED_BUILD = b?"([0-9A-F]+)"'
        for name, pattern in copies.items():
            with self.subTest(name):
                found = re.findall(pattern, (ROOT / name).read_text(encoding='utf-8'))
                self.assertEqual(found, [want])


class NroPlacementTests(unittest.TestCase):
    def test_place(self):
        slot, base = modcache.SLOT, modcache.NRO_BASE
        self.assertEqual(modcache.place('a', 0x100000, []), base)
        rows = [['a', f'{base:x}', f'{base + 0x100000:x}', 'A'], ['b', f'{base + slot:x}', f'{base + 3 * slot + 5:x}', 'B']]
        self.assertEqual(modcache.place('c', 0x100000, rows), base + 4 * slot)   # (past every module)
        full = [['z', f'{base + slot:x}', f'{modcache.NRO_LIMIT:x}', 'Z']]
        self.assertEqual(modcache.place('d', slot, full), base)   # (no room past them: the first free slots)
        with self.assertRaises(SystemExit):
            modcache.place('e', 2 * slot, full + [['y', f'{base:x}', f'{base + slot:x}', 'Y']])


if __name__ == '__main__':
    unittest.main()
