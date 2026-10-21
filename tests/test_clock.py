"""The port's clock (runtime/clock.h) through tools/dev/clocktest.c: built with the game's compiler and
run as is, with a system timer slowed to a microsecond a read (as on a PC timed by the HPET), and
with the CPU's counter asked for. Skipped where there is no clang-cl (it builds nothing else)."""
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
class ClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.exe = {}
        for name, extra in (('plain', []), ('slow', ['/DSLOW_QPC'])):
            exe = os.path.join(cls.dir, 'clocktest_%s.exe' % name)
            r = subprocess.run([cc.CLANG, '/nologo', '/O2', *cc.LINK_FLAGS, *cc.BASE_CFLAGS, *extra, str(ROOT / 'tools' / 'dev' / 'clocktest.c'),
                                '/Fe' + exe, '/Fo' + os.path.join(cls.dir, name + '.obj'), '/link', *cc.LINK_OPTIONS],
                               capture_output=True, text=True)
            if r.returncode:
                raise AssertionError('clocktest.c did not build:\n' + r.stdout + r.stderr)
            cls.exe[name] = exe

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def run_test(self, name, secs, clock=None):
        env = dict(os.environ)
        env.pop('SSBU_CLOCK', None)
        if clock:
            env['SSBU_CLOCK'] = clock
        r = subprocess.run([self.exe[name], str(secs)], capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_as_is(self):
        """On this PC as it is (the CPU's counter only if its own timer is slow)."""
        self.run_test('plain', 1.5)

    def test_slow_system_timer(self):
        """A microsecond a read: the CPU's counter takes over, rescaled after some 2 s, never back."""
        out = self.run_test('slow', 3)
        self.assertIn('clock: CPU counter', out)

    def test_cpu_counter_asked_for(self):
        self.assertIn('clock: CPU counter', self.run_test('plain', 1.5, 'tsc'))

    def test_system_timer_asked_for(self):
        """SSBU_CLOCK=qpc keeps the system timer, slow as it is."""
        self.assertIn('clock: system timer', self.run_test('slow', 1, 'qpc'))


if __name__ == '__main__':
    unittest.main()
