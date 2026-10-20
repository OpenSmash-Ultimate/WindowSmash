"""The macOS installer (Install WindowSmash.command, tools/mac/install_mac.py) and launchers
(Play SSBU.command, launchers/*.command, tools/mac/launch.sh and pick_version.sh, the Play SSBU app the
installer makes). What decides is tested on any POSIX system with bash; what needs macOS (open,
osacompile, hdiutil, sips, codesign) only on a Mac, where a stand-in game app records how it was
started. No dialog is ever shown: osascript is stood in for. No game files."""
from pathlib import Path
import contextlib
import io
import json
import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import time
import types
import unittest
from unittest import mock

if os.name != 'posix':
    raise unittest.SkipTest('the macOS installer and launchers (bash scripts, POSIX paths)')

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'mac'))
import install_mac as im  # noqa: E402

BASH = '/bin/bash' if os.path.exists('/bin/bash') else shutil.which('bash')
DARWIN = sys.platform == 'darwin'
INSTALL = ROOT / 'Install WindowSmash.command'
SCRIPTS = [ROOT / n for n in im.LAUNCHERS]


def write(path, text, mode=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, newline='\n')
    if mode:
        path.chmod(mode)
    return path


def shell_function(path, name):
    """A function's text out of a script: from `name() {` to the `}` at its indentation."""
    lines = Path(path).read_text().splitlines()
    for i, line in enumerate(lines):
        if line.strip() == f'{name}() {{':
            indent = line[:len(line) - len(line.lstrip())]
            for j in range(i + 1, len(lines)):
                if lines[j] == indent + '}':
                    return '\n'.join(lines[i:j + 1]) + '\n'
    raise AssertionError(f'no function {name} in {path}')


class CheckTests(unittest.TestCase):
    def test_macos_version(self):
        self.assertEqual(im.macos_version('14.6.1\n'), (14, 6))
        self.assertEqual(im.macos_version('27.0'), (27, 0))
        self.assertEqual(im.macos_version('15'), (15, 0))
        self.assertIsNone(im.macos_version(''))
        self.assertIsNone(im.macos_version('macOS'))
        self.assertTrue(im.macos_version('13.7.2') < im.MIN_MACOS <= im.macos_version('14.0'))

    def test_firewall_state(self):
        self.assertEqual(im.firewall_state('Firewall is disabled. (State = 0)\n'), 'off')
        self.assertEqual(im.firewall_state('Firewall is enabled. (State = 1)\n'), 'on')
        self.assertEqual(im.firewall_state('Firewall is blocking all non-essential incoming connections. (State = 2)'), 'all')
        self.assertEqual(im.firewall_state('Firewall is enabled.'), 'on')
        self.assertEqual(im.firewall_state(''), 'off')

    def test_mount_types(self):
        text = ('/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)\n'
                '/dev/disk3s5 on /System/Volumes/Data (apfs, local, journaled, nobrowse, protect, root data)\n'
                '/dev/disk5s1 on /Volumes/My Games (exfat, local, nodev, nosuid, noowners)\n'
                '//me@nas/share on /Volumes/share (smbfs, nodev, nosuid, mounted by me)\n'
                'map auto_home on /System/Volumes/Data/home (autofs, automounted, nobrowse)\n')
        self.assertEqual(im.mount_types(text), [('/', 'apfs'), ('/System/Volumes/Data', 'apfs'), ('/Volumes/My Games', 'exfat'),
                                                ('/Volumes/share', 'smbfs'), ('/System/Volumes/Data/home', 'autofs')])

    def test_location_warnings(self):
        home = '/Users/me'
        synced = {'/Users/me/Library/Mobile Documents/com~apple~CloudDocs/Desktop'}
        self.assertEqual(im.location_warnings('/Users/me/WindowSmash', home, synced.__contains__, 'apfs'), [])
        self.assertEqual(im.location_warnings('/Users/me/Documents/WindowSmash', home, synced.__contains__, 'apfs'), [])
        one = im.location_warnings('/Users/me/Desktop/WindowSmash', home, synced.__contains__, 'apfs')
        self.assertEqual(len(one), 1)
        self.assertIn('iCloud Drive', one[0])
        self.assertIn('iCloud Drive', im.location_warnings('/Users/me/Library/Mobile Documents/com~apple~CloudDocs/WS', home,
                                                           lambda p: False)[0])
        self.assertIn('Downloads', im.location_warnings('/Users/me/Downloads/WindowSmash-main', home, lambda p: False)[0])
        self.assertIn('exfat', im.location_warnings('/Volumes/Games/WindowSmash', home, lambda p: False, 'exfat')[0])
        self.assertEqual(im.location_warnings('/Volumes/Games/WindowSmash', home, lambda p: False, 'hfs'), [])
        self.assertEqual(im.location_warnings('/Users/me/Desktops/WindowSmash', home, lambda p: True, None), [])

    def test_names(self):
        self.assertEqual(im.clean_name('  Mario   Bros \n'), 'Mario Bros')
        self.assertEqual(len(im.clean_name('x' * 40)), im.NAME_MAX)
        self.assertEqual(im.typed_path('/Users/me/My\\ Games/keys '), '/Users/me/My Games/keys')
        self.assertEqual(im.typed_path("'/Users/me/My Games'"), '/Users/me/My Games')
        self.assertEqual(im.typed_path('/Users/me/My Games'), '/Users/me/My Games')
        self.assertEqual(im.typed_path("/Users/me/Bob's games"), "/Users/me/Bob's games")
        self.assertEqual(im.typed_path('~/MyKeys'), os.path.expanduser('~/MyKeys'))

    def test_updates_follow_the_branch(self):
        self.assertEqual(im.il.ROOT, im.ROOT)   # (install_linux's helpers work on this folder)
        self.assertEqual(im.VENV, os.path.join(im.ROOT, 'deps', 'venv'))
        seen = []
        real, linux_branch = im.il.step_update, im.il.BRANCH
        im.il.step_update = lambda a: seen.append(im.il.BRANCH) or real(a)
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out):
                self.assertFalse(im.step_update(arguments(no_update=True, dry_run=True)))
        finally:
            im.il.step_update = real
        self.assertEqual(seen, [im.BRANCH])   # (this branch's updates, while it is not merged)
        self.assertEqual((im.il.BRANCH, im.il.step), (linux_branch, im.step))   # (and install_linux as it was)
        self.assertIn(f'[1/{im.STEPS}] Get the newest WindowSmash', out.getvalue())
        self.assertIn('not checking (--no-update)', out.getvalue())
        self.assertNotIn('[1/8]', out.getvalue())   # (install_linux's own heading)


