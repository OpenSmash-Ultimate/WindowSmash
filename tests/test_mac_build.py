"""The macOS build (tools/mac: toolchain.py, build_mac.py, make_overlay_font.m), build_game.py's macOS
steps and the arm64 codec configuration (tools/codec_libs.py, third_party/libvpx/config-arm64). What needs a Mac -
the Command Line Tools, SDL3.framework, Core Text, codesign - runs only on one; the rest anywhere,
with the toolchain stood in for. No game files."""
from pathlib import Path
import contextlib
import importlib.util
import io
import json
import os
import plistlib
import runpy
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
MAC = ROOT / 'tools' / 'mac'
sys.path.insert(0, str(ROOT / 'tools'))
import codec_libs  # noqa: E402

DARWIN = sys.platform == 'darwin'
X86_FLAGS = ('-mavx2', '-mfma', '-mf16c', '-mbmi', '-mbmi2', '-mlzcnt', '-mpopcnt', '/arch:AVX2')


def real_toolchain():
    """tools/mac/toolchain.py, loaded under a name of its own (tools/linux has a toolchain module too)."""
    spec = importlib.util.spec_from_file_location('mac_toolchain_under_test', MAC / 'toolchain.py')
    tc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tc)
    return tc


def stub_toolchain(runtime=ROOT / 'runtime', sdl3_dir=ROOT / 'deps' / 'SDL3'):
    """What build_mac.py takes from the toolchain, with no compiler behind it."""
    tc = types.ModuleType('toolchain')
    tc.AR, tc.CLANG, tc.CODESIGN, tc.NM = 'ar', 'clang', '/usr/bin/codesign', 'nm'
    tc.TARGET = ['-arch', 'arm64', '-mmacosx-version-min=14.0', '-isysroot', '/sdk']
    tc.CFLAGS = tc.TARGET + ['-mcpu=apple-m1', '-ffp-contract=off', '-w']
    tc.CXXFLAGS = ['-x', 'c++', '-std=gnu++20', '-fexceptions']
    tc.MIN_MACOS, tc.REPO, tc.RUNTIME = '14.0', str(ROOT), str(runtime)
    tc.SDL3_DIR, tc.SDL3_FRAMEWORK = str(sdl3_dir), str(Path(sdl3_dir) / 'SDL3.framework')
    tc.compile_each = lambda jobs, n=16, extra=(): []
    tc.run = lambda cmd: (0, '')
    return tc


def load_build_mac(argv, toolchain):
    """tools/mac/build_mac.py's globals, as run with these arguments and this toolchain."""
    saved = sys.modules.get('toolchain'), sys.argv, list(sys.path)
    sys.modules['toolchain'] = toolchain
    sys.argv = ['build_mac.py', *argv]
    try:
        return runpy.run_path(str(MAC / 'build_mac.py'), run_name='build_mac_under_test')['main'].__globals__
    finally:
        if saved[0] is None:
            sys.modules.pop('toolchain', None)
        else:
            sys.modules['toolchain'] = saved[0]
        sys.argv, sys.path[:] = saved[1], saved[2]


def write(path, text=''):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, newline='\n')
    return path


def fake_module(build, name, rows, nro=False):
    """A generated module dir, as recomp.py writes it: manifest.tsv, its files, g_module.c."""
    d = build / 'nro' / name if nro else build / name
    for fname, _insns, _biggest, text in rows:
        write(d / fname, text)
    write(d / 'g_module.c', '#include "module.h"\n')
    write(d / 'manifest.tsv', ''.join(f'{f}\t{i}\t{b}\n' for f, i, b, _t in rows))
    return d


# ---- the toolchain ---------------------------------------------------------------------------
@unittest.skipUnless(DARWIN, 'needs the Command Line Tools')
class ToolchainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tc = real_toolchain()

    def test_flags(self):
        f = self.tc.CFLAGS
        for flag in ('-mmacosx-version-min=14.0', '-mcpu=apple-m1', '-ffp-contract=off', '-fno-strict-aliasing',
                     '-fno-stack-protector', '-fvisibility=hidden', '-w'):
            self.assertIn(flag, f)
        self.assertEqual(f[f.index('-arch') + 1], 'arm64')
        self.assertEqual(f[f.index('-isysroot') + 1], self.tc.SDK)
        self.assertFalse(set(X86_FLAGS) & set(f))
        self.assertFalse({'-fno-pic', '-fno-plt', '-ffast-math', '-ffp-contract=fast', '-ffp-contract=on'} & set(f))
        # (both together drop the unwind tables C++ exceptions need through the generated C)
        self.assertFalse({'-fno-unwind-tables', '-fno-asynchronous-unwind-tables'} <= set(f))
        self.assertEqual(self.tc.MIN_MACOS, '14.0')
        for tool in (self.tc.CLANG, self.tc.AR, self.tc.NM):
            self.assertTrue(os.access(tool, os.X_OK), tool)

    def test_sdk_floor(self):
        """An SDK older than the runtime needs is refused at once, with what to do, rather than
        when the runtime fails to compile after the game was turned into C."""
        self.assertGreaterEqual(self.tc.SDK_VERSION, self.tc.MIN_SDK)
        with tempfile.TemporaryDirectory(prefix='old sdk ') as tmp:
            write(Path(tmp) / 'SDKSettings.json', json.dumps({'CanonicalName': 'macosx14.4', 'Version': '14.4'}))
            self.assertEqual(self.tc.sdk_version(tmp), (14, 4))
            with mock.patch.dict(os.environ, {'SDKROOT': tmp}):
                with self.assertRaises(SystemExit) as e:
                    real_toolchain()
        text = str(e.exception)
        self.assertIn(f'the macOS SDK is 14.4 ({tmp}), and WindowSmash needs the macOS 15 SDK or newer', text)
        self.assertIn('System Settings > General > Software Update', text)
        self.assertIn('SDKROOT names this SDK', text)
        sdks = Path(self.tc.SDK).parent
        for old in sorted(sdks.glob('MacOSX15*.sdk')):   # (an SDK 15 the Command Line Tools keep: at the floor)
            self.assertEqual(self.tc.sdk_version(str(old))[0], 15)

    def test_languages(self):
        self.assertEqual(self.tc.language('window.m'), ['-fobjc-arc'])
        self.assertEqual(self.tc.language('eh.cpp'), ['-x', 'c++', '-std=gnu++20', '-fexceptions'])
        self.assertEqual(self.tc.language('g0001_cxx.cpp'), self.tc.language('eh.cpp'))
        self.assertEqual(self.tc.language('fiber_arm64.S'), [])
        self.assertEqual(self.tc.language('host.c'), [])

    def test_run_nice_not_taskpolicy(self):
        calls = []
        real = subprocess.run
        try:
            subprocess.run = lambda cmd, **kw: calls.append(cmd) or types.SimpleNamespace(returncode=0, stdout='', stderr='')
            self.tc.run(['clang', '-v'])
        finally:
            subprocess.run = real
        self.assertEqual(calls, [['nice', '-n', '10', 'clang', '-v']])

    def test_objects(self):
        """What the flags make: arm64 objects for macOS 14 whose multiply-adds stay two
        operations (IEEE-exact, as the guest's), and each kind of source compiled."""
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            write(t / 'fma.c', 'float f(float a, float b, float c) { return a * b + c; }\n')
            write(t / 'objc.m', '#import <Foundation/Foundation.h>\nNSString *name(void) { return @"x"; }\n')
            write(t / 'cxx.cpp', '#include <stdexcept>\nextern "C" int g(void) { try { throw 1; } catch (int) { return 2; } }\n')
            write(t / 'asm.S', '.text\n.globl _h\n_h:\n    ret\n')
            jobs = [(str(t / n), str(t / (n + '.o')), '-O2') for n in ('fma.c', 'objc.m', 'cxx.cpp', 'asm.S')]
            self.assertEqual(self.tc.compile_each(jobs, 4), [])
            otool = subprocess.run(['otool', '-l', str(t / 'fma.c.o')], capture_output=True, text=True).stdout
            self.assertIn('minos 14.0', otool)
            asm = subprocess.run([self.tc.CLANG, '-S', '-O2', *self.tc.CFLAGS, str(t / 'fma.c'), '-o', '-'],
                                 capture_output=True, text=True).stdout
            self.assertNotIn('fmadd', asm)
            fused = subprocess.run([self.tc.CLANG, '-S', '-O2', '-arch', 'arm64', str(t / 'fma.c'), '-o', '-'],
                                   capture_output=True, text=True).stdout
            self.assertIn('fmadd', fused)   # (without the flag clang fuses them: the test sees the difference)


