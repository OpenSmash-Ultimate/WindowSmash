from pathlib import Path
import os
import runpy
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
BUILD = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_reference')
sys.path.insert(0, str(ROOT / 'tests'))
import test_arc_table as arc_table  # noqa: E402  (made-up data.arc's)
pick_program, jobs_for_ram, romfs_cfg_ok = BUILD['pick_program'], BUILD['jobs_for_ram'], BUILD['romfs_cfg_ok']
Failed = BUILD['Failed']
PROGRAM, META, CONTROL = 0, 1, 2
BASE, UPDATE = 0x01006A800016E000, 0x01006A800016E800
MAIN_13 = '21450C647B8C5940ED5B68218EA128FD3C5D597D'   # (13.0.5's main module; 11.0.0's below)
MAIN_11 = '4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C'


def write_main(path, build_id):
    """A stand-in for dump/exefs/main: just the NSO header the build id is read from."""
    head = bytearray(0x100)
    head[:4] = b'NSO0'
    head[0x40:0x40 + len(build_id) // 2] = bytes.fromhex(build_id)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(head)


class PickProgramTests(unittest.TestCase):
    def test_the_one_program(self):
        ncas = [('meta.cnmt.nca', META, BASE), ('program.nca', PROGRAM, BASE), ('control.nca', CONTROL, BASE)]
        self.assertEqual(pick_program(ncas), 'program.nca')

    def test_of_two_the_update(self):
        ncas = [('base.nca', PROGRAM, BASE), ('update.nca', PROGRAM, UPDATE), ('control.nca', CONTROL, UPDATE)]
        self.assertEqual(pick_program(ncas), 'update.nca')

    def test_no_program(self):
        with self.assertRaises(Failed):
            pick_program([('meta.cnmt.nca', META, BASE), ('control.nca', CONTROL, BASE)])
        with self.assertRaises(Failed):
            pick_program([])

    def test_ambiguous(self):
        with self.assertRaises(Failed):
            pick_program([('a.nca', PROGRAM, BASE), ('b.nca', PROGRAM, BASE)])
        with self.assertRaises(Failed):
            pick_program([('a.nca', PROGRAM, UPDATE), ('b.nca', PROGRAM, UPDATE)])


class GameSourceTests(unittest.TestCase):
    def test_same_game(self):
        same_game, describe = BUILD['same_game'], BUILD['describe_identity']
        identity = BUILD['game_identity']
        update = {'app': '01006A800016E000', 'kind': 'update', 'version': 2031616}
        base = {'app': '01006A800016E000', 'kind': 'base', 'version': 0}
        self.assertEqual(identity(update), '01006A800016E000 update 2031616')
        self.assertEqual(identity(dict(base, version=None)), '01006A800016E000 base -')
        self.assertTrue(same_game(identity(update), identity(update)))
        self.assertFalse(same_game(identity(base), identity(update)))              # 11.0.0 folder, 13.0.5 files
        self.assertFalse(same_game(identity(update), identity(dict(update, version=1966080))))
        self.assertTrue(same_game(identity(base), identity(dict(base, version=None))))   # unknown: not held against it
        self.assertEqual(describe(identity(update)), "the game's update (title version v31)")
        self.assertEqual(describe(identity(update), '13.0.5'), "13.0.5 (the game's update)")
        self.assertEqual(describe('01006A800016E000 base -'), 'the game')


class BuildStepsTests(unittest.TestCase):
    """build() from the folders to the fighters, with what reads game files stubbed: which tools it
    runs, and with what."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='build-steps-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.game, keys, firmware = top / 'game', top / 'keys', top / 'firmware'
        for d in (self.game, keys, firmware):
            d.mkdir()
        dump = top / 'port' / 'dump'
        exefs = dump / 'exefs'
        exefs.mkdir(parents=True)
        (dump / 'modules').mkdir()
        for n in ('main.npdm', 'rtld', 'sdk', 'subsdk0'):   # (the program is copied out already: 13.0.5's)
            (exefs / n).write_bytes(b'')
        write_main(exefs / 'main', MAIN_13)
        self.top, self.exefs = top, exefs
        self.program = {'path': str(self.game / 'update.nsp'), 'name': 'prog.nca', 'container': 'update.nsp',
                        'app': '01006A800016E000', 'kind': 'update', 'version': 2031616, 'title_id': '01006A800016E000'}
        self.contents = [self.program]
        self.calls = []
        self.logged = []
        # (the module's own globals, which build() reads: run_path returns a copy of them)
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_steps')['build'].__globals__
        self.check_game_data = self.ns['check_game_data']
        self.cfg = dump / 'modules' / 'romfs.cfg'
        folders = {'game': str(self.game), 'keys': str(keys), 'firmware': str(firmware)}
        self.ns.update(
            ROOT=str(top / 'port'), DUMP=str(dump), EXEFS=str(exefs), FONTS=str(dump / 'fonts'), MODULES=str(dump / 'modules'),
            SAVED=str(dump / 'build_folders.txt'), SOURCE=str(dump / 'game_source.txt'), EXE=str(top / 'port' / 'build' / 'ssbu.exe'),
            log=lambda msg='': self.logged.append(msg), check_tools=lambda: None, install_packages=lambda: None,
            ask_folder=lambda key, given, saved: folders[key],
            find_program=lambda game, keys: (self.program['path'], BASE, self.program, {'contents': self.contents}),
            romfs_cfg_ok=lambda cfg: True, missing_fonts=lambda folder: [], check_same_game=lambda rec: None,
            check_game_data=lambda cfg, game: None,
            run=lambda args, failure: self.calls.append([str(a) for a in args]))
        self.args = types.SimpleNamespace(game=str(self.game), keys=str(keys), firmware=str(firmware), jobs=None,
                                          prepare_only=True)

    def aoc_call(self):
        calls = [c for c in self.calls if 'aoc-cfg' in c]
        return calls[0] if calls else None

    def test_dlc_next_to_the_game(self):
        # an emulator's folder, or loose DLC .nsp files: no dlc folder (this raised KeyError: 1)
        self.contents.append({'kind': 'dlc', 'app': '01006A800016E000'})
        self.ns['build'](self.args)
        call = self.aoc_call()
        self.assertEqual(call[call.index('aoc-cfg') + 1], str(self.game))
        self.assertEqual(call[call.index('--app') + 1], '01006A800016E000')
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))   # and on to the fighters

    def test_compiler_downloaded_while_the_game_files_are_prepared(self):
        # no compiler: step 1 hands back the download, started once the packages are in (step 2),
        # running through the game-file steps, waited for at step 9 (also with --prepare-only)
        download = types.SimpleNamespace(start=lambda: self.calls.append(['compiler start']),
                                         finish=lambda: self.calls.append(['compiler finish']))
        self.ns.update(check_tools=lambda: download, install_packages=lambda: self.calls.append(['packages']))
        self.ns['build'](self.args)
        order = [' '.join(c) for c in self.calls]
        start, finish = order.index('compiler start'), order.index('compiler finish')
        self.assertEqual(order.index('packages'), start - 1)
        self.assertTrue(any('arc.py' in c for c in order[start + 1:finish]))   # the fighters (step 8) in between
        self.assertEqual(finish, len(order) - 1)

    def test_dlc_folder(self):
        (self.game / 'dlc').mkdir()
        self.ns['build'](self.args)
        call = self.aoc_call()
        self.assertEqual(call[call.index('aoc-cfg') + 1], str(self.game / 'dlc'))

    def test_no_dlc(self):
        self.contents.append({'kind': 'dlc', 'app': '0100000000010000'})   # another game's
        self.ns['build'](self.args)
        self.assertIsNone(self.aoc_call())
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))

    def game_data(self, parts):
        """romfs.cfg (step 6's, done) reading a RomFS that holds this data.arc (tests/test_arc_table.py)."""
        image = Path(self.temp.name) / 'romfs.bin'
        image.write_bytes(arc_table.romfs(arc_table.flat(parts)))
        self.cfg.write_text(f'{image}\n0\n')
        self.ns['check_game_data'] = self.check_game_data

    def test_an_early_release_stops_at_step_6(self):
        self.game_data(arc_table.EARLY[('uncompressed', 'v1')])
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        text = str(e.exception)
        self.assertIn("WindowSmash needs version 13.0.5 of the game, and the game data connected in step 6 is an early "
                      "release's", text)
        self.assertIn(f"Put the game's 13.0.5 update (an .nsp file) in {self.game}, next to the game.", text)
        self.assertNotIn('romfs.bin', text)
        steps = [m.strip() for m in self.logged if m.lstrip().startswith('[')]
        self.assertTrue(steps[-1].startswith('[6/9]'), steps[-1])
        self.assertEqual(self.calls, [])   # (no DLC, no fighters)

    def test_another_version_stops_at_step_6(self):
        parts = arc_table.readable_arc(version=0x0B0000)   # (11.0.0's data.arc)
        self.game_data(parts)
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('the game data connected in step 6 is version 11.0.0', str(e.exception))
        self.assertEqual(self.calls, [])

    def test_13_0_5_goes_on(self):
        self.game_data(arc_table.readable_arc())
        self.ns['write_version'] = lambda: None
        self.ns['build'](self.args)
        self.assertTrue(any('arc.py' in ' '.join(c) for c in self.calls))
        self.assertIn('  game data version 13.0.5', self.logged)

    def test_unreadable_game_data(self):
        self.game_data({0: bytes(0x38)})   # (no data.arc as the game's begins)
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('damaged or incomplete', str(e.exception))

    def test_told_by_the_scan_before_decrypting(self):
        self.ns['romfs_cfg_ok'] = lambda cfg: False   # (step 6 to do)
        self.program.update(kind='base', romfs='encrypted', data_version='', display_version='1.0.0')
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('this is the game at 1.0.0 without its 13.0.5 update', str(e.exception))
        self.assertFalse(any('decrypt' in c for c in self.calls))

    # ---- the program's version (step 4), and a folder set up from another version before

    def extracted(self, build_id):
        """extract_exefs, as if it copied out a program whose main module has this build id."""
        def extract(rec, keys):
            self.exefs.mkdir(parents=True, exist_ok=True)
            for n in ('main.npdm', 'rtld', 'sdk', 'subsdk0'):
                (self.exefs / n).write_bytes(b'')
            write_main(self.exefs / 'main', build_id)
            (self.top / 'port' / 'dump' / 'game_source.txt').write_text('01006A800016E000 base 0\n\t\t\n')
            self.calls.append(['extract'])
        return extract

    def old_folder(self):
        """This folder as one set up from 11.0.0 left it: what the game files made, and the player's own."""
        port = self.top / 'port'
        write_main(self.exefs / 'main', MAIN_11)
        files = {'dump/modules/romfs.cfg': 'x', 'dump/modules/aoc.cfg': 'x', 'dump/modules/main.reloc.bin': 'x',
                 'dump/modules/romfs_patch.bin': 'x', 'dump/modules/update-01006a800016e000.nca': 'x',
                 'dump/modules/base-01006a800016e000.nca': 'base', 'dump/nro/bases.txt': 'x', 'dump/game_source.txt': 'x',
                 'version.txt': '11.0.0\n', 'module_bases.txt': 'x', 'build/ssbu.exe': 'x', 'build/main/g0000.c': 'c',
                 'save/user/save_data/system_data.bin': 'save', 'dump/fonts/a.bfttf': 'font',
                 'dump/shadercache/v1_ps_a.dxbc': 'shader', 'display.ini': 'mine', 'v13.0.5/save/x.bin': 'tree save'}
        for name, text in files.items():
            (port / name).parent.mkdir(parents=True, exist_ok=True)
            (port / name).write_text(text)
        return port

    def test_a_fresh_program_of_another_version_is_refused(self):
        for n in os.listdir(self.exefs):
            os.remove(self.exefs / n)
        self.ns['extract_exefs'] = self.extracted(MAIN_11)
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        text = str(e.exception)
        self.assertIn('WindowSmash needs version 13.0.5 of the game, and the program in prog.nca in update.nsp is '
                      'version 11.0.0', text)
        self.assertFalse(self.exefs.exists())   # (the folder is not left set up from it)
        self.assertFalse((self.top / 'port' / 'dump' / 'game_source.txt').exists())

    def test_a_fresh_program_of_13_0_5_goes_on(self):
        for n in os.listdir(self.exefs):
            os.remove(self.exefs / n)
        self.ns['extract_exefs'] = self.extracted(MAIN_13)
        self.ns['build'](self.args)
        self.assertEqual(self.calls[0], ['extract'])
        self.assertTrue(any(m.startswith('  game version 13.0.5 (main module 21450C647B8C5940)') for m in self.logged))

    def test_an_old_folder_is_set_up_again(self):
        port = self.old_folder()
        self.assertFalse(self.ns['prepared']())
        self.ns['extract_exefs'] = self.extracted(MAIN_13)
        self.ns['write_romfs_cfg'] = lambda game, keys, the_program: self.calls.append(['romfs'])
        self.args.game = None   # (not named: asked, as the folders' answers are there)
        self.ns['build'](self.args)
        shown = '\n'.join(self.logged)
        self.assertIn('This folder was set up from version 11.0.0 of the game. WindowSmash needs 13.0.5 now: setting it '
                      f'up again from {self.game}. Your save, settings and prepared shaders stay.', shown)
        self.assertIn(f"v13.0.5{os.sep} is not used any more", shown)
        self.assertEqual([c[0] for c in self.calls[:2]], ['extract', 'romfs'])
        for gone in ('dump/modules/romfs.cfg', 'dump/modules/aoc.cfg', 'dump/modules/main.reloc.bin',
                     'dump/modules/romfs_patch.bin', 'dump/modules/update-01006a800016e000.nca', 'dump/nro',
                     'version.txt', 'module_bases.txt', 'build/ssbu.exe'):
            self.assertFalse((port / gone).exists(), gone)
        for kept in ('dump/modules/base-01006a800016e000.nca', 'build/main/g0000.c', 'save/user/save_data/system_data.bin',
                     'dump/fonts/a.bfttf', 'dump/shadercache/v1_ps_a.dxbc', 'display.ini', 'v13.0.5/save/x.bin'):
            self.assertTrue((port / kept).exists(), kept)

    def test_an_old_folder_is_left_as_it_is_while_the_game_runs(self):
        port = self.old_folder()
        before = sorted(str(p) for p in port.rglob('*'))
        self.ns['in_use'] = lambda path: True
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('close it, then run this again', str(e.exception))
        self.assertEqual(sorted(str(p) for p in port.rglob('*')), before)

    def test_an_old_folder_with_unlabelled_files_is_checked_first(self):
        """Renamed files (no metadata): the program is copied out on the side and looked at before
        anything is cleared."""
        port = self.old_folder()
        self.program.update(version=None)
        probe = self.top / 'port' / 'dump' / 'exefs.probe'
        for build_id, refused in (('1234567890ABCDEF1234567890ABCDEF12345678', True), (MAIN_13, False)):
            def run(args, failure, build_id=build_id):
                self.calls.append([str(a) for a in args])
                if 'exefs.probe' in ' '.join(map(str, args)):
                    write_main(probe / 'main', build_id)
            self.ns['run'] = run
            self.ns['extract_exefs'] = self.extracted(MAIN_13)
            self.ns['write_romfs_cfg'] = lambda game, keys, the_program: None
            before = sorted(str(p) for p in port.rglob('*'))
            if refused:
                with self.assertRaises(self.ns['Failed']) as e:
                    self.ns['build'](self.args)
                self.assertIn('the program in prog.nca in update.nsp is another version', str(e.exception))
                self.assertEqual(sorted(str(p) for p in port.rglob('*')), before)   # (nothing cleared)
            else:
                self.ns['build'](self.args)
                self.assertFalse((port / 'dump' / 'modules' / 'romfs_patch.bin').exists())   # (set up again)
            self.assertFalse(probe.exists())

    def test_a_program_labelled_13_0_5_that_is_not(self):
        for n in os.listdir(self.exefs):
            os.remove(self.exefs / n)
        self.ns['extract_exefs'] = self.extracted('1234567890ABCDEF1234567890ABCDEF12345678')
        with self.assertRaises(self.ns['Failed']) as e:   # (the metadata says v31; the program isn't 13.0.5's)
            self.ns['build'](self.args)
        self.assertIn('the program in prog.nca in update.nsp is another version', str(e.exception))

    def test_an_old_folder_without_13_0_5_is_left_as_it_is(self):
        port = self.old_folder()
        self.program.update(kind='base', version=0, data_version='11.0.0', romfs='plain')
        before = sorted(str(p) for p in port.rglob('*'))
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('This WindowSmash folder was set up from version 11.0.0 of the game. WindowSmash needs version '
                      '13.0.5, and this is the game at 11.0.0 without its 13.0.5 update: put that update (.nsp) in '
                      'this folder.', str(e.exception))
        self.assertEqual(sorted(str(p) for p in port.rglob('*')), before)   # (nothing cleared)
        self.assertEqual(self.calls, [])


class CompilerDownloadTests(unittest.TestCase):
    """Step 1 with no working compiler (Windows): the build gets one without Visual Studio (a made-up
    msvc_deps here: no downloads)."""

    def setUp(self):
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_compiler')['build'].__globals__
        self.logged = []
        self.deps = types.ModuleType('msvc_deps')
        self.deps.Failed = type('Failed', (Exception,), {})
        self.deps.log = print
        self.deps.answer, self.deps.result, self.deps.removed = (True, {'license': 'https://x/license'}), 0, False
        self.deps.agree = lambda: self.deps.answer
        self.deps.remove = lambda: setattr(self.deps, 'removed', True)

        def install(agreed):
            self.deps.log('  deps\\llvm: LLVM 23.1.3, 326 files, 226 MB')
            self.deps.log('    LLVM: 50% of 276 MB')
            if isinstance(self.deps.result, Exception):
                raise self.deps.result
            return self.deps.result
        self.deps.install = install
        self.problems = []
        self.ns.update(LINUX=False, MACOS=False, log=lambda msg='': self.logged.append(msg),
                       windows_compiler_problem=lambda cc: self.problems.pop(0) if self.problems else None)
        patcher = mock.patch.dict(sys.modules, {'msvc_deps': self.deps})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_check_only_says_so(self):
        self.problems = ['No compiler (clang-cl.exe) was found.']
        with self.assertRaisesRegex(self.ns['Failed'], 'No compiler'):
            self.ns['check_tools'](fetch=False)   # (--check: the installer decides what to get)
        self.assertIsNone(self.ns['check_tools'](fetch=False))   # a compiler that works: nothing to do

    def test_downloaded(self):
        self.problems = ['No compiler (clang-cl.exe) was found.']
        import cc
        with mock.patch.object(cc, 'WINSYSROOT', None):   # (this PC's own choice aside: no deps\msvc)
            download = self.ns['check_tools']()
        self.assertIsNotNone(download)
        self.assertFalse(self.deps.removed)
        self.assertTrue(any('about 570 MB' in m for m in self.logged))
        download.start()
        download.finish()
        shown = '\n'.join(self.logged)
        self.assertIn('deps\\llvm: LLVM 23.1.3', shown)
        self.assertNotIn('50% of', shown)   # (progress only while it is waited for)
        self.assertIs(self.deps.log, print)   # its own log again

    def test_broken_deps_set_up_afresh(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('SSBU_CLANG', None)
            self.problems = ['the test program does not run.']
            with mock.patch.object(self.ns['CompilerDownload'], 'ask', classmethod(lambda cls, broken=False: broken)):
                import cc
                with mock.patch.object(cc, 'WINSYSROOT', 'C:\\x\\deps\\msvc'):
                    self.assertIs(self.ns['check_tools'](), True)   # (asked with broken=True)

    def test_declined_or_offline(self):
        self.deps.answer = (False, {'license': 'https://x/license'})
        with self.assertRaisesRegex(self.ns['Failed'], "Microsoft's license terms were not accepted"):
            self.ns['CompilerDownload'].ask()

        def offline():
            raise self.deps.Failed("Microsoft's Visual Studio channel list could not be read: no network.")
        self.deps.agree = offline
        with self.assertRaisesRegex(self.ns['Failed'], 'Check the internet connection'):
            self.ns['CompilerDownload'].ask()

    def test_download_failed(self):
        for result in (1, OSError('disk full')):
            self.deps.result = result
            download = self.ns['CompilerDownload'].ask()
            download.start()
            with self.assertRaisesRegex(self.ns['Failed'], 'could not be downloaded'):
                download.finish()
            self.assertIs(self.deps.log, print)

    def test_ssbu_clang_is_not_replaced(self):
        self.problems = ['SSBU_CLANG\'s compiler could not build a test program.']
        with mock.patch.dict(os.environ, {'SSBU_CLANG': 'C:\\mine\\clang-cl.exe'}):
            with self.assertRaisesRegex(self.ns['Failed'], 'could not build'):
                self.ns['check_tools']()


class MarkerTests(unittest.TestCase):
    def test_python_version_markers(self):
        marker_applies = BUILD['marker_applies']
        here = sys.version_info[:2]
        self.assertEqual(marker_applies('python_version < "3.14"'), here < (3, 14))
        self.assertEqual(marker_applies(' python_version >= "3.14" '), here >= (3, 14))
        self.assertTrue(marker_applies('python_version >= "3.10"'))
        self.assertFalse(marker_applies('python_version < "3.0"'))
        self.assertFalse(marker_applies('python_version >= "3.10" and python_version < "3.1"'))
        self.assertTrue(marker_applies('platform_system == "Linux"'))   # (unknown clauses are taken as holding)


class JobsTests(unittest.TestCase):
    def test_by_installed_ram(self):
        for gb, jobs in ((4, 4), (8, 4), (12, 4), (15.9, 4), (16, 8), (24, 8), (32, 16), (64, 16)):
            self.assertEqual(jobs_for_ram(gb), jobs, gb)


class RomfsCfgTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='build-game-test-')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.image = self.folder / 'romfs.bin'
        self.image.write_bytes(b'image')
        self.cfg = self.folder / 'romfs.cfg'

    def test_plain(self):
        self.assertFalse(romfs_cfg_ok(self.cfg))
        self.cfg.write_text(f'{self.image}\n0\n')
        self.assertTrue(romfs_cfg_ok(self.cfg))
        self.image.unlink()
        self.assertFalse(romfs_cfg_ok(self.cfg))

    def test_layered(self):
        layers = self.folder / 'romfs.layers'
        layers.write_bytes(b'layers')
        self.cfg.write_text(f'layered\n{layers}\n{self.image}\n{self.folder / "patch.bin"}\n')
        self.assertFalse(romfs_cfg_ok(self.cfg))
        (self.folder / 'patch.bin').write_bytes(b'patch')
        self.assertTrue(romfs_cfg_ok(self.cfg))


class VersionFileTests(unittest.TestCase):
    """version.txt, the game version the launcher window and the game's settings menu show."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='version-test-')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name)
        self.logged = []
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_version')['build'].__globals__
        self.ns.update(ROOT=str(self.top), MODULES=str(self.top / 'dump' / 'modules'), EXEFS=str(self.top / 'dump' / 'exefs'),
                       log=self.logged.append)
        self.arc = types.ModuleType('arc')
        self.arc.Arc = lambda cfg: types.SimpleNamespace(version=0x0D0005)   # (data.arc 13.0.5)
        nso = types.ModuleType('nso')
        nso.Module = lambda path: types.SimpleNamespace(build_id='ABCD')
        patcher = mock.patch.dict(sys.modules, {'arc': self.arc, 'nso': nso})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_written_once(self):
        self.ns['write_version']()
        text = (self.top / 'version.txt').read_text()
        self.assertEqual(text.splitlines()[0], '13.0.5')
        self.assertTrue(text.endswith('main ABCD\ndata.arc 13.0.5\n'))
        (self.top / 'version.txt').write_text('13.0.5 (mine)\n')
        self.ns['write_version']()   # (a version.txt there is kept: reset_game_files removes it)
        self.assertEqual((self.top / 'version.txt').read_text(), '13.0.5 (mine)\n')

    def test_unreadable_does_not_stop_the_build(self):
        def broken(cfg):
            raise OSError('no game data')
        self.arc.Arc = broken
        self.ns['write_version']()
        self.assertFalse((self.top / 'version.txt').exists())
        self.assertIn('could not be read', self.logged[-1])


if __name__ == '__main__':
    unittest.main()
