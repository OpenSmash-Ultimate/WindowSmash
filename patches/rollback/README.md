# Rollback netplay for WindowSmash (preview)

Online play with rollback netcode for 2–4 players, each on their own PC, for game version 13.0.5.
It's a **preview for discussion**: it works (full matches in sync over a home network and over the
internet, two to four players), but it is built as a separate exe next to the normal one rather than
as changes to `runtime\`, so the maintainers can judge the design before anything is folded in.

The design follows ssbu-recomp PR #116 (OpenSmash-Ultimate/ssbu-recomp, `refactor/rollback-readability`),
reimplemented on WindowSmash's HLE kernel. No code is copied; both projects are GPL-3.0.

The test-by-test history (every build, log and measurement) is in [DEVLOG.md](DEVLOG.md).

## Contents

- [Trying it](#trying-it)
- [How the build is put together](#how-the-build-is-put-together)
- [Savestates](#savestates)
- [Rollback and re-running frames](#rollback-and-re-running-frames)
- [Netcode](#netcode)
- [Starting a match the same way everywhere](#starting-a-match-the-same-way-everywhere)
- [Fixes around the fight](#fixes-around-the-fight)
- [Files](#files)
- [Settings](#settings)
- [Tests](#tests)
- [Known limits and open work](#known-limits-and-open-work)

## Trying it

1. Build WindowSmash as usual (your own 13.0.5 game files), then run `patches\rollback\build_rollback.bat`.
   It links `build\ssbu_rollback.exe` in about a minute after the first time (see below).
2. Every player starts `launchers\Rollback Netplay.bat`: a small window (no command prompt).
   - One player clicks **Host a match**, picks 2–4 players and **Start hosting**. The window shows the
     address to share (the home-network one and the internet one), with Copy buttons.
   - Everyone else clicks **Join a match**, pastes the address, **Join**.
3. In the game: Smash, pick a fighter, start. The host's rules and stage are used for everyone.

The host's UDP port 7701 is opened on the router with UPnP when the router allows it; otherwise it must be
forwarded by hand. Joining players need nothing open. More detail: [TWO_PC_SETUP.md](TWO_PC_SETUP.md).

`launchers\Rollback Netplay (command prompt).bat` does the same in text, with test options (a fixed
delay, bots). `launchers\Play SSBU Offline (rollback).bat` runs the rollback exe offline.

## How the build is put together

`build_rollback.bat` → `build_rollback.py`:

1. Copies `runtime\` to `build\rb_root\runtime\` (a scratch tree, rebuilt every time).
2. Overlays `patches\rollback\src\*`: new files, plus full copies of the runtime files this build changes.
   Each changed file's original is kept in `patches\rollback\base\`; if `runtime\` no longer matches it,
   the build stops instead of mixing versions. (The NVIDIA shader-eviction fix, "nvfix", is already in this
   build's own `gpu_d3d11.c` and `hle_nv.c`; a separate `patches\nvfix` overlay isn't needed.)
3. Links with WindowSmash's own `tools\recomp.py link`, pointed at the assembled tree. Runtime objects go
   to `build\rt_rollback`. The rollback wraps guest functions (`HLE_WRAP`), which changes `main`'s
   generated C, so this build keeps its own `build\main_rb`, seeded once from `build\main`; only the files
   the wraps touch are recompiled.

With `SSBU_RB` unset, `ssbu_rollback.exe` behaves like the normal build. Folding this into `runtime\`
behind a setting is mostly mechanical: the changes to existing files are small (about 900 changed lines
across 11 files; the rest is about 4,000 lines of new files), and each is already gated on the rollback
switch.

## Savestates

Recompiled guest calls are host calls, so the game's state is guest memory **plus the host stacks of every
guest thread and fiber**: the lifted C keeps values in locals and real call frames. A snapshot is valid only when:

- the main thread is at a fixed **safe point**, reached through the same call chain every frame (the same
  host stack pointer), so its stack above that point can be saved and restored byte for byte. The safe point
  is the 13.0.5 equivalent of the PR's frame kick, found by matching code between the two versions' `main`;
- every other guest thread is parked in `kwait()` (`kernel.c`): every guest wait goes through it, so one
  hold there parks them all, and their host stacks are frozen and can be copied;
- every suspended fiber is frozen at its switch (`hle_fiber.c`), and its host stack is copied too.

WindowSmash makes this more workable than it looks: guest memory lives in fixed areas (`host.h`: heap,
alias, stacks, TLS, modules), so "where the state is" is known rather than scanned for.

A save (`hle_rb_state.c`) copies the committed pages of every guest area, the modules' `.data/.bss`, every
parked thread's `Cpu` and host stack, every fiber's stack, and the host-side state the game can observe
(syncpoint counters, the kernel object table, the clock). A load does the reverse and tells the GPU caches
which guest pages changed.

To keep saves cheap the build keeps a **shadow copy** of guest memory and a ring of **undo pages**: write
watching finds the pages written since the last save, and a save stores only those (about 4 ms; a load
5–10 ms). Saves are **lazy**: they're only taken while a guess about a remote player's input is
outstanding, so a frame with all inputs known costs nothing.

When a load can't be done because a thread is in the middle of a job (a "fiber-refused" load), it waits
briefly if that thread is busy right now, and otherwise falls back to one of up to 3 older states and
re-runs from there. (This was the cause of the one 4-player desync seen; it's been in sync since.)

## Rollback and re-running frames

The fight runs on **logic frames** with a fixed clock (dropped refreshes cleared), so re-running a frame
with the same inputs gives the same result. Inputs come from an **input tape** keyed by logic frame
(`hle_hid.c` asks `rb_pad()` for each player's state in a fight).

When a remote input arrives that differs from the guess, the game loads the state from that frame and
re-runs to the present **fast and unseen**: presentation work is skipped during re-runs (camera, render
layers, the recorder, dynamic resolution; but not the UI, lighting or effect emitters, which the
simulation reads). One frame back plus catching up costs about 2–3 ms; seven back about 20 ms.

**Desync detection:** both games hash the fight state every 10 frames and compare. A frame's hash is only
final when that frame's own inputs were all real (not guessed); before that rule, KO launches showed
false desyncs.

**Audio** (`ipc_audio.c`): a save records the audio write position; a load drops audio not yet played and
skips audio already heard, so re-runs don't repeat sounds. Audio stays on during fights. There is some
doubled sound right at the start of a match that settles by itself.

## Netcode

`hle_rb_net.c/.h` (messages and sync, no I/O) and `hle_rb_link.c` (UDP).

- **Star topology through the host** (player 1): every message goes to the host, which relays it. Joining
  players only send out, so only the host needs an open port.
- **Messages** (wire version 9): `PADS` (inputs, run-length encoded, with per-player acks), `HELLO`
  (join and the round trip), `CHOICE` (each player's fighter), `RULES` (the host's ruleset, chunked),
  `READY` and `MENU` (results-screen presses).
- **Input delay** is chosen from the measured round trip (e.g. delay 2 at about 70 ms).
- **Frame-advantage sync**: a game that runs ahead of the others holds for part of a refresh (a dropped
  vsync), decided every 8 frames from each player's samples, so no one drifts into constant rollbacks.
- **Host/Join**: a joining game starts without a player number; the host assigns the lowest free slot and
  reuses slots that went quiet for 5 s outside a fight. Every PC's own controller and keyboard are its local
  player (`controllers_rollback_p1.ini`); the joiner's physical slot is swapped with its assigned slot.
- **Addresses**: the link learns each player's address from valid messages, so a router that changes the
  source port still works. UPnP uses Windows' `IUPnPNAT` COM API; the host's addresses are written to
  `build\rollback_address.txt` for the launcher window. The port mapping is removed when the game closes.

## Starting a match the same way everywhere

Every game must start the fight from the same state:

- **The seed**: the game's global random generator is pinned at the match set-up to a value the host sends.
- **The fighters (`CHOICE`)**: each PC joins only its own controller and puts the others' choices into
  their slots.
- **Name tags (in `CHOICE`, version 10)**: a tag's controls turn the same pads into other moves, so each PC
  sends its player's tag (the head of its save slot: name and controls, 0x60 bytes). The others write it into a
  free slot of their own tag list (0xF7D8-byte slots; the game keeps two copies in memory, found by searching) for
  the match, point that player's panel at it (panel + 0x214 = slot + 1), and put the slot back once the results
  are over. They go back when the fight ends: the game saves its tag list as the results come.
- **Only a match's set-up arms net mode**: the controls screen builds player records too (and runs a test fight
  whose flag stays set afterwards), so the handshake, rules, tags and the fight itself start only for a record built
  from the match set-up (0x710178f3b8); other screens play locally. A tag slot is told by its control layout
  (blank memory has none); a panel pointing at a slot without a named tag plays with no tag on every PC.
- **Controllers (version 12)**: a GameCube controller plays as one on every PC (`style = auto` in
  `controllers_rollback_p1.ini`; `CHOICE` says so), so its analog triggers (light shield) and the GameCube controls
  agree everywhere; every other pad plays as a Pro Controller. The rollback `hle_hid.c` also writes a GameCube
  state's triggers at +36/+40 (upstream writes +40/+44, which the game reads as the wrong trigger): every PC must
  write them alike. Every panel is built as a human's (+0x450 = 1; the PR's 0 left everyone on default controls).
- **The rules (`RULES`)**: at the match set-up the host reads the game's rule header (0x588 bytes; bytes
  0–1, the mode, kept), the stage set-up's inputs (its seed and history: without the history, stages with
  variations desync) and each panel's rule kind and stock count, and sends them as a 0x5ee-byte blob. Joining
  games write them over their own. As a guard, a header that differs in more than 256 bytes is left alone.

## Fixes around the fight

- **The GO! stutter.** The first full save copied all of memory at the moment GO! appeared (358–935 ms).
  Now the full copy is taken at the match set-up (as loading starts) and kept current every 4 frames through
  the entrances, each refresh waiting at most 4 ms for busy threads (or skipped). The ring is dropped when a
  fight ends. A wait-for-everyone barrier before the entrances exists behind `SSBU_RB_PRE_BARRIER=1` but is
  off: there's no reliable "loading done" signal yet.
- **The results screen.** After a fight each game runs its menus locally, so nobody saw the others' cards
  ready. While the results show (`FighterManager__is_result_mode_impl`), each game presses its own player's
  A/B on every player's card and passes them on (`MENU`). It stops when the fighters are let go (the
  fighter manager's entry or fighter count changes), with the next set-up and 3 minutes as backstops. This is
  a workaround rather than true shared menus.

## Files

| File | What it does |
| :--- | :--- |
| `src/hle_rb_core.c` | Net mode: the match flow, handshakes (seed, CHOICE, RULES), Host/Join, results-screen mirroring, desync checks |
| `src/hle_rb_state.c` | Savestates: the hold, shadow copy and undo ring, save/load, fallbacks |
| `src/hle_rb_net.c/.h` | Message builders/parsers and sync logic (no I/O, unit-tested) |
| `src/hle_rb_link.c` | UDP link, address learning, UPnP |
| `src/kernel.c`, `hle_fiber.c`, `hle_setjmp.c` | The hold in `kwait()`; fiber and setjmp state that saves must include |
| `src/hle_hid.c` | Inputs from the tape in a fight; remote slots shown as connected Pro Controllers |
| `src/ipc_vi.c`, `ipc_audio.c` | Dropped vsync for frame advantage; the audio splice |
| `src/gpu*.c`, `hle_nv.c`, `host.c`, `debug_profile.c` | Telling the GPU caches about loaded pages, the clock, small hooks |
| `base/` | The upstream originals of the changed runtime files (checked at build time) |
| `tests/` | `rb_net_test.c`, `rb_link_test.c`, `run_rb_net_test.bat` |
| `../../launchers/rollback_netplay.ps1` | The Host/Join window |

## Settings

Everything is off unless `SSBU_RB=1`. The launchers set what's needed:

- `SSBU_RB_NET=net,<player>,<port>,<peer port>,<delay>,<frames>,<host>`: net mode (`<player>` −1 = join,
  delay −1 = automatic). `SSBU_RB_NET_PLAYERS` (host): 2–4.
- `SSBU_RB_NET_LIVE=0`: a bot instead of a live controller (tests).
- `SSBU_RB_UPNP=0`: don't touch the router.

The many other `SSBU_RB_*` variables are tuning and diagnostics from development (profiling, verification
replays, ring size, re-run options); their defaults are the tested values.

## Tests

`tests\run_rb_net_test.bat` builds and runs the netcode test with no game: two to four peers through a
relaying host, input delays 0/2/4, a peer that runs slow, and a run with rollbacks disabled that must
fail. `rb_link_test.c` tests the UDP link over loopback (address learning, a moved port, strangers ignored,
bad host names). Both need no game files.

Tested in the game: full 5-stock matches on two PCs (home network and internet over a phone connection,
about 70 ms round trip), 3 and 4 players across two PCs, thousands of rollbacks in stress runs without a
crash, back-to-back matches through the results screen.

## Known limits and open work

- **Game version 13.0.5 only**: guest addresses (the safe point, rule header, seeds) are for 13.0.5.
- **Save cost on slower PCs**: about 9 ms per save on a GTX 1660 Ti PC (half GPU drain, half memory copy).
- **No relay or room codes**: the host shares an IP address and needs UPnP or a forwarded port. Hosts
  behind carrier-grade NAT can't host yet; WindowSmash's own relay (`runtime/net_relay.c`) is the natural fix.
- **The pre-entrance barrier is off**, see above. **The results screen** is a mirroring workaround.
- **No pause menu online**: + is ignored in fights in net mode (wire version 11), as pads are keyed by the logic frame, which stands still while paused. Offline the pause menu works (live pads while paused).
- **Windows only** (UPnP through COM, the launcher window through WinForms).
- **Separate exe**: not yet folded into `runtime\` behind a setting; happy to do that if the approach is wanted.
