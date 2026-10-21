"""The launcher window's file writer (tools/launcher.ps1: Set-IniValue, Set-LineValue) and its game
version list, through PowerShell with the script dot-sourced under -NoWindow (no window opens).
The writer must give the same bytes as the game's own (runtime/hle_settings.c, which the
in-game settings menu uses), so the same edits are also made by a small C driver built on it
with the game's compiler, and the two files compared. Skipped off Windows; the comparison is
skipped where there is no clang-cl."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / 'tools' / 'launcher.ps1'
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

POWERSHELL = shutil.which('powershell') if os.name == 'nt' else None
HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)

# One edit: ('ini', section or None, key, value or None) or ('line', text).
DISPLAY = (b'; display.ini\n'
           b'\n'
           b'; window = windowed | borderless | fullscreen\n'
           b'window = windowed\n'
           b'pacing = clock   ; the clock\n'
           b'latency = 3\n')
PADS = (b'[general]\n'
        b'keyboard_player = 1\n'
        b'background = 1\n'
        b'\n'
        b'[players]\n'
        b'; 2 = DualSense Wireless Controller\n'
        b'\n'
        b'[device default]\n'
        b'layout = positional\n'
        b'deadzone = 0\n'
        b'; R = righttrigger\n'
        b'\n'
        b'[device  ps5]\n'
        b'rumble = 60\n')
SERVER = (b'# The online lobby\n'
          b'# host:port\n'
          b'\n'
          b'smash.example:39421 friends\n')

# (name, the file before (None: missing), edits): tools/dev/settingstest.c's scenarios, then more.
SCENARIOS = [
    ('flat', DISPLAY, [('ini', None, 'pacing', 'vrr'), ('ini', None, 'window', 'borderless'), ('ini', None, 'overlay', '0')]),
    ('flat_remove', DISPLAY, [('ini', None, 'latency', None), ('ini', None, 'gpu', None), ('ini', None, 'pacing', 'clock')]),
    ('sections', PADS, [('ini', 'device default', 'deadzone', '10'),
                        ('ini', 'players', '2', '050000004c050000e60c000000810000'),
                        ('ini', 'device ps5', 'rumble', '40'),
                        ('ini', 'device default', 'ZR', 'rightshoulder'),
                        ('ini', 'device Xbox Series X Controller', 'deadzone', '15'),
                        ('ini', 'general', 'keyboard_player', '0')]),
    ('unpin', PADS, [('ini', 'players', '2', 'x'), ('ini', 'players', '2', None)]),
    ('crlf_bom', b'\xef\xbb\xbfwindow = windowed\r\npacing = clock\r\n',
     [('ini', None, 'pacing', 'display'), ('ini', None, 'hide_cursor', '0')]),
    ('new_file', None, [('ini', 'general', 'background', '0')]),
    ('new_flat_file', None, [('ini', None, 'show_launcher', '0')]),
    ('line_replace', SERVER, [('line', 'off')]),
    ('line_added', b'# only a comment\n', [('line', 'Player1234')]),
    ('line_new_file', None, [('line', 'on')]),
    ('line_same', SERVER, [('line', 'smash.example:39421 friends')]),
    ('line_indented', b'  \t\n  ; not a comment here\n', [('line', 'x')]),
    # more of the rules
    ('comment_tab', b'gpu = auto\t# the card\nwidth=1280 ;w\n', [('ini', None, 'gpu', 'nvidia'), ('ini', None, 'WIDTH', '1920')]),
    ('trailing_spaces', b'window = windowed   \nheight =\n', [('ini', None, 'window', 'fullscreen'), ('ini', None, 'height', '1080')]),
    ('no_space_comment', b'name = a#b;c\nk = ;only\n', [('ini', None, 'name', 'z'), ('ini', None, 'k', 'v')]),
    ('same_value', b'pacing  =   vrr  ; c\n', [('ini', None, 'pacing', 'vrr')]),
    ('header_spacing', b'[ General ]\n  KEYBOARD_PLAYER\t= 2\n[x]\nk = 1\n', [('ini', 'general', 'keyboard_player', '3'), ('ini', 'general', 'background', '0')]),
    ('twice', b'[general]\na = 1\n\n[other]\nb = 2\n\n[general]\nc = 3\n\n\n', [('ini', 'general', 'a', '9'), ('ini', 'general', 'd', '4')]),
    ('new_section_after_blank', b'[a]\nk = 1\n\n', [('ini', 'b', 'k', '2')]),
    ('new_section_no_newline', b'[a]\nk = 1', [('ini', 'b', 'k', '2')]),
    ('flat_trailing_blank', b'a = 1\n\n\n', [('ini', None, 'b', '2')]),
    ('flat_in_section_file', b'[general]\nkeyboard_player = 1\n', [('ini', None, 'keyboard_player', '4')]),
    ('empty_file', b'', [('ini', None, 'a', '1'), ('ini', 'general', 'b', '2')]),
    ('one_line_no_eol', b'window = windowed', [('ini', None, 'window', 'borderless')]),
    ('lf_after_crlf', b'a = 1\r\nb = 2\nc = 3\r\n', [('ini', None, 'b', '5')]),
    ('bare_cr', b'a = 1\rb = 2\n', [('ini', None, 'a', '7')]),
    ('section_key_elsewhere', PADS, [('ini', 'general', 'deadzone', '5'), ('ini', 'players', 'layout', None)]),
]


def run_ps(script, cwd=None, timeout=120):
    r = subprocess.run([POWERSHELL, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                       capture_output=True, text=True, timeout=timeout, cwd=cwd)
    return r


def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


def ps_edits(path, edits):
    lines = []
    for e in edits:
        if e[0] == 'line':
            lines.append(f'[void](Set-LineValue {ps_quote(path)} {ps_quote(e[1])})')
        else:
            _, section, key, value = e
            lines.append(f'[void](Set-IniValue {ps_quote(path)} {ps_quote(section or "")} {ps_quote(key)} '
                         f'{"$null" if value is None else ps_quote(value)})')
    return '\n'.join(lines)


def dot_source(root):
    return f'$ErrorActionPreference = "Stop"\n. {ps_quote(str(LAUNCHER))} -NoWindow -Root {ps_quote(str(root))}\n'


@unittest.skipUnless(POWERSHELL, 'needs Windows PowerShell')
class LauncherIniTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def edit(self, before, edits, name='f.ini'):
        """The file's bytes after the edits through launcher.ps1 (before None: no file)."""
        path = self.dir / name
        if before is not None:
            path.write_bytes(before)
        r = run_ps(dot_source(self.dir) + ps_edits(str(path), edits))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertFalse(Path(str(path) + '.tmp').exists(), 'a .tmp left behind')
        return path.read_bytes() if path.exists() else None

    def test_comments_and_order_kept(self):
        got = self.edit(DISPLAY, [('ini', None, 'window', 'borderless')])
        self.assertEqual(got, DISPLAY.replace(b'window = windowed\n', b'window = borderless\n'))

    def test_in_place_with_inline_comment(self):
        got = self.edit(DISPLAY, [('ini', None, 'PACING', 'vrr')])
        self.assertEqual(got, DISPLAY.replace(b'pacing = clock   ; the clock', b'pacing = vrr   ; the clock'))

    def test_flat_append_and_remove(self):
        got = self.edit(DISPLAY, [('ini', None, 'overlay', '0'), ('ini', None, 'latency', None), ('ini', None, 'gpu', None)])
        self.assertEqual(got, b'; display.ini\n\n; window = windowed | borderless | fullscreen\nwindow = windowed\n'
                              b'pacing = clock   ; the clock\noverlay = 0\n')

    def test_section_append(self):
        got = self.edit(PADS, [('ini', 'device default', 'ZR', 'rightshoulder'), ('ini', 'players', '2', 'pad')])
        self.assertEqual(got, PADS.replace(b'; R = righttrigger\n', b'; R = righttrigger\nZR = rightshoulder\n')
                                  .replace(b'; 2 = DualSense Wireless Controller\n', b'; 2 = DualSense Wireless Controller\n2 = pad\n'))

    def test_remove_in_section(self):
        got = self.edit(PADS, [('ini', 'general', 'background', None), ('ini', 'general', 'rumble', None)])
        self.assertEqual(got, PADS.replace(b'background = 1\n', b''))

    def test_new_section(self):
        got = self.edit(PADS, [('ini', 'device xbox', 'deadzone', '15')])
        self.assertEqual(got, PADS + b'\n[device xbox]\ndeadzone = 15\n')

    def test_crlf_kept_bom_dropped(self):
        got = self.edit(b'\xef\xbb\xbfwindow = windowed\r\npacing = clock\r\n', [('ini', None, 'pacing', 'display'), ('ini', None, 'overlay', '1')])
        self.assertEqual(got, b'window = windowed\r\npacing = display\r\noverlay = 1\r\n')

    def test_new_file(self):
        self.assertEqual(self.edit(None, [('ini', 'general', 'background', '0')]), b'[general]\r\nbackground = 0\r\n')
        self.assertEqual(self.edit(None, [('ini', None, 'show_launcher', '0')], 'g.ini'), b'show_launcher = 0\r\n')
        self.assertIsNone(self.edit(None, [('ini', None, 'gone', None)], 'h.ini'), 'removing from no file made one')

    def test_unchanged_file_not_written(self):
        path = self.dir / 'same.ini'
        path.write_bytes(DISPLAY)
        os.utime(path, (1_000_000_000, 1_000_000_000))
        r = run_ps(dot_source(self.dir) + f'$a = Set-IniValue {ps_quote(str(path))} "" "pacing" "clock"\n'
                   f'$b = Set-LineValue {ps_quote(str(path))} "; display.ini"\n"$a $b"')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ['False', 'False'])
        self.assertEqual(int(path.stat().st_mtime), 1_000_000_000)

    def test_online_server_first_line(self):
        got = self.edit(SERVER, [('line', 'smash.recomps.wtf:39421 abc')], 'online_server.txt')
        self.assertEqual(got, SERVER.replace(b'smash.example:39421 friends', b'smash.recomps.wtf:39421 abc'))
        shipped = (ROOT / 'online_server.txt').read_bytes()
        got = self.edit(shipped, [('line', 'off')], 'shipped.txt')
        want = shipped.replace(b'smash.recomps.wtf:39421', b'off')
        self.assertEqual(got, want)
        self.assertEqual(sum(1 for l in got.splitlines() if l.startswith(b'#')), sum(1 for l in shipped.splitlines() if l.startswith(b'#')))

    def test_reading(self):
        path = self.dir / 'r.ini'
        path.write_bytes(b'\xef\xbb\xbfpacing = vrr  ; c\r\n[general]\r\nKeyboard_Player = 3 # x\r\n')
        line = self.dir / 'l.txt'
        line.write_bytes(b'# c\n\n  Zo\xc3\xab Smash  \n')
        r = run_ps(dot_source(self.dir) + f'$p = {ps_quote(str(path))}\n'
                   '"[" + (Get-IniValue $p "" "PACING") + "]"\n'
                   '"[" + (Get-IniValue $p "general" "keyboard_player") + "]"\n'
                   '"[" + ($null -eq (Get-IniValue $p "players" "keyboard_player")) + "]"\n'
                   f'"[" + ([int][char](Get-LineValue {ps_quote(str(line))})[2]) + "]"\n'
                   f'"[" + ($null -eq (Get-LineValue {ps_quote(str(self.dir / "none.txt"))})) + "]"')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ['[vrr]', '[3]', '[True]', '[235]', '[True]'])

    def test_utf8_value(self):
        r = run_ps(dot_source(self.dir) + f'[void](Set-LineValue {ps_quote(str(self.dir / "n.txt"))} ("Zo" + [char]0xEB))')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.dir / 'n.txt').read_bytes(), b'Zo\xc3\xab\r\n')

    def test_game_version(self):
        d = self.dir / 'port'
        d.mkdir()
        script = dot_source(d) + '"[" + (Get-GameVersion $Root) + "|" + (Test-GameBuilt $Root) + "]"\n'
        self.assertEqual(run_ps(script).stdout.strip(), '[|False]')
        (d / 'build').mkdir()
        (d / 'build' / 'ssbu.exe').write_bytes(b'')
        (d / 'version.txt').write_bytes(b'\r\n; a comment\r\n13.0.5\r\n# game version of this folder\r\n')
        r = run_ps(script + '"cmd|" + (Get-GameCommand)\n')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.splitlines(), [
            '[13.0.5|True]',
            'cmd|build\\ssbu.exe dump\\modules --status 30 --online > build\\play.log 2>&1',
        ])

    def test_auto_starts_directly(self):
        """Play SSBU.bat's path with show_launcher = 0: no window, the game's command (here only printed)."""
        (self.dir / 'build').mkdir()
        (self.dir / 'build' / 'ssbu.exe').write_bytes(b'')
        (self.dir / 'launcher.ini').write_bytes(b'; c\r\nshow_launcher = 0\r\n')
        r = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(LAUNCHER), '-Auto', '-DryRun',
                            '-Root', str(self.dir)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('start "SSBU log" /min cmd /c "build\\ssbu.exe dump\\modules --status 30 --online > build\\play.log 2>&1"', r.stdout)
        (self.dir / 'build' / 'ssbu.exe').unlink()
        r = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(LAUNCHER), '-Auto', '-DryRun',
                            '-Root', str(self.dir)], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)


