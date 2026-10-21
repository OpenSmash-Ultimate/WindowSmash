"""The programs in tools/ that the installer, the build and the launchers run find the modules next
to them under any Python: the embeddable one the installer sets up on a PC without Python keeps a
script's own folder off sys.path (its ._pth file turns on safe-path mode, as python -P does)."""
from pathlib import Path
import ast
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'tools'
CALLERS = [TOOLS / 'build_game.py', TOOLS / 'install_windowsmash.ps1',
           TOOLS / 'mac' / 'install_mac.py', *ROOT.glob('*.bat'), *(ROOT / 'launchers').glob('*.bat'), *TOOLS.glob('*.bat'),
           *ROOT.glob('*.command'), *(ROOT / 'launchers').glob('*.command'), *ROOT.glob('*.sh'),
           *(ROOT / 'launchers').glob('*.sh'), *(TOOLS / 'mac').glob('*.sh')]
NAMED = re.compile(r'''tools[\\/]+(\w+)\.py|["']tools["'],\s*["'](\w+)\.py|TOOLS,\s*["'](\w+)\.py''')
# loads the script as a module (its imports, not its main); with "folder", also says whether its
# folder is then on sys.path (for the imports it makes later, inside functions)
LOAD = ('import os, runpy, sys; path = sys.argv[1]; runpy.run_path(path, run_name="loaded"); '
        'folder = os.path.normcase(os.path.dirname(os.path.abspath(path))); '
        'sys.exit(0 if sys.argv[2:] != ["folder"] or folder in [os.path.normcase(os.path.abspath(p)) for p in sys.path if p] else 3)')


def programs():
    """The tools/*.py programs (with a main) that those files run."""
    names = set()
    for caller in CALLERS:
        for m in NAMED.finditer(caller.read_text(encoding='utf-8', errors='replace')):
            names.add(next(g for g in m.groups() if g))
    out = []
    for name in sorted(names):
        path = TOOLS / f'{name}.py'
        if path.exists() and '__name__ == "__main__"' in path.read_text(encoding='utf-8'):
            out.append(path)
    return out


def imports_neighbours(path):
    """Whether the script imports a module of its own folder anywhere (also inside a function)."""
    here = {p.stem for p in path.parent.glob('*.py')}
    for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
        if isinstance(node, ast.Import):
            names = [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            names = [node.module]
        else:
            continue
        if any(n.split('.')[0] in here for n in names):
            return True
    return False


@unittest.skipIf(sys.version_info < (3, 11), 'python -P needs Python 3.11')
class SafePathTests(unittest.TestCase):
    def test_programs_found(self):
        names = {p.stem for p in programs()}
        self.assertTrue({'build_game', 'recomp', 'update', 'nca', 'codec_libs'} <= names, names)

    def test_each_finds_its_modules(self):
        with tempfile.TemporaryDirectory(prefix='safe-path-test-') as elsewhere:   # (not the port's folder)
            for path in programs():
                with self.subTest(path.name):
                    need = ['folder'] if imports_neighbours(path) else []
                    r = subprocess.run([sys.executable, '-P', '-c', LOAD, str(path), *need], cwd=elsewhere,
                                       capture_output=True, text=True, timeout=120,
                                       env={**os.environ, 'PYTHONPATH': ''})
                    self.assertEqual(r.returncode, 0, f'{path.name} under python -P:\n{r.stderr[-1500:]}')


if __name__ == '__main__':
    unittest.main()
