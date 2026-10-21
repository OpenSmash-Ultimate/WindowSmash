#!/usr/bin/env python3
"""Unlock every fighter in a save, without earning them.

Usage:
  unlock_all.py [--save <system_data.bin>] [--dry-run]

The save is by default the one the game uses, save/<user>/save_data/system_data.bin. Where the
tables are depends on the game version: the tool checks that the game is 13.0.5 (its main
module's build id, LAYOUTS) and otherwise changes nothing rather than write at the wrong place.
The save keeps two tables indexed by each fighter's save_no from ui_chara_db.prc (0 Mario ... 73 Mii Gunner,
74-83 the DLC fighters, then 84 Kazuya (12.0.0) and 85 Sora (13.0.0): the game unlocks those
itself when their add-on content is installed, so this tool leaves them alone):
  FLAGS    8 bytes per fighter: [0] 1 = selectable, [1..3] challenger state (a challenger the
           player lost to, still to be rematched), [4] the order fighters were first met.
  RECORDS  the "joined the battle" milestone, 8 bytes: bits 0-11 year, 12-15 month, 16-20 day,
           21-25 hour, 26-31 minute, 32-37 second, 48-55 how (3 for most), 63 set. The eight
           starters have none.
Each locked fighter gets what the game writes when one joins: the flag set, the challenger
state cleared, the next order number, and a milestone dated now (the game then shows the
"joined the battle!" milestones once at the next boot; X skips them). Found by comparing the
save with the roster the game shows (2026-10-17, game version 11.0.0). 13.0.5 keeps the same
layout (same save format 0x1b at offset 4; offset 8 is the version that last wrote the save,
0xb000 / 0xd005): its game reads an 11.0.0 save as it is and writes Kazuya and Sora at FLAGS
84-85, their milestones after the other DLC fighters' (2026-10-18).

A copy of the save is written first, to save_backups/ beside the save folder
(system_data.bin.before-unlock-<time>); copy it back over system_data.bin to undo.
Run it only while the game is closed: the game writes its save from memory.
"""
import argparse
import datetime
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE = os.path.join("save", "0123456789abcdeffedcba9876543210", "save_data", "system_data.bin")
# the save layout, by the build id of the game's main module (tools/game_version.py's; this file
# is copied into packages on its own): (game version, save size, FLAGS, RECORDS)
LAYOUTS = {
    "21450C647B8C5940ED5B68218EA128FD3C5D597D": ("13.0.5", 0x5B4AF8, 0x53FF40, 0x54FEA8),
}
STARTERS = {0, 1, 2, 3, 5, 6, 7, 8}   # Mario, Donkey Kong, Link, Samus, Yoshi, Kirby, Fox, Pikachu
FIGHTERS = ["Mario", "Donkey Kong", "Link", "Samus", "Dark Samus", "Yoshi", "Kirby", "Fox", "Pikachu", "Luigi",
            "Ness", "Captain Falcon", "Jigglypuff", "Peach", "Daisy", "Bowser", "Ice Climbers", "Sheik", "Zelda",
            "Dr. Mario", "Pichu", "Falco", "Marth", "Lucina", "Young Link", "Ganondorf", "Mewtwo", "Roy", "Chrom",
            "Mr. Game & Watch", "Meta Knight", "Pit", "Dark Pit", "Zero Suit Samus", "Wario", "Snake", "Ike",
            "Pokemon Trainer", "Diddy Kong", "Lucas", "Sonic", "King Dedede", "Olimar", "Lucario", "R.O.B.",
            "Toon Link", "Wolf", "Villager", "Mega Man", "Wii Fit Trainer", "Rosalina & Luma", "Little Mac",
            "Greninja", "Palutena", "Pac-Man", "Robin", "Shulk", "Bowser Jr.", "Duck Hunt", "Ryu", "Ken", "Cloud",
            "Corrin", "Bayonetta", "Richter", "Inkling", "Ridley", "King K. Rool", "Simon", "Isabelle",
            "Incineroar", "Mii Brawler", "Mii Swordfighter", "Mii Gunner"]   # by save_no
