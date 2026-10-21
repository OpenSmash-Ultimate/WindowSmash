"""The game's link (tools/cc.py link_command) with a system root (deps\\msvc): the library folders
clang-cl hands lld-link, read from clang-cl -### (which prints the linker's command and runs
nothing). Players without Visual Studio had their builds stop at the link, every one of SYSTEM_LIBS
"not found": clang-cl had not been given the root, and lld-link, which puts its own /winsysroot:'s
folders on its search path only once an object file has told it the machine type, met the libraries
before the objects. A PC with Visual Studio hides that: there clang-cl hands lld-link VS's folders.
So this checks that every folder comes from the root (or is clang's own) and none from a Visual
Studio. Skipped where there is no clang-cl."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)
MSVC, SDK = '14.51.36231', '10.0.26100.0'
FOLDERS = (f'VC/Tools/MSVC/{MSVC}/include', f'VC/Tools/MSVC/{MSVC}/lib/x64',
           f'Windows Kits/10/Include/{SDK}/um', f'Windows Kits/10/Include/{SDK}/ucrt',
           f'Windows Kits/10/Lib/{SDK}/um/x64', f'Windows Kits/10/Lib/{SDK}/ucrt/x64')


def folder(path):
    return os.path.normcase(os.path.realpath(path))   # (clang names its own folder by its real path)


@unittest.skipUnless(HAVE_CLANG, 'needs Windows and clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class LinkCommandTests(unittest.TestCase):
    def linker_folders(self, root):
        """The -libpath: folders of the lld-link command clang-cl would run for the game's link."""
        objs = os.path.join(root, 'objs.rsp')
        Path(root, 'a.obj').write_bytes(b'')   # (clang-cl wants its inputs to exist, even for -###)
        Path(objs).write_text('"' + os.path.join(root, 'a.obj') + '"')
        flags, options = cc.link_flags(root)
        with mock.patch.object(cc, 'LINK_FLAGS', flags), mock.patch.object(cc, 'LINK_OPTIONS', options):
            command = cc.link_command(objs, os.path.join(root, 'game.exe'), ['/HIGHENTROPYVA:NO'], ['opus.lib'])
        r = subprocess.run([command[0], '-###', *command[1:]], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        linker = [line for line in r.stderr.splitlines() if 'link' in line.lower() and '-out:' in line][-1]
        args = shlex.split(linker, posix=True)
        self.assertIn('/winsysroot:' + root, args)   # (lld-link adds no Visual Studio's folders of its own)
        self.assertIn('winmm.lib', args)
        return [folder(a[len('-libpath:'):]) for a in args if a.lower().startswith('-libpath:')]

    def test_libraries_come_from_the_system_root(self):
        with tempfile.TemporaryDirectory(prefix='windowsmash-root-') as root:
            for d in FOLDERS:
                os.makedirs(os.path.join(root, *d.split('/')))
            found = self.linker_folders(root)
            clang_own = folder(os.path.dirname(os.path.dirname(shutil.which(cc.CLANG) or cc.CLANG)))
            for d in (f'VC/Tools/MSVC/{MSVC}/lib/x64', f'Windows Kits/10/Lib/{SDK}/um/x64', f'Windows Kits/10/Lib/{SDK}/ucrt/x64'):
                self.assertIn(folder(os.path.join(root, *d.split('/'))), found)
            stray = [f for f in found if not f.startswith(folder(root)) and not f.startswith(clang_own)]
            self.assertEqual(stray, [], 'library folders from outside the system root')

    def test_no_root_no_root_arguments(self):
        flags, options = cc.link_flags(None)
        self.assertEqual(options, [])
        self.assertNotIn('/winsysroot', flags)


if __name__ == '__main__':
    unittest.main()
