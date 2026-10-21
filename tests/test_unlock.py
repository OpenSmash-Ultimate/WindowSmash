from pathlib import Path
import hashlib
import os
import runpy
import sys
import struct
import subprocess
import tempfile
import unittest

WORK = Path(__file__).resolve().parent
PS = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
SCRIPT = WORK.parent / 'tools/Unlock-All-Fighters.ps1'
ORIGINAL = runpy.run_path(str(WORK.parent / 'tools/unlock_all.py'), run_name='unlock_reference')
# The tools check the game version by its main module: 13.0.5's build ID.
BUILD_ID = '21450C647B8C5940ED5B68218EA128FD3C5D597D'
_VERSION, SAVE_SIZE, FLAGS, RECORDS = ORIGINAL['LAYOUTS'][BUILD_ID]


def write_main(tree, build_id):
    """A stand-in for <tree>/dump/exefs/main: just the NSO header fields the tools read."""
    path = tree / 'dump/exefs/main'
    path.parent.mkdir(parents=True, exist_ok=True)
    head = bytearray(0x100)
    head[:4] = b'NSO0'
    head[0x40:0x40 + len(build_id) // 2] = bytes.fromhex(build_id)
    path.write_bytes(head)


class UnlockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='unlock-test-', dir=WORK)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        write_main(self.root, BUILD_ID)
        self.save = self.root / 'save/user/save_data/system_data.bin'
        self.save.parent.mkdir(parents=True)
        data = bytearray(SAVE_SIZE)
        struct.pack_into('<I', data, 0, len(data))
        for n in ORIGINAL['STARTERS']:
            data[FLAGS + n * 8] = 1
        # A locked rematch candidate with an existing encounter order.
        data[FLAGS + 10 * 8 + 1] = 3
        data[FLAGS + 10 * 8 + 4] = 1
        self.before = bytes(data)
        self.save.write_bytes(data)

    def invoke(self, *args, mock_processes=True):
        # Only the process listing is mocked: the user's real running game stays untouched.
        command = 'function Get-Process { @() }; ' if mock_processes else ''
        command += (f"& '{str(SCRIPT).replace(chr(39), chr(39)*2)}' -Root '{self.root}' -SavePath '{self.save}' "
                    + ' '.join(args))
        return subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True, text=True)

    def test_dry_run_preserves_save(self):
        result = self.invoke('-DryRun')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Fighters to unlock: 66', result.stdout)
        self.assertEqual(self.save.read_bytes(), self.before)
        self.assertFalse((self.root / 'save_backups').exists())

    def test_unlock_matches_reference_and_preserves_other_bytes(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.save.read_bytes()
        flags, records = FLAGS, RECORDS
        changed = {i for i, (a, b) in enumerate(zip(self.before, data)) if a != b}
        allowed = set()
        for n in range(len(ORIGINAL['FIGHTERS'])):
            allowed.update(range(flags + 8 * n, flags + 8 * n + 5))
            allowed.update(range(records + 8 * n, records + 8 * n + 8))
            self.assertEqual(data[flags + 8 * n], 1)
        self.assertLessEqual(changed, allowed)
        self.assertEqual(data[flags + 10 * 8 + 1:flags + 10 * 8 + 4], b'\0\0\0')
        self.assertEqual(data[flags + 10 * 8 + 4], 1)
        for n in set(range(len(ORIGINAL['FIGHTERS']))) - ORIGINAL['STARTERS']:
            record = struct.unpack_from('<Q', data, records + 8 * n)[0]
            self.assertTrue(record & (1 << 63))
            self.assertEqual((record >> 48) & 255, 3)
            self.assertGreaterEqual(record & 4095, 2026)
        backups = list((self.root / 'save_backups').iterdir())
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.before)
        reference = self.root / 'reference/save/user/save_data/system_data.bin'
        reference.parent.mkdir(parents=True)
        reference.write_bytes(self.before)
        comparison = subprocess.run([sys.executable, str(WORK.parent / 'tools/unlock_all.py'), '--root', str(self.root),
                                     '--save', str(reference)], capture_output=True, text=True)
        self.assertEqual(comparison.returncode, 0, comparison.stdout + comparison.stderr)
        self.assertEqual(reference.read_bytes()[flags:flags + 8 * 74], data[flags:flags + 8 * 74])
        again = self.invoke()
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertEqual(self.save.read_bytes(), data)
        self.assertEqual(len(list((self.root / 'save_backups').iterdir())), 1)

    def test_invalid_layout_rejected(self):
        data = bytearray(self.before)
        data[FLAGS] = 0
        self.save.write_bytes(data)
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.save.read_bytes(), data)
        self.assertFalse((self.root / 'save_backups').exists())

    def test_unknown_game_version_rejected(self):
        # Another game version (11.0.0's main): both refuse and change nothing.
        write_main(self.root, '4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C')
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('knows the save layout', result.stdout + result.stderr)
        python = subprocess.run([sys.executable, str(WORK.parent / 'tools/unlock_all.py'), '--root', str(self.root),
                                 '--save', str(self.save)], capture_output=True, text=True)
        self.assertNotEqual(python.returncode, 0)
        self.assertEqual(self.save.read_bytes(), self.before)
        self.assertFalse((self.root / 'save_backups').exists())

    def test_running_game_guard_preserves_save(self):
        # A deterministic process-list mock with one item tests the refusal path.
        command = f"function Get-Process {{ [pscustomobject]@{{Id=1}} }}; & '{SCRIPT}' -Root '{self.root}' -SavePath '{self.save}'"
        result = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Close every SSBU', result.stdout)
        self.assertEqual(self.save.read_bytes(), self.before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
