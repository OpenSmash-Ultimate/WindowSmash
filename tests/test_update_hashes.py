"""update.py's hash tree: every block of the updated image checked against the hash the update
carries for it; an update that takes some of its hashes from the original release (13.0.5 never
does) is not 13.0.5's, and is refused. Made-up hash trees with 64-byte blocks (the game's are
16 KB); no game files."""
from pathlib import Path
import hashlib
import struct
import sys
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import update  # noqa: E402

BLOG = 6
BS = 1 << BLOG   # two hashes per block, so every level of a small tree is used
BLOCKS = 16


def sha(data):
    return hashlib.sha256(data).digest()


def ivfc(l6):
    """Levels 1 to 6 (each padded with zeros to whole blocks) and the master hash: each hash
    level holds the SHA-256 of every block of the level below."""
    levels = [l6]
    for _ in range(5):
        below = levels[0]
        h = b''.join(sha(below[j:j + BS]) for j in range(0, len(below), BS))
        levels.insert(0, h + bytes(-len(h) % BS))
    return levels, sha(levels[0][:BS])


def data():
    return b''.join(bytes([b]) * BS for b in range(BLOCKS))


def digests(l6, corrupt=()):
    return b''.join(sha(l6[b * BS:(b + 1) * BS] if b not in corrupt else b'x' * BS) for b in range(BLOCKS))


class UnconfirmedTests(unittest.TestCase):
    def test_every_block_against_its_hash(self):
        levels, _master = ivfc(data())
        patch = types.SimpleNamespace(hashes=levels[4], blocks=BLOCKS)
        self.assertEqual(update.unconfirmed(patch, digests(data())), [])
        self.assertEqual(update.unconfirmed(patch, digests(data(), corrupt={3, 9})), [3, 9])


def patch_section(reused):
    """An update's RomFS section (BKTR) over the tree of data(): its own data is the image itself,
    except the virtual ranges in `reused`, which it takes from the original release."""
    levels, master = ivfc(data())
    image, offs = b''.join(levels), []
    for level in levels:
        offs.append(sum(len(x) for x in levels[:len(offs)]))
    pieces, at = [], 0
    for a, z in reused:
        pieces += [(at, at, 1), (a, 0x40, 0)]
        at = z
    pieces.append((at, at, 1))

    def table(entries, fmt, end):
        node = bytearray(0x4000)
        struct.pack_into('<IIQ', node, 0, 0, len(entries), end)
        size = struct.calcsize(fmt)
        for j, e in enumerate(entries):
            struct.pack_into(fmt, node, 0x10 + j * size, *e)
        head = bytearray(0x4000)
        struct.pack_into('<IIQ', head, 0, 0, 1, end)
        return bytes(head + node)
    ind_off = len(image)
    ind = table(pieces, '<QQi', len(image))
    ctr_off = ind_off + len(ind)
    ctr = table([(0, 0, 0)], '<QIi', ind_off)
    physical = image + ind + ctr
    fsh = bytearray(0x200)
    fsh[0x08:0x0C] = b'IVFC'
    for i, level in enumerate(levels):
        struct.pack_into('<QQI', fsh, 0x18 + i * 0x18, offs[i], len(level), BLOG)
    fsh[0xC8:0xE8] = master
    struct.pack_into('<QQ', fsh, 0x100, ind_off, len(ind))
    fsh[0x110:0x114] = b'BKTR'
    struct.pack_into('<QQ', fsh, 0x120, ctr_off, len(ctr))
    fsh[0x130:0x134] = b'BKTR'
    sec = types.SimpleNamespace(enc_type=4, fs_header=bytes(fsh),
                                read_raw=lambda off, size, ctr_hi=None: physical[off:off + size])
    nca = types.SimpleNamespace(content_type=0, section=lambda index: sec if index == 0 else None)
    return nca, offs


class OpenTests(unittest.TestCase):
    def open(self, reused_entries):
        l5 = sum(len(level) for level in ivfc(data())[0][:4])
        nca, _offs = patch_section([(l5 + a * 32, l5 + z * 32) for a, z in reused_entries])
        with mock.patch.object(update, 'nsp_ncas', lambda nsp, keydir: [('update.nca', nca)]):
            return update.Patch('update.nsp', 'keys')

    def test_an_update_with_all_its_hashes(self):
        p = self.open([])
        self.assertEqual(p.block_hash(3), sha(data()[3 * BS:4 * BS]))
        self.assertEqual(update.unconfirmed(p, digests(data(), corrupt={7})), [7])

    def test_an_update_reusing_hashes_is_not_13_0_5(self):
        """(What a player's other update did: level 5 partly the original release's.)"""
        with self.assertRaises(SystemExit) as e:
            self.open([(4, 12)])
        self.assertIn('WindowSmash needs version 13.0.5 of the game, and this update takes some of its hashes from '
                      'the original release', str(e.exception.code))


if __name__ == '__main__':
    unittest.main()
