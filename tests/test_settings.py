"""The settings files' writer (runtime/hle_settings.c), which the in-game settings menu changes
display.ini, controllers.ini and online_*.txt with, through tools/dev/settingstest.c; and the
controller layer's own offline checks (tools/dev/padtest.c), which take the menu's reload too;
and where the game goes online from those files (tools/dev/netcfgtest.c).
Built with the game's compiler; skipped where there is no clang-cl."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


@unittest.skipUnless(HAVE_CLANG, 'needs Windows and clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class SettingsFileTests(unittest.TestCase):
    def build_and_run(self, name):
        d = tempfile.mkdtemp()
        try:
            exe = os.path.join(d, name + '.exe')
            r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(ROOT / 'tools' / 'dev' / (name + '.c')),
                                '/Fe' + exe, '/Fo' + os.path.join(d, name + '.obj'), '/link', *cc.LINK_OPTIONS],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, name + '.c did not build:\n' + r.stdout + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60, cwd=d)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return r.stdout
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_settings_writer(self):
        self.assertIn('0 failed', self.build_and_run('settingstest'))

    def test_pads(self):
        self.build_and_run('padtest')

    def test_online_lobby_from_files(self):
        """Started with --online, the lobby in online_server.txt is the one the Online mode logs in
        through, as SSBU_NET_RELAY's was when Play SSBU.bat set it."""
        self.assertIn('0 failed', self.build_and_run('netcfgtest'))


if __name__ == '__main__':
    unittest.main()
