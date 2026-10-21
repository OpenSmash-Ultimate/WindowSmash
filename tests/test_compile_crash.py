"""A compiler crash in the build (tools/cc.py compile_each): the file is compiled again on its own once
the others are done, then at /O1, and the build stops with a plain explanation only if it keeps
crashing; an ordinary error in the code is not compiled again. Made-up compiler runs, except the last
tests, which crash the real clang-cl on purpose (skipped where there is none)."""
from pathlib import Path
import os
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)

# what a player's clang-cl printed (shortened), exit code 1 like any failed compile
CRASH = ('PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ and include the crash backtrace, '
         'preprocessed source, and associated run script.\n'
         'Stack dump:\n'
         '0.\tProgram arguments: "clang-cl.exe" /c /O2 /nologo build\\main\\g0053.c /Fobuild\\main\\g0053.obj\n'
         '1.\t<eof> parser at end of file\n'
         '2.\tOptimizer\n'
         '4.\tRunning pass "sroa<modify-cfg>" on function "f_7100855db0"\n'
         'Exception Code: 0xC0000005\n'
         ' #0 0x00007ff6925febf5 (clang-cl.exe+0x12eebf5)\n'
         ' #1 0x00007ff6925f80e2 (clang-cl.exe+0x12e80e2)\n'
         'clang-cl: error: clang frontend command failed due to signal (use -v to see invocation)\n')


class Compiler:
    """Stands in for subprocess.run: each file's results in turn ((exit code, output); the last one
    repeats), every command line kept."""
    def __init__(self, results):
        self.results = {name: list(r) for name, r in results.items()}
        self.calls = []

    def __call__(self, args, **kw):
        src = next(a for a in args if a.endswith('.c'))
        self.calls.append((src, args[2], args))
        queue = self.results.get(src, [(0, '')])
        code, out = queue.pop(0) if len(queue) > 1 else queue[0]
        return types.SimpleNamespace(returncode=code, stdout=out, stderr='')


def run(results, jobs=(('a.c', 'a.obj', '/O2'), ('b.c', 'b.obj', '/O2'))):
    compiler, log = Compiler(results), []
    with mock.patch.object(cc.subprocess, 'run', side_effect=compiler):
        failures = cc.compile_each(list(jobs), 2, log=log.append)
    return failures, compiler.calls, log


class CompileCrashTests(unittest.TestCase):
    def test_a_crash_is_compiled_again_on_its_own(self):
        failures, calls, log = run({'b.c': [(1, CRASH), (0, '')]})
        self.assertEqual(failures, [])
        self.assertEqual(sorted(c[:2] for c in calls[:2]), [('a.c', '/O2'), ('b.c', '/O2')])
        self.assertEqual(calls[2][:2], ('b.c', '/O2'))   # after the others, at its own level
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(cc.NO_CRASH_FILES in c[2] for c in calls))
        self.assertEqual(log, ['the compiler crashed on b.c (exception 0xC0000005): compiling it again on its own',
                               'b.c compiled'])

    def test_then_at_O1(self):
        failures, calls, log = run({'b.c': [(1, CRASH), (1, CRASH), (0, '')]})
        self.assertEqual(failures, [])
        self.assertEqual([c[1] for c in calls if c[0] == 'b.c'], ['/O2', '/O2', '/O1'])
        self.assertIn('compiling it again on its own, at /O1', log[1])
        self.assertIn('b.c compiled (at /O1', log[2])

    def test_a_file_that_keeps_crashing_stops_the_build_with_an_explanation(self):
        failures, calls, log = run({'b.c': [(1, CRASH)]})
        self.assertEqual([c[1] for c in calls if c[0] == 'b.c'], ['/O2', '/O2', '/O1'])
        self.assertEqual(len(failures), 1)
        src, text = failures[0]
        self.assertEqual(src, 'b.c')
        self.assertTrue(text.startswith('the compiler (clang-cl) crashed on this file 3 times'))
        self.assertIn('ran out of memory', text)
        self.assertIn('Running pass "sroa<modify-cfg>" on function "f_7100855db0"', text)
        self.assertIn('Exception Code: 0xC0000005', text)
        for noise in ('Program arguments', '#0 0x', 'PLEASE submit'):
            self.assertNotIn(noise, text)

    def test_files_built_at_O1_are_tried_at_O1_again(self):
        _failures, calls, _log = run({'b.c': [(1, CRASH)]}, jobs=[('b.c', 'b.obj', '/O1')])
        self.assertEqual([c[1] for c in calls], ['/O1', '/O1', '/O1'])

    def test_an_error_in_the_code_is_not_compiled_again(self):
        error = "b.c(3,1): error: expected ';' after expression\n1 error generated.\n"
        failures, calls, log = run({'b.c': [(1, error)]})
        self.assertEqual(failures, [('b.c', error)])
        self.assertEqual(len(calls), 2)
        self.assertEqual(log, [])

    def test_an_error_after_a_crash_stops_the_retries(self):
        error = "b.c(3,1): error: unknown type name 'Cpu'\n"
        failures, calls, _log = run({'b.c': [(1, CRASH), (1, error)]})
        self.assertEqual(failures, [('b.c', error)])
        self.assertEqual(len(calls), 3)

    def test_clang_cl_crashing_outright_and_running_out_of_memory(self):
        _f, _c, log = run({'b.c': [(0xC0000005, ''), (0, '')]})
        self.assertIn('(exception 0xC0000005)', log[0])
        _f, _c, log = run({'b.c': [(1, 'LLVM ERROR: out of memory\nAllocation failed\n'), (0, '')]})
        self.assertIn('(out of memory)', log[0])


@unittest.skipUnless(HAVE_CLANG, 'needs Windows and clang-cl (deps\\llvm, or Visual Studio\'s C++ Clang tools)')
class RealCompilerTests(unittest.TestCase):
    def test_a_real_crash_is_seen_as_one_and_leaves_no_files(self):
        with tempfile.TemporaryDirectory() as d:
            src, temp = Path(d, 'crash.c'), Path(d, 'temp')
            src.write_text('#pragma clang __debug crash\nint x;\n')
            temp.mkdir()
            log = []
            with mock.patch.dict(os.environ, {'TEMP': str(temp), 'TMP': str(temp)}):
                failures = cc.compile_each([(str(src), str(Path(d, 'crash.obj')), '/O2')], 1, log=log.append)
            self.assertEqual(len(log), 2, log)
            self.assertTrue(log[0].startswith('the compiler crashed on crash.c (exception 0x'), log[0])
            self.assertEqual(len(failures), 1)
            self.assertIn('crashed on this file 3 times', failures[0][1])
            self.assertEqual(list(temp.iterdir()), [])   # no preprocessed copy for a bug report

    def test_a_real_error_is_not(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d, 'error.c')
            src.write_text('int x = ;\n')
            log = []
            failures = cc.compile_each([(str(src), str(Path(d, 'error.obj')), '/O2')], 1, log=log.append)
            self.assertEqual(log, [])
            self.assertEqual(len(failures), 1)
            self.assertIn('error', failures[0][1])
            self.assertFalse(cc.crashed(1, failures[0][1]))


if __name__ == '__main__':
    unittest.main()