HOW = 3


def record(t):
    return (t.year | t.month << 12 | t.day << 16 | t.hour << 21 | t.minute << 26 | t.second << 32
            | HOW << 48 | 1 << 63)


def main_build_id(root):
    """The build id of the game's main module (an NSO: the id is in its header)."""
    with open(os.path.join(root, "dump", "exefs", "main"), "rb") as f:
        head = f.read(0x60)
    if head[:4] != b"NSO0":
        raise SystemExit(f"{root}: dump/exefs/main is not a game module")
    return head[0x40:0x60].hex().rstrip("0").upper()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)   # (the WindowSmash folder: tests)
    ap.add_argument("--save", help="the save to change (default: " + SAVE.replace(os.sep, "/") + ")")
    ap.add_argument("--dry-run", action="store_true", help="list what would be unlocked, change nothing")
    a = ap.parse_args()
    build_id = main_build_id(a.root)
    if build_id not in LAYOUTS:
        vt = os.path.join(a.root, "version.txt")
        version = open(vt).readline().strip() if os.path.exists(vt) else "unknown"
        raise SystemExit(f"{os.path.abspath(a.root)} has game version {version} (main {build_id[:16]}): this tool knows the save "
                         f"layout of {', '.join(v[0] for v in LAYOUTS.values())} only, so it changes nothing here")
    version, SAVE_SIZE, FLAGS, RECORDS = LAYOUTS[build_id]
    a.save = a.save or os.path.join(a.root, SAVE)
    print(f"game version {version}: {a.save}")
    if not os.path.isfile(a.save):   # (the game makes it: the first time it reaches the main menu)
        launcher = "launchers/Unlock All Fighters.command" if sys.platform == "darwin" else "Unlock All Fighters"
        raise SystemExit(f"No save found ({a.save}). Start the game once, reach the main menu, close it, then run "
                         f"{launcher} again.")

    data = bytearray(open(a.save, "rb").read())
    if len(data) != SAVE_SIZE or struct.unpack_from("<I", data, 0)[0] != SAVE_SIZE:
        raise SystemExit(f"{a.save}: not a system_data.bin of the expected layout ({len(data)} bytes)")
    flags = [data[FLAGS + 8 * n: FLAGS + 8 * n + 8] for n in range(len(FIGHTERS))]
    if any(flags[n][0] != 1 for n in STARTERS) or any(f[0] > 1 or any(f[5:]) for f in flags):
        raise SystemExit("the fighter table does not look as expected: this save's layout is not what this tool knows")
    for n, name in enumerate(FIGHTERS):
        old = struct.unpack_from("<Q", data, RECORDS + 8 * n)[0]
        if old and not old >> 63:
            raise SystemExit(f"unexpected milestone {old:#x} for {name}: this save's layout is not what this tool knows")

    rec = record(datetime.datetime.now())
    order = max(f[4] for f in flags)
    added = []
    for n, name in enumerate(FIGHTERS):
        if flags[n][0]:
            continue
        if not flags[n][4]:
            order += 1
        data[FLAGS + 8 * n: FLAGS + 8 * n + 5] = bytes([1, 0, 0, 0, flags[n][4] or order])
        if not struct.unpack_from("<Q", data, RECORDS + 8 * n)[0]:
            struct.pack_into("<Q", data, RECORDS + 8 * n, rec)
        added.append(name)
    if not added:
        print("every fighter is already unlocked")
        return
    print(f"{'would unlock' if a.dry_run else 'unlocking'} {len(added)}: {', '.join(added)}")
    if a.dry_run:
        return
    # save/<user>/save_data/system_data.bin -> save_backups/ beside save/, out of the game's folders
    backups = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(a.save))))),
                           "save_backups")
    os.makedirs(backups, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = os.path.join(backups, f"system_data.bin.before-unlock-{stamp}")
    shutil.copy2(a.save, backup)
    with open(a.save, "wb") as f:
        f.write(data)
    print(f"saved; the previous save is {backup}")


if __name__ == "__main__":
    main()