DRIVER = r'''
#include "%s"
/* driver <file> then edits: ini <section|-> <key> <value|-> | line <text>; "-" is NULL */
int main(int argc, char **argv) {
    for (int i = 2; i < argc;) {
        if (!strcmp(argv[i], "ini") && i + 3 < argc) {
            const char *s = strcmp(argv[i + 1], "-") ? argv[i + 1] : NULL, *v = strcmp(argv[i + 3], "-") ? argv[i + 3] : NULL;
            if (!settings_ini_set(argv[1], s, argv[i + 2], v)) return 2;
            i += 4;
        } else if (!strcmp(argv[i], "line") && i + 1 < argc) {
            if (!settings_line_write(argv[1], argv[i + 1])) return 2;
            i += 2;
        } else return 3;
    }
    return 0;
}
'''


@unittest.skipUnless(POWERSHELL and HAVE_CLANG, 'needs Windows PowerShell and clang-cl (Visual Studio\'s C++ Clang tools)')
class LauncherParityTests(unittest.TestCase):
    """The same edits through launcher.ps1 and the game's writer give byte-identical files."""

    @classmethod
    def setUpClass(cls):
        cls.dir = Path(tempfile.mkdtemp())
        src = cls.dir / 'inidriver.c'
        src.write_text(DRIVER % (ROOT / 'runtime' / 'hle_settings.c').as_posix())
        cls.exe = cls.dir / 'inidriver.exe'
        r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(src), '/Fe' + str(cls.exe),
                            '/Fo' + str(cls.dir / 'inidriver.obj'), '/link', *cc.LINK_OPTIONS], capture_output=True, text=True)
        if r.returncode:
            shutil.rmtree(cls.dir, ignore_errors=True)
            raise AssertionError('the C driver did not build:\n' + r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def test_same_bytes(self):
        (self.dir / 'c').mkdir()
        (self.dir / 'ps').mkdir()
        script = dot_source(self.dir)
        for name, before, edits in SCENARIOS:
            for side in ('c', 'ps'):
                if before is not None:
                    (self.dir / side / name).write_bytes(before)
            args = [str(self.exe), str(self.dir / 'c' / name)]
            for e in edits:
                args += ['line', e[1]] if e[0] == 'line' else ['ini', e[1] or '-', e[2], '-' if e[3] is None else e[3]]
            r = subprocess.run(args, capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, f'{name}: C driver: {r.stdout}{r.stderr}')
            script += ps_edits(str(self.dir / 'ps' / name), edits) + '\n'
        r = run_ps(script)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for name, before, edits in SCENARIOS:
            with self.subTest(name):
                c, ps = self.dir / 'c' / name, self.dir / 'ps' / name
                self.assertEqual(c.exists(), ps.exists())
                if c.exists():
                    self.assertEqual(ps.read_bytes(), c.read_bytes())


if __name__ == '__main__':
    unittest.main()