@unittest.skipUnless(DARWIN and shutil.which('security'), 'needs macOS')
class StepTests(unittest.TestCase):
    """Steps 3 and 4 when something is not as it should be, the Mac stood in for."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='steps test ')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name)
        self.said = []
        for name, value in (('info', self.said.append), ('step_header', lambda n, t: None),
                            ('would', lambda m: self.said.append('WOULD ' + m))):
            patcher = mock.patch.object(im, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    @staticmethod
    def python(path, version, arch='arm64'):
        """A stand-in Python, answering the questions the installer asks of one."""
        ok = 0 if tuple(map(int, version.split('.')[:2])) >= (3, 10) and arch == 'arm64' else 1
        return write(path, '#!/bin/bash\n'
                     f'case "$2" in\n  *"sys.exit(not"*) exit {ok} ;;\n  *"platform.machine())"*) echo "{version} {arch}" ;;\n'
                     f'  *Cryptodome*) exit 0 ;;\n  *) echo "{version}" ;;\nesac\n', 0o755)

    def test_python_problem(self):
        self.assertEqual(im.python_problem(str(self.top / 'absent' / 'python')), 'missing')
        self.assertEqual(im.python_problem(str(self.python(self.top / 'old', '3.9.6'))), '3.9.6 arm64')
        self.assertEqual(im.python_problem(str(self.python(self.top / 'intel', '3.12.4', 'x86_64'))), '3.12.4 x86_64')
        self.assertIsNone(im.python_problem(str(self.python(self.top / 'good', '3.13.1'))))
        if os.uname().machine == 'arm64':   # (this test's own Python, on a Mac)
            self.assertIsNone(im.python_problem(sys.executable))
        if DARWIN and os.path.exists('/usr/bin/python3') and os.path.isdir('/Library/Developer/CommandLineTools'):
            real = subprocess.run(['/usr/bin/python3', '-c', 'import sys; print(sys.version_info[:2] >= (3, 10))'],
                                  capture_output=True, text=True).stdout.strip()
            if real == 'False':   # (Apple's own 3.9)
                self.assertIn('3.9', im.python_problem('/usr/bin/python3'))

    def test_old_venv_made_again(self):
        """A deps/venv of Python 3.9 (docs' python3 -m venv with Apple's /usr/bin/python3) is made
        again from the installer's own Python, rather than kept and the build stopped at step 5."""
        venv = self.top / 'deps' / 'venv'
        self.python(venv / 'bin' / 'python', '3.9.6')
        maker = write(self.top / 'python3.14', '#!/bin/bash\n'   # (the installer's Python: makes a good venv)
                      f'[ "$1 $2" = "-m venv" ] && mkdir -p "$3/bin" && cp "{self.top}/good" "$3/bin/python" && exit 0\nexit 1\n', 0o755)
        self.python(self.top / 'good', '3.14.8')
        a = types.SimpleNamespace(dry_run=True)
        with mock.patch.object(im, 'VENV', str(venv)), mock.patch.object(im.il, 'VENV', str(venv)), \
                mock.patch.object(im.sys, 'executable', str(maker)):
            im.step_python(a)
            self.assertTrue(any(m.startswith('WOULD make a Python of WindowSmash') for m in self.said), self.said)
            a.dry_run = False
            im.step_python(a)
            self.assertIsNone(im.python_problem(str(venv / 'bin' / 'python')))
            self.assertTrue(any('is a Python the tools cannot use (3.9.6 arm64' in m for m in self.said), self.said)
            self.assertIn('packages: installed', self.said)
            os.remove(venv / 'bin' / 'python')   # (and an installer Python that cannot make a usable one: said)
            self.python(self.top / 'good', '3.9.6')
            with self.assertRaises(im.Stop) as e:
                im.step_python(a)
            self.assertIn('cannot run the tools: they need Python 3.10 or newer, as arm64', str(e.exception))

    def test_old_sdk(self):
        self.assertEqual(im.MIN_SDK, (15, 0))
        tc = (ROOT / 'tools' / 'mac' / 'toolchain.py').read_text()
        self.assertIn('MIN_SDK = (15, 0)', tc)   # (the same floor as the build's)
        with mock.patch.object(im, 'developer_dir', lambda: '/Library/Developer/CommandLineTools'), \
                mock.patch.object(im, 'try_compiler', lambda: ('sdk', '14.4')):
            with self.assertRaises(im.Stop) as e:
                im.step_tools(types.SimpleNamespace(dry_run=False))
        self.assertIn('the macOS 14.4 SDK, and the game needs the macOS 15 SDK or newer', str(e.exception))
        self.assertIn('Software Update', str(e.exception))
        real = im.output

        def sdk_says(version):
            return lambda cmd, **kw: (0, version) if cmd == ['xcrun', '--show-sdk-version'] else real(cmd, **kw)
        if DARWIN and im.developer_dir():
            with mock.patch.object(im, 'output', sdk_says('14.4')):
                self.assertEqual(im.try_compiler(), ('sdk', '14.4'))
            with mock.patch.object(im, 'output', sdk_says('15.0')):
                self.assertIsNone(im.try_compiler()[0])

    def wait(self, window, installed_at=None, minutes=60):
        """wait_for_tools with a clock of its own: window(t) says whether the installer's window is
        open at t seconds; the tools are there from installed_at on. Returns (Stop or None, t)."""
        clock = [0.0]
        with mock.patch.object(im, 'developer_dir', lambda: installed_at is not None and clock[0] >= installed_at), \
                mock.patch.object(im, 'clt_installer_open', lambda: window(clock[0])), \
                mock.patch.object(im.time, 'time', lambda: clock[0]), \
                mock.patch.object(im.time, 'sleep', lambda n: clock.__setitem__(0, clock[0] + n)):
            try:
                im.wait_for_tools(minutes)
            except im.Stop as e:
                return e, clock[0]
        return None, clock[0]

    def test_tools_window_closed(self):
        """Cancel or Not Now in macOS's window: nothing will be installed, and that is said within
        half a minute rather than after an hour of waiting."""
        stop, t = self.wait(lambda t: t < 8)   # (opened, then Not Now)
        self.assertIn("window installing the Command Line Tools was closed", str(stop))
        self.assertLess(t, 40)
        stop, t = self.wait(lambda t: False)   # (closed before it was ever seen)
        self.assertIn('was closed', str(stop))
        stop, t = self.wait(lambda t: True, installed_at=600)   # (open all along, installed after 10 minutes)
        self.assertIsNone(stop)
        self.assertEqual(t, 600)
        stop, t = self.wait(lambda t: t < 600, installed_at=601)   # (Done clicked a moment before the tools show)
        self.assertIsNone(stop)
        stop, t = self.wait(lambda t: None, minutes=2)   # (no telling - another macOS: waited out, as before)
        self.assertIn('are not installed yet', str(stop))
        self.assertGreater(t, 120)
        self.assertTrue(any('Ctrl+C stops this' in m for m in self.said))

    def test_tools_window_is_found(self):
        app = self.top / 'Install Command Line Developer Tools.app'
        self.assertIsNone(im.clt_installer_open(str(app)))   # (no such app: cannot tell)
        app.mkdir()
        with mock.patch.object(im, 'output', lambda cmd, **kw: (1, '') if cmd == ['pgrep', '-f', str(app)] else (None, '')):
            self.assertIs(im.clt_installer_open(str(app)), False)
        with mock.patch.object(im, 'output', lambda cmd, **kw: (0, '123') if cmd == ['pgrep', '-f', str(app)] else (None, '')):
            self.assertIs(im.clt_installer_open(str(app)), True)
        if DARWIN:
            self.assertTrue(os.path.isdir(im.CLT_INSTALLER), im.CLT_INSTALLER)


class UnlockTests(unittest.TestCase):
    def test_no_save_yet(self):
        """Before the game was ever started there is no save: said as Windows' launcher says it."""
        with tempfile.TemporaryDirectory(prefix='unlock test ') as tmp:
            main = Path(tmp) / 'dump' / 'exefs' / 'main'
            main.parent.mkdir(parents=True)
            main.write_bytes(b'NSO0' + bytes(0x3C) + bytes.fromhex('4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C') + bytes(12))
            r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'unlock_all.py'), '--tree', tmp], capture_output=True,
                               text=True, timeout=60)
        self.assertEqual(r.returncode, 1)
        self.assertNotIn('Traceback', r.stderr)
        self.assertIn('No save found', r.stderr)
        self.assertIn('Start the game once, reach the main menu, close it, then run '
                      + ('launchers/Unlock All Fighters.command' if DARWIN else 'Unlock All Fighters') + ' again.', r.stderr)


class CertificateTests(unittest.TestCase):
    def test_python_without_certificates(self):
        import ssl
        nowhere = ssl.DefaultVerifyPaths('/no/cert.pem', '/no/certs', 'SSL_CERT_FILE', '/no/cert.pem', 'SSL_CERT_DIR', '/no/certs')
        env = {k: v for k, v in os.environ.items() if k != 'SSL_CERT_FILE'}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(im.ssl, 'get_default_verify_paths', lambda: nowhere):
            im.ssl_certificates(dry=True)
            pem = os.environ.get('SSL_CERT_FILE')
        self.assertEqual(pem, os.path.join(tempfile.gettempdir(), 'macos-root-certificates.pem'))   # (a dry run: not deps/)
        self.addCleanup(os.remove, pem)
        context = ssl.create_default_context(cafile=pem)
        self.assertGreater(len(context.get_ca_certs()), 50)   # (macOS's own roots)
        with mock.patch.dict(os.environ, {'SSL_CERT_FILE': '/mine.pem'}):
            im.ssl_certificates(dry=True)   # (one named already: left alone)
            self.assertEqual(os.environ['SSL_CERT_FILE'], '/mine.pem')


def arguments(**kw):
    base = dict(game=None, keys=None, firmware=None, online_name=None, jobs=None, skip_build=False, no_update=False,
                no_wizard=False, yes=False, dry_run=False)
    base.update(kw)
    return types.SimpleNamespace(**base)


class CandidateTests(unittest.TestCase):
    def test_folder_candidates(self):
        c = im.mac_folder_candidates('/Users/me', '/Users/me/Library/Application Support', '/Users/me/.local/share',
                                     '/Users/me/.config', ['/Volumes/Games/Switch'])
        self.assertEqual(c['keys'][0], '/Users/me/Library/Application Support/Ryujinx/system')   # (Ryujinx first)
        self.assertEqual(c['firmware'][0], '/Users/me/Library/Application Support/Ryujinx/bis/system/Contents/registered')
        self.assertEqual(c['game'][0], '/Volumes/Games/Switch')   # (where Ryujinx plays it from, before its NAND)
        for key, want in (('keys', '/Users/me/Library/Application Support/eden/keys'),
                          ('keys', '/Users/me/.local/share/citron/keys'),
                          ('keys', '/Users/me/.config/Ryujinx/system'),
                          ('firmware', '/Users/me/Library/Application Support/torzu/nand/system/Contents/registered'),
                          ('game', '/Users/me/Library/Application Support/Ryujinx/bis/user/Contents/registered'),
                          ('game', '/Users/me/Library/Application Support/Ryujinx/sdcard/Nintendo/Contents/registered'),
                          ('game', '/Users/me/.local/share/yuzu/sdmc/Nintendo/Contents/registered'),
                          ('game', '/Users/me/Library/Application Support/sudachi/nand/user/Contents/registered')):
            self.assertIn(want, c[key])
        self.assertEqual(c['keys'][-1], '/Users/me/.switch')
        # Application Support before ~/.local/share for each fork
        self.assertLess(c['keys'].index('/Users/me/Library/Application Support/yuzu/keys'),
                        c['keys'].index('/Users/me/.local/share/yuzu/keys'))
        d = im.mac_folder_candidates('/h', '/h/AS', '/x/data', '/x/config')
        self.assertTrue(all(p.startswith(('/h/', '/x/')) for p in d['keys'] + d['firmware'] + d['game']))
        self.assertIn('/x/data/suyu/nand/user/Contents/registered', d['game'])
        self.assertIn('/x/config/Ryujinx/bis/user/Contents/registered', d['game'])

    def test_ryujinx_game_dirs(self):
        text = json.dumps({'version': 57, 'game_dirs': ['/Volumes/Games/Switch', '', 3, '/Users/me/Roms'], 'other': 1})
        self.assertEqual(im.ryujinx_game_dirs(text), ['/Volumes/Games/Switch', '/Users/me/Roms'])
        self.assertEqual(im.ryujinx_game_dirs('{"game_dirs": null}'), [])
        self.assertEqual(im.ryujinx_game_dirs('not json'), [])
        self.assertEqual(im.ryujinx_game_dirs('[1, 2]'), [])
        self.assertEqual(im.ryujinx_game_dirs(''), [])

    def test_smash_first(self):
        names = {'/a': ['Zelda.nsp'], '/b': ['Super Smash Bros Ultimate [01006A800016E000][v0].nsp'],
                 '/c': ['01006a800016e800.nsp'], '/d': []}
        self.assertEqual(im.smash_first(['/a', '/b', '/c', '/d'], names.get), ['/b', '/c', '/a', '/d'])
        self.assertEqual(im.smash_first(['/a', '/d'], names.get), ['/a', '/d'])