# ---- build_mac.py ------------------------------------------------------------------------------
class BuildMacTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mac build test ')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name)
        self.tree = self.top / 'tree'
        build = self.tree / 'build'
        fake_module(build, 'main', [('g0000.c', 100, 50, 'void f(void) {}\n'),
                                    ('g0001.c', 90000, 60000, 'x; __try { } __except (1) { }\n'),
                                    ('g0002.c', 30000, 25000, 'void g(void) {}\n'),
                                    ('g0003.c', 30000, 25000, 'y; __try { }\n')])
        fake_module(build, 'subsdk0', [('g0000.c', 1, 1, '')])
        fake_module(build, 'sdk', [('g0000.c', 1, 1, '')])
        fake_module(build, 'fighter', [('g0000.c', 1, 1, '')], nro=True)
        (build / 'nro' / 'half_done').mkdir()   # (no manifest.tsv yet: not a module)
        self.ns = load_build_mac(['--port', str(self.tree)], stub_toolchain())

    def test_paths(self):
        out = self.tree / 'build-mac'
        self.assertEqual(self.ns['OUT'], str(out))
        self.assertEqual(self.ns['EXE'], str(out / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'))
        inc = self.ns['RUNTIME_INCLUDES']
        self.assertEqual(inc, ['-I', str(ROOT / 'runtime' / 'macos' / 'include'), '-I', str(ROOT / 'runtime' / 'linux' / 'include'),
                               '-I', str(ROOT / 'runtime'), '-I', str(out / 'gen'), '-F', str(ROOT / 'deps' / 'SDL3'),
                               '-I', str(ROOT / 'third_party' / 'libvpx')])
        self.assertEqual(self.ns['RUNTIME_DEFINES'], ['-DSSBU_HAVE_OPUS', '-DSSBU_HAVE_VPX', '-DSSBU_HAVE_SDL3'])

    def test_modules(self):
        names = [(n, os.path.relpath(o, self.ns['OBJ'])) for n, _s, o in self.ns['modules']()]
        self.assertEqual(names, [('main', 'main'), ('subsdk0', 'subsdk0'), ('sdk', 'sdk'),
                                 ('fighter', os.path.join('nro', 'fighter'))])

    def test_game_jobs(self):
        main_obj = Path(self.ns['OBJ']) / 'main'
        by_obj = {os.path.relpath(j[1], self.ns['OBJ']): j for j in self.ns['game_jobs']()}
        self.assertEqual(len(by_obj), 5 + 2 * 3)   # (main's four and g_module.c, two in each of the others)
        self.assertEqual(by_obj[os.path.join('main', 'g0000.o')][2], '-O2')
        self.assertEqual(by_obj[os.path.join('main', 'g0002.o')][2], '-O1')        # (biggest function > 20000)
        self.assertEqual(by_obj[os.path.join('main', 'g_module.o')][2], '-O1')
        self.assertEqual(by_obj[os.path.join('main', 'g0001.o')][2], '-O0')        # (C++, biggest > 50000)
        self.assertEqual(by_obj[os.path.join('main', 'g0003.o')][2], '-O1')
        wrapper = Path(by_obj[os.path.join('main', 'g0001.o')][0])
        self.assertEqual(wrapper, main_obj / 'g0001_cxx.cpp')
        self.assertEqual(wrapper.read_text(), '#include "recomp.h"\nextern "C" {\n#include "%s"\n}\n'
                         % (self.tree / 'build' / 'main' / 'g0001.c'))
        for j in by_obj.values():
            self.assertEqual(j[3], ['-I', str(ROOT / 'runtime')])

    def compile(self, jobs, ns=None):
        """The jobs as compile_all does them, the compiler stood in for: each object written (an
        hour later than now) and recorded."""
        ns = ns or self.ns
        later = time.time() + 3600
        for j in jobs:
            write(j[1])
            os.utime(j[1], (later, later))
            ns['record_compiled'](j)

    def test_up_to_date_and_stale_objects(self):
        self.compile(self.ns['game_jobs']())
        stale = Path(self.ns['OBJ']) / 'main' / 'g0009.o'   # (an older partitioning's file)
        write(stale)
        self.assertEqual(self.ns['game_jobs'](), [])
        self.assertFalse(stale.exists())
        objs = [os.path.basename(o) for o in self.ns['game_objects'](str(self.tree / 'build' / 'main'), str(Path(self.ns['OBJ']) / 'main'))]
        self.assertEqual(objs, ['g0000.o', 'g0001.o', 'g0002.o', 'g0003.o', 'g_module.o'])
        # objects with nothing to say what they were compiled from (a build_mac.py before the record): all again
        os.remove(self.ns['GAME_SOURCES'])
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(len(self.ns['game_jobs']()), 11)
        self.assertIn('no record of the C its objects were compiled from: compiling all of it again', out.getvalue())

    def test_older_c_of_another_module(self):
        """The C of another build of the module put in its place, older than the objects (as an
        earlier WindowSmash's sharing between folders did: hard links keep the other folder's times):
        every file it changed is compiled again, though none is newer than its object."""
        self.compile(self.ns['game_jobs']())
        main = self.tree / 'build' / 'main'
        old = time.time() - 7 * 86400
        for n in ('g0000.c', 'g_module.c'):
            write(main / n, '/* the module of another build id */\n')
            os.utime(main / n, (old, old))
        again = sorted(os.path.relpath(j[1], self.ns['OBJ']) for j in self.ns['game_jobs']())
        self.assertEqual(again, [os.path.join('main', 'g0000.o'), os.path.join('main', 'g_module.o')])
        self.compile(self.ns['game_jobs']())
        self.assertEqual(self.ns['game_jobs'](), [])

    def test_stopped_half_way(self):
        """What a build stopped part of the way through compiled is kept: each object is recorded
        as it is compiled."""
        jobs = self.ns['game_jobs']()
        self.compile(jobs[:4])
        self.assertEqual(len(self.ns['game_jobs']()), len(jobs) - 4)

    def test_compile_command_and_levels(self):
        self.compile(self.ns['game_jobs']())
        tc = stub_toolchain()
        tc.CFLAGS = tc.CFLAGS + ['-fno-unwind-tables']
        ns = load_build_mac(['--port', str(self.tree)], tc)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(len(ns['game_jobs']()), 11)   # (another compile command: all of them)
        self.assertIn('the compile command changed since its objects were compiled', out.getvalue())
        self.compile(ns['game_jobs'](), ns)
        self.assertEqual(ns['game_jobs'](), [])
        tc.TARGET[-1] = tc.CFLAGS[tc.CFLAGS.index('-isysroot') + 1] = '/another/sdk'   # (not the SDK's path)
        self.assertEqual(load_build_mac(['--port', str(self.tree)], tc)['game_jobs'](), [])
        manifest = self.tree / 'build' / 'main' / 'manifest.tsv'   # (a file's level changed: that one)
        manifest.write_text(manifest.read_text().replace('g0000.c\t100\t50', 'g0000.c\t100\t30000'))
        again = ns['game_jobs']()
        self.assertEqual([(os.path.basename(j[1]), j[2]) for j in again], [('g0000.o', '-O1')])

    def test_generated_tables(self):
        text = self.ns['wrap_missing_text']([0x7100001234, 0x71000ABCD0])
        self.assertIn('__attribute__((weak, visibility("hidden"))) void f_7100001234_orig(Cpu *c) { rt_wrap_missing(c); }\n', text)
        self.assertIn('f_71000abcd0_orig', text)
        self.assertNotIn('__attribute__((weak))', text)
        nro = self.ns['nro_table_text'](['fighter'])
        self.assertIn('extern const RtModule rt_module_fighter;', nro)
        self.assertIn('const u32 rt_nro_count = 1;', nro)
        hle = self.ns['hle_table_text'](['a', 'b'])
        self.assertIn('{"a", hle_a},', hle)
        self.assertIn('const u32 hle_count = 2;', hle)
        srcs = self.ns['generated_sources']()
        self.assertEqual([os.path.basename(s) for s, _o, _f in srcs], ['hle_table.c', 'wrap_missing.c', 'nro_table.c'])
        self.assertIn('rt_module_fighter', Path(srcs[2][0]).read_text())

    def test_info_plist(self):
        p = plistlib.loads(plistlib.dumps(self.ns['info_plist']('13.0.5')))
        self.assertEqual(p['CFBundleExecutable'], 'ssbu')
        self.assertEqual(p['CFBundleIdentifier'], 'io.github.sno2207.windowsmash')
        self.assertEqual((p['CFBundleName'], p['CFBundleDisplayName']), ('WindowSmash', 'WindowSmash'))
        self.assertEqual(p['CFBundleIconFile'], 'windowsmash')
        self.assertEqual(p['CFBundlePackageType'], 'APPL')
        self.assertEqual(p['LSMinimumSystemVersion'], '14.0')
        self.assertIs(p['NSHighResolutionCapable'], True)
        self.assertEqual(p['LSApplicationCategoryType'], 'public.app-category.action-games')
        self.assertIs(p['LSSupportsGameMode'], True)
        self.assertTrue(p['NSLocalNetworkUsageDescription'])
        self.assertEqual(p['CFBundleShortVersionString'], '13.0.5')
        self.assertEqual(self.ns['info_plist']()['CFBundleVersion'], '1.0')
        write(self.tree / 'version.txt', '13.0.5\n# ...\n')
        self.assertEqual(self.ns['tree_version'](), '13.0.5')
        write(self.tree / 'version.txt', 'odd\n')
        self.assertIsNone(self.ns['tree_version']())

    def test_icon_picture(self):
        png = self.ns['ico_png'](str(ROOT / 'docs' / 'assets' / 'windowsmash.ico'))
        self.assertTrue(png.startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertEqual(struct.unpack('>II', png[16:24]), (256, 256))   # (its IHDR)
        self.assertIsNone(self.ns['ico_png'](str(ROOT / 'docs' / 'assets' / 'windowsmash.ico'), 512))

    def test_use_tree(self):
        out = Path(self.ns['OUT'])
        (out / 'obj' / 'main').mkdir(parents=True)
        write(out / 'port.txt', '/somewhere/else/tree\n')   # (this tree's own build-mac, the tree since moved)
        with contextlib.redirect_stdout(io.StringIO()):
            self.ns['use_tree']()
        self.assertEqual((out / 'port.txt').read_text().strip(), str(self.tree))
        copied_from = self.top / 'WindowSmash-before'   # (this tree a copy of one that is still there: still its own build-mac)
        (copied_from / 'build').mkdir(parents=True)
        write(out / 'port.txt', f'{copied_from}\n')
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.ns['use_tree']()
        self.assertIn(f"this tree's own, built when it was at {copied_from}", said.getvalue())
        self.assertTrue((out / 'obj' / 'main').is_dir())
        self.assertEqual((out / 'port.txt').read_text().strip(), str(self.tree))
        other = load_build_mac(['--port', str(self.tree), '--out', str(self.top / 'shared')], stub_toolchain())
        shared = self.top / 'shared'
        (shared / 'obj' / 'main').mkdir(parents=True)
        write(shared / 'port.txt', '/another/tree\n')   # (an --out of another tree's game)
        with self.assertRaises(SystemExit):
            other['use_tree']()
        self.assertTrue((shared / 'obj' / 'main').is_dir())

    def test_replace_own(self):
        """--replace starts this tree's own build over too (not with --link-only)."""
        out = Path(self.ns['OUT'])
        for p in ('obj/main/g0000.o', 'obj/nro/fighter/g0000.o', 'obj/rt/host.o', 'obj/game_sources.tsv', 'ssbu.syms',
                  'WindowSmash.app/Contents/MacOS/ssbu'):
            write(out / p)
        write(out / 'port.txt', f'{self.tree}\n')
        saved = sys.argv
        try:   # (use_tree reads the command line as it runs)
            sys.argv = ['build_mac.py', '--port', str(self.tree), '--replace', '--link-only']
            self.ns['use_tree']()
            self.assertTrue((out / 'obj' / 'main' / 'g0000.o').exists())
            sys.argv = ['build_mac.py', '--port', str(self.tree), '--replace']
            with contextlib.redirect_stdout(io.StringIO()):
                self.ns['use_tree']()
        finally:
            sys.argv = saved
        for p in ('obj/main', 'obj/nro', 'obj/game_sources.tsv', 'ssbu.syms', 'WindowSmash.app'):
            self.assertFalse((out / p).exists(), p)
        self.assertTrue((out / 'obj' / 'rt' / 'host.o').exists())   # (the runtime has its own flags stamp)

    def test_object_names(self):
        names = self.ns['object_names'](['/r/macos/fiber.S', '/r/macos/fiber.c', '/r/macos/window.m'], '/o')
        self.assertEqual(names, [os.path.join('/o', n) for n in ('fiber_S.o', 'fiber_c.o', 'window.o')])

    def test_runtime_sources(self):
        rt = self.top / 'runtime'
        for n in ('host.c', 'kernel.c', 'gpu.c', 'gpu_d3d11.c', 'gpu_overlay.c', 'gpu_menu.c', 'gpu_texture.c', 'hle_fs.c', 'ipc_audio.c',
                  'net_relay.c', 'debug_watchdog.c', 'debug_profile.c', 'smoke.c', 'difftest.c', '._host.c', 'recomp.h',
                  'linux/win32.c', 'linux/upnp.c', 'linux/eh.cpp', 'linux/window.c', 'linux/profile.c',
                  'linux/d3dcompile_vkd3d.c', 'linux/include/windows.h', 'macos/window.m', 'macos/memory.c',
                  'macos/fiber_arm64.S', 'macos/gpu_metal.m', 'macos/gpu_metal.h', 'macos/._window.m',
                  'macos/include/dxgiformat.h', 'macos/include/x.c'):
            write(rt / n)
        ns = load_build_mac(['--port', str(self.tree)], stub_toolchain(runtime=rt))
        got = {os.path.relpath(s, rt): os.path.relpath(o, ns['OBJ']) for s, o in ns['runtime_sources']()}
        want = ['debug_watchdog.c', 'gpu.c', 'gpu_menu.c', 'gpu_texture.c', 'hle_fs.c', 'host.c', 'ipc_audio.c', 'kernel.c', 'net_relay.c',
                'linux/win32.c', 'linux/upnp.c', 'linux/eh.cpp', 'macos/fiber_arm64.S', 'macos/gpu_metal.m',
                'macos/memory.c', 'macos/window.m']
        self.assertEqual(sorted(got), sorted(n.replace('/', os.sep) for n in want))
        self.assertEqual(got[os.path.join('macos', 'window.m')], os.path.join('rt', 'macos', 'window.o'))
        self.assertEqual(got[os.path.join('linux', 'eh.cpp')], os.path.join('rt', 'linux', 'eh.o'))
        self.assertEqual(got['host.c'], os.path.join('rt', 'host.o'))
        headers = {os.path.relpath(h, rt) for h in ns['runtime_headers']() if h.startswith(str(rt))}
        self.assertEqual(headers, {'recomp.h', os.path.join('linux', 'include', 'windows.h'),
                                   os.path.join('macos', 'gpu_metal.h'), os.path.join('macos', 'include', 'dxgiformat.h')})

    def test_runtime_flags_stamp(self):
        with contextlib.redirect_stdout(io.StringIO()):
            first = self.ns['runtime_jobs']()
        self.assertTrue(first)
        for src, obj, opt, flags in first:
            self.assertEqual(opt, '-O1' if os.path.dirname(src) == self.ns['GEN'] else '-O2')
            if not src.endswith('nro_table.c'):
                self.assertEqual(flags, self.ns['RUNTIME_INCLUDES'] + self.ns['RUNTIME_DEFINES'])
            write(obj)
            later = time.time() + 3600
            os.utime(obj, (later, later))
        write(Path(self.ns['GEN']) / 'overlay_font.h')
        os.utime(Path(self.ns['GEN']) / 'overlay_font.h', (1, 1))
        # without the stamp of the command they were compiled with: all of them again
        self.assertEqual(len(self.ns['runtime_jobs']()), len(first))
        write(Path(self.ns['OBJ']) / 'rt' / 'cflags.txt', self.ns['runtime_command']())
        self.assertEqual(self.ns['runtime_jobs'](), [])

    def test_overlay_font_fallback(self):
        """No compiler: a header of empty glyphs, and the build goes on."""
        tc = stub_toolchain()
        tc.run = lambda cmd: (1, 'no compiler')
        ns = load_build_mac(['--port', str(self.tree)], tc)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            ns['overlay_font']()
        self.assertIn('could not draw the overlay font', out.getvalue())
        text = (Path(ns['GEN']) / 'overlay_font.h').read_text()
        self.assertEqual(text, 'static const unsigned char overlay_font[110592] = {0};\n')


@unittest.skipUnless(DARWIN, 'needs Core Text, sips, iconutil and codesign')
class BuildMacOnMacTests(unittest.TestCase):
    """The parts of build_mac.py that need macOS's own tools, with the real toolchain."""

    @classmethod
    def setUpClass(cls):
        cls.tc = real_toolchain()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mac build test ')
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name) / 'tree'
        self.tree.mkdir()
        self.ns = load_build_mac(['--port', str(self.tree)], self.tc)

    def test_overlay_font(self):
        """make_overlay_font.m's header: gpu_overlay.c's atlas, in make_overlay_font.py's layout."""
        with contextlib.redirect_stdout(io.StringIO()):
            self.ns['overlay_font']()
        lines = (Path(self.ns['GEN']) / 'overlay_font.h').read_text().splitlines()
        self.assertTrue(lines[0].startswith('/* Generated by tools/mac/make_overlay_font.m: 384x288 coverage, printable ASCII in 24x48 cells.'))
        self.assertEqual(lines[1], 'static const unsigned char overlay_font[110592] = {')
        self.assertEqual(lines[-1], '};')
        rows = lines[2:-1]
        self.assertEqual(len(rows), 110592 // 32)
        values = [int(v) for row in rows for v in row.rstrip(',').split(',')]
        self.assertEqual(len(values), 110592)
        self.assertTrue(all(0 <= v <= 255 for v in values))
        for source in ('gpu_overlay.c', 'macos/gpu_metal_overlay.m'):
            overlay = (ROOT / 'runtime' / source).read_text()
            for name, value in (('CELL_W', 24), ('CELL_H', 48), ('COLS', 16)):
                self.assertRegex(overlay, rf'#define {name} {value}\b', source)

        def cell(c):
            i = ord(c) - 32
            x0, y0 = i % 16 * 24, i // 16 * 48
            return [values[(y0 + y) * 384 + x0 + x] for y in range(48) for x in range(24)]
        self.assertEqual(max(cell(' ')), 0)
        self.assertEqual(max(cell(chr(127))), 0)   # (the solid cell: gpu_overlay.c fills it)
        for c in 'AMgW|~':
            self.assertGreater(sum(v > 128 for v in cell(c)), 10, c)
        # the glyphs' tops below the cell's first rows (TextOut at y + 2), the baseline in the cell
        a = cell('A')
        rows_inked = [y for y in range(48) if max(a[y * 24:(y + 1) * 24]) > 128]
        self.assertGreater(rows_inked[0], 2)
        self.assertLess(rows_inked[-1], 44)

    def test_icon(self):
        icns = self.ns['app_icon']()
        with open(icns, 'rb') as f:
            self.assertEqual(f.read(4), b'icns')
        mtime = os.path.getmtime(icns)
        self.assertEqual(self.ns['app_icon'](), icns)   # (made once)
        self.assertEqual(os.path.getmtime(icns), mtime)

    @unittest.skipUnless((ROOT / 'deps' / 'SDL3' / 'SDL3.framework' / 'SDL3').exists(), 'needs deps/SDL3')
    def test_bundle(self):
        """The app around a program: Info.plist, SDL3.framework, the icon, signed as a whole."""
        exe = Path(self.ns['EXE'])
        src = Path(self.temp.name) / 'main.c'
        write(src, '#include <stdio.h>\nint main(void) { puts("ok"); return 0; }\n')
        exe.parent.mkdir(parents=True)
        code, out = self.tc.run([self.tc.CLANG, *self.tc.TARGET, str(src), '-o', str(exe)])
        self.assertEqual(code, 0, out)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(self.ns['bundle']())
        app = Path(self.ns['APP'])
        plist = plistlib.loads((app / 'Contents' / 'Info.plist').read_bytes())
        self.assertEqual(plist['CFBundleExecutable'], 'ssbu')
        self.assertTrue((app / 'Contents' / 'Frameworks' / 'SDL3.framework' / 'Versions' / 'Current').is_symlink())
        self.assertTrue((app / 'Contents' / 'Resources' / 'windowsmash.icns').is_file())
        r = subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = subprocess.run(['codesign', '-dv', str(app)], capture_output=True, text=True)
        self.assertIn('Identifier=io.github.sno2207.windowsmash', r.stderr)
        self.assertIn('Signature=adhoc', r.stderr)


# ---- codecs ------------------------------------------------------------------------------------
class CodecListsTests(unittest.TestCase):
    def test_windows_lists_unchanged(self):
        lib, _root, folders, includes, defines, _have = codec_libs.LIBRARIES['opus']
        self.assertTrue(lib.endswith('opus.lib'))
        self.assertIn('celt/x86', folders)
        self.assertIn('OPUS_X86_PRESUME_AVX2', defines)
        self.assertEqual(codec_libs.LIBRARIES['libvpx'][3], ('config', ''))

    def test_arm64_lists(self):
        libs = codec_libs.static_libraries('/out')
        lib, root, folders, includes, defines, have = libs['opus']
        self.assertEqual(lib, os.path.join('/out', 'codecs', 'libopus.a'))
        self.assertFalse([f for f in folders if 'x86' in f])
        self.assertFalse([d for d in defines if 'X86' in d])
        srcs = codec_libs.sources(root, folders)
        self.assertGreater(len(srcs), 130)
        self.assertFalse([s for s in srcs if os.sep + 'x86' + os.sep in s])
        self.assertIn(os.path.join(codec_libs.OPUS, 'src', 'opus_decoder.c'), srcs)
        self.assertTrue(have())
        lib, root, folders, includes, defines, have = libs['libvpx']
        self.assertEqual(lib, os.path.join('/out', 'codecs', 'libvpx.a'))
        self.assertEqual(includes, ('config-arm64', ''))
        self.assertEqual(defines, ())
        self.assertEqual(folders, codec_libs.VPX_FOLDERS)
        self.assertTrue(have())
        flags = codec_libs.static_flags(['-arch', 'arm64'], root, includes, defines)
        self.assertEqual(flags[:3], ['-arch', 'arm64', '-O2'])
        self.assertIn(os.path.join(codec_libs.VPX, 'config-arm64'), flags)
        self.assertNotIn(os.path.join(codec_libs.VPX, 'config'), flags)
        with self.assertRaises(ValueError):
            codec_libs.static_libraries('/out', 'x86_64')

    def test_vpx_arm64_config(self):
        cfg = Path(codec_libs.VPX, 'config-arm64')
        config = (cfg / 'vpx_config.h').read_text()
        for line in ('#define VPX_ARCH_AARCH64 1', '#define VPX_ARCH_X86_64 0', '#define VPX_ARCH_X86 0', '#define HAVE_NEON 0',
                     '#define HAVE_PTHREAD_H 1', '#define HAVE_UNISTD_H 1', '#define CONFIG_GCC 1', '#define CONFIG_MSVS 0',
                     '#define INLINE      inline', '#define CONFIG_VP9_DECODER 1', '#define CONFIG_VP9_ENCODER 0',
                     '#define CONFIG_RUNTIME_CPU_DETECT 0'):
            self.assertIn(line + '\n', config)
        windows = (Path(codec_libs.VPX, 'config') / 'vpx_config.h').read_text()
        self.assertEqual(len(config.splitlines()), len(windows.splitlines()))   # (the same settings, other values)
        for name in ('vp9_rtcd.h', 'vpx_dsp_rtcd.h', 'vpx_scale_rtcd.h'):
            text = (cfg / name).read_text()
            self.assertNotIn('x86', text)
            self.assertIn('static void setup_rtcd_internal(void)\n{\n}\n', text)
            # every function as on Windows: its C version
            defines = lambda t: sorted(l for l in t.splitlines() if l.startswith('#define '))   # noqa: E731
            self.assertEqual(defines(text), defines((Path(codec_libs.VPX, 'config') / name).read_text()))
        self.assertEqual((cfg / 'vpx_version.h').read_text(), (Path(codec_libs.VPX, 'config') / 'vpx_version.h').read_text())

    def test_appledouble_files_left_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(Path(tmp) / 'src' / 'a.c')
            write(Path(tmp) / 'src' / '._a.c', '\0\5\x16\7')
            self.assertEqual(codec_libs.sources(tmp, ('src',)), [os.path.join(tmp, 'src', 'a.c')])

    def test_static_staleness(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = write(root / 'src' / 'a.c', 'int a;')
            old = time.time() - 100
            os.utime(source, (old, old))
            lib = root / 'libx.a'
            lib.write_bytes(b'!<arch>\n')
            self.assertFalse(codec_libs.static_is_current(str(lib), str(root), ('src',), (), 'clang -O2\n'))   # (no stamp)
            write(str(lib) + '.flags', 'clang -O2\n')
            self.assertTrue(codec_libs.static_is_current(str(lib), str(root), ('src',), (), 'clang -O2\n'))
            self.assertFalse(codec_libs.static_is_current(str(lib), str(root), ('src',), (), 'clang -O3\n'))


DECODE_CHECK = r"""#include <stdio.h>
#include <string.h>
#include "opus.h"
#include "opus_multistream.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
int main(void) {
    int err = 0;
    OpusDecoder *d = opus_decoder_create(48000, 2, &err);
    if (!d || err != OPUS_OK) return 10;
    short pcm[960 * 2];
    if (opus_decode(d, NULL, 0, pcm, 960, 0) != 960) return 11;   /* a lost packet: concealment */
    unsigned char map[6] = {0, 1, 2, 3, 4, 5};
    OpusMSDecoder *ms = opus_multistream_decoder_create(48000, 6, 4, 2, map, &err);
    if (!ms || err != OPUS_OK) return 12;
    vpx_codec_ctx_t ctx;
    vpx_codec_dec_cfg_t cfg = {4, 0, 0};
    if (vpx_codec_dec_init(&ctx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return 13;
    unsigned char junk[64];
    memset(junk, 0x5A, sizeof junk);
    if (vpx_codec_decode(&ctx, junk, sizeof junk, NULL, 0) == VPX_CODEC_OK) return 14;   /* not a frame: refused */
    printf("%s; %s\n", opus_get_version_string(), vpx_codec_iface_name(vpx_codec_vp9_dx()));
    vpx_codec_destroy(&ctx);
    opus_multistream_decoder_destroy(ms);
    opus_decoder_destroy(d);
    return 0;
}
"""


@unittest.skipUnless(DARWIN, 'needs the Command Line Tools')
class CodecBuildTests(unittest.TestCase):
    def test_build_and_decode(self):
        """libopus.a and libvpx.a built for arm64 with the program's flags, linked into a program
        that sets up an opus, an opus multistream and a VP9 decoder."""
        tc = real_toolchain()
        with tempfile.TemporaryDirectory(prefix='codecs ') as tmp:
            log = []
            libs = codec_libs.build_static(tmp, tc.CLANG, tc.CFLAGS, tc.AR, log=log.append)
            self.assertEqual([os.path.basename(l) for l in libs], ['libopus.a', 'libvpx.a'])
            for lib in libs:
                r = subprocess.run(['lipo', '-archs', lib], capture_output=True, text=True)
                self.assertEqual(r.stdout.strip(), 'arm64', lib)
            log.clear()
            self.assertEqual(codec_libs.build_static(tmp, tc.CLANG, tc.CFLAGS, tc.AR, log=log.append), libs)
            self.assertEqual(log, [])   # (up to date: nothing built again)
            src, exe = os.path.join(tmp, 'check.c'), os.path.join(tmp, 'check')
            write(src, DECODE_CHECK)
            code, out = tc.run([tc.CLANG, *tc.CFLAGS, '-O1', '-I', os.path.join(codec_libs.OPUS, 'include'),
                                '-I', codec_libs.VPX, src, *libs, '-o', exe])
            self.assertEqual(code, 0, out)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('libopus 1.5.2', r.stdout)
            self.assertIn('VP9 Decoder v1.15.2', r.stdout)


# ---- tools/mac's test programs, built as their first lines say and run ---------------------------
@unittest.skipUnless(DARWIN, 'needs the Command Line Tools, and Metal')
class TestProgramTests(unittest.TestCase):
    """shimtest.c (the platform layer, and the loader and kernel on a made-up game), msltest.m (the
    shader translator's Metal output on the GPU) and gfxtest.m (the Metal renderer, off screen):
    each exits 0 when all its checks pass. A minute or so with Metal's shader cache cold, much
    less after. (tools/mac/boottest, which shows the game's window, is run by hand.)"""
    F = ['-arch', 'arm64', '-mmacosx-version-min=14.0']
    INCLUDES = ['-I', 'runtime/macos/include', '-I', 'runtime/linux/include', '-I', 'runtime']

    def build(self, *cmd):
        r = subprocess.run(['clang', *cmd], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def run_program(self, exe, *args, cwd=None, env=None):
        r = subprocess.run([exe, *args], cwd=cwd, env=env, capture_output=True, text=True, timeout=600)
        if 'no Metal device' in r.stderr:
            self.skipTest('no Metal device')
        self.assertEqual(r.returncode, 0, (r.stdout + r.stderr)[-6000:])
        return r.stdout

    def test_shimtest(self):
        with tempfile.TemporaryDirectory(prefix='shimtest ') as tmp:
            f = [*self.F, '-mcpu=apple-m1', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-stack-protector', '-fvisibility=hidden',
                 '-O2', *self.INCLUDES]
            shim = ['runtime/linux/win32.c', 'runtime/macos/memory.c', 'runtime/macos/fiber.c', 'runtime/macos/fiber_arm64.S',
                    'runtime/macos/platform.c', 'runtime/macos/profile.c']
            host, exe = os.path.join(tmp, 'host.o'), os.path.join(tmp, 'shimtest')
            self.build(*f, '-Dmain=rt_host_main', '-c', 'runtime/host.c', '-o', host)
            self.build(*f, '-DSHIMTEST_HOST', 'tools/mac/shimtest.c', host, 'runtime/kernel.c', 'runtime/linux/upnp.c', *shim, '-o', exe)
            with open(os.path.join(tmp, 'ssbu.syms'), 'w') as syms:   # (the profiler's names)
                subprocess.run(['nm', '-n', '--defined-only', exe], stdout=syms, check=True)
            out = self.run_program(exe, cwd=tmp)
            self.assertRegex(out, r'\n\d+ checks, 0 failed\n$')

    def test_msltest(self):
        with tempfile.TemporaryDirectory(prefix='msltest ') as tmp:
            exe = os.path.join(tmp, 'msltest')
            self.build(*self.F, '-fobjc-arc', '-fno-strict-aliasing', '-ffp-contract=off', '-O1', *self.INCLUDES,
                       'tools/mac/msltest.m', 'runtime/gpu_shader.c', '-framework', 'Metal', '-framework', 'Foundation', '-o', exe)
            out = self.run_program(exe, 'all', cwd=tmp)
            self.assertRegex(out, r'corpus: \d+ programs, every one of the 147 encodings, .*: ok')   # (more programs as tests are added)
            self.assertRegex(out, r'gpu: \d+ checks, 0 failed')

    def test_gfxtest(self):
        with tempfile.TemporaryDirectory(prefix='gfxtest ') as tmp:
            font, exe = os.path.join(tmp, 'font'), os.path.join(tmp, 'gfxtest')
            self.build(*self.F, '-fobjc-arc', '-O1', 'tools/mac/make_overlay_font.m', '-framework', 'CoreText', '-framework', 'CoreGraphics',
                       '-framework', 'Foundation', '-o', font)
            subprocess.run([font, os.path.join(tmp, 'overlay_font.h')], capture_output=True, check=True)
            self.build(*self.F, '-mcpu=apple-m1', '-fobjc-arc', '-fno-strict-aliasing', '-ffp-contract=off', '-O1', '-w', *self.INCLUDES,
                       '-I', tmp, 'tools/mac/gfxtest.m', 'runtime/macos/gpu_metal.m', 'runtime/macos/gpu_metal_overlay.m',
                       'runtime/gpu_shader.c', 'runtime/gpu_texture.c', 'runtime/linux/win32.c', 'runtime/macos/memory.c',
                       'runtime/macos/platform.c', 'runtime/macos/fiber.c', 'runtime/macos/fiber_arm64.S', '-framework', 'Metal',
                       '-framework', 'QuartzCore', '-framework', 'CoreGraphics', '-framework', 'Foundation', '-o', exe)
            work = os.path.join(tmp, 'work')
            os.makedirs(work)
            out = self.run_program(exe, cwd=tmp, env={**os.environ, 'GFXTEST_DIR': work})
            self.assertRegex(out, r'\d+ checks, 0 failed')


# ---- build_game.py on macOS ----------------------------------------------------------------------
class BuildOnMacTests(unittest.TestCase):
    """build_game.py's macOS steps (the host stood in for): the C generated, then compiled and
    linked by tools/mac/build_mac.py; romfs.bin, the RAM, the program in use, what is current."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mac-build-test-')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name)
        self.port = top / 'port'
        dump = self.port / 'dump'
        (dump / 'exefs').mkdir(parents=True)
        (dump / 'modules').mkdir()
        for n in ('main.npdm', 'rtld', 'sdk', 'subsdk0'):
            (dump / 'exefs' / n).write_bytes(b'')
        main = bytearray(0x100)   # (13.0.5's main module, as far as its header goes)
        main[:4], main[0x40:0x54] = b'NSO0', bytes.fromhex('21450C647B8C5940ED5B68218EA128FD3C5D597D')
        (dump / 'exefs' / 'main').write_bytes(main)
        self.calls, self.popens, self.logged = [], [], []
        self.ns = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_mac')['build'].__globals__
        fake_subprocess = types.SimpleNamespace(**{k: getattr(subprocess, k) for k in dir(subprocess) if not k.startswith('_')})
        fake_subprocess.Popen = lambda args, **kw: self.popens.append(args)
        self.exe = self.port / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'
        self.ns.update(
            HOST='macos', LINUX=False, MACOS=True, ROOT=str(self.port), TOOLS=str(self.port / 'tools'), DUMP=str(dump),
            EXEFS=str(dump / 'exefs'), FONTS=str(dump / 'fonts'), MODULES=str(dump / 'modules'),
            SAVED=str(dump / 'build_folders.txt'), SOURCE=str(dump / 'game_source.txt'), EXE=str(self.exe),
            log=lambda msg='': self.logged.append(msg), check_tools=lambda: None, install_packages=lambda: None,
            ask_folder=lambda key, given, saved: str(top), romfs_cfg_ok=lambda cfg: True, check_game_data=lambda cfg, game: None, missing_fonts=lambda folder: [],
            check_same_game=lambda rec: None, find_program=lambda game, keys: ('p', 0x01006A800016E000, {}, {'contents': []}),
            installed_ram_gb=lambda: 8.0, subprocess=fake_subprocess,
            run=lambda args, failure: self.calls.append([str(a) for a in args]))
        (dump / 'modules' / 'aoc.cfg').write_text('')
        self.args = types.SimpleNamespace(game=None, keys=None, firmware=None, jobs=None, prepare_only=False)

    def tools_run(self):
        return [next((Path(a).name for a in c if a.endswith('.py')), c[0]) for c in self.calls]

    def test_step_9(self):
        self.ns['build'](self.args)
        self.assertEqual(self.tools_run(), ['arc.py', 'recomp.py', 'build_mac.py'])
        recomp, mac = self.calls[1:]
        self.assertIn('--generate-only', recomp)
        self.assertEqual(recomp[recomp.index('--jobs') + 1], '4')   # (8 GB of RAM)
        self.assertEqual(mac[mac.index('--port') + 1], str(self.port))
        self.assertEqual(mac[mac.index('--jobs') + 1], '4')
        self.assertEqual(Path(mac[1]).relative_to('tools'), Path('mac', 'build_mac.py'))
        self.assertEqual(self.popens, [['caffeinate', '-i', '-w', str(os.getpid())]])   # (kept awake while it builds)
        text = '\n'.join(self.logged)
        self.assertIn(f"[9/9] Build the game ({os.path.join('build-mac', 'WindowSmash.app')})", text)
        self.assertIn('Play SSBU.command', text)
        self.assertNotIn('.bat', text)

    def test_running_game_not_overwritten(self):
        write(self.exe)
        self.ns['running_from'] = lambda path: path == str(self.exe)
        with self.assertRaises(self.ns['Failed']) as e:
            self.ns['build'](self.args)
        self.assertIn('The game is running', str(e.exception))
        self.assertEqual(self.tools_run(), ['arc.py'])

    def test_dispatch(self):
        seen = []
        g = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_check')['check_tools'].__globals__
        g.update(MACOS=True, LINUX=False, log=lambda m='': None, check_mac_tools=lambda: seen.append('mac'),
                 check_linux_tools=lambda: seen.append('linux'))
        g['check_tools']()
        self.assertEqual(seen, ['mac'])
        self.assertEqual(self.ns['shown']('dump\\modules\\romfs.cfg'), 'dump/modules/romfs.cfg')

    def test_romfs_bin(self):
        """A romfs.bin in the game folder: checked and read in place, as on Linux (no PowerShell)."""
        game = Path(self.temp.name) / 'game'
        write(game / 'romfs.bin')
        (game / 'romfs.bin').write_bytes(struct.pack('<Q', 0x50) + bytes(72))
        self.ns['write_romfs_cfg'](str(game), None, None)
        cfg = (self.port / 'dump' / 'modules' / 'romfs.cfg').read_text()
        self.assertEqual(cfg, f"{game / 'romfs.bin'}\n0\n")
        self.assertEqual(self.calls, [])
        (game / 'romfs.bin').write_bytes(b'NCA3' + bytes(76))
        with self.assertRaises(self.ns['Failed']):
            self.ns['write_romfs_cfg'](str(game), None, None)

    def test_build_is_current(self):
        port = self.port
        for n in ('runtime/host.c', 'runtime/linux/win32.c', 'runtime/linux/include/windows.h', 'runtime/macos/window.m',
                  'runtime/macos/include/dxgiformat.h', 'tools/recomp.py', 'tools/mac/build_mac.py', 'tools/mac/make_overlay_font.m'):
            write(port / n)
        old = time.time() - 100
        for dp, _d, files in os.walk(port):
            for f in files:
                os.utime(os.path.join(dp, f), (old, old))
        current = self.ns['build_is_current']
        self.assertFalse(current())   # (no program)
        write(self.exe)
        self.assertFalse(current())   # (no SDL3.framework in the app)
        write(self.exe.parent.parent / 'Frameworks' / 'SDL3.framework' / 'SDL3')
        self.assertTrue(current())
        for n in ('runtime/macos/window.m', 'runtime/macos/include/dxgiformat.h', 'runtime/linux/include/windows.h',
                  'tools/mac/make_overlay_font.m'):
            later = time.time() + 100
            os.utime(port / n, (later, later))
            self.assertFalse(current(), n)
            os.utime(port / n, (old, old))
        self.assertTrue(current())

    @unittest.skipUnless(DARWIN, 'libproc, and the Command Line Tools to build a program to run')
    def test_running_from(self):
        tc = real_toolchain()
        with tempfile.TemporaryDirectory(prefix='running from ') as tmp:
            sleep = os.path.join(tmp, 'ssbu')
            write(os.path.join(tmp, 'sleep.c'), '#include <unistd.h>\nint main(void) { sleep(30); return 0; }\n')
            code, out = tc.run([tc.CLANG, *tc.TARGET, os.path.join(tmp, 'sleep.c'), '-o', sleep])
            self.assertEqual(code, 0, out)
            self.assertFalse(self.ns['running_from'](sleep))
            p = subprocess.Popen([sleep, '30'])
            try:
                deadline = time.time() + 10
                while not self.ns['running_from'](sleep) and time.time() < deadline:
                    time.sleep(0.05)
                self.assertTrue(self.ns['running_from'](sleep))
                self.assertTrue(self.ns['in_use'](sleep))
            finally:
                p.kill()
                p.wait()
            self.assertFalse(self.ns['running_from'](sleep))
            self.assertFalse(self.ns['in_use'](os.path.join(tmp, 'absent')))


class BuildGameMacTests(unittest.TestCase):
    """build_game.py's questions and checks on a Mac (and the other POSIX host, Linux)."""

    def setUp(self):
        self.g = runpy.run_path(str(ROOT / 'tools/build_game.py'), run_name='build_game_folders')['ask_folder'].__globals__

    def test_dragged_folders(self):
        """A folder dragged into Terminal comes escaped as the shell would (My\\ Games, with a
        space after it); on Windows in quotes."""
        with tempfile.TemporaryDirectory(prefix='typed ') as tmp:
            dump = Path(tmp) / 'Switch Dump'
            dump.mkdir()
            odd = Path(tmp) / "Bob's games"
            odd.mkdir()
            self.g.update(MACOS=True, LINUX=False)
            typed = self.g['typed_folder']
            self.assertEqual(typed(str(dump).replace(' ', '\\ ') + ' '), str(dump))
            self.assertEqual(typed(f"'{dump}'"), str(dump))
            self.assertEqual(typed(f'"{dump}" '), str(dump))
            self.assertEqual(typed(f' {dump} '), str(dump))    # (as it is, when that is a folder)
            self.assertEqual(typed(str(odd)), str(odd))        # (an apostrophe of the name)
            self.assertEqual(typed('~/MyDump'), '~/MyDump')
            self.assertEqual(typed(''), '')
            saved = {'hinted': True}
            self.g.update(SAVED=os.path.join(tmp, 'build_folders.txt'), log=lambda m='': None)
            with mock.patch('builtins.input', lambda prompt: str(dump).replace(' ', '\\ ') + ' '):
                self.assertEqual(self.g['ask_folder']('game', None, saved), str(dump))
            self.g.update(MACOS=False, LINUX=True)
            self.assertEqual(self.g['typed_folder'](f"'{dump}' "), str(dump))
            self.g.update(MACOS=False, LINUX=False)   # (Windows: as before, its backslashes left alone)
            self.assertEqual(self.g['typed_folder'](' "D:\\My Dump" '), 'D:\\My Dump')
            self.assertEqual(self.g['typed_folder']('D:\\Games\\Smash'), 'D:\\Games\\Smash')

    def test_runtime_compiled_for_errors(self):
        """--check compiles the runtime's sources for their errors (-fsyntax-only), so an SDK
        that lacks something the runtime uses is found at step 1."""
        tc = stub_toolchain()
        tc.SDK, tc.SDK_VERSION = '/sdk', (15, 4)
        seen = []

        def compile_each(jobs, n=16, extra=()):
            seen.append((jobs, extra))
            return [(jobs[0][0], "gpu_metal.m:2463:39: error: property 'lodBias' not found")]
        tc.compile_each = compile_each
        saved = sys.modules.get('toolchain'), sys.modules.pop('build_mac', None), list(sys.path)
        sys.modules['toolchain'] = tc
        sys.path.insert(0, str(MAC))
        try:
            with tempfile.TemporaryDirectory(prefix='check ') as tmp:
                with self.assertRaises(self.g['Failed']) as e:
                    self.g['check_mac_runtime'](tc, tmp)
                self.assertTrue((Path(tmp) / 'overlay_font.h').exists())
        finally:
            sys.modules.pop('build_mac', None)
            if saved[0] is None:
                sys.modules.pop('toolchain', None)
            else:
                sys.modules['toolchain'] = saved[0]
            if saved[1] is not None:
                sys.modules['build_mac'] = saved[1]
            sys.path[:] = saved[2]
        jobs, extra = seen[0]
        self.assertEqual(extra, ('-fsyntax-only',))
        sources = {os.path.relpath(j[0], ROOT / 'runtime') for j in jobs}
        self.assertIn(os.path.join('macos', 'gpu_metal.m'), sources)
        self.assertIn('host.c', sources)
        self.assertFalse([n for n in sources if n.endswith('.S')])
        self.assertIn('-fsyntax-only', extra)
        self.assertIn('The Command Line Tools (macOS SDK 15.4: /sdk) cannot compile the game\'s runtime', str(e.exception))
        self.assertIn('lodBias', str(e.exception))


if __name__ == '__main__':
    unittest.main()
