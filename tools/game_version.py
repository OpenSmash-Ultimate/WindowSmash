#!/usr/bin/env python3
"""The one game version WindowSmash builds and plays: Super Smash Bros. Ultimate 13.0.5, the
game's last update. One version for everyone: players can always play together (the online
lobby groups players by the game's build), and a bug is found and fixed in one game.

What tells the version, at each point:
  - a game folder's files (tools/gamefiles.py, the installers' window): the update's title
    version (v31), else the game's data.arc version, else the control data's display version
    (scan_problem); a renamed file may tell nothing, and then the build's step 4 decides
  - the program (tools/build_game.py step 4): the main module's build id (main_version)
  - the game data (step 6): data.arc's version (tools/arc.py data_version)
  - the game itself, when it starts (runtime/host.c), and the online lobby (tools/ssbu_relay.py)
    hold their own copy of MAIN_BUILD_ID; tests/test_game_version.py checks that they match
"""
REQUIRED = "13.0.5"
MAIN_BUILD_ID = "21450C647B8C5940ED5B68218EA128FD3C5D597D"   # main's build id, as tools/nso.py writes it
UPDATE_TITLE_VERSION = 0x1F0000   # 2031616 (v31): the 13.0.5 update's title version
DATA_ARC_VERSION = 0x0D0005       # data.arc's version field
EARLY = "an early release"   # (program_version of a game whose data.arc is older than the version field)
KNOWN_BUILDS = {   # other main build ids, to name them in messages
    "4A3F8ED7DCCE3AD4541A3A2F7622B5B57FC8A55C": "11.0.0",
}


def version_text(arc_version):
    """data.arc's version field as the game shows it: 0x0D0005 -> 13.0.5."""
    return f"{arc_version >> 16}.{(arc_version >> 8) & 0xFF}.{arc_version & 0xFF}"


def build_name(build_id):
    """A main build id for a person: 13.0.5, a version known by its build id, or None."""
    build_id = (build_id or "").upper()
    return REQUIRED if build_id == MAIN_BUILD_ID else KNOWN_BUILDS.get(build_id)


def main_build_id(path):
    """The build id of a main module (dump/exefs/main: an NSO, the id in its header), as
    tools/nso.py writes it; None when it can't be read."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x60)
    except OSError:
        return None
    if len(head) < 0x60 or head[:4] != b"NSO0":
        return None
    return head[0x40:0x60].hex().rstrip("0").upper() or "0"


def needs(what, where=None):
    """The one message for a game that isn't 13.0.5: what was found, and what to do."""
    place = f"in {where}, next to the game" if where else "next to the game"
    return (f"WindowSmash needs version {REQUIRED} of the game, and {what}. Put the game's {REQUIRED} update "
            f"(an .nsp file) {place}.")


def program_version(program):
    """What version a scan's program record is, as far as its files tell: ("13.0.5" or another
    version's name, or None when they don't say)."""
    if program.get("kind") == "update":
        if program.get("version") is not None:   # (the title version decides; the display one only names it)
            if program["version"] == UPDATE_TITLE_VERSION:
                return REQUIRED
            display = program.get("display_version")
            return display if display and display != REQUIRED else f"v{program['version'] >> 16}"
        return program.get("display_version")
    data = program.get("data_version")
    if data:
        return data
    if data == "":   # (an early release's data.arc, from before the version field)
        return program.get("display_version") or EARLY
    return program.get("display_version")


def scan_problem(program):
    """Why a game folder's program can't be built, for a person (the installers' window shows it
    under the folder), or None: it is 13.0.5, or its files don't say (step 4 then decides)."""
    version = program_version(program)   # (short: the installer's window has two lines for it)
    if version is None or version == REQUIRED:
        return None
    if program.get("kind") == "update":
        return (f"WindowSmash needs version {REQUIRED}, and this is the game's update {version}: put the {REQUIRED} "
                "update (.nsp) in this folder instead.")
    what = "an early release of the game" if version == EARLY else f"the game at {version}"
    return (f"WindowSmash needs version {REQUIRED}, and this is {what} without its {REQUIRED} update: put that "
            "update (.nsp) in this folder.")