class SdlTests(unittest.TestCase):
    def release(self, tag, assets, pre=False, draft=False):
        return {'tag_name': tag, 'prerelease': pre, 'draft': draft, 'assets': assets}

    def asset(self, name, digest=None):
        a = {'name': name, 'browser_download_url': f'https://example.invalid/{name}'}
        if digest is not None:
            a['digest'] = digest
        return a

    def test_select(self):
        sha = 'AB' * 32
        releases = [
            self.release('release-3.5.0', [self.asset('SDL3-3.5.0.dmg', 'sha256:' + 'cd' * 32)], pre=True),
            self.release('release-3.4.20', [self.asset('SDL3-3.4.20.dmg', 'sha256:' + 'ef' * 32)], draft=True),
            self.release('release-3.4.18', [self.asset('SDL3-devel-3.4.18-VC.zip', 'sha256:' + '00' * 32),
                                            self.asset('SDL3-3.4.18.dmg', 'sha256:' + sha),
                                            self.asset('SDL3-3.4.18.dmg.sig')]),
            self.release('release-3.4.9', [self.asset('SDL3-3.4.9.dmg')]),
            self.release('release-2.32.10', [self.asset('SDL2-2.32.10.dmg', 'sha256:' + '11' * 32)]),
        ]
        best = im.select_sdl3_asset(releases)
        self.assertEqual((best['version'], best['name'], best['sha256']), ('3.4.18', 'SDL3-3.4.18.dmg', sha.lower()))
        self.assertEqual(best['url'], 'https://example.invalid/SDL3-3.4.18.dmg')
        # (3.4.9 < 3.4.18: by number, not by text; and an asset without a digest has no SHA-256)
        only = im.select_sdl3_asset([releases[3]])
        self.assertEqual((only['version'], only['sha256']), ('3.4.9', None))
        self.assertIsNone(im.select_sdl3_asset([releases[0], releases[4]]))
        self.assertIsNone(im.select_sdl3_asset([]))
        self.assertIsNone(im.select_sdl3_asset(None))
        self.assertIsNone(im.select_sdl3_asset([{'message': 'API rate limit exceeded'}, 'junk']))

    def test_fallback(self):
        f = im.SDL_FALLBACK
        self.assertEqual(f['version'], '3.4.18')
        self.assertEqual(f['sha256'], 'c1adf3da91108dbc3e4205da5dacebc203085d283fa89e86eb2c889abd03a853')
        self.assertTrue(f['url'].endswith('/release-3.4.18/' + f['name']))
        self.assertEqual(im.select_sdl3_asset([self.release('release-3.4.18', [
            self.asset(f['name'], 'sha256:' + f['sha256'])])])['url'], f'https://example.invalid/{f["name"]}')

    @unittest.skipUnless(DARWIN and (ROOT / 'deps' / 'downloads' / 'SDL3-3.4.18.dmg').is_file(),
                         "needs macOS and SDL's 3.4.18 DMG in deps/downloads")
    def test_framework_from_the_dmg(self):
        dmg = ROOT / 'deps' / 'downloads' / 'SDL3-3.4.18.dmg'
        self.assertEqual(im.sha256_of(str(dmg)), im.SDL_FALLBACK['sha256'])
        with tempfile.TemporaryDirectory(prefix='sdl3 test ') as tmp:
            dest = os.path.join(tmp, 'deps', 'SDL3')
            os.makedirs(dest)
            Path(dest, 'old.txt').write_text('replaced')
            im.install_sdl3_framework(str(dmg), dest)
            fw = os.path.join(dest, 'SDL3.framework')
            self.assertTrue(im.sdl3_ready(fw))
            self.assertEqual(im.sdl3_version(fw), '3.4.18')
            self.assertTrue(os.path.islink(os.path.join(fw, 'Versions', 'Current')))   # (ditto keeps the links)
            self.assertTrue(os.path.islink(os.path.join(fw, 'SDL3')))
            self.assertTrue(os.path.isfile(os.path.join(dest, 'LICENSE.txt')))
            self.assertFalse(os.path.exists(os.path.join(dest, 'old.txt')))
            self.assertFalse(os.path.exists(dest + '.partial'))
            arches = subprocess.run(['lipo', '-archs', os.path.join(fw, 'SDL3')], capture_output=True, text=True).stdout
            self.assertIn('arm64', arches)
            r = subprocess.run(['codesign', '--verify', fw], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
        mounted = subprocess.run(['hdiutil', 'info'], capture_output=True, text=True).stdout
        self.assertNotIn('windowsmash-sdl3-', mounted)


FAKE_OSASCRIPT = r'''#!/bin/bash
# stands in for osascript: records its arguments and script (a record: RS, each argument and US, GS,
# the script), answers from the queue (one a line, \n for a line break)
{ printf '\036'; for a in "$@"; do printf '%s\037' "$a"; done; printf '\035'; cat; } >> "$FAKE_LOG"
answer=$(head -n 1 "$FAKE_ANSWERS")
tail -n +2 "$FAKE_ANSWERS" > "$FAKE_ANSWERS.next" && mv "$FAKE_ANSWERS.next" "$FAKE_ANSWERS"
case "$answer" in
    FAIL*) echo "execution error: No user interaction allowed. (-1713)" >&2; exit 1 ;;
esac
printf '%b\n' "$answer"
'''


class FakeOsascript:
    """osascript on the PATH as a stand-in, with the answers it gives in turn; .calls: [(argv, script)]."""

    def __init__(self, test, answers):
        self.dir = tempfile.mkdtemp(prefix='osascript-test-')
        test.addCleanup(shutil.rmtree, self.dir, True)
        write(os.path.join(self.dir, 'bin', 'osascript'), FAKE_OSASCRIPT, 0o755)
        self.log = os.path.join(self.dir, 'log.txt')
        self.answers = os.path.join(self.dir, 'answers.txt')
        write(self.answers, ''.join(a.replace('\n', '\\n') + '\n' for a in answers))
        self.env = {'PATH': os.path.join(self.dir, 'bin') + os.pathsep + os.environ.get('PATH', ''),
                    'FAKE_LOG': self.log, 'FAKE_ANSWERS': self.answers}

    @contextlib.contextmanager
    def active(self):
        old = {k: os.environ.get(k) for k in self.env}
        os.environ.update(self.env)
        try:
            yield self
        finally:
            for k, v in old.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    @property
    def calls(self):
        if not os.path.exists(self.log):
            return []
        out = []
        for record in Path(self.log).read_text().split('\x1e')[1:]:
            args, _, script = record.partition('\x1d')
            args = args.split('\x1f')[:-1]
            assert args[:1] == ['-'], args   # (the script comes on standard input; every string after it)
            out.append((args[1:], script))
        return out


class StubChecks:
    """The installer's folder checks, without reading game files: a folder is usable when it exists."""

    @staticmethod
    def check(folder, ok_text, what):
        ok = bool(folder) and os.path.isdir(folder)
        return ok, ok_text if ok else f'choose the {what} folder'

    def keys(self, folder):
        return self.check(folder, 'keys here', 'keys')

    def firmware(self, folder):
        return self.check(folder, 'firmware here', 'firmware')

    def game(self, folder, keys):
        return self.check(folder, 'the game: 13.0.5', 'game')

    def all(self, f):
        return {'game': self.game(f['game'], f['keys']), 'keys': self.keys(f['keys']), 'firmware': self.firmware(f['firmware'])}


class DialogTests(unittest.TestCase):
    def test_arguments_never_in_the_script(self):
        quoted = 'x" & (do shell script "touch /tmp/windowsmash-quoted") & "'
        cmd, script = im.osascript_command(im.DIALOG, [quoted, 'Title', 7])
        self.assertEqual(cmd, ['osascript', '-', quoted, 'Title', '7'])
        self.assertEqual(script, im.DIALOG)
        self.assertNotIn('windowsmash-quoted', script)
        for name in ('DIALOG', 'ASK', 'CHOOSE', 'FOLDER', 'ALERT', 'ADMIN'):
            text = getattr(im, name)
            self.assertTrue(text.startswith('on run argv\n'), name)
            self.assertTrue(text.isascii(), name)   # (what is shown goes in the arguments, which carry any text)

    @unittest.skipUnless(DARWIN and shutil.which('osascript'), 'needs macOS')
    def test_arguments_stay_text(self):
        # (the real osascript, with a script that shows nothing: what it is given comes back as it was)
        with tempfile.TemporaryDirectory() as tmp:
            marker = os.path.join(tmp, 'ran')
            text = f'x" & (do shell script "touch {marker}") & "\nline 2 – ✓'
            echo = 'on run argv\n    return "=" & item 1 of argv & "|" & (count of argv)\nend run\n'
            self.assertEqual(im.osascript(echo, text, 'two'), text + '|2')
            self.assertFalse(os.path.exists(marker))

    @unittest.skipUnless(DARWIN and shutil.which('osacompile'), 'needs macOS')
    def test_scripts_compile(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ('DIALOG', 'ASK', 'CHOOSE', 'FOLDER', 'ALERT', 'ADMIN'):
                src = write(os.path.join(tmp, name + '.applescript'), getattr(im, name))
                r = subprocess.run(['osacompile', '-o', os.path.join(tmp, name + '.scpt'), str(src)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{name}: {r.stderr}')

    @unittest.skipUnless(BASH, 'needs bash')
    def test_answers(self):
        fake = FakeOsascript(self, ['=Set up and build', '', '=Keys\nFirmware', '=', 'FAIL', '!The rule was refused'])
        with fake.active():
            self.assertEqual(im.dialog('Some text', ['Cancel', 'OK'], 'OK', 'Cancel'), 'Set up and build')
            self.assertIsNone(im.ask_text('Name?', 'Player1'))
            self.assertEqual(im.choose('Which?', ['Game files', 'Keys', 'Firmware'], ['Keys', 'Nope'], several=True), ['Keys', 'Firmware'])
            self.assertEqual(im.choose('Which?', ['a']), [])
            with self.assertRaises(im.NoDialogs):
                im.alert('t', 'm')
            with self.assertRaises(im.NoDialogs):
                im.osascript(im.ADMIN, 'tool', 'app', 'prompt')
        calls = fake.calls
        self.assertEqual(calls[0][0], ['Some text', im.TITLE, 'OK', 'Cancel', 'Cancel', 'OK'])
        self.assertEqual(calls[1][0], ['Name?', im.TITLE, 'Player1'])
        self.assertEqual(calls[2][0], ['Which?', im.TITLE, '1', '1', 'Keys', 'Game files', 'Keys', 'Firmware'])
        self.assertEqual(calls[3][0], ['Which?', im.TITLE, '0', '0', 'a'])
        self.assertTrue(calls[0][1].startswith('on run argv'))

    @unittest.skipUnless(BASH, 'needs bash')
    def test_wizard(self):
        with tempfile.TemporaryDirectory(prefix='wizard test ') as tmp:
            game, keys, firmware, other = (os.path.join(tmp, n) for n in ('My Game', 'keys', 'firmware', 'other keys'))
            for d in (game, keys, firmware, other):
                os.makedirs(d)
            fake = FakeOsascript(self, [
                '=Set up and build', '=',                                # (no firmware yet: the alert says so)
                '=Change…', '=Firmware', '=' + firmware + '/',
                '=Change…', '=Keys', '=' + other + '/',
                '=Change…', '=Online name', '=' + 'N' * 40,
                '=Change…', '=Options', '=',                             # (nothing ticked)
                '=Change…', '',                                          # (the list cancelled)
                '=Set up and build'])
            with fake.active():
                folders, name, options = im.wizard({'game': game, 'keys': keys, 'firmware': None}, 'Player1234',
                                                   {'launcher': True, 'firewall': True}, 'on', StubChecks())
            self.assertEqual(folders, {'game': game, 'keys': other, 'firmware': firmware})
            self.assertEqual(name, 'N' * im.NAME_MAX)
            self.assertEqual(options, {'launcher': False, 'firewall': False})
            calls = fake.calls
            self.assertEqual(len(calls), 17)
            summary = calls[0][0][0]
            self.assertIn('Game files: ' + im.shown(game) + '\n    ✓ the game: 13.0.5', summary)
            self.assertIn('Firmware: (none yet)\n    ✗ choose the firmware folder', summary)
            self.assertIn('Online name (what other players see): Player1234', summary)
            self.assertIn('Options: ' + im.OPTION_LAUNCHER + '; ' + im.OPTION_FIREWALL, summary)
            self.assertEqual(calls[0][0][1:], [im.TITLE, 'Set up and build', 'Cancel', 'Cancel', 'Change…', 'Set up and build'])
            self.assertEqual(calls[1][0], ['Not ready yet', 'Firmware: choose the firmware folder\n\nChoose Change… to pick the folder.'])
            self.assertEqual(calls[3][0], ['What do you want to change?', im.TITLE, '0', '1', 'Firmware',   # (the one to fix first)
                                           'Game files', 'Keys', 'Firmware', 'Online name', 'Options'])
            self.assertEqual(calls[4][0][0], im.PROMPTS['firmware'])
            self.assertTrue(os.path.isdir(calls[4][0][1]))   # (it opens where it can: Application Support, or home)
            self.assertEqual(calls[7][0], [im.PROMPTS['keys'], keys])   # (at the folder chosen so far)
            self.assertEqual(calls[10][0][:3], ['The name other players see online (up to 32 characters; kept in online_name.txt):',
                                                im.TITLE, 'Player1234'])
            self.assertEqual(calls[13][0], ['Tick what you want:', im.TITLE, '1', '2', im.OPTION_LAUNCHER, im.OPTION_FIREWALL,
                                            im.OPTION_LAUNCHER, im.OPTION_FIREWALL])
            self.assertIn('Online name (what other players see): ' + 'N' * 32, calls[16][0][0])
            self.assertIn('Options: none', calls[16][0][0])
            for argv, script in calls:   # (no folder, name or text of any kind in a script)
                self.assertNotIn(tmp, script)
                self.assertNotIn('NNNN', script)

    @unittest.skipUnless(BASH, 'needs bash')
    def test_wizard_cancel(self):
        fake = FakeOsascript(self, [''])
        with fake.active(), self.assertRaises(im.Stop):
            im.wizard({'game': None, 'keys': None, 'firmware': None}, 'P', {'launcher': True, 'firewall': False}, 'off', StubChecks())
        self.assertEqual(len(fake.calls), 1)
        self.assertNotIn(im.OPTION_FIREWALL, fake.calls[0][0][0])   # (the firewall is off: no such option)


class TerminalTests(unittest.TestCase):
    """The questions in the Terminal window (--no-wizard, or no screen for dialogs) and --yes."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='terminal test ')
        self.addCleanup(self.temp.cleanup)
        self.top = self.temp.name
        self.dirs = {n: os.path.join(self.top, n) for n in ('My Game', 'My Keys', 'My Firmware')}
        for d in self.dirs.values():
            os.makedirs(d)

    def test_questions(self):
        typed = [self.dirs['My Keys'].replace(' ', '\\ ') + ' ',   # (dragged into Terminal: escaped, a space after)
                 '',                                                 # (the firmware found: kept)
                 '/no/such/folder',                                  # (asked again)
                 f'"{self.dirs["My Game"]}"',
                 'M' * 40,
                 'n',                                                # (no Play SSBU in Applications)
                 '']                                                 # (the firewall rule: as offered)
        found = {'game': None, 'keys': None, 'firmware': self.dirs['My Firmware']}
        with mock.patch('builtins.input', side_effect=typed), contextlib.redirect_stdout(io.StringIO()) as out:
            folders, name, options = im.terminal_answers(arguments(), found, 'Player1', {'launcher': True, 'firewall': True},
                                                         'on', StubChecks(), True)
        self.assertEqual(folders, {'game': self.dirs['My Game'], 'keys': self.dirs['My Keys'], 'firmware': self.dirs['My Firmware']})
        self.assertEqual(name, 'M' * im.NAME_MAX)
        self.assertEqual(options, {'launcher': False, 'firewall': True})
        self.assertIn('  game: /no/such/folder - choose the game folder\n', out.getvalue())

    def test_yes(self):
        a = arguments(yes=True, game=self.dirs['My Game'], keys=self.dirs['My Keys'], firmware=self.dirs['My Firmware'])
        with mock.patch.object(im, 'ROOT', self.top), mock.patch.object(im, 'Checks', StubChecks), \
                mock.patch.object(im, 'detected_folders', lambda: {'game': None, 'keys': None, 'firmware': None}), \
                mock.patch('builtins.input', side_effect=AssertionError('asked')), contextlib.redirect_stdout(io.StringIO()):
            files = im.step_files(a, 'on')
        self.assertEqual(files, {'game': self.dirs['My Game'], 'keys': self.dirs['My Keys'], 'firmware': self.dirs['My Firmware'],
                                 'name': files['name'], 'launcher': True, 'firewall': False})   # (no password asked for)
        self.assertRegex(files['name'], r'^Player\d{4}$')
        with open(os.path.join(self.top, 'online_name.txt'), 'rb') as f:
            self.assertEqual(f.read(), files['name'].encode() + b'\n')
        a.game = os.path.join(self.top, 'missing')
        with mock.patch.object(im, 'ROOT', self.top), mock.patch.object(im, 'Checks', StubChecks), \
                mock.patch.object(im, 'detected_folders', lambda: {'game': None, 'keys': None, 'firmware': None}), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(im.Stop) as stop:
            im.step_files(a, 'off')
        self.assertIn('Name it with --game', str(stop.exception))


@unittest.skipUnless(DARWIN and shutil.which('codesign'), 'needs macOS')
class FirewallTests(unittest.TestCase):
    """Step 9, with the firewall's tool and the password dialog stood in for."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='firewall test ')
        self.addCleanup(self.temp.cleanup)
        top = Path(self.temp.name).resolve()
        self.app = str(make_tree(top, '.', data=False) / 'build-mac' / 'WindowSmash.app')
        self.assertEqual(subprocess.run(['codesign', '--force', '--sign', '-', self.app], capture_output=True).returncode, 0)
        self.tool = write(top / 'socketfilterfw', f'#!/bin/bash\n[ "$1" = --listapps ] && cat "{top}/listed.txt" 2>/dev/null\n'
                                                  'exit 0\n', 0o755)
        self.listed = top / 'listed.txt'
        self.patches = [mock.patch.object(im, 'APP', self.app), mock.patch.object(im, 'ROOT', str(top)),
                        mock.patch.object(im, 'FIREWALL', str(self.tool))]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def step(self, state, wanted=True, dry=False, answers=()):
        fake = FakeOsascript(self, list(answers))
        out = io.StringIO()
        with fake.active(), contextlib.redirect_stdout(out):
            im.step_firewall(arguments(dry_run=dry), {'firewall': wanted}, state)
        return out.getvalue(), fake.calls

    def test_states(self):
        self.assertIn('off (macOS', self.step('off')[0])
        self.assertIn('hosting a private room needs that turned off', self.step('all')[0])
        self.assertIn('no rule wanted', self.step('on', wanted=False)[0])
        said, calls = self.step('on', dry=True)
        self.assertIn('(dry run) would let build-mac/WindowSmash.app through the firewall', said)
        self.assertEqual(calls, [])

    def test_rule(self):
        said, calls = self.step('on', answers=['='])
        self.assertIn('the game is let through', said)
        self.assertEqual(calls[0][0][:2], [str(self.tool), self.app])   # (the tool and the app as arguments)
        self.assertNotIn(self.app, calls[0][1])
        stamp = Path(im.ROOT, 'build-mac', 'firewall.txt').read_text().strip()
        self.assertEqual(stamp, im.cdhash(self.app))
        self.listed.write_text(f'1 : {self.app}\n             (Allow incoming connections)\n')
        said, calls = self.step('on')
        self.assertIn('let through already', said)
        self.assertEqual(calls, [])
        subprocess.run(['codesign', '--force', '--sign', '-', '--identifier', 'other', self.app], capture_output=True)
        said, calls = self.step('on', answers=[''])      # (rebuilt: asked again; no password given)
        self.assertIn('no password given', said)
        said, calls = self.step('on', answers=['FAIL'])  # (no screen for the dialog: the commands to type)
        self.assertIn(f'sudo {self.tool} --add', said)


class LauncherAppTests(unittest.TestCase):
    def test_info_plist(self):
        info = im.launcher_info()
        self.assertEqual(info['CFBundleExecutable'], 'play')
        self.assertEqual(info['CFBundleIdentifier'], 'io.github.sno2207.windowsmash.play')
        self.assertEqual(info['CFBundleIconFile'], 'windowsmash')
        self.assertEqual(info['CFBundlePackageType'], 'APPL')
        self.assertEqual(info['LSMinimumSystemVersion'], '14.0')
        self.assertIs(info['LSUIElement'], True)   # (no Dock icon of its own: the game has one)
        self.assertIs(info['NSHighResolutionCapable'], True)
        self.assertEqual(info['CFBundleName'], 'Play SSBU')
        self.assertEqual(im.launcher_info('io.example.test')['CFBundleIdentifier'], 'io.example.test')
        self.assertEqual(plistlib.loads(plistlib.dumps(info)), info)

    @unittest.skipUnless(BASH, 'needs bash')
    def test_script(self):
        root = "/Users/me/Win Smash/it's $HOME"
        text = im.launcher_script(root)
        self.assertTrue(text.startswith('#!/bin/bash\n'))
        self.assertIn('exec /bin/bash "$root/tools/mac/launch.sh" --gui play\n', text)
        with tempfile.TemporaryDirectory() as tmp:
            script = write(os.path.join(tmp, 'play'), text + 'echo unreachable\n', 0o755)
            r = subprocess.run([BASH, '-n', str(script)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            # the root, as written, is the root (quoted for the shell), and a missing one says so
            fake = FakeOsascript(self, ['='])
            r = subprocess.run([BASH, '-c', text.replace('exec /bin/bash', 'echo would run') + '\necho "ROOT=$root"\n'],
                               capture_output=True, text=True, env={**os.environ, **fake.env})
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            argv = fake.calls[0][0]
            self.assertEqual(argv[0], 'WindowSmash is not where it was')
            self.assertIn(f'starts the game from {root}, which is not there any more.', argv[1])
            os.makedirs(os.path.join(tmp, 'tools', 'mac'))
            write(os.path.join(tmp, 'tools', 'mac', 'launch.sh'), 'echo "LAUNCH $*"\n')
            r = subprocess.run([BASH, str(write(os.path.join(tmp, 'play2'), im.launcher_script(tmp), 0o755))],
                               capture_output=True, text=True)
            self.assertEqual(r.stdout, 'LAUNCH --gui play\n')

    def test_ico_png(self):
        png = im.ico_png((ROOT / 'docs' / 'assets' / 'windowsmash.ico').read_bytes())
        self.assertTrue(png.startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertEqual(int.from_bytes(png[16:20], 'big'), 256)   # (the IHDR's width)
        self.assertIsNone(im.ico_png(b''))
        self.assertIsNone(im.ico_png(b'\0\0\1\0\0\0'))

    @unittest.skipUnless(DARWIN and shutil.which('iconutil'), 'needs macOS')
    def test_make_app(self):
        with tempfile.TemporaryDirectory(prefix='launcher app ') as tmp:
            app = os.path.join(tmp, 'Applications', 'Play SSBU.app')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertTrue(im.make_launcher_app(app, '/Users/me/Win Smash', 'io.github.sno2207.windowsmash.test-launcher'))
            exe = os.path.join(app, 'Contents', 'MacOS', 'play')
            self.assertTrue(os.access(exe, os.X_OK))
            self.assertIn("root='/Users/me/Win Smash'", Path(exe).read_text())
            plist = os.path.join(app, 'Contents', 'Info.plist')
            self.assertEqual(subprocess.run(['plutil', '-lint', plist], capture_output=True).returncode, 0)
            with open(plist, 'rb') as f:
                self.assertEqual(plistlib.load(f)['CFBundleIdentifier'], 'io.github.sno2207.windowsmash.test-launcher')
            icns = Path(app, 'Contents', 'Resources', 'windowsmash.icns').read_bytes()
            self.assertEqual(icns[:4], b'icns')   # (the game's, or made from docs/assets/windowsmash.ico)
            self.assertEqual(Path(app, 'Contents', 'PkgInfo').read_text(), 'APPL????')
            r = subprocess.run(['codesign', '--verify', app], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertFalse(os.path.exists(app + '.partial'))
            with contextlib.redirect_stdout(out):
                self.assertFalse(im.make_launcher_app(app, '/Users/me/Win Smash', 'io.github.sno2207.windowsmash.test-launcher'))
                self.assertTrue(im.make_launcher_app(app, '/Users/me/Elsewhere', 'io.github.sno2207.windowsmash.test-launcher'))
            self.assertIn("root=/Users/me/Elsewhere", Path(exe).read_text())


@unittest.skipUnless(BASH, 'needs bash')
class BootstrapTests(unittest.TestCase):
    """Install WindowSmash.command: which Python it runs the installer with, and the Python it
    downloads when there is none (stood in for: curl copies a local file)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bootstrap test ')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name).resolve()   # (as bash's $PWD has it)
        self.bin = self.top / 'bin'

    def fake_python(self, path, usable=True, code=0):
        """A Python stand-in: answers the check (usable or not), or says how it was run."""
        return write(path, textwrap.dedent(f'''\
            #!/bin/bash
            if [ "$1" = -c ]; then exit {0 if usable else 1}; fi
            echo "RAN $0 $*"
            exit {code}
            '''), 0o755)

    def finder(self, cwd, path):
        funcs = ''.join(shell_function(INSTALL, n) for n in ('usable_python', 'python_candidates', 'find_python'))
        r = subprocess.run([BASH, '-c', funcs + 'find_python'], cwd=cwd, capture_output=True, text=True,
                           env={**os.environ, 'PATH': path})
        return r.stdout.strip()

    def test_python_order(self):
        root = self.top / 'root'
        root.mkdir()
        old = self.fake_python(self.bin / 'old' / 'python3', usable=False)        # (3.9, or an Intel one)
        new = self.fake_python(self.bin / 'new' / 'python3')
        minor = self.fake_python(self.bin / 'minor' / 'python3.12')
        self.assertEqual(self.finder(root, f'{old.parent}:{new.parent}:{minor.parent}:/usr/bin:/bin'), str(new))
        if not any(os.path.exists(p) for p in ('/opt/homebrew/bin/python3', '/usr/local/bin/python3',
                                               '/Library/Frameworks/Python.framework/Versions/Current/bin/python3')):
            self.assertEqual(self.finder(root, f'{old.parent}:{minor.parent}:/usr/bin:/bin'), str(minor))
        if os.path.exists('/usr/bin/python3'):
            self.assertNotEqual(self.finder(root, '/usr/bin:/bin'), '/usr/bin/python3')
        own = self.fake_python(root / 'deps' / 'python' / 'bin' / 'python3')
        self.assertEqual(self.finder(root, f'{new.parent}:/usr/bin:/bin'), str(own))   # (WindowSmash's own first)
        funcs = shell_function(INSTALL, 'usable_python')
        for p in ('/usr/bin/python3', '/usr/bin/python', ''):
            r = subprocess.run([BASH, '-c', funcs + 'usable_python "$1"', 'x', p], capture_output=True)
            self.assertEqual(r.returncode, 1, p)   # (Apple's 3.9, never run: it may only offer to install the tools)
        real = subprocess.run([BASH, '-c', funcs + 'usable_python "$1"', 'x', sys.executable], capture_output=True)
        self.assertEqual(real.returncode, 0 if sys.version_info >= (3, 10) and os.uname().machine == 'arm64' else 1)

    def test_runs_the_installer(self):
        root = self.top / 'Win Smash'
        root.mkdir()
        shutil.copy2(INSTALL, root / INSTALL.name)
        write(root / 'tools' / 'mac' / 'install_mac.py', '# (stand-in)\n')
        py = self.fake_python(self.bin / 'python3', code=7)
        r = subprocess.run([BASH, str(root / INSTALL.name), '--dry-run', '--game', '/My Games'], cwd='/',
                           capture_output=True, text=True, stdin=subprocess.DEVNULL,
                           env={**os.environ, 'PATH': f'{py.parent}:/usr/bin:/bin:/usr/sbin:/sbin'})
        if os.uname().machine != 'arm64':
            self.assertEqual(r.returncode, 1)
            self.assertIn('Apple silicon', r.stdout)
            return
        self.assertEqual(r.returncode, 7, r.stdout + r.stderr)   # (the installer's exit code)
        self.assertIn(f'RAN {py} -u tools/mac/install_mac.py --dry-run --game /My Games', r.stdout)
        log = (root / 'install.log').read_text()
        self.assertIn('WindowSmash setup: ', log)
        self.assertIn('RAN ', log)
        self.assertNotIn('Press Return', r.stdout)   # (no one to press it)

    def python_download(self, sha_matches):
        root = self.top / 'root'
        root.mkdir(exist_ok=True)
        # a stand-in python-build-standalone archive: python/bin/python3
        pkg = self.top / 'pkg'
        self.fake_python(pkg / 'python' / 'bin' / 'python3')
        tgz = self.top / 'cpython.tar.gz'
        with tarfile.open(tgz, 'w:gz') as t:
            t.add(pkg / 'python', arcname='python')
        sha = subprocess.run(['shasum', '-a', '256', str(tgz)], capture_output=True, text=True).stdout.split()[0]
        write(self.bin / 'curl', f'#!/bin/bash\n# (stands in: -o <file> <url>)\nwhile [ "$1" != -o ]; do shift; done\n'
                                 f'cp "{tgz}" "$2"\necho "$3" > "{self.top}/curl-url.txt"\n', 0o755)
        funcs = ''.join(shell_function(INSTALL, n) for n in ('usable_python', 'get_python'))
        consts = (f'PY_VERSION=3.14.8\nPY_RELEASE=20261013\nPY_FILE=cpython-test.tar.gz\nPY_URL=https://example.invalid/python\n'
                  f'PY_SHA256={sha if sha_matches else "0" * 64}\n')
        return root, subprocess.run([BASH, '-c', consts + funcs + 'get_python'], cwd=root, capture_output=True, text=True,
                                    env={**os.environ, 'PATH': f'{self.bin}:/usr/bin:/bin'})

    @unittest.skipUnless(shutil.which('shasum'), 'needs shasum')
    def test_python_download(self):
        root, r = self.python_download(True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.access(root / 'deps' / 'python' / 'bin' / 'python3', os.X_OK))
        self.assertFalse((root / 'deps' / 'python.partial').exists())
        self.assertFalse((root / 'deps' / 'downloads' / 'cpython-test.tar.gz').exists())   # (not kept)
        self.assertEqual((self.top / 'curl-url.txt').read_text().strip(), 'https://example.invalid/python')

    @unittest.skipUnless(shutil.which('shasum'), 'needs shasum')
    def test_python_download_checked(self):
        root, r = self.python_download(False)
        self.assertEqual(r.returncode, 1)
        self.assertIn('does not match its SHA-256', r.stdout)
        self.assertFalse((root / 'deps' / 'python').exists())
        self.assertEqual(list((root / 'deps' / 'downloads').iterdir()), [])   # (the bad download is gone)

    def test_pinned_python(self):
        text = INSTALL.read_text()
        self.assertIn('PY_VERSION=3.14.8\n', text)
        self.assertIn('PY_RELEASE=20261013\n', text)
        self.assertIn('PY_SHA256=709f419d175eb015f810b68aa54bdae1d776c9824cb362aa701c93b0a9f2a9e7\n', text)
        self.assertIn('https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/cpython-$PY_VERSION%2B'
                      '$PY_RELEASE-aarch64-apple-darwin-install_only.tar.gz', text)


def make_tree(top, tree='.', version=None, data=True, built=True):
    """A game version folder as a build leaves it, with a stand-in game app (when built) that records
    how it was started (to <top>/started.txt: its working folder, its arguments, the SSBU_ variables),
    writes a line to its log, and runs a second, so that open can wait for it."""
    t = Path(top) / tree
    (t / 'dump' / 'modules').mkdir(parents=True, exist_ok=True)
    if data:
        nca = Path(top) / 'game data' / 'program.nca'
        nca.parent.mkdir(parents=True, exist_ok=True)
        nca.write_bytes(b'data')
        (t / 'dump' / 'modules' / 'romfs.cfg').write_text(f'{nca}\n1234\n')
    if version:
        (t / 'version.txt').write_text(version + '\n')
    if built:
        app = t / 'build-mac' / 'WindowSmash.app' / 'Contents'
        with open(write(app / 'Info.plist', '').as_posix(), 'wb') as f:
            plistlib.dump({'CFBundleExecutable': 'ssbu', 'CFBundleIdentifier': 'io.github.sno2207.windowsmash.test-stub',
                           'CFBundlePackageType': 'APPL', 'LSBackgroundOnly': True}, f)
        write(app / 'MacOS' / 'ssbu', textwrap.dedent(f'''\
            #!/bin/bash
            out="{Path(top) / 'started.txt'}"
            {{ echo "cwd=$PWD"; printf 'arg=%s\\n' "$@"; env | grep '^SSBU_' | sort; }} > "$out.partial"
            mv "$out.partial" "$out"
            echo "stand-in game: running" >> "$SSBU_LOG"
            sleep 1
            '''), 0o755)
    return t


def copy_launchers(top):
    for name in ('Play SSBU.command', 'launchers/Play SSBU Offline.command', 'launchers/Host SSBU Netplay.command',
                 'launchers/Join SSBU Netplay.command', 'launchers/Unlock All Fighters.command',
                 'launchers/Build WindowSmash.command', 'tools/mac/launch.sh', 'tools/mac/pick_version.sh'):
        (Path(top) / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, Path(top) / name)


@unittest.skipUnless(BASH, 'needs bash')
class LaunchScriptTests(unittest.TestCase):
    """tools/mac/launch.sh's parts (sourced: its functions only), on a folder laid out as a build leaves it."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='launch test ')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name).resolve() / 'Win Smash'   # (as the launcher sees it: pwd -P)
        self.top.mkdir()
        copy_launchers(self.top)

    def sh(self, body, stdin='', env=None, gui=False):
        script = f'. "{self.top}/tools/mac/launch.sh"; cd "{self.top}"; gui={1 if gui else 0}\n' + body
        clean = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        return subprocess.run([BASH, '-c', script], capture_output=True, text=True, input=stdin,
                              env={**clean, **(env or {})}, timeout=60)

    def online(self, server, name=None, direct=None, stdin='', gui=False, env=None):
        """go_online on these files: the lines it says. It sets no SSBU_NET_* variable: the game reads
        the files itself (--online)."""
        for f, text in (('online_server.txt', server), ('online_name.txt', name), ('online_direct.txt', direct)):
            p = self.top / f
            if text is None:
                p.unlink(missing_ok=True)
            else:
                p.write_bytes(text.encode())
        r = self.sh('go_online; echo "set: $(set | grep -c "^SSBU_NET_")"', stdin, env=env, gui=gui)
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[-1], 'set: 0')
        return lines[:-1]

    def test_online_server(self):
        head = '# The online lobby\n# host:port, then an optional lobby code\n\n'
        change = 'To change them: Esc in the game. To play offline: launchers/Play SSBU Offline.command'
        self.assertEqual(self.online(head + 'smash.example:39421\n', 'Mario\n'),
                         ['Online: lobby smash.example:39421, as Mario.', change])
        self.assertEqual(self.online(head + '  lobby.example:39420   FRIENDS  extra\n', 'Mario\n')[0],
                         'Online: lobby lobby.example:39420 (lobby code FRIENDS), as Mario.')
        self.assertEqual(self.online((head + 'lobby.example:1 CODE\n').replace('\n', '\r\n'), '\r\nLuigi\r\n')[0],
                         'Online: lobby lobby.example:1 (lobby code CODE), as Luigi.')
        self.assertEqual(self.online(head + 'lobby.example:1', 'Peach')[0], 'Online: lobby lobby.example:1, as Peach.')   # (no last newline)
        offline = ['Playing offline (no lobby in online_server.txt).']
        self.assertEqual(self.online(head + 'OFF CODE\nlobby.example:1\n', 'Mario\n'), offline)
        self.assertEqual(self.online(head, 'Mario\n'), offline)
        self.assertEqual(self.online(None, 'Mario\n'), offline)
        self.assertEqual(self.online(head + 'a.example:1\n', 'Mario\n', 'off\n')[0], 'Online: lobby a.example:1, as Mario.')

    def test_name_left_to_the_game(self):
        """No name in online_name.txt: nothing is asked (the game picks one and keeps it there)."""
        picks = 'Online: lobby a.example:1, as the name the game picks.'
        self.assertEqual(self.online('a.example:1\n', None, None, 'Samus Aran\n')[0], picks)
        self.assertFalse((self.top / 'online_name.txt').exists())
        self.assertEqual(self.online('a.example:1\n', '\n\n')[0], picks)
        fake = FakeOsascript(self, ['=Zelda'])
        self.assertEqual(self.online('a.example:1\n', None, gui=True, env=fake.env), [])   # (nothing said: no window to say it in)
        self.assertEqual(fake.calls, [])
        self.assertFalse((self.top / 'online_name.txt').exists())

    def test_version_trees(self):
        r = self.sh('list_trees')
        self.assertEqual(r.stdout, '')
        make_tree(self.top, '.', '11.0.0')
        make_tree(self.top, 'v13.0.5', '13.0.5')
        make_tree(self.top, 'v12 no data', '12.0.3', data=False)     # (no romfs.cfg: not ready)
        make_tree(self.top, 'v9', '9.0.2', built=False)              # (not built)
        make_tree(self.top, 'v8 no label')
        (self.top / 'v8 no label' / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu').chmod(0o644)  # (not runnable)
        make_tree(self.top, 'v7', '7.0.1')
        r = self.sh('list_trees')
        self.assertEqual(r.stdout.splitlines(), ['.\t11.0.0  - build-mac/, save/', 'v13.0.5\t13.0.5  - v13.0.5/', 'v7\t7.0.1  - v7/'])
        (self.top / 'v7' / 'version.txt').unlink()
        self.assertEqual(self.sh('list_trees').stdout.splitlines()[2], 'v7\tv7  - v7/')

    def pick(self, stdin='', env=None, gui=False):
        r = self.sh('pick_version; echo "status=$? tree=$SSBU_TREE"', stdin, env, gui)
        return r.stdout.splitlines()

    def test_pick_version(self):
        out = self.pick()
        self.assertIn('No game version is built here yet: double-click Install WindowSmash.command', out[0])
        self.assertEqual(out[-1], 'status=1 tree=')
        make_tree(self.top, '.', '11.0.0')
        self.assertEqual(self.pick(), ['Game version 11.0.0  - build-mac/, save/', 'status=0 tree=.'])   # (one: no question)
        self.assertEqual((self.top / 'last_version.txt').read_text(), '.\n')
        make_tree(self.top, 'v13.0.5', '13.0.5')
        out = self.pick('\n')
        self.assertEqual(out[:3], ['Game versions built here:', '  1. 11.0.0  - build-mac/, save/', '  2. 13.0.5  - v13.0.5/'])
        self.assertEqual(out[-1], 'status=0 tree=.')                 # (last time's)
        self.assertEqual(self.pick('x\n9\n2\n')[-4:], ['Type one of the numbers above.', 'Type one of the numbers above.',
                                                        'Game version 13.0.5  - v13.0.5/', 'status=0 tree=v13.0.5'])
        self.assertEqual((self.top / 'last_version.txt').read_text(), 'v13.0.5\n')
        self.assertEqual(self.pick('\n')[-1], 'status=0 tree=v13.0.5')
        (self.top / 'last_version.txt').write_text('V13.0.5\r\n')     # (as Windows wrote it)
        self.assertEqual(self.pick('')[-1], 'status=0 tree=v13.0.5')
        (self.top / 'last_version.txt').unlink()
        self.assertEqual(self.pick('')[-1], 'status=0 tree=v13.0.5')  # (none: the last listed, as Windows)
        self.assertEqual(self.pick('', {'SSBU_TREE': '.'}), ['status=0 tree=.'])   # (named beforehand: no question)
        self.assertEqual(self.pick('1\n', {'SSBU_TREE': 'v9'})[-1], 'status=0 tree=.')   # (not built: asked)

    def test_pick_version_dialog(self):
        make_tree(self.top, '.', '11.0.0')
        make_tree(self.top, 'v13.0.5', '13.0.5')
        (self.top / 'last_version.txt').write_text('.\n')
        fake = FakeOsascript(self, ['=13.0.5  - v13.0.5/', ''])
        self.assertEqual(self.pick(env=fake.env, gui=True), ['status=0 tree=v13.0.5'])
        self.assertEqual(fake.calls[0][0], ['Which game version?', '11.0.0  - build-mac/, save/', '11.0.0  - build-mac/, save/',
                                            '13.0.5  - v13.0.5/'])
        self.assertEqual(self.pick(env=fake.env, gui=True), ['status=1 tree='])   # (cancelled)

    def test_game_checks(self):
        make_tree(self.top, '.', '11.0.0')
        r = self.sh('pick_game >/dev/null; echo "tree=$tree"')
        self.assertEqual(r.stdout.strip(), f'tree={self.top}')
        make_tree(self.top, 'v13.0.5', '13.0.5')
        r = self.sh('pick_game >/dev/null; echo "tree=$tree"', env={'SSBU_TREE': 'v13.0.5'})
        self.assertEqual(r.stdout.strip(), f'tree={self.top}/v13.0.5')
        (self.top / 'game data' / 'program.nca').unlink()
        r = self.sh('pick_game; echo "not here"', env={'SSBU_TREE': 'v13.0.5'})
        self.assertEqual(r.returncode, 1)
        self.assertIn('The game data is not where it was when the game was built: ', r.stdout)
        self.assertNotIn('not here', r.stdout)
        (self.top / 'v13.0.5' / 'dump' / 'modules' / 'romfs.cfg').write_text(
            f'layered\n{self.top}/v13.0.5/dump/modules/romfs.layers\n{self.top}/base.nca\n')
        for name in ('v13.0.5/dump/modules/romfs.layers', 'base.nca'):
            (self.top / name).write_bytes(b'x')
        r = self.sh('pick_game >/dev/null; echo "ok"', env={'SSBU_TREE': 'v13.0.5'})
        self.assertEqual(r.stdout.strip(), 'ok')
        (self.top / 'base.nca').unlink()
        r = self.sh('pick_game; echo "not here"', env={'SSBU_TREE': 'v13.0.5'})
        self.assertIn(f'built: {self.top}/base.nca.', r.stdout)

    def test_not_built(self):
        for name in ('Play SSBU.command', 'launchers/Play SSBU Offline.command', 'launchers/Host SSBU Netplay.command',
                     'launchers/Join SSBU Netplay.command', 'launchers/Unlock All Fighters.command'):
            r = subprocess.run([BASH, str(self.top / name)], capture_output=True, text=True, stdin=subprocess.DEVNULL,
                               cwd='/', timeout=60)
            self.assertEqual(r.returncode, 1, name)
            self.assertIn('No game version is built here yet: double-click Install WindowSmash.command', r.stdout, name)
        r = subprocess.run([BASH, str(self.top / 'launchers/Build WindowSmash.command')], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, cwd='/', timeout=60)
        self.assertEqual(r.returncode, 1)
        self.assertIn("WindowSmash's Python (deps/venv) is not set up yet: double-click Install WindowSmash.command", r.stdout)

    def test_unlock(self):
        make_tree(self.top, '.', '11.0.0')
        write(self.top / 'deps' / 'venv' / 'bin' / 'python', '#!/bin/bash\n[ "$1" = -c ] && exit 0\necho "PY $PWD | $*"\n', 0o755)
        fake = Path(self.temp.name) / 'bin'
        write(fake / 'pgrep', '#!/bin/bash\nexit 1\n', 0o755)   # (no game running)
        env = {**os.environ, 'PATH': f'{fake}:{os.environ["PATH"]}'}
        r = subprocess.run([BASH, str(self.top / 'launchers/Unlock All Fighters.command'), '--dry-run'], capture_output=True,
                           text=True, stdin=subprocess.DEVNULL, cwd='/', env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f'PY {self.top} | tools/unlock_all.py --tree . --dry-run', r.stdout)
        write(fake / 'pgrep', '#!/bin/bash\n[ "$1 $2" = "-x ssbu" ] && exit 0\nexit 1\n', 0o755)   # (the game is running)
        r = subprocess.run([BASH, str(self.top / 'launchers/Unlock All Fighters.command')], capture_output=True,
                           text=True, stdin=subprocess.DEVNULL, cwd='/', env=env, timeout=60)
        self.assertEqual(r.returncode, 1)
        self.assertIn('Close every SSBU game window before unlocking fighters, then run this again.', r.stdout)
        self.assertNotIn('PY ', r.stdout)

    def test_old_venv(self):
        """A deps/venv the tools cannot use (one made by hand from Apple's Python 3.9, or an Intel
        one): the launcher says to run the installer, which makes it again."""
        write(self.top / 'deps' / 'venv' / 'bin' / 'python', '#!/bin/bash\n[ "$1" = -c ] && exit 1\necho "PY $*"\n', 0o755)
        r = subprocess.run([BASH, str(self.top / 'launchers/Build WindowSmash.command')], capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, cwd='/', timeout=60)
        self.assertEqual(r.returncode, 1)
        self.assertIn("WindowSmash's Python (deps/venv) is one the tools cannot use (they need Python 3.10 or newer, as "
                      "arm64): double-click Install WindowSmash.command, which makes it again.", r.stdout)
        self.assertNotIn('PY ', r.stdout)

    def test_build(self):
        write(self.top / 'deps' / 'venv' / 'bin' / 'python', '#!/bin/bash\n[ "$1" = -c ] && exit 0\necho "PY $PWD | $*"\nexit 4\n', 0o755)
        fake = Path(self.temp.name) / 'bin'
        write(fake / 'caffeinate', '#!/bin/bash\necho "AWAKE $1"\nshift\nexec "$@"\n', 0o755)
        r = subprocess.run([BASH, str(self.top / 'launchers/Build WindowSmash.command'), '--jobs', '4'], capture_output=True,
                           text=True, stdin=subprocess.DEVNULL, cwd='/', timeout=60,
                           env={**os.environ, 'PATH': f'{fake}:{os.environ["PATH"]}'})
        self.assertEqual(r.returncode, 4)
        self.assertEqual(r.stdout.splitlines()[:2], ['AWAKE -i', f'PY {self.top} | tools/build_game.py --jobs 4'])


@unittest.skipUnless(DARWIN and BASH and shutil.which('open'), 'needs macOS')
class OpenTests(unittest.TestCase):
    """The launchers start a stand-in game app with the real open: its arguments, working folder and
    environment are what the game expects (the runtime reads SSBU_CWD and SSBU_LOG, design: Launch)."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='open test ')
        self.addCleanup(self.temp.cleanup)
        self.top = Path(self.temp.name).resolve() / 'Win Smash'
        self.top.mkdir()
        copy_launchers(self.top)
        self.started = self.top / 'started.txt'

    def record(self):
        lines = self.started.read_text().splitlines()
        env = dict(line.split('=', 1) for line in lines if line.startswith('SSBU_'))
        return lines[0], [line[4:] for line in lines if line.startswith('arg=')], env

    def run_launcher(self, name, stdin='', **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith('SSBU_')}
        return subprocess.run([BASH, str(self.top / name)], capture_output=True, text=True, input=stdin, cwd='/',
                              env={**clean, **env}, timeout=120)

    def test_play(self):
        tree = make_tree(self.top, '.', '13.0.5')
        (self.top / 'online_server.txt').write_text('# the lobby\nlobby.example:39421 FRIENDS\n')
        (self.top / 'online_name.txt').write_text('Tester\n')
        (self.top / 'online_direct.txt').write_text('off\n')
        r = self.run_launcher('Play SSBU.command', SSBU_HITCH='40', SSBU_NET_HOST='1', SSBU_DATA_ROOT='/elsewhere',
                              SSBU_NET_BIND='127.0.0.1', SSBU_NET_ROOM='mine', SSBU_NET_DIRECT='1', SSBU_NET_PORT='1',
                              SSBU_LOG='/tmp/not-this', SSBU_CWD='/tmp')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        cwd, args, env = self.record()
        self.assertEqual(args, [f'{tree}/dump/modules', '--status', '30', '--online'])
        self.assertEqual(cwd, 'cwd=/')   # (open starts apps in /: the runtime goes to SSBU_CWD)
        # (what Play SSBU.bat clears is gone, SSBU_LOG and SSBU_CWD are the launcher's, the rest passes on;
        # the lobby, name and direct connections are the game's to read from the files: --online)
        self.assertEqual(env, {'SSBU_CWD': str(self.top), 'SSBU_LOG': f'{tree}/build/play.log', 'SSBU_HITCH': '40',
                               'SSBU_TREE': '.'})
        self.assertIn('Online: lobby lobby.example:39421 (lobby code FRIENDS), as Tester.', r.stdout)
        self.assertIn('stand-in game: running', r.stdout)   # (the log, shown as it goes)
        self.assertEqual((tree / 'build' / 'play.log').read_text(), 'stand-in game: running\n')
        self.assertFalse((tree / 'build' / 'open.txt').exists())

    def test_offline_version_tree(self):
        make_tree(self.top, '.', '11.0.0')
        tree = make_tree(self.top, 'v13.0.5', '13.0.5')
        (self.top / 'online_server.txt').write_text('lobby.example:39421\n')
        r = self.run_launcher('launchers/Play SSBU Offline.command', '2\n', SSBU_NET_RELAY='lobby.example:1', SSBU_NET_NAME='x')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _cwd, args, env = self.record()
        self.assertEqual(args, [f'{tree}/dump/modules', '--status', '30'])
        self.assertEqual(env, {'SSBU_CWD': str(self.top), 'SSBU_LOG': f'{tree}/build/play.log', 'SSBU_TREE': 'v13.0.5'})
        self.assertEqual((self.top / 'last_version.txt').read_text(), 'v13.0.5\n')

    def test_host(self):
        tree = make_tree(self.top, 'v13.0.5', '13.0.5')
        # (the stand-in writes the address where the runtime does: <version folder>/netplay_address.txt)
        exe = tree / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'
        exe.write_text(exe.read_text().replace('sleep 1', 'echo "203.0.113.7:$SSBU_NET_PORT" > "$(dirname "$(dirname "$1")")/'
                                                          'netplay_address.txt"\nsleep 1'))
        (self.top / 'netplay_address.txt').write_text('stale, in the wrong place\n')
        r = self.run_launcher('launchers/Host SSBU Netplay.command', 'Host Name\n\n40000\n')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _cwd, _args, env = self.record()
        self.assertEqual((env['SSBU_NET_HOST'], env['SSBU_NET_NAME'], env['SSBU_NET_ROOM'], env['SSBU_NET_PORT']),
                         ('1', 'Host Name', 'smash', '40000'))
        self.assertNotIn('SSBU_NET_RELAY', env)
        self.assertIn('203.0.113.7:40000', r.stdout)
        self.assertIn('Starting the game as host of room "smash"...', r.stdout)

    def test_join(self):
        make_tree(self.top, '.', '13.0.5')
        r = self.run_launcher('launchers/Join SSBU Netplay.command', '203.0.113.7:39420\n\nfriends\n')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _cwd, _args, env = self.record()
        self.assertEqual((env['SSBU_NET_RELAY'], env['SSBU_NET_NAME'], env['SSBU_NET_ROOM']),
                         ('203.0.113.7:39420', os.environ.get('USER') or subprocess.run(['id', '-un'], capture_output=True,
                                                                                        text=True).stdout.strip(), 'friends'))
        self.assertNotIn('SSBU_NET_HOST', env)
        r = self.run_launcher('launchers/Join SSBU Netplay.command', '\n')
        self.assertEqual(r.returncode, 1)
        self.assertIn('An address is needed - ask the host for theirs.', r.stdout)

    def test_stopped_with_an_error(self):
        tree = make_tree(self.top, '.', '13.0.5')
        exe = tree / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'
        exe.write_text(exe.read_text().replace('sleep 1', 'echo "*** stopped during boot: cannot open x" >> "$SSBU_LOG"\nsleep 1'))
        (self.top / 'online_server.txt').write_text('off\n')
        r = self.run_launcher('Play SSBU.command')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('The game stopped with an error: stopped during boot: cannot open x. Its report is in', r.stdout)

    def test_did_not_start(self):
        """A game that exits at once, writing no log: open's status is 0 even so (or it cannot block on it)."""
        tree = make_tree(self.top, '.', '13.0.5')
        exe = tree / 'build-mac' / 'WindowSmash.app' / 'Contents' / 'MacOS' / 'ssbu'
        exe.write_text(exe.read_text().replace('echo "stand-in game: running" >> "$SSBU_LOG"\nsleep 1\n', 'exit 0\n'))
        (self.top / 'online_server.txt').write_text('off\n')
        r = self.run_launcher('Play SSBU.command')
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn('The game did not start.', r.stdout)

    def test_the_app(self):
        """The Play SSBU app the installer makes, opened as Finder would: no dialog (one version, offline)."""
        tree = make_tree(self.top, '.', '13.0.5')
        (self.top / 'online_server.txt').write_text('off\n')
        app = Path(self.temp.name) / 'Applications' / 'Play SSBU.app'
        with contextlib.redirect_stdout(io.StringIO()):
            im.make_launcher_app(str(app), str(self.top), 'io.github.sno2207.windowsmash.test-launcher')
        r = subprocess.run(['open', '-n', '-W', str(app)], capture_output=True, text=True, timeout=120,
                           env={k: v for k, v in os.environ.items() if not k.startswith('SSBU_')})
        self.assertEqual(r.returncode, 0, r.stderr)
        for _ in range(100):   # (open -W can return as the launcher's own open is still waiting)
            if self.started.exists():
                break
            time.sleep(0.1)
        cwd, args, env = self.record()
        self.assertEqual(args, [f'{tree}/dump/modules', '--status', '30'])
        self.assertEqual(env['SSBU_CWD'], str(self.top))
        self.assertEqual(env['SSBU_LOG'], f'{tree}/build/play.log')
        self.assertNotIn('SSBU_NET_RELAY', env)


@unittest.skipUnless(BASH, 'needs bash')
class ScriptSyntaxTests(unittest.TestCase):
    def test_bash_syntax(self):
        scripts = SCRIPTS + sorted((ROOT / 'launchers').glob('*.command'))
        self.assertEqual(len({s.name for s in scripts}), 9)
        for s in scripts:
            with self.subTest(s.name):
                r = subprocess.run([BASH, '-n', str(s)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{s.name}: {r.stderr}')
                data = s.read_bytes()
                self.assertNotIn(b'\r', data, s.name)
                self.assertTrue(data.endswith(b'\n'), s.name)
                if s.name != 'pick_version.sh':   # (sourced only)
                    self.assertTrue(data.startswith(b'#!/bin/bash\n'), s.name)
                    self.assertTrue(os.stat(s).st_mode & stat.S_IXUSR, f'{s.name} is not executable')

    def test_launchers_listed(self):
        # (the installer makes each runnable: every .command, and the scripts they run)
        commands = {p.relative_to(ROOT).as_posix() for p in [*ROOT.glob('*.command'), *(ROOT / 'launchers').glob('*.command')]}
        self.assertEqual(commands | {'tools/mac/launch.sh', 'tools/mac/pick_version.sh'}, set(im.LAUNCHERS))
        self.assertTrue(all((ROOT / n).is_file() for n in im.LAUNCHERS))

    def test_install_body_is_one_block(self):
        # (the installer may replace this file while it runs: bash has read the whole block by then)
        lines = [line for line in INSTALL.read_text().splitlines() if line and not line.startswith('#')]
        self.assertEqual((lines[0], lines[-1]), ('{', '}'))
        self.assertIn('    exit "$status"', lines[-2])


if __name__ == '__main__':
    unittest.main()
