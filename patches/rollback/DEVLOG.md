# WindowSmash rollback test build

A research build that adds rollback netcode to WindowSmash. It's a separate exe,
`build\ssbu_rollback.exe`, next to the normal `build\ssbu.exe` and the nvfix
`build\ssbu_nvfix.exe`. It doesn't touch either of them, and it doesn't touch `runtime\`.

Reference design: ssbu-recomp PR #116 (OpenSmash-Ultimate/ssbu-recomp, branch
`refactor/rollback-readability`), cloned read-only for reference.
That code can't be merged: it's a different runtime (Vulkan/GL NVN layer, `nn*.c` SDK
reimplementation) for game 13.0.4. Here, the design is reimplemented on WindowSmash's HLE
kernel for 13.0.5. No code is copied verbatim. Both projects are GPL-3.0, so borrowing ideas
and structure is fine.

## How the build is put together

`build_rollback.bat` → `build_rollback.py`:

1. Copies `runtime\` to `build\rb_root\runtime\` (a scratch tree, rebuilt every time).
2. Overlays the nvfix patches (`patches\nvfix\*.patched`), so the NVIDIA shader-eviction fix is
   in. This is skipped with a warning if `runtime\` no longer matches nvfix's `.orig` files.
3. Overlays `patches\rollback\src\*` (new files, plus full copies of any runtime files this build
   changes). Each changed file has its original in `patches\rollback\base\`. If `runtime\` no
   longer matches it, the build stops instead of mixing versions.
4. Links with WindowSmash's own `tools\recomp.py link`, pointed at `build\rb_root\runtime` instead of
   `runtime\`. The runtime objects go to `build\rt_rollback`. Game module objects are shared with
   the normal build, except `main`: an `HLE_WRAP` changes main's generated C, so this build
   keeps its own `build\main_rb`. It's seeded once from `build\main` (1 GB), and only the files the
   wraps change are recompiled. A relink takes about a minute.

New source files must match the linker's runtime pattern
`(host|kernel|gpu*|hle*|ipc*|debug*|net*).c`. State and save code is `hle_rb_*.c`, and netcode
is `net_rb_*.c`.

Everything is off unless `SSBU_RB` is set. With it unset, `ssbu_rollback.exe` should behave like
`ssbu_nvfix.exe`.

## Why this is more workable here than it looks

From reading both runtimes:

| Need | ssbu-recomp (PR) | WindowSmash |
|---|---|---|
| Know where guest state lives | Scans ranges; "extras" found by hand | Fixed areas in `host.h`: heap `0x74..`, alias `0x60..` (the game's 62 GB), stacks `0x77..`, TLS `0x7780..`, modules `0x71..`. All at `RT_MEMBASE` + guest address |
| Park other threads | Hooks in nn::os mutex/event code | Every guest wait goes through one function, `kwait()` in `kernel.c` |
| Fibers | Host fibers, guest-stack ring (`rg_gstk`) | Host fibers in `hle_fiber.c` (single switch point `SwitchToFiber`) |
| GPU | NVN reimplemented; command buffers are host state | Maxwell pushbuffers live in guest memory; host state is the register file + caches derived from guest memory |
| Write tracking | `MEM_WRITE_WATCH` on window 1, VEH "learned set" | Alias is already `MEM_WRITE_WATCH`, but `gpu_d3d11.c` consumes and resets it (`watch_written`). That needs a shared broker, see M3 |

## The core problem (same in both)

Recompiled guest calls are host calls, so the game's state is guest memory plus the host stacks
of every guest thread and fiber. Those stacks hold real call frames, and the lifted C code keeps
values in locals. A snapshot is only valid when:

- the main thread is at a fixed **safe point**, reached through the same call chain every frame
  (same host SP). Then the host stack above SP can be saved and restored byte for byte;
- every other guest thread is parked in `kwait()`. Its host stack is frozen and can be copied;
- every suspended fiber is frozen at `SwitchToFiber`. Its host stack is copied too.

The PR found its crashes came from missing exactly this kind of state: fiber guest stacks,
memory committed after the base, and objects destroyed inside the rolled-back window. Here,
thread and fiber host stacks are first-class parts of the snapshot from day one.

## Milestones

Each one ends in something testable on one PC before any netcode exists.

- **M0, scaffold** (done when `build_rollback.bat` links and the log shows `[rb]` lines). Separate
  exe, launchers, env switch.
- **M1, find the safe point. DONE 2026-10-29, see the status log.** Locate the 13.0.5 equivalent of the PR's frame kick (13.0.4 guest
  `0x7103749b24`, "the kick after the submission"), by matching code bytes and call structure
  between the two `main` NSOs. Instrument it: per frame, log host SP, how many guest threads are
  in `kwait`, and how many fibers are live. Done when, during a fight, SP is identical every frame
  and all non-main threads reach `kwait` within a few ms.
- **M2, full savestate (single PC). Savestates DONE 2026-10-29 (100 loads, no crash). Replay: the same number of logic frames every time (fixed clock, dropped refreshes cleared); replays with recorded input match the fight hash on every frame (input tape, 10 s, 3 replays); the controls fail as they should: see the status log.** Hotkeys: F5 to save, F7 to load, in training mode. Save:
  committed pages of every guest area, module `.data/.bss`, every parked thread's `Cpu` and host
  stack, every fiber's host stack and token, and the host-side state the game observes (syncpoint
  counters in `hle_nv.c`/`gpu_host1x.c`, the kernel object table, the clock offset). Load: the
  reverse, and the GPU caches are told every guest page changed. Done when 100 save/load cycles in
  a busy training match never crash, and a load replayed with the same recorded pads gives the
  same checksum for the same frames.
- **M3, cheap captures. Partly done 2026-10-30: page-by-page save (about 4 ms) and load (about 5–10 ms) of one state; the ring is not built yet.** A per-frame ring (8 states) storing only pages written since the last
  capture, using write-watch on heap and alias. The heap reservation gains `MEM_WRITE_WATCH` in
  this build. A broker in `hle_rb_watch.c` reads and resets the watch once per frame and hands the
  dirty list to both the ring and `gpu_d3d11.c`. Target: under 3 ms per capture at 60 fps.
- **M4, rollback simulation (single PC). DONE 2026-10-30.** Re-run frames fast and unseen (`rb_fast`), with the
  presentation work they leave out (`SSBU_RB_LITE`, default 57: camera, render layers, recorder, dynamic
  resolution; NOT UI 2, lighting 4, effect emitter 64) and the job-group kicks always run (`SSBU_RB_SKIP` 0: 49
  crashed fight ends). Rollback 1 back + catch-up ~2-3 ms, 7 back ~20 ms. Long fights with thousands of rollbacks
  end cleanly (stress runs, see the status log).
- **M5, netcode. DONE 2026-10-30 (two PCs on a home network).** `hle_rb_net.c` (input queues, prediction,
  PADS / HELLO / CHOICE messages, frame advantage; `tests\run_rb_net_test.bat`), `hle_rb_link.c` (UDP), net mode
  in `hle_rb_core.c`: the seed handshake (pins the game's global generator 0x71052391c0 at the match set-up), the
  match handshake at the player record builder (each PC joins its own controller, a CPU in the other slot; player
  1's stage), live controllers, desync detection (fight hashes every 10 frames), lazy saves (only while a guess is
  coming). Full 5-stock matches between two PCs in sync with no crash (twopc10-15). How to: `TWO_PC_SETUP.md`,
  `launchers\Rollback Netplay.bat`, `pack_for_other_pc.bat`.
- **M6, polish. Internet play WORKS (2026-10-31, inet2-3: phone-tethered PC 2, delay 2 from a ~70 ms trip, smooth, in sync): host/join, the host's port opened by UPnP (no router set-up), the delay from the round trip, PADS in runs. Audio during rollback fixed (the splice, heard 2026-10-31). Started 2026-10-30: time sync fixed (holds leave a refresh out; twopc16); the fight hash 40 ms -> 0.02 ms on PC 2 (no VirtualQuery); twopc20 at delay 1: 60 fps on both, "felt like an offline match".** Next: input delay from the round trip; play over the internet (a relay or port forwarding);
  audio during rollback; 3+ players. (The second PC is done: PC 2, GTX 1660 Ti.)

## Open questions to answer early

- **Determinism.** Does fight simulation read anything host-side that differs between the original
  run and a re-run (svcGetSystemTick, `rt_clock_ns`, thread scheduling order of the job workers)?
  M4's verifier is the test.
- **Same start on two PCs.** Partly answered by the PR's `services/handshake.h`: the host proposes
  both players' ui_chara and colour, control layouts, the stage entry, hazards, items, stocks and a
  seed. Both games write those through a debug-match hook, re-seed `nn::os::GenerateRandomBytes`,
  fix two clock reads used as seeds, and copy the host's stage set-up history (`0x71050c5ad0`,
  13.0.4). Without that history copy, stage variation differed and the games desynced from
  frame 7. Every one of these addresses needs mapping to 13.0.5 (hostrt.c `g_code_13_0_5` has
  most of them).
- **GPU work on re-run frames.** Can it be skipped (no present, and syncpoints advanced by hand), or
  does the game read GPU results back?
- **Memory budget.** The PR copies about 2000 pages (8 MB) per capture in its dense test.

## Status log

- 2026-10-29: M0 done. `build_rollback.bat` links `build\ssbu_rollback.exe` (916 MB, 15 s, with the
  nvfix patches applied). A 12 s smoke run with `SSBU_RB=1` printed `[rb] ... on`, applied the
  nvfix cache budgets and booted through the static constructors. `runtime\` still matches
  nvfix's `.orig` files. The milestone design is from reading code; nothing past M0 has run yet.
- 2026-10-29: M1 done. The safe point is the return from FUN 0x7103549720 at 0x710374a0d4, which
  is the PR's 13.0.4 0x7103749b24 mapped through ssbu-recomp's verified `g_code_13_0_5` table.
  It's hooked with `HLE_WRAP` in `src/hle_rb_core.c`. That needs its own lifted main
  (`build\main_rb`); only g0363.c changed. `kernel.c` and `hle_fiber.c` got census counters.
  Measured over one regular Smash fight with 3 CPUs, Steve placing blocks: 15,337 frames (~4 min).
  - Host SP at the safe point was `0x2bd5f400` on all 15,337 frames.
  - Every other guest thread was waiting on all 15,337 frames: 23 threads, 20 in kwait, up to 3 in
    svcSleepThread, 0 busy. The time until they were quiet was p50 0.2–0.4 ms; the worst window's
    p95 was 0.86 ms and the worst single frame 3.89 ms.
  - Fibers: 20–40 alive, 6 marked running. These are probably the fibers the parked threads are
    on, so their host stacks are those threads' stacks. Check in M2.
  - Frames were 16.2–16.4 ms on average. One 168 ms gap at the fight's first frames (loading).
  - Fight detection through `lib::Singleton<app::FighterManager>::instance_` + 0xd2 works on
    13.0.5 (start and end were both seen).
  - The svcSleepThread threads aren't parked by a wait. M2's hold has to gate them as they return
    from Sleep.
- 2026-10-29: M2 first working savestates (F7 save, F8 load at the safe point). Code:
  `src/hle_rb_state.c` handles the hold, parking, scratch-stack loads, memory, and the order of a
  load. Kernel, fiber, setjmp, display-queue and audio-queue state live in their own files
  (`kernel.c`, `hle_fiber.c`, `hle_setjmp.c`, `ipc_vi.c`, `ipc_audio.c`). Bugs found on the way:
  1. On Windows the guest lives at its own addresses (`RT_MEMBASE` 0), among host stacks and heaps.
     Walking the "guest address range" saved 5.5 GB including host memory, and the load wrote it
     back over the main thread's own stack. Now only the kernel's memory map is saved
     (`kernel_rb_regions`: module data, heap, stacks, TLS, mapped memory): 2 GB, 0.32 s.
  2. vi's buffer-queue slot states are host state. Without them a load showed stale frames
     (flicker), and every load leaked the buffers the game had forgotten it held. After two loads
     none was free, and `DEQUEUE_BUFFER` said "would block" forever (freeze). Saved now.
     audout's held/released lists had the same leak (sound runs dry); saved now.
  3. A guard from the PR: a fiber marked switched away whose host stack is in use is not
     copied back (0 seen so far).
  Training test by hand: 1 save, 4 loads at 94–97 ms each, clean picture, no crash. A linker map
  (`build\ssbu_rollback.map`) is written for reading crash and freeze reports.
- 2026-10-29 (evening, unattended): **M2 stress and replay tests.**
  - **Stress test** (`SSBU_RB_AUTO=600,120,100`, Steve vs. level 8 CPU, regulation rules): 1 save
    and 100 loads (97 of them in one fight) took 95–121 ms each (median 100). There were no crashes,
    freezes or refused loads, and the fight ran on normally afterwards (12,600 frames). Log:
    `build\play_rollback_m2stress.log`. The first 3 loads were in the attract-mode demo fight;
    `fight_on()` counts that as a fight too.
  - **Replay check** (`SSBU_RB_VERIFY=<frame>,<k>,<rounds>`, code in `rb_ref_capture` /
    `rb_ref_compare`): save, run k frames, copy all saved regions (the reference), then repeatedly
    load and run the same k frames with no input. Each page is compared with the reference and with
    the previous replay. Per-page detail goes to `build\rb_verify_<round>.txt` (kept in
    `build\rb_verify_k60\`, `build\rb_verify_k600\`). The first try crashed: the game decommits
    GPU memory between save and reference, so the save's region list had pages that were gone.
    `page_there()` now checks.
  - Results with k=60 (1 s), 5 rounds: about 1,000 of 496,000 pages differ from the reference;
    consecutive replays differ in about 300.
    - Module data: about 12 pages, about 100 bytes. These are counters off by exactly one (0x295
      against 0x296, 0x1a4 against 0x1a5, 0x3c07 against 0x3c06), a stored clock value
      (0x710593b470), and a few floats.
    - Guest stacks: about 8 pages (dead stack below the live frames, plus a few time values).
    - Alias (the game's 62 GB reservation: game heap and GPU memory): about 1,000 pages, in 64 KB
      blocks. They are bimodal, either a few bytes per page (median 35) or whole buffers rewritten.
      943 of the pages are the same set from round to round.
  - Results with k=600 (10 s), 3 rounds: about 3,400 pages differ (alias 2,300–3,400), and module
    data stays at about 13–15 pages and 200 bytes. Ten times the frames gave three times the
    differences, which is not an explosion. The fight may or may not stay in sync: these counts
    can't tell.
  - **What it means.** The savestate itself restores cleanly: the replays start from identical
    memory. The game reads the real clock, and its frame pacing lets the number of logic frames run
    per safe point drift (counters off by one). Rollback needs both fixed, as ssbu-recomp's does
    (its `hostrt.c` lists "clock read return address (fixed seed)" sites and drives its own frames):
    1. during a fight, a clock that advances exactly one frame per safe point, not wall time;
    2. one logic frame per safe point (find the 13.0.5 frame-skip/catch-up path);
    3. then repeat this check: module data should reach 0 differing bytes. What remains in alias
       should be GPU/audio buffers only (classify by checking which blocks the GPU or audio code
       writes).
  - **Next:** that work is M4's determinism groundwork, and it can start before M3's cheaper
    captures. A load takes 100 ms and a save 300 ms, too slow for per-frame rollback, so M3 (copy
    only the pages written since the last capture) is still needed for netplay.
- 2026-10-29 (late, unattended): **Determinism: the fixed clock and one logic frame per safe point.**
  Every run is `SSBU_RB_VERIFY=600,60,5` (or `600,600,3`), Steve against a level 8 CPU with nobody
  at the controls. Logs are `build\play_rollback_<name>.log` and per-page detail is in
  `build\rb_verify_<name>\`.
  - **Fixed clock** (`src\kernel.c`, end of file; `rb_clock_frame` is called from the safe point in
    `hle_rb_core.c`). Both the lifted CNTVCT reads and svc 0x1E go through `rt_ticks`. In a fight,
    `rt_ticks` returns `rb_vtick`, which moves exactly 320,000 ticks (1/60 s) per safe point, plus
    one tick per read the calling thread has made since that safe point. Without that per-read
    tick, a loop waiting for the tick to move would hang. After the fight it is the real counter
    plus an offset, so it never goes backwards. The clock state is saved in KSnap, and a load starts
    every thread's per-read count again. `SSBU_RB_CLOCK=0` turns it off. Result (`clock60`): the
    stored clock value 0x710593b470 no longer differs, but the frame counters still came out 0–2
    ahead.
  - **GPU timer** (`src\gpu.c` semaphore timestamps; `src\hle_nv.c` GetGpuTime, on top of the
    nvfix copy): both now come from `rb_gpu_ns()`, which follows `rb_vtick` in a fight and keeps
    the `rt_now_ns` origin. A first attempt that used `rt_ticks()*32` instead moved the timer's
    origin, and the game hung at a black screen on boot: it kept presenting frames but drew almost
    nothing (`play_rollback_gputs_hang.log`). Moving the GPU timer alone did not change the frame
    drift (`clock60c`, `clock60d`).
  - **The drift's cause.** A logic-frame counter (0x71052c5184) is logged at each safe point.
    Replays often ran 2 logic frames in one main-loop pass: always at frame 2 after a load (catching
    up after the 100 ms hitch), and sometimes mid-run (frames 28 and 49). ssbu-recomp's
    `fighter_host.c fh_frame_end` explains it. The graphics frame end, 13.0.5 FUN 0x71038701e0
    (13.0.4 0x710386fc30), compares the count of the game's own vsync-count thread with the last
    frame's count. It reports the difference minus one as "dropped" refreshes through its third
    argument (result 0xF0044007 / 0xF0044005), and the game then runs that many EXTRA logic frames.
    That count follows wall time.
  - **Fix:** `HLE_WRAP(71038701e0)` in `hle_rb_core.c` clears the report in a fight, as the PR does
    (`SSBU_RB_DROPS=1` keeps it). It logs the first 5 reports it clears (always 1–2 refreshes:
    around saves and loads, and now and then mid-fight).
  - **Results** (pages differing from the reference; module data in bytes):

    | run | logic frames per replay | total pages | module data |
    |---|---|---|---|
    | before, k=60 (`verify60`) | 0–2 extra | ~1,000 | ~12 pages, ~100 B |
    | fixed clock only, k=60 (`clock60`) | 0–2 extra | 470–1,500 | 4–10 pages, 60–215 B |
    | clock + drops cleared, k=60 (`det60`) | **exactly 60, every round** | 400–590 | 4–5 pages, 66–85 B |
    | before, k=600 (`verify600`) | drifting | ~3,400 | 13–15 pages, ~200 B |
    | clock + drops cleared, k=600 (`det600`) | **exactly 600, every round** | 1,236–1,834 | 5–6 pages, 40–82 B |

  - **What still differs.**
    - Module data: floats around 0.05–0.2 at 0x7105298ac0..af0, 0x7106dd5314 and 0x7106df6020; a
      counter at 0x710529b844; a pointer at 0x7106d42691 to one worker thread's guest stack or
      another; and 0x710593d390 (0x3c against 0x3d). These look like the job system's timings and
      statistics (which worker ran a job), not fight state. Not proven yet.
    - Guest stacks: dead stack below the live frames. TLS: 1–2 bytes.
    - Alias: still unclassified, 400–1,800 pages.
  - **Next:**
    1. A fight hash, as ssbu-recomp's `SSBU_FIGHT_HASH`: positions, damage and status of each
       fighter per frame, compared across replays. It proves that the fight itself replays exactly
       and that the remaining bytes are only bookkeeping.
    2. Classify the alias pages (GPU memory against game heap; gpu_d3d11.c knows which ranges are
       GPU buffers) and drop GPU/audio ranges from the compare.
    3. Player 1's pad: the replays ran with no input. Rollback replays recorded input, which the
       safe point must feed per logic frame (the PR counts fight frames at the pad poll).
    4. Then M3 (cheaper captures).
- 2026-10-30: **Fight hash: the fight replays exactly.** Code: `hle_rb_core.c` (`fight_frame`,
  `fight_hash`).
  - **What it reads, per safe point.** For each entry slot 0–7: `app::fighter::get_id_from_entry_id`
    (it returns 0x50000000 for an empty slot), then `app::sv_battle_object::module_accessor`. The
    exported lua_bind getters then give status kind, situation, motion kind and frame, position,
    facing, the summed speed and damage. It also reads the 9 script random generators of
    `app::sv_math::rand` (180 bytes at [[[0x71052c35b0]]], 13.0.4 0x71052c25b0, and their call
    counters). The getters are called with `rt_callsym` on the main thread's context, and its
    registers are put back afterwards. An FNV-1a hash is taken over all of it.
  - **The replay check** (`SSBU_RB_VERIFY`) stores each reference frame and compares each replay
    frame against it. It prints "matches on all k frames", or the first frame that differs with
    both fighters' values side by side. `SSBU_RB_HASH=1` writes every fight frame to
    `build\rb_hash.txt`, one line per fighter.
  - **Results** (Steve against a level 8 CPU, no input):
    - k=60, 5 rounds (`hash60`): every replay **matches on all 60 frames**.
    - k=600, 3 rounds (`hash600`): every replay **matches on all 600 frames**. The CPU hit Steve
      during them (14% to 44% damage), the states changed throughout, and the generators were drawn
      some 11,000 times.
    - Control, with `SSBU_RB_CLOCK=0 SSBU_RB_DROPS=1` (`hashneg`): every replay **fails at frame 3
      or 4**. Each replay frame is the reference's NEXT frame (replay frame 3 hashes the same as
      reference frame 4). The old drift was only the game's extra catch-up logic frames; the
      simulation itself never diverged.
  - So the bytes still differing in the page compare (job-system timings and statistics, alias
    pages) are not fight state. M2's replay check is done, without input.
  - **Next:**
    1. Recorded pad input fed per logic frame, and the hash checked with input. The PR counts fight
       frames at player 1's pad poll.
    2. M3, cheaper captures: a 290 ms save and a 100 ms load are far too slow for per-frame
       rollback.
- 2026-10-30: **Input tape: replays with input are exact.** Code: `hle_rb_core.c` (`rb_pad`,
  `rb_pad_sampling`), with `src\hle_hid.c` (`npad_state` calls them; its original is in
  `base\hle_hid.c`).
  - **How it works.** In a fight, each player's pad state (after the pad, the keyboard and
    SSBU_INPUT) is keyed by the logic-frame counter at 0x71052c5184. That counter is guest memory,
    so a load restores it. The first read of a frame records the state; every later read of that
    frame, including in a replay after a load, gets the recorded state. The tape is cleared at
    fight start. In a fight, the sampling number the game sees is a base plus the logic frame, so
    it is the same in a replay; outside a fight it is a plain counter that never steps back.
    `SSBU_RB_TAPE=0` turns the tape off.
  - **Measured.** Player 1's pad is read exactly once per logic frame, always on the same thread,
    and the counter equals the fight frame (1 at fight frame 1). There is no ambiguity about which
    frame an input belongs to.
  - **Test input**, `SSBU_RB_INPUT="<period>;<frame>:<button>[/<hold>],..."`, runs on WALL time,
    so a replay's live input differs from the first pass's. The runs used
    `120;0:SRIGHT/25,30:A/4,45:SLEFT/25,75:X/4,90:B/4,104:SUP/3,104:A/3`: walk, jab, walk back,
    jump, special, up-air.
  - **Results** (Steve against a level 8 CPU):
    - k=60, 5 rounds (`tape60`): every replay **matches on all 60 frames**; 60 tape playbacks per
      replay.
    - k=600, 3 rounds (`tape600`): every replay **matches on all 600 frames**. Steve went through
      dozens of states, took 32% and dealt 14.5%.
    - Control, `SSBU_RB_TAPE=0` (`tapeneg`): every replay **fails at frame 7–9**, player 1's
      status first, as soon as the live input differs.
  - **M2 is done:** a savestate plus the input tape replays a fight exactly, with input, verified
    by the fight hash.
  - **Next: M3, cheaper captures.** A save takes about 290 ms and a load about 100 ms. Rollback
    needs a capture every frame and a load plus several re-run frames within one frame. Plan:
    write-watch only the pages written since the last capture (the alias region already uses
    MEM_WRITE_WATCH), and on a load copy back only the differing pages. The PR's `game_state.c`
    does this ("page compare, only differing pages are copied back").
- 2026-10-30: **M3, page-by-page saves and loads.** Code: `hle_rb_state.c` (the guest memory
  section: `layout`, `sync`, `memory_save`, `memory_load`), `src\gpu_d3d11.c` (on top of the
  nvfix copy; original in `base\`: `watch_rb_take` / `watch_rb_next` and a lock around
  `watch_take`), `src\kernel.c` (`rb_mem_changed` and MEM_WRITE_WATCH on the heap, stack and
  thread-local areas), `src\host.c` (MEM_WRITE_WATCH on the module images and NRO ranges).
  - **The shadow.** The savestate's copy of guest memory is kept from save to save, one copy per
    committed run. A save copies into it, and a load copies back from it, only the pages written
    since memory last matched. Written pages are found as follows:
    - alias area: the renderer's write-watch broker. Its per-page stamps are shared, so the
      textures' news stays theirs.
    - heap, stacks, TLS and modules: their own write watch, which only this code reads.
    - anything else: compared with the shadow.
  - **Layout changes.** The kernel logs every commit and decommit (`rb_mem_note`, up to 256 per
    interval). Those ranges are copied whole: a page decommitted and committed again is zeros, with
    no write seen. A load recommits them first. A save re-walks the runs (VirtualQuery, about
    16 ms) only when the kernel's map or the change log moved; a load never walks them.
  - **Switches:** `SSBU_RB_FULL=1` copies everything every time (the M2 behaviour).
    `SSBU_RB_CHECKLOAD=1` compares every page with the shadow after each save and load (2: loads
    only). `SSBU_RB_BENCH=<frame>,<count>` saves at every fight frame.
  - **Results** (`play_rollback_m3e.log`: benchmark 300,300, then replay check 600,60,5 with the
    input tape and test input):

    | | M2 | M3 |
    |---|---|---|
    | save, memory part | ~230–290 ms (2 GB) | **2.0 ms p50** (about 3,000 pages, 11–13 MB) |
    | save, total | ~290 ms | **4 ms p50**, 20 ms p95 (layout re-walks), hold 1.5 ms |
    | load, memory part | ~95 ms (2 GB) | **5 ms** (about 5,600 pages, 21 MB, after 60 frames) |

    The load totals logged (84 ms) include the 2 GB check; without it a load is roughly 5–10 ms.
    The first save copies everything (about 300 ms). 18 of 301 saves re-walked the layout (16 ms).
  - **Correctness:** 0 pages differed from the shadow after every save (`CHECKLOAD=1`, 301 saves)
    and every load (`CHECKLOAD=2`, 5 loads). Every replay matched the fight hash on all 60 frames,
    including a save made page by page on top of 300 others.
  - **Next:**
    - The layout re-walk: re-query only the changed ranges.
    - The 1.5 ms hold.
    - A ring of several states for real rollback: one load back N frames, re-run with corrected
      input, keep saving (M4). Undo records per frame (the shadow's old pages) would let a load go
      back to any state in the ring.
- 2026-10-30: **M3, the ring: going back N frames works.** Code: `hle_rb_state.c` (save / load:
  the ring; `Undo`, `undo_push`, `undo_apply`), `hle_rb_core.c` (`ring_test`).
  - **How it works.** It keeps the last N states (`rb_ring_size`, up to 64). Guest memory has one
    copy, the shadow, which is the newest state. Each older state keeps an undo record: the pages a
    later save overwrote in the shadow, with their content in that state. A load of the state
    `age` saves back works in three steps:
    1. memory goes back to the newest state (page by page, as before);
    2. the undo records are applied newest-first, in memory and in the shadow;
    3. the newer states are dropped.
  - **Host state, per state:** kernel, fibers, setjmp records, display and audio queues, and every
    thread's stack and registers (`ThrSave`; `RbThread.ld` points at the record a load restores).
  - **API:** `rb_save` returns 1 (saved), 0 (not) or 2 (a load came back here); `rb_load_age`.
  - **Barriers.** A save that finds a run added, dropped or copied whole has no undo for it, so the
    older states are dropped there. That happened on about 3% of saves (60 of 2,139): the game's
    commits and decommits split and merge runs.
  - **Test:** `SSBU_RB_RING=<frame>,<every>,<depth>,<count>[,<kept>]`. From that fight frame it
    saves every frame; every <every> frames it loads the state <depth> back, <count> times. Each
    fight frame's hash is kept by logic frame on its first run, and every re-run must match it. The
    re-run frames go at normal speed here; running them inside one frame is M4.
  - **Results:** `SSBU_RB_RING=300,20,7,100`, with the input tape and test input, against a level 8
    CPU.
    - `ring1` (with `CHECKLOAD=2`): 100 loads, each 7 back, and every one matched the shadow
      exactly. 694 frames ran again, all **the same** as their first run, 0 different.
    - `ring2` (no check): the same result, 694 the same and 0 different. A load **7 frames back
      takes 8.2 ms mean** (max 25): about 22,000 undo pages plus the newest-state copy. A save takes
      5.5 ms mean, counting the barrier saves (16–300 ms) and layout re-walks.
  - **Next:**
    - Shadow by fixed 2 MB chunks instead of runs. Commits and decommits would then touch only
      their chunks: no barriers, and no layout walk at all.
    - Undo pages kept in one slab per state rather than realloc'd arrays.
    - M4: run the frames after a load inside one display frame (no present, input from the tape),
      as the PR's rollback re-run does.
- 2026-10-30: **M3, the shadow by 2 MB chunks: no more barriers, and a load restores the layout.**
  Code: `hle_rb_state.c` (guest memory: `Chunk`, `settle_page`, `settle_range`, `settle_all`,
  `sync`), `src\kernel.c` (`rb_mem_note` exported), `src\host.c` (module loads noted).
  - **The shadow.** It is kept in 2 MB chunks of the guest's address space, each with a bit per
    page: present, that is committed and read-write in a guest region when memory last matched.
    Pages written since are found as before (stamps, write watch, compare), but only where present.
  - **Committed and decommitted pages** (the kernel's list, and module loads) are settled page by
    page:
    - a save takes their content and presence, with undo entries: content, "was absent", or "was
      decommitted since";
    - a load gives them back. It recommits what the shadow has and decommits what it does not, so
      memory's layout is that state's too (in M2 a load left later-committed memory in place).
    This step runs before the written pages, because a page written and then decommitted is no
    longer there to copy. Each page gets one undo entry per save: the first, holding the content
    of then.
  - **Layout walk.** The whole layout is asked of Windows only at the first save, or when the
    kernel's map changed with nothing in the list (`kmap_sig`, refreshed after a load puts the
    kernel back).
  - **Checks:** `CHECKLOAD` now also checks presence: present bits against what is committed in
    guest regions. The check after the undo records runs after the kernel's map is put back. Run
    before that, it compared against the newest map and reported false differences.
  - **Results** (`SSBU_RB_RING=300,20,7,100`, input tape and test input):
    - `chunk2` (checked): 100 loads, each 7 back. After the newest state and after the undo
      records: **0 pages differ, 0 pages' presence differs**, every time. 694 re-run frames, all the
      same.
    - `chunk3` (no checks): 694 the same. **Saves 4.2 ms mean** over 2,000 (the first, a full copy,
      takes 365 ms). Only 16 others took over 8 ms (0.8%), and **no barrier**: in `ring2`, 3% of
      saves dropped the older states. **A load 7 back takes 7.8 ms mean, 9.6 ms max** (undo about
      22,700 pages in 6.6 ms).
  - **Next:**
    - The slow saves (up to 24 ms) are `settle_range` walking a large mapped range page by page
      with nothing changed. Skip chunks the shadow doesn't have, and whole ranges that are absent
      both then and now.
    - M4: run the frames after a load inside one display frame.
    - Caveat: a load back across a module load (NRO) would decommit that module's pages. Modules
      load before a fight, so not seen yet.
- 2026-10-30: **Slow saves trimmed; M4 begun: re-run frames unseen and unpaced.**
  - **`settle_range`.** A range not committed now only matters where the shadow has pages, so
    chunks the shadow lacks are skipped whole and others go by their present bits. In `fast1`
    (checked), all 85 loads had 0 differences in content and presence, after the newest state and
    after the undo records.
  - **Fast re-run** (`src\ipc_vi.c` vsync thread, `hle_rb_core.c` `rb_fast`). After a load, for the
    frames back to where the game was, the vsync thread neither waits for the refresh nor presents:
    each frame is taken off the queue as soon as it is drawn (it still waits for the GPU), and the
    vsync event fires at once. `SSBU_RB_FAST=0` shows them at normal speed (the M3 test).
  - **Results** (`fast2`: `SSBU_RB_RING=300,20,7,100`, input tape and test input, no checks):
    - every re-run frame matched its first pass: 694 the same, 0 different;
    - a load 7 back took 8.1 ms;
    - **back 7 frames and caught up again, unseen: 108 ms mean (max 165)**, that is 14.2 ms a re-run
      frame including its save (saves are 4.15 ms mean). `fast1` (checked, the match ended at 85
      loads) also had 0 different among the 518 counted at its last progress line.
  - **What that means.** Rollback must fit the load and the re-run in one 16.7 ms frame. With about
    10 ms of game work a frame (drawing included, then thrown away) plus a 4 ms save, only about
    one frame of rollback fits now.
  - **Next (M4):**
    1. Don't draw re-run frames: skip the game's GPU submission and present work while `rb_fast`.
       The PR's 13.0.4 "render loop call site, workers hold" / "run-ahead save point" entries map
       where to cut in 13.0.5.
    2. Measure the game's logic-only frame cost.
    3. Cheaper saves while re-running: the 1.2–1.9 ms of alias stamps and the hold.
- 2026-10-30: **M4, re-run frames without their drawing work (the PR's skips).** Code:
  `hle_rb_core.c` (`kick_skipped`, `LITE`, 19 new `HLE_WRAP`s: main is relifted).
  - **What is left out.** While `rb_fast > 1` (re-run frames but the last, whose picture is shown
    next):
    - `SSBU_RB_SKIP=<mask>` (default 49, the PR's): the job-group kicks (FUN 0x7103549720, the same
      function the safe point wraps) from the render preparation (1: 0x710374a0d4, 0x710374baa8,
      0x710374bad8, 0x710374bb08), preparation list A (16: 0x7103749aac) and list B
      (32: 0x7103749b48);
    - `SSBU_RB_LITE=<mask>` (default 127): 19 presentation callees of the main loop (FUN
      0x7103747820..0x710374d820), only when called from it, each returning a fixed value. The
      groups are camera, UI, lighting, render layers, recorder, dynamic resolution and effect
      emitter tick. 13.0.4 addresses come from ssbu-recomp's `ring_simulation.c` and `runahead.c`;
      13.0.5 ones from its verified table; signatures from `recomp.py wrap-sig`.
  - **Results** (`skip1`: `SSBU_RB_RING=300,20,7,100`, input tape, test input):
    - 694 re-run frames, **all the same** as their first pass;
    - 2,975 kicks and 32,065 presentation calls left out;
    - **back 7 and caught up: 90 ms mean** (was 108), **11.5 ms a re-run frame** with its save
      (was 14.2): game work about 7 ms a frame (was about 10), save 4.3 ms.
  - **Next:**
    1. Profile a re-run frame to see where the 7 ms goes (the PR's SSBU_RB_SIM_PROF=3 samples the
       main thread). Suspects: the GPU submission of the previous recording; the vsync thread's
       `gpu_queue_wait`; the save's `gpu_queue_drain`; the safe point's census.
    2. A cheaper save in re-run frames.
- 2026-10-30: **The skips' green bug: lighting (group 4) is out of the default.** The user saw
  fighters and items (Steve's blocks, the CPU) draw flat, bright green from the first hitch on,
  for good. The fight hash stayed clean (it is fighter state, not the picture). Split by runs
  of 10 rollbacks (`SSBU_RB_RING=300,20,7,10`), with screenshots during and after:
  - `green1`, kicks only (49): colours right;
  - `green2`, presentation only (127): green during and after;
  - `green3`, lighting only (4: 0x7103576550, 0x7103576210, 0x7103576970, 0x71035941f0): green,
    and it saved nothing (13.3 ms a frame, as with kicks only).
  The default `SSBU_RB_LITE` is now **123** (all but lighting). `green4` (49 / 123, 30 rollbacks):
  colours right during and after, 204 re-run frames all the same. **8.8 ms a re-run frame**
  (save 3.5 ms), **back 7 and caught up: 68 ms mean (max 91)**. Faster than with 127 (11.5 ms):
  skipping lighting apparently cost time, maybe work the renderer redid for the flat-shaded
  draws. Not checked yet:
  - with the kicks skipped (`green1`, `green4`), a screenshot during the rollbacks showed rectangle
    outlines at the top left (the off-screen magnifier's frame?), gone after;
  - group 4's "persistent" damage suggests one of its functions also sets state the next frames
    use. Which of the four is unknown, since the whole group is simply left on.
- 2026-10-30: **Profile of a re-run frame.** Code: `hle_rb_core.c` (`fp_*`, `fp_report`: each re-run
  frame timed in parts), `src\debug_profile.c` (the runtime's sampling profiler, `profile_start_gated`:
  samples only while `rb_fast`, every 0.2 ms; `SSBU_RB_PROFILE=<s>`).
  - **The sampling profiler distorts:** it suspends every thread (about 100) at each sample.
    `prof1` ran slower (87.6 ms a catch-up) and gave the main thread only 425 stacks. Roughly:
    - our safe point and save were about 31% of its busy samples, mostly memcpy;
    - the game's task tree walker (FUN 0x7103725030, its logic step) about 40%;
    - the main thread spent 57% of samples in system calls (waits);
    - the vsync thread was 16% busy re-signalling in fast mode (it spins with SwitchToThread).
  - **The timed parts are exact** (`prof2`, no sampler, 40 rollbacks of 7, 274 re-run frames all the
    same). Means, lite re-run frames:

    | part | ms |
    |---|---|
    | game | 2.60 |
    | census wait | 0.01 |
    | save: hold + GPU drain | 0.28 |
    | save: memory | 2.25 |
    | save: the rest | 0.15 |
    | **the test's own fight hash** (`fight_frame`, about 20 getter calls; test only) | **5.57** |

    The last re-run frame (all its presentation work): game 2.20, census 0.56. A load 7 back takes
    7.35 ms.
  - **So rollback proper** is a load (7.4 ms for 7 back, fewer pages for fewer frames) plus **about
    5.3 ms a re-run frame** (game 2.6, save 2.7). That is about 36 ms for 7 back and about 13 ms for
    2 back. The fight hash made the test's catch-up figures (68–75 ms) about twice the real cost.
  - **Next:** time the catch-up without the hash (a switch to hash only the first and last re-run
    frames). The save's memory part (alias stamps 1.2–1.9 ms) and the load's undo copies are what's
    left to cut; then M5, a network test with two instances.
- 2026-10-30: **Rollback cost by depth, measured.** The ring test now hashes the fight only on normal
  frames and the last re-run frame (`SSBU_RB_HASHALL=1`: every frame), so the catch-up is timed
  without it. `SSBU_RB_RING_ROTATE=1` makes the loads go back 1, 2 .. <depth> frames in turn. `depths`
  (`SSBU_RB_RING=300,20,7,70`, rotate, input tape, test input): 69 checked ends of a catch-up, all the
  same.

  | back | load + catch-up, unseen (mean) | the load |
  |---|---|---|
  | 1 | **8.0 ms** | 3.2 |
  | 2 | **15.7 ms** | 4.2 |
  | 3 | **22.9 ms** | 5.0 |
  | 4 | 27.7 ms | 5.5 |
  | 5 | 33.1 ms | 6.4 |
  | 6 | 42.4 ms | 6.7 |
  | 7 | 47.0 ms | 8.5 |

  A re-run frame costs about 6–7 ms: game 3.4, save 3.1 (memory 2.4: half the renderer's
  write-watch stamps, half finding and copying about 3,000 pages with their undo). Every normal
  frame also pays a save (about 3–4 ms) in netplay. To fit a 2–3-frame rollback inside a 60 Hz
  frame, the save has to get much cheaper:
  - the stamps' GetWriteWatch scans whole alias spans each frame, about 1.2–1.9 ms;
  - the page copies and undo pushes are single-threaded;
  - the GPU-side work of the main loop still runs in re-run frames (only the PR's kicks and
    presentation calls are left out).
- 2026-10-30: **Alias stamps: a call per run of chunks stays (one big call is 10x slower); one
  unexplained mismatch.**
  - `stamps` (`SSBU_RB_STAMPS=0`, the two strategies on alternate saves): a call per run of
    consecutive alias chunks took **1.27 ms a save** (22 calls); one call from the first chunk to the
    last took **13.6 ms** (the range is some 60 GB). The cost goes with the range scanned, not the
    number of calls, and the committed alias area is about 2 GB, so tighter ranges won't save much.
    The default is per run (1); 2 remains a switch.
  - **In that run, one re-run frame was DIFFERENT:** logic frame 1228, generators differing, the
    first in about 3,000 checked so far. The next save's hold took 9.8 ms (about 1 ms usually).
    `recheck` (default stamps, `SSBU_RB_HASHALL=1`, 100 rollbacks at depths 1–7): **394 re-run frames,
    all the same**. Not reproduced: either the one-big-call mode (now off) or a rare race in the hold.
    Open: run longer, and if it recurs, look at which thread made the hold slow.
- 2026-10-30: **Page copies on several threads: loads faster, saves not.** Code: `hle_rb_state.c`
  (`CopyJob`, `job_add`, `jobs_run`, `undo_reserve`). A save, a load and each undo record first list
  their page copies (the undo slot is reserved, its content copied by the job), then
  `SSBU_RB_COPY_THREADS` host threads (default 4) and the main thread copy them. Batches under 256
  pages run on the main thread alone.
  - **Correct** (`par1`: `CHECKLOAD=2`, `HASHALL=1`, 50 rollbacks at depths 1–7): after every load and
    every undo record, 0 pages differ and 0 pages' presence differs; 197 re-run frames, all the same.
  - **Timing** (`par2`, same as `depths`): loads 4.3 ms mean (was 5.5); 1 back 2.3 ms, 7 back 6.5 ms
    (were 3.2 and 8.5). Back and caught up: 1 → **6.2 ms** (was 8.0), 7 → **41.8 ms** (was 47.0);
    the middle depths moved within the noise, both ways. **Saves did not get faster** (memory 2.53 ms,
    was 2.37). Their copying was already small; they go on the alias stamps (1.3 ms) and on finding
    the written pages.
  - **What a re-run frame costs now:** game 3.2 ms, save 3.5 ms (hold + GPU drain 0.8, memory 2.5).
    Below this, it comes down to fewer saves (e.g. not saving every re-run frame) or a cheaper way to
    learn which pages were written than the per-frame write-watch scan.
- 2026-10-30 (unattended): **Fewer saves: the frames of a catch-up need not be saved.** Code:
  `hle_rb_state.c` (`Slot.tag`, `rb_save_tag`, `rb_ring_find`), `hle_rb_core.c` (`SSBU_RB_RERUN_SAVE`,
  `pending_fast`).
  - **How it works.** States are found by their logic frame, not by how many saves back: `rb_save_tag`
    is set before each save, and `rb_ring_find(target)` returns the newest state at or before
    `target`. A load to frame F takes that state, and the catch-up runs every frame from it to now.
    With `SSBU_RB_RERUN_SAVE=0`, the re-run frames of a catch-up are not saved, except the last. A
    later rollback that lands in that stretch takes the state before it and re-runs more frames. The
    undo record of the state loaded then covers several frames, which works as it is (the shadow is
    not synced in between).
  - **Correct** (`nosave1`: `SSBU_RB_RING=300,5,7,60`, rotate, saves off, `CHECKLOAD=2`, `HASHALL=1`;
    rollbacks every 5 frames, so many land inside the previous catch-up): after every load and every
    undo record, 0 pages differ and 0 pages' presence differs. 375 re-run frames, all the same. At
    depths 4–7 the loads fell back to older states and re-ran 4.5–16 frames.
  - **Timing** (`nosave2`, set up as `par2`; all 69 checked ends the same):

    | back | saves on (`par2`) | saves off |
    |---|---|---|
    | 1 | 6.2 ms | 8.7 ms |
    | 2 | 20.2 ms | **7.8 ms** |
    | 3 | 21.9 ms | **9.8 ms** |
    | 4 | 26.3 ms | **13.0 ms** |
    | 5 | 35.8 ms | **14.8 ms** |
    | 6 | 41.9 ms | **14.8 ms** |
    | 7 | 41.8 ms | **14.6 ms** (7.8 frames re-run) |

    A lite re-run frame is now game work alone: **2.0 ms**. The catch-up figure stops at the last
    re-run frame's safe point, before its own save (about 3.3 ms), which happens in the same display
    frame. So rollbacks of up to about 7 frames fit a 16.7 ms frame, plus the normal frame's own
    work.
  - **The trade-off:** with frequent rollbacks (every 5 frames in `nosave1`), loads land in unsaved
    stretches and re-run more (up to 16 frames). A middle ground: save every Nth re-run frame. Not
    built; the default stays "save every frame" (`SSBU_RB_RERUN_SAVE` unset).
- 2026-10-30: **The middle ground: every 3rd re-run frame saved (the new default).**
  `SSBU_RB_RERUN_SAVE=<n>`: of a catch-up's re-run frames every n-th is saved, and the last
  (default **3**; 1: all, the old behaviour; 0: only the last). A load back into an unsaved stretch
  re-runs at most n - 1 frames more. Compared under frequent rollbacks (`SSBU_RB_RING=300,5,7,60`,
  rotate: one every 5 frames, depths 1–7; `n1`, `n3`, `n0`; each run 59 checked catch-up ends, all
  the same):

  | back | n=1 | **n=3** | n=0 |
  |---|---|---|---|
  | 1 | 4.6 ms | **4.3** | 10.9 |
  | 2 | 17.5 | **10.2** | 13.2 |
  | 3 | 21.9 | **12.7** | 15.1 |
  | 4 | 32.4 | **17.9** | 14.1 |
  | 5 | 33.4 | **21.6** | 18.8 |
  | 6 | 33.0 | **22.5** | 23.1 |
  | 7 | 39.5 | **28.0** (9.0 frames re-run) | 26.3 (16.0 re-run) |

  A re-run frame costs about 2 ms of game work against about 3.5 ms for a save, so re-running a
  few more frames is cheaper than saving them. n=3 keeps nearly all of n=0's gain while bounding
  how far back a load can fall. Under rare rollbacks (every 20 frames, `nosave2`) n=0 reached
  7.8 ms for 2 back and 14.6 for 7; n=3 should be close to that. Not measured there yet.
- 2026-10-30: **The render kick can't be skipped; the vsync thread no longer spins in a catch-up.**
  - **`SSBU_RB_SKIP` bit 2 (the render list's kick, 0x710374c688, with the PR's flag byte at
    [0x71053375a8] + 0x140) crashes.** `rend_crash`: in the first rollbacks a task worker made an
    indirect call to 0x608746d390 from the job runner (main+0x35487f0); report
    `reports\stopped_20261030_014558.txt`. Left out of the default (49); marked in the code.
  - **The vsync thread in fast mode.** With nothing queued it signalled the vsync event in a
    SwitchToThread spin: thousands of signals, each through the kernel's lock. The game's vsync
    count thread then reported 2,598 and 5,963 "dropped refreshes" in one catch-up (cleared, but
    wasted). Now (`src\ipc_vi.c`, condition variable `queue.queued`, woken by QUEUE_BUFFER) it waits
    for the game's next frame, up to 2 ms, before signalling one refresh. The safe point's census
    (M1's measure) no longer waits in catch-up frames either.
  - **Results** (`vsync`, set up as `n3`: rollbacks every 5 frames, depths 1–7; 59 checked ends,
    all the same): dropped refreshes are 1–3 again; **game work in a lite re-run frame 1.99 ms**
    (was 2.43). Back and caught up:

    | back | `n3` | `vsync` |
    |---|---|---|
    | 1 | 4.3 | 4.3 |
    | 2 | 10.2 | **9.7** |
    | 3 | 12.7 | **11.2** |
    | 4 | 17.9 | **16.3** |
    | 5 | 21.6 | **19.4** |
    | 6 | 22.5 | **20.0** |
    | 7 | 28.0 | **20.1** (9 frames re-run) |

  - **Where the time goes now:**
    - a re-run frame is about 2 ms of game work; a saved one adds about 2.5 ms (memory 2.15, hold
      and drain 0.34);
    - a load is 2–4 ms: the alias stamps (about 1.2 ms) and the undo records;
    - so up to 3 frames back fits in 16.7 ms with room left; 4–7 back takes 16–20 ms.
- 2026-10-30 (autonomous session, stopped early: the monitors went to sleep, so the menus could not
  be driven). **Re-run frames don't wait: their 2 ms is the game computing.** Timers added:
  `src\kernel.c` kwait (`rb_main_wait_ns`: the guest main thread asleep while `rb_fast`) and
  `src\ipc_vi.c` (`rb_vsync_gpu_wait_qpc`: the vsync thread's `gpu_queue_wait` in fast mode), reported
  by `fp_waits_report`. `waits` (set up as `vsync`; 59 checked ends, all the same): the main thread
  slept **0.03 ms a re-run frame**, and the vsync thread waited for the GPU 0.07 ms a frame. The game
  work (2.3 ms here; this run was slower than `vsync` throughout, e.g. back 7 30.9 ms against 20.1,
  maybe the power state with the monitors going off) is the main thread's own compute: the logic step
  plus what's left of the presentation side.
  - **Next session, in order:**
    1. Load: skip the second hold and the memory sync when the load comes right after a save at the
       same safe point (the ring test's case, and netplay's: save the current frame, then roll
       back). Nothing runs between them if the threads stay held; that saves the hold (about
       0.3–1 ms) and the alias stamps (about 1.2 ms) of the load.
    2. Look for more presentation-side callees of the main loop to leave out. The PR's profile 2
       tooling (task tree walker 0x7103725030, by node vtable) can show which steps of the logic
       tree are presentation. Check each with screenshots and the fight hash.
    3. Re-measure `vsync` and `waits` back to back to settle the run-to-run spread.
- 2026-10-30 (autonomous): **A load straight after its save keeps the hold and skips the sync.** Code:
  `hle_rb_state.c` (`rb_keep_hold_next`, `rb_release_held`, `memory_untouched`), `hle_rb_core.c` (the ring
  test asks for it when a load will follow; `SSBU_RB_KEEPHOLD=0`: off). A rollback saves the frame it is
  in and loads an older one at once. Now the save leaves every guest thread held, and the load takes
  them as they are: no second hold, and no memory sync, since nothing ran in between.
  `rb_release_held` lets them go when no load follows, and also clears `memory_untouched` after a
  refused load.
  - `keephold1` (`CHECKLOAD=2`, `HASHALL=1`, rollbacks every 5 frames): all 50 loads came straight
    after their save; **0 pages changed in the gap** (checked), 0 after the undo records, 225 re-run
    frames all the same.
  - `keephold2` (set up as `vsync`): **loads 1.86 ms mean** (`vsync` 3.08); 1 back 0.7 ms (2.1). Back and
    caught up: 1 → 3.6, 2 → 11.3, 3 → 11.5, 4 → 14.6, 5 → 22.7, 6 → 20.6, 7 → 22.6 ms; 59 checked ends,
    all the same.
  - **The catch-up totals are noisy between runs** (game work per re-run frame 1.99–2.31 ms). Each run
    gets a random stage, and stages differ in cost. Comparisons finer than about 2 ms need a fixed
    stage: next session, set the 5-stock ruleset's stage to Battlefield (or another fixed one).
- 2026-10-30 (autonomous, cont.): **Where a re-run frame's 2 ms goes, and a fix built but NOT
  TESTED.** Stopped when the screenshots went black (monitors off) and the menus could no longer be
  driven.
  - **Task tree** (`SSBU_RB_TREE=1`, `HLE_WRAP(7103725030)`, the logic step's tree walker, 13.0.4
    0x7103724a80; `tree`): only 2 node classes show (the walker does not recurse through itself). In
    all it is about **0.7 ms a re-run frame** of the 2.07 ms of game work.
  - **By part** (`seg_report`; marks at the safe point, player 1's pad read and the frame end wrap;
    `parts`, 263 re-run frames):

    | part | ms |
    |---|---|
    | safe point → pad poll | 0.20 |
    | pad poll → frame end (the logic step) | 1.09 |
    | **the frame end call (FUN 0x71038701e0)** | **0.89** |
    | frame end → safe point | 0.02 |

    The frame end acquires the next display buffer. In a catch-up it waits for the vsync thread's
    round trip (queued frame → wake → GPU wait → free → signal).
  - **Built, not yet run** (`src\ipc_vi.c` transact QUEUE_BUFFER, `SSBU_RB_QFREE=0` turns it off): a
    frame queued while `rb_fast > 0` is freed at once (the slot and any ACQUIRED one), and buffer_event,
    `freed` and vsync_event are signalled after the queue's lock is released. The vsync thread is not
    involved. Expected: most of the 0.89 ms gone. **First thing next session:** run it as `parts` (ring
    300,5,7,60, rotate), with screenshots during and after the rollbacks (the picture) and the hash
    (the fight). If it misbehaves, `SSBU_RB_QFREE=0` returns to the vsync thread path.
  - Usage at the stop: 5-hour limit about 80%.

### 2026-10-30 morning: SSBU_RB_QFREE tested, it works

`play_rollback_qfree1.log` (ring 300,5,7,60, rotate; stage fixed from here on, Final Destination or Battlefield): 59 re-run checks the same, 0 DIFFERENT. The frame end call in re-run frames went from 0.89 ms to **0.28 ms**; logic 0.91 (was 1.09); the vsync thread no longer waits for the GPU in re-run frames (0 waits). Back 1 + catch-up 2.0 ms (was 3.8), back 2 5.6 (9.3), back 3 7.5 (10.3), back 7 18.9 (22.7). One save of 452 ms (an outlier, not yet looked at). QFREE stays on by default.

### 2026-10-30 late morning: narrower write-watch resets (SSBU_RB_WGAP), built, NOT TESTED in a fight

Where a normal save's memory time goes (qfree1, SAVE 154): 2.3 ms = alias stamps 1.4 + alias pages 0.7 + watched 0.2. The stamps are `watch_take_locked` (src\gpu_d3d11.c): after a cheap "anything written?" query, `GetWriteWatch(WRITE_WATCH_FLAG_RESET)` ran from the first written page to the end of the range, and resetting costs per page walked even where nothing was written (some 1.5 GB of alias area a save, 27 calls). Now: list the written pages without resetting, then reset only spans around them (pages at most SSBU_RB_WGAP apart, default 32, form one span). A write after the list and outside the spans keeps its bit: news next time, so nothing is lost. `SSBU_RB_WGAP=0` restores the old path. The renderer's own watch_take calls go through the same code, so normal frames may gain too ("ms watching writes" in the status lines).

**To test:** ring 300,5,7,60 with rotate, as qfree1; compare "alias stamps, a call per run of chunks: X ms a save" (qfree1: 1.380) and the SAVE lines' "alias stamps" part, plus 0 DIFFERENT.

Unattended menus: the PC was locked (LockApp), so computer use couldn't reach the game. Blind scripting with `SSBU_INPUT` (wall-time presses) plus `SSBU_CAPTURE` (the game saves its own frames as BMPs) gets from boot to the ruleset screen: A at about 34, 40 and 46 s. But the ruleset screen ignores scripted A and + (tried at 52–66 s, held 0.2–1.0 s), although the scripted stick moves its cursor there. Unsolved; a fight still needs the user or an unlocked screen.

Result (`play_rollback_wgap1.log`, Final Destination): SLOWER. Alias stamps 2.92 ms a save (qfree1: 1.38); 0 DIFFERENT. The cost of GetWriteWatch is scanning the range, not resetting it: the old path's first query stops at the first written page (count 1), and the list query scans the whole range again. Default now SSBU_RB_WGAP=0 (old path), rebuilt. Same run otherwise: frame end call in re-run frames 0.17 ms, logic 1.00; back 1 + catch-up 2.2 ms, back 3 7.4, back 7 20.7. Next idea for the stamps: scan less (only chunks with present pages the game writes; or keep the renderer's own answers and ask only for ranges no one asked since).

### Parallel alias stamps (SSBU_RB_STAMPS=3): SLOWER, kept as an option only

`play_rollback_stamps3.log` (STAMPS=0 now alternates modes 1 and 3 save by save, same fight, Final Destination): mode 1 (a call per run of chunks, one thread) 1.44 ms a save; mode 3 (runs cut to 32 MB, on the 4 copy threads too, under the renderer's lock) 1.91 ms. 0 DIFFERENT. GetWriteWatch calls on one reservation don't run side by side (the address space lock, presumably), and more, smaller calls cost more. With WGAP (rescanning) also slower, the alias stamps look like a floor for GetWriteWatch: about 1.4 ms a save for some 1.5 GB scanned. Getting under it means scanning less (knowing which alias chunks the game writes in fights without asking Windows, e.g. page protection or the renderer's own knowledge), which is a bigger design change. Default stays STAMPS=1.

## M5 started (2026-10-30): the netcode's own part, tested alone

Save cost left as it is (the alias stamps are a GetWriteWatch floor, above); a rollback 1 back plus catch-up is 2–3 ms, 3 back 7–8 ms, 7 back about 20 ms, against a 16.7 ms frame. So on to M5.

**Step 1, done: `src\rb_net.c` / `rb_net.h`** (plain C, no Windows, no game; written here after the PR's design, its `rollback.h`, `input_history.c`, `clock_sync.c`, `wire_format.c`, no code taken). Pads by logic frame (the counter the input tape uses) with the GC triggers too: local pads kept `delay` frames ahead; remote pads from the network, a frame run before its pad came runs on the last pad held, and a real pad that contradicts that guess is a mismatch (the frame to roll back to). `rbn_can_run` makes a game wait rather than guess further back than 7 frames. Wire: one PADS message carrying every unacknowledged pad (lost messages need no resend), the ack, the sender's frame and frame advantage, and its fight hash at its newest confirmed frame (desyncs are detected, which the PR never did). Time sync: GGPO's frame advantage (the game further ahead waits half the difference, one frame in four).

**Test: `tests\run_rb_net_test.bat`** (clang-cl from deps, builds `build\rb_net_test.exe`). Two peers, a toy fight hashed from both pads, a link losing 1 in 10 messages and delaying them 1–6 frames (so out of order), peer B starting 7 frames late; 4000 frames at input delays 0, 2, 4. All PASS: both peers' confirmed states equal a run with every pad known on every frame, the hashes exchanged all agree, the deepest rollback is 7 (delay 0), 5–6 (delay 2), 3–5 (delay 4); waits 0–5. With rollbacks turned off (`rb_net_test 2 norollback`) it FAILS at frame 3, as it should. (First version: both sides waited about 150 times at delay 4, because the advantage a side sent and the one it measured were estimated differently; fixed in the test, same estimate both ways.)

**Next steps:**
- **Step 2, loop mode in the game** (one PC, one game): port 2's pads come from a bot through rb_net with a fake link (late, lossy), and the game rolls back on real mismatches with `rb_load_age` + re-run (what the ring test does on a timer now). Needs port 2 to be a player, not a CPU, in the fight: a second controller or the keyboard as P2 at character select. Done when a 5-stock fight runs with the fight hash agreeing between the rolled-back run and a run with the bot's pads known in advance.
- **Step 3, two games on one PC** over UDP on loopback (`net_relay.c`'s transport): the same start. The PR's handshake (`services\handshake.h`) sends both players' characters, colours, control layouts, the stage entry, items, stocks, a seed, and the host's stage set-up history (`0x71050c5ad0` in 13.0.4; without it, stage variation differed and the games desynced at frame 7). Every address needs mapping to 13.0.5 (`hostrt.c` `g_code_13_0_5`).
- Then two PCs (M6: a second PC).

### M5 step 2: loop mode in the game (2026-10-30 midday)

`SSBU_RB_NET=loop[,<delay>,<lag min>,<lag max>,<loss %>,<frames>]` (hle_rb_core.c `net_step`): both players are bots by logic frame (`net_pad`), player 2's pads reach the game through hle_rb_net.c (the netcode's files are named `hle_rb_net.*` because the runtime build compiles only `host|kernel|gpu*|hle*|ipc*|debug*|net*` .c files, tools/recomp.py), late by <lag> of this game's own frames (not wall time: on wall time a hitch at the fight's start put the game behind for good and nothing was ever guessed, loop1). A wrong guess loads the newest state at or before the frame before it (`rb_ring_find(m - 1)`) and runs the frames since again, fast. `<lag max>` -1: every pad known (the reference). Each frame's fight hash once final goes to `build\rb_loop_<lag max>.txt`. Port 2 must be a human at character select: the user uses an Xbox pad (its rumble is off: `[device xboxone] rumble = 0` added to controllers.ini) besides the GameCube one; join GameCube first (Steve), then the Xbox pad.

Results:
- **Mechanics work** (loop2, seed5): 30 s of fight with lag 1–6 frames and 10 % loss: 123 rollbacks (depths 2–7), 430 frames run again, catch-up 11–13 ms mean (max 30–35), 6 waits for pads, none too deep for the ring, no crash.
- Bugs fixed on the way: wall-time arrival (above); with every pad known, deliveries ran 128 frames ahead each frame and overran rb_net's 256-frame ring, so player 2 got empty pads and Steve stood still (seed2): now at most 64 ahead; the fight start is detected by fight number (S.frames == 1 came twice once).
- **Two runs are not the same fight yet, so the rollback runs can't be checked against a reference.** Without seeding they differ at frame 0 (the script random generators). `SSBU_RB_SEED=<n>` sets the 9 generators at the fight's first safe point: frame 0 then matches. But (1) the starting sides are random and chosen before the fight (Steve left in seed3/seed5, right in seed4/seed6), and (2) two references on the same side (seed4, seed6) still differ from frame 1: at fight frame 2 one run has made one random call more (generators "1 calls" vs "0 calls"), the fighters otherwise identical. The rollback run (seed5) differs from its same-side reference in the same way, so its mismatch says nothing about rollback.
- **Next:** find that call: an HLE_WRAP on app::sv_math::rand (or the generator's caller) logging the caller and logic frame in the first fight frames of two runs; suspects are something wall-time dependent at the fight's start, or pads read before loop mode takes over (the first reads of a fight, before its first safe point, are live). Then the starting sides (step 3's handshake: the stage set-up and seeds before the match). Only then compare a rollback run with a reference.

**Step 2 DONE (2026-10-30 afternoon): a rollback run gives the same fight as one with every pad known.**
- The fight itself repeats across runs (given the seed and the same starting sides): four references on the same side (seed6, dbg1–3) had both fighters identical at every once-a-second sample for 30 s. What differed was **script random generator 1**: something outside the fight (on Final Destination; its background, presumably) draws from it about every other frame plus one in 20, starting at a different frame each run (frame 2, 11, or not by frame 8), and not through app::sv_math::rand / randf (wrapped, `[rb] draw` lines: none). The loop-mode fight hash now leaves generator 1 out (fighters + generators 0, 2–8).
- **lag9 (lag 1–6, 10 % lost; 123 rollbacks, depths 2–7, catch-up 13.5 ms mean, max 37) against ref7 (every pad known), same starting side: 1086 final frames compared, 0 differ.** lag8 (the other starting side) differs on every frame, as it must.
- A trap on the way: the comparisons used `join` on sorted text, which paired the wrong lines (it said "1086 differ, first at 1" while frames 0–3 were plainly equal). Compare by frame number (awk keyed on column 1, `\r` stripped): `build\rb_*.txt` are written with CRLF.
- Still open for two PCs: the starting sides (random, chosen before the fight: the handshake must agree on them, or on the seed that picks them), and generator 1's start if anything in a fight ever reads it.

**Next: step 3**, two games on one PC over UDP (net_relay.c's transport): both run loop mode's machinery with the other game's real pads, exchanging rb_net PADS messages, with the fight hash in each message compared (desync detection).

### M5 step 3: two games on one PC (2026-10-30 afternoon) — IN SYNC

`SSBU_RB_NET=net,<0|1>,<my port>,<peer port>[,<delay>,<frames>,<host>]` (hle_rb_core.c net mode; transport `src\hle_rb_link.c`, UDP, non-blocking): two instances (`net,0,7701,7702` and `net,1,7702,7701`, `SSBU_WINDOW=windowed`, `SSBU_WINDOW_SIZE=1280x720`, same `SSBU_RB_SEED`), each game's bot drives its own player and its pads go to the other in PADS messages every normal frame; hashes of final frames ride along and are compared both on the comparing side's own schedule and on arrival. Hash files `build\rb_net_<0|1>.txt`.

Setup on one PC (learned the hard way):
- The GameCube adapter (WinUSB) opens in ONE process only: the second game never sees it. controllers.ini `[players]` now pins `2 = xboxone` and `3 = gamecube` (added for these tests, remove to go back), so player 1 is the keyboard in both games and player 2 the Xbox pad (which reaches both windows at once: background input).
- Player 1 must be the same controller TYPE in both games: with a GameCube pad as P1 in one and the keyboard (a Pro Controller to the game) in the other, the same pad gave Steve different actions (net2, desync at frame 7).
- Players join in the same order in both (keyboard first, then the Xbox pad): the game numbers them by join order (net1).
- The starting sides are still random per game: about half the starts mismatch, and must be retried (step 3b, the handshake).

Results:
- net3: in sync to frame 87, then "different" at 88 on one side only (9 of 207). Cause: the hashes of a pass about to be rolled back were compared and sent as final in the same step that found the wrong guess. Fixed: net_receive drops the hashes after a wrong guess at once.
- **net4: 30 s, both games IN SYNC: 138 and 248 hashes compared, 0 different; the hash files agree on all 31 frames both kept.**
- But one game (player 2's) got only 928 of the ~2000 messages the other sent: Windows' default UDP receive buffer overflowed while it was busy running frames again; so it waited 27 s in all (534 waits), rolled back 136 times nearly always 7–9 deep, and its HUD showed "GO!" stuck (re-run frames leave out the presentation work that animates it). Fixed (not yet tested): SO_RCVBUF 1 MB.
- **net5 (SO_RCVBUF 1 MB): 30 s, IN SYNC, and smoother.** Hashes compared: player 1's game 1362, player 2's 1773, 0 different; the two hash files agree on all 1352 frames both kept. Player 2's game got every message (2556) and rolled back 11 times (depth 2), waited 0 s; player 1's 41 rollbacks (26 of them 7 deep), waited 6 s in all, 45 frames held back. Two whole games share one CPU and GPU here, so one lags at times; on two PCs each has its own. The HUD's "GO!" lingered a little in one window and then went (re-run frames leave its animation out): polish (M6).
- Backgrounds differ between the two windows (each animates its own, generator 1): cosmetic, the fight hashes agree.

**Next:** step 3b, the handshake: the starting sides (and the seed, instead of SSBU_RB_SEED) agreed before the match, so a start never mismatches (about half did); then real players instead of bots (each game's own pad as its player); then two PCs (M6).

### M5 step 3b: the same start on both games — DONE (2026-10-30 evening)

The starting sides (and everything else random in the match set-up) come from the game's **global xorshift128 at 0x71052391c0** (13.0.5; 13.0.4's 0x71052381c0 one page on, as hostrt.c `ga()` maps all of main's data): the match set-up (13.0.4 0x71014d8620) stores its next value in the match block and the fight's random set-up and script generators come from it (found by ssbu-recomp PR #116's handshake, debug_mode.c). The menus advance it, so two games started differently about half the time. In net mode with `SSBU_RB_SEED`, it is now set from the seed at every safe point outside fights (hle_rb_core.c, savestates): the log shows it different in the two games at boot and equal from then on.
- side1, side2 (with the in-fight seeding too) and side3 (`SSBU_RB_SEED_FIGHT=0`: the match seed alone): **three starts in a row on the same side in both games, all three IN SYNC** (side3: 510 / 2040 hashes, 0 different; files agree on 511 frames). At fight frame 1 of side3 both games' script generators already held the same 119 draws and state (`4f978f12e5c14b2e`): the match seed alone makes the start the same. Final Destination's background also runs alike in both windows now.
- So the in-fight seeding (`SSBU_RB_SEED` at the first fight safe point) is not needed in net mode; a handshake only has to agree on the seed for the global generator before the match.
- Performance on one PC varies run to run (side3: one game waited 35 s for the other in all; side1: 11 s; net5: 6 s): two whole games share one CPU and GPU, and the unfocused window falls behind. Not a correctness question.

**Next:** real players instead of bots (each game's own pad as its player, through rbn_local), a handshake for the seed (and the characters / stage), then two PCs.

### M5 step 4: real players — IN SYNC (2026-10-30 evening)

`SSBU_RB_NET_LIVE=1` (net mode): each game's own player is played with its controllers: the newest live read of its slot (hle_hid.c's pad + keyboard, before the tape) becomes its pad for the frame `delay` on and goes to the peer, as the bot's did; the other player's slot ignores this game's controllers (the Xbox pad reaches both windows), only the peer's pads count. Before the fight's first safe point both games hold an empty pad.
- live1 (one PC: player 1 on the keyboard in window A, focused; player 2 on the Xbox pad in window B): **60 s played by hand, IN SYNC: 1856 and 2454 hashes compared, 0 different; the hash files agree on all 1869 frames both kept.**
- Window B (in the background) ran slower than A, so A was always ahead: A rolled back 196 times (mostly 7–9 deep) and waited 47 s in all for B's pads, while B never waited or rolled back. Two games on one PC, the unfocused one slowed: on two PCs each game is in the foreground.

**Next:** a handshake (the seed, characters and stage agreed between the games instead of set by hand in both windows), then two PCs (M6: a second PC). Before that, a look at why the background window runs slow (its pacing) would make one-PC tests closer to real play.

### M5 step 5: the seed handshake — works (2026-10-30 evening)

HELLO messages (hle_rb_net: type 2 | player | version | agreed | seed u64, 16 bytes; round trip checked in rb_net_test) every 10 safe points in the menus (hle_rb_core.c `net_hello`): player 1's game proposes the match seed (`SSBU_RB_SEED` if set, for tests that must repeat; else from the clock and process id), player 2's takes it and answers with it; once agreed, both set the global generator from it outside fights (step 3b). Other messages heard in the menus are dropped (pads come again in every message). The fight's start says whether the seed was agreed; a desync on the fight's first frames says the two games did not start the same match (characters, colours, stage, rules or seed).
- hs1 (no SSBU_RB_SEED; bots): seed `f3173dcc44a240d6` proposed by player 1's game and taken by player 2's; the generator equal in both after; the same start; **IN SYNC** (241 / 121 hashes, 0 different; files agree on 91 frames).
- Still by hand in both windows: characters, colours, stage, rules. Writing them into the player set-up (the PR's FN_PLAYER_RECORD 13.0.4 0x7101788260 and the panel states, debug_mode.c) is a later step.

**HUD on re-run-heavy games (polish, M6):** the game that rolls back most showed "GO!" lingering and the stock counter ("4 - 5" after a KO) jittery and stuck while the fight itself was the same in both windows (17 % on both, screenshot). Every load puts the HUD's animation back too, and the re-run frames leave out the presentation work (the lite wraps, M4) that seems to advance it. To check: `SSBU_RB_LITE=0` (no lite skips) should make it behave; then find the one wrap that animates the HUD and stop skipping it.

### HUD fixed; cheaper sync check (2026-10-30 evening)

- **HUD:** ui1 (`SSBU_RB_LITE=121`: the UI group, 2, no longer left out of re-run frames): "GO!" and the stock counter now only last a little longer in the slower window and go away smoothly (user), instead of jittering and sticking; in sync (~2300 hashes, 0 different). **The default LITE is now 121** (all lite groups but 2, UI, and 4, lighting).
- **`SSBU_RB_NET_CHECK_EVERY=<n>`, default 10:** the fight hash (some 5 ms of getter calls) only on logic frames that are multiples of n, the same frames in both games. With a hash every frame the test window played in slow motion.
- live2 (live controllers, both defaults): "felt decent, a little slow at first; HUD behaving well" (user). In sync: the hash files agree on all 359 frames both kept (60 s at one in 10); rollbacks 0 and 3 (depth 2); player 1's game waited 11.5 s for the other, mostly at the start. (Its "3523 hashes compared" counted the peer's repeats of one frame each time it came: now once.)

### Two PCs: the kit (2026-10-30 evening)

- `launchers\Rollback Netplay.bat`: asks player 1 or 2 and the other PC's address; points `dump\modules\romfs*.cfg` (full paths) at its own folder; runs net mode with live controllers, delay 2, 7200 frames checked, and `SSBU_CONTROLLERS` set to the player's own settings file.
- `controllers_rollback_p1.ini` / `_p2.ini`: every controller as a Pro Controller (`style = pro`: both PCs must give the game the same kind in each slot); the player's own pad in its slot, the keyboard in the other slot only to join it at character select. `_onepc.ini`: the one-PC test setup (keyboard P1 in both windows, Xbox pad P2, GameCube pad P3). **controllers.ini is back as it was** (the test pins removed; its old copy in `build\controllers_before_restore.ini`).
- `patches\rollback\pack_for_other_pc.bat`: copies what the build needs to run (dump 21 GB, the exe and SDL3.dll, save, settings without display.ini's gpu line, the launcher, the docs) to a folder; the other PC needs no build. The exe needs only Windows' own DLLs and SDL3.dll (checked); the build's image files and nro objects are build-time only.
- `patches\rollback\TWO_PC_SETUP.md`: the steps for the other PC.
- Not yet tested on two PCs.

### Two PCs, first runs (2026-10-30)

- twopc1: player 2's slot picked with the wrong controller on the other PC (user error): desync at frame 700.
- twopc2: seed agreed (`60f5a1c80bfa4690`) but the starting sides differed between the PCs: desync at the first check (frame 10). Setting the global generator at every menu safe point is not enough across two PCs: a slower PC's menus run extra logic frames between safe points (dropped refreshes are cleared only in fights), and they can draw. Now it is also set as player 1's record is built (HLE_WRAP 0x71017880c0, the PR's FN_PLAYER_RECORD 0x7101788260 mapped by its table), right before the match set-up draws, as the PR's handshake does. The start-check message now goes with the first comparison (with one check in 10 frames, frame 10).
- twopc3 (name tags off, the record hook in): the seed agreed, the generator equal at the set-up on both, the same starting sides; still a desync at the first check. PC 2's log: Kazuya's wait motion at frame 64 against 63 on PC 1, and Steve already on another motion with one random draw more: the two games began the fight a frame apart. The entrance and countdown before "GO" run on logic frames, but dropped refreshes were cleared only in fights, so the slower PC (GTX 1660 Ti) ran a catch-up frame there. Now in net mode they are cleared outside fights too, and from a match set-up until the fight players 1 and 2 hold nothing (each PC would read its own, different controllers in both slots during the countdown).
- twopc4 (no catch-up frames outside fights, idle pads in the countdown): still apart, and PC 2 the same in all three runs: at the fight's first frame Steve 5 frames into his wait motion against 95 on PC 1, Kazuya 64 against 63, two draws more. Not random: the fighters' entrances begin once their files are loaded (checked each frame), so the frame they began on follows each PC's disk; on one PC both games loaded alike. Trying: in net mode, from the match set-up until the fight, each frame waits for the game's other threads to be at a wait (loading done), up to `SSBU_RB_NET_SETTLE_MS` (500); the log counts the frames settled and timed out.
- twopc5 (settle wait): every pre-fight frame settled on both PCs, none timed out, and the start still differed the same way (Steve 5 against 95, Kazuya 64 against 63; the cosmetic generator 1 at 110 against 112 draws): not loading. Next suspect: real time in the entrance and countdown (the fixed clock started only with the fight). Now in net mode `rb_clock_frame` runs fixed from the match set-up on.

### CHECKPOINT (2026-10-30, 14:50) — two PCs

- **Both PCs run the same build (14:36)**: build\ssbu_rollback.exe and .map copied to PC 2 (192.168.0.x, GTX 1660 Ti) through the pack folder shared from this PC.
- **The real cause of every two-PC desync so far: the lobbies differed**, not the build. PC 2 picked the fighters with swapped devices (its Xbox pad picked Steve, its keyboard Kazuya), so PC 2 ran Kazuya as P1 and Steve as P2 against this PC's Steve P1 / Kazuya P2. The "different entrance timing" read in twopc3-5 (P1 on another motion at the first frame, the same every run) was that different fighter. In twopc6 (14:46) the first frame was identical on both PCs (same motions, frames, positions); the desync came a few frames later, with the fighters swapped.
- The rule for the next run: **each PC's controller picks its own player's fighter, the keyboard the other one; both lobbies P1 = Steve, P2 = Kazuya.** This PC: GameCube pad → Steve (P1), keyboard → Kazuya (P2). PC 2: keyboard → Steve (P1), Xbox pad → Kazuya (P2).
- Changes made while chasing the wrong cause (twopc2-5), all net-mode only and kept for now, but NOT shown to be needed: the global generator also set at player 1's record build (HLE_WRAP 0x71017880c0); dropped refreshes cleared outside fights; idle pads from the match set-up to the fight; the pre-fight settle wait (`SSBU_RB_NET_SETTLE_MS`, 500); the fixed clock from the match set-up. To re-check one by one once a two-PC run syncs (the settle wait makes the countdown stutter).
- A better fix for the confusion itself: the character/stage handshake (each PC sets both slots from the agreed match), so the lobbies can't differ.
- **twopc4-7: the seed was never agreed** ("the match seed was NOT agreed" on both PCs; twopc2-3 agreed). The handshake ran only in the menus, and from twopc4 on one PC was set up and started first, the other a room away later, so the first was already past its menus. Fixed: at the match set-up (player 1's record) each game waits until the seed is agreed (up to 10 min, hellos both ways), player 2 sends its answer five times when it agrees and answers hellos in the fight too. twopc7's lobby was right (user: PC 1 GameCube Steve / keyboard Kazuya, PC 2 keyboard Steve / Xbox Kazuya); its starting sides differed because of the seed.

### **TWO PCs IN SYNC (2026-10-30, twopc8, build 16:08)**

PC 1 (this PC, GameCube pad, Steve) and PC 2 (192.168.0.x, GTX 1660 Ti, Xbox pad, Kazuya), on the home network: the seed agreed after PC 1 waited 81 s at its match set-up for PC 2; the same spawns (Steve left, Kazuya right), and damage and positions after hits the same on both screens (user); **no desync over ~95 s** (about 5700 logic frames, a check every 10); both logs end with the same state (Steve on the respawn platform at 0,50; Kazuya at 8.1,0). PC 2's save was copied from this PC's again before the run (the ruleset had been changed and set back).

What made it work, in order of importance: the lobbies wired the same (each PC's own pad on its own player's fighter, the keyboard on the other: both lobbies P1 Steve, P2 Kazuya); the seed agreed before the set-up (the wait at player 1's record); the same save. Unproven extras still in (twopc2-5): the seed also set at the record build, dropped refreshes cleared outside fights, idle pads in the countdown, the pre-fight settle wait, the fixed clock from the set-up.

**Next:** the character/stage handshake (each game wires both slots from the agreed match, so the lobbies can't differ and no keyboard stand-in is needed); then re-check the extras one by one; longer matches, a whole 5-stock game.

### Crashes in long matches (2026-10-30 evening)
- twopc9 (two PCs, the full-match try): PC 1's game crashed about 2 min in (logic frame ~7790), just after a load 2 back, Steve falling below the stage (a KO): host exception, null read in guest thread 12 (a task worker), guest main+0x3954060 <- 392bae0 <- 392ba20 <- 392e4d0 <- 392eb40 <- 3815ca0 <- 381f3b0 <- 38136d0 <- 3816730 <- 35a4400 (the job runner); reports\crash_20261030_162106. PC 2 then waited for pads (its music still playing).
- stress1 (one PC, loop mode, bots, lag 1-6, 10 % lost): crashed about 4 min in (logic frame ~13860): null + 0x190 on the guest MAIN thread, main+0x36d51b0 <- 49cf50 <- 48da00 <- 48f4b0 <- 3e87a0 <- 468220; reports\crash_20261030_162928. Two different places: something left stale by the loads. The tests before ran 30 s to a few minutes. First suspect: the re-run frames' skips (kick mask 49, lite 121; bit 2 already crashed with a stale job pointer). Test: SKIP=0 LITE=0.
- stress2 (SKIP=0 LITE=0): two whole games (~16 000 logic frames of rollbacks), no crash. stress3 (heavy: lag 4-9, 30 % lost; SKIP 49): crashed at the winner screen, null read in guest thread 3 in the job system (main+0x35c4580 <- 354a6d0 <- 353d4f0 <- 37cca70; 0x354a6d0 is next to the kick function 0x3549720). stress4 (the same, SKIP=0, LITE 121): **the very same fight** (bots by logic frame: 6359 frames both times) finished and went to the results without a crash. **Cause: the job-group kicks left out of re-run frames (SKIP 49). Default SKIP is now 0**; the lite (presentation) skips stay (121). Re-run frames cost more; to measure.

### **A FULL MATCH ON TWO PCs, IN SYNC (2026-10-30, twopc10, build 16:43)**

A whole 5-stock game (Steve on PC 1's GameCube pad, Kazuya on PC 2's Xbox pad) to the results screen: **no desync, no crash.** PC 1: 629 hashes compared, 0 different; 1258 rollbacks (depths 2-9, 5248 frames run again, catch-up 12.2 ms mean, max 38.2), 18.6 s waited for pads in all, 696 frames held back for the frame advantage. PC 2 (the slower PC): 1275 hashes compared, 0 different; 0 rollbacks (it ran a little behind, so PC 1's pads were always there); 11.5 s waited once at the start (the seed handshake: 58 s at its set-up for PC 1). The user: "it went well".

M5's core is done. Next: the character/stage handshake (no keyboard stand-ins, lobbies that can't differ); re-check the unproven extras (record-build seed, drops outside fights, idle countdown pads, the settle wait, the early fixed clock); input delay and the time sync tuned for the slower PC; the cost of re-run frames with the kicks in (SKIP 0).

### M5 step 6: the match's set-up agreed (CHOICE), built 16:56, not yet tested

CHOICE messages (hle_rb_net: type 3, 48 bytes; round trip checked in rb_net_test): at the player record builder (HLE_WRAP 0x71017880c0, x0 the panel state, x1 the index), each game reads its own player's ui_chara (+ 0x88) and colour (+ 0x90) and sends them; player 1's game adds the stage choice entry (13.0.5 0x710530a7c0 + 0x20 * [0x710530a968], hazards 0x710530a985: the PR's 13.0.4 addresses one page on). For the other player's slot each game waits for the other's choice (10 min) and writes it into the panel state before the record is built (the stage too). Both slots are made human (+ 0x78 = 0) on their own controller slot (+ 0x210 = index, whatever joined the panel first) with no name tag (+ 0x450 = 0). Choices are answered in the menus, the waits and the fight (a lost one would leave the other game waiting), and forgotten when the fight is over. The other slot still has to be joined at character select (the keyboard), but what it picks no longer matters.
- **twopc11: it works.** PC 1 picked Steve + Wolf (stand-in) on Final Destination; PC 2 Mario (stand-in) + Kazuya on Battlefield: both games played Steve vs Kazuya on Final Destination (user), in sync (443 / 884 hashes, 0 different), no crash. The logs show each game taking the other's choice (and PC 2 the stage). One flaw: some 30 000 messages each way, because each game answered every CHOICE, answers included (a ping-pong). Now only a waiting game asks (flags bit 2) and only asks are answered (build 17:0x).
- Build 17:06 (not yet tested): no stand-in needed. Each game's own player is the human panel its own controller joined (controller slot = the game's player, from controllers_rollback_p<n>.ini), found at whichever index; the other slot can be a CPU (turned into a human on the right controller slot with the other game's fighter). Both slots also get no name-tag profile (+ 0x214 = -1, + 0x454 / + 0x455 = 0: the game's default controls), as the PR's handshake. Every panel is logged as the select screen left it (type, controller slot, ui_chara). Player 1's PC must join with its own controller before the CPU.
- **twopc12 (build 17:06): no stand-in, it works.** PC 1: panel 1 the GameCube pad (Steve), panel 2 a CPU; PC 2: panel 1 the Xbox pad (Kazuya: its controller slot 1), panel 2 a CPU. Each game moved its own player to its index, turned the CPU into the other game's player and took the stage from PC 1: both played Steve vs Kazuya on Final Destination, in sync (709 / 1427 hashes, 0 different), no crash; messages back to normal (ping-pong gone). TWO_PC_SETUP.md now says: each PC joins with its own controller, picks its own fighter, a CPU in the other slot; player 1's PC picks the stage.

### Clean-up of the twopc2-5 extras (build 17:14, to test)

Kept: the seed set at player 1's record (the handshake wait lives there) and idle pads for players 1 and 2 from the match set-up to the fight (the countdown's input is not exchanged). Now OFF by default, to be shown unneeded on two PCs: the pre-fight settle wait (`SSBU_RB_NET_SETTLE_MS`, default 0; it made the countdown stutter), and, behind `SSBU_RB_NET_EARLY=1`, dropped refreshes cleared outside fights and the fixed clock from the match set-up. If a two-PC run desyncs without them, they come back one by one.
- **twopc13 (build 17:14, the three extras OFF): in sync** (260 / 532 hashes, 0 different), the countdown without the stutter: the extras stay off. But PC 1's game (695 rollbacks) crashed right after the fight, the SAME place as stress3 (main+0x35c4580 <- 354a6d0 <- 353d4f0 <- 37cca70, guest thread 3), with the kicks running (SKIP 0): so the kick skipping was not the cause (stress3/4 differed by timing). New cause found: a catch-up running when the fight ends (a rollback at the last KO) left catch-up mode (rb_fast: presentation work left out, the vsync thread unpaced) on after the fight, since it was only switched off at fight safe points: the results screen ran in it. Now ended at "fight over" (and held threads let go): build 17:2x. SKIP stays 0 until re-measured.
- stress5 (one PC, heavy loop stress, SKIP 0, LITE 121, the catch-up reset at fight end): crashed at the end of the first fight again, same chain (main+0x35c4580 <- 354a6d0 <- 353d4f0 <- 37cca70), on the guest main thread this time, and NO catch-up was running when the fight ended: the catch-up theory is wrong (the reset stays: harmless). f_71035c4580 walks reference-counted handles (shared_ptr-like, entries of 0x70 bytes), as graphics/effect resources do.
- Fight-end results so far: stress2 (SKIP 0, LITE 0, 2 games) clean; stress3 (49 / 121) crash; stress4 (0 / 121) clean; twopc13 (0 / 121) crash; stress5 (0 / 121) crash. **Suspect now: the lite skips (presentation work left out of re-run frames), group 64 (the effect emitter tick) first.** Next tests: heavy stress with LITE=0 (2-3 games, expect clean), then LITE=57 (121 without 64).

### CHECKPOINT (2026-10-30 ~17:30)
- Two-PC rollback works: full 5-stock matches in sync (twopc10, twopc12, twopc13), the seed + CHOICE handshake (each PC joins only its own controller, a CPU in the other slot), TWO_PC_SETUP.md current. PC 2 updated by copying build\ssbu_rollback.exe/.map from the pack folder share.
- Open bug: a crash at the end of a fight after many rollbacks (the PC that rolls back), see above. Build 17:22 (catch-up reset) is on this PC only; PC 2 has 17:14.
- Defaults now: SKIP 0, LITE 121, CHECK_EVERY 10, settle 0, NET_EARLY 0.
- stress6 (heavy, SKIP 0, LITE 0): three whole games, no crash; games 1 and 3 were the same 7591-frame fight that crashed in stress5 with LITE 121, game 2 the 6359-frame fight of stress3/4 (the bots' fights repeat). **The lite skips are the cause.** Next: LITE 57 (121 without 64, the effect emitter tick).
- stress7 (heavy, SKIP 0, LITE 57 = 121 without 64): the 7591-frame fight twice, no crash. **Cause of the fight-end crash: lite group 64, the effect emitter tick, left out of re-run frames** (effects made in a catch-up and never ticked; the fight's end trips on them). **Default LITE is now 57** (build after 17:4x). SKIP could likely go back to 49 (the kick skip was a bystander); to re-test before changing.
- **twopc14 (build 17:40, LITE 57): two whole games on two PCs, both IN SYNC, no crash on either**, the second with ~200 rollbacks on PC 1 up to the end (the fight-end crash is fixed). Open: PC 1 waited much longer for PC 2's pads than before (about 46 s in game 1, 33 s in game 2; twopc10 18.6 s for a whole game): PC 2 ran below full speed (its window unfocused, or this build's cost there): to look at with the tuning. The report's counters (rollbacks, waits, hashes, messages) add up over the session's fights instead of starting again per fight: to fix.

### Speed on the slower PC: lazy saves (build 17:52, to test)

PC 2 (GTX 1660 Ti) has always run at about 20 ms a frame in net mode (frame gap means ~20 ms in twopc10 and twopc14), so both games went at its pace (PC 1 waited 33-46 s a game in twopc14). Its saves cost about 9 ms each (hold ~3 ms, memory 5-8 ms: alias stamps 2-4, alias pages 3-4) against ~2 ms on PC 1, every frame - and it never rolls back (it's behind: the other's pads are always there). Now a state is saved at `key` only while the other player's pad for key + 1 hasn't come (a wrong guess at m loads a state tagged m - 1 or before; once pad key + 1 is there, no guess can go wrong before key + 2). `SSBU_RB_NET_LAZY=0`: a save every frame. The report says how many saves were made and left out.
- **twopc15 (build 17:52, lazy saves): it works.** PC 2 made 15 saves and left out 4156; its frame gap went from ~20-22 ms at the start to 16.7 ms (full speed) by the end, where it stayed near 20 ms before. PC 1 (ahead, guessing) made 5292 saves, left out 26, rolled back 725 times, and waited only 2.4 s in all for PC 2 (33-46 s a game before). In sync (393 / 792 hashes, 0 different), no crash. The start of a match is still slower on PC 2 (shaders / effects on the slower card, presumably).
- PC 2's slower first minute is the renderer warming up, not rollback: no shader first-use over 4 ms, but "GPU-resident buffers made" grows 65 -> 472 over the first two minutes and stops, as its frames presented go from ~50 to 60 a second. (To compare with the normal build on PC 2 some time.)
- The report's counts (rollbacks, waits, saves, hashes, messages) now start from zero at each fight (build 18:0x).
- stress8 (heavy, SKIP 49, LITE 57): the 6359-frame fight crashed at its end, the same chain. So the kick skips leave work undone too: fight-end clean only with SKIP 0 and LITE 57 (stress4: 0/121 clean once by luck; stress5, twopc13: 0/121 crash; stress7: 0/57 clean twice; stress8: 49/57 crash). **SKIP 0 and LITE 57 stay the defaults.**

## M6 started (2026-10-30 evening): time sync

### Holds that really hold (build 18:13, to test on two PCs)

twopc15's PC 1 rolled back 725 times, 2-9 frames deep, on a home network where a round trip is ~1 ms: it ran 4-6 frames ahead of PC 2 the whole match, though it "held back" 276 frames for the frame advantage. **The holds did nothing:** a hold was `Sleep(16)` at the safe point, and the vsync event stays signalled until the game takes it (kernel.c: only ClearEvent / ResetSignal clear it). A frame of ~6 ms work plus 16 ms asleep still ends before the refresh after next, finds the event set, and goes on: no frame is lost, the game ahead stays ahead.
- **Now a hold leaves one refresh out** (`rb_vsync_skip`, ipc_vi.c): the vsync thread lets that refresh pass with nothing signalled or shown, so the game's frame end waits one more and the game is one frame later for sure (the screen shows the last frame twice, as with a dropped frame). `SSBU_RB_NET_HOLD=sleep`: the old way, for A/B.
- **The controller decides every 8 frames** (at least 4 fresh samples, at most 8 frames owed, one hold in 4 frames), not every 32 at most 4: that could not keep up with a peer at ~20 ms a frame (5 frames lost in 32). Samples are dropped once a wait is paid (taken while paying, they'd ask for part of it again). `tests\rb_net_test.c` has a new case, `2 slow` (B misses one tick in 6 for the first half): stalls 241 with the old cadence, 71 with every 16, **3 with every 8**; the normal runs (delay 0, 2, 4) about the same. All pass, the sabotaged run fails.
- **A `net sync` line once a second** in net mode: both games' frame advantage (ours, the peer's), holds and refreshes left out so far, rollbacks, waits for pads, and the ack round trip (when the peer first acks our pad for a frame against when it first went out: the real trip plus up to a frame each of the peer's sending and our polling, so read the second's least). The report gives the mean.
- **What to look for in twopc16:** both advantages near 0 (within ±2) after the first seconds, rollbacks on PC 1 shallow (depth 1-3) and far fewer than 725, both PCs a few rollbacks each, "refreshes left out" equal to the holds. PC 2 needs the new exe and .map.

**Next in M6:** the input delay from the round trip (for play over the internet), then the internet itself (port forwarding first: the link already takes any IPv4 address), audio during rollback, 3+ players.
- **twopc16 (build 18:13): it works.** A whole game, 6246 fight frames, in sync on both PCs (625 / 1247 hashes, 0 different), no crash. **0 rollbacks on either PC** (twopc15: 725 on PC 1, 2-9 deep). PC 1's frame advantage went from 7 at the start to 3 by frame 660 and **0 from frame ~1860 to the end** (PC 2's: -9 → -1); the holds really held (572 holds = 572 refreshes left out). PC 1 waited 319 frames for pads (7.8 s), nearly all in the first 10 s (313 by frame 660), while PC 2 warmed up. Both games ran at the same pace throughout, set by PC 2: frame gap mean 19.4 ms in the first minute, 17.1 by mid-game, 16.7 at the end (the renderer warm-up seen before). PC 1's holds kept coming, ~30 a 10 s, as PC 2 stayed a bit under 60 fps. The ack round trip reads 3-16 ms least a second (~1 ms of real trip plus a frame of sending and polling on each side: as a measure it only bounds the trip to within a frame or two).
- With the games aligned and no rollbacks at delay 2, **delay 1 on a home network** is the next thing to try (half the input lag, a few shallow rollbacks expected); then the delay chosen from the round trip.
- **Delay 1 (to test, twopc17):** `launchers\Rollback Netplay.bat` now takes the delay from `RB_DELAY` (default **1**; set it before starting the launcher, the same on both PCs). No rebuild: the 18:13 exe. To look for: rollbacks a few and shallow (depth 1-2), still in sync, and how the controls feel against twopc16.
- **twopc17 (delay 1, build 18:13): in sync, no crash.** A whole game (4305 logic frames; 809 / 399 hashes, 0 different). PC 1: **244 rollbacks (depths 2-9), all in the first ~17 s** (none after logic frame ~1000), 1263 frames run again, catch-up 17.4 ms mean (max 98.9); 303 frames waited for pads (6.4 s, mostly at the start); 814 holds (all real: refreshes left out). PC 2: 0 rollbacks, as before. The frame advantage was 0 to 6 (twopc16 settled at 0): **PC 2 ran slower this game, frame gap ~19-20 ms nearly all match (18.2 at the end) against 17 in twopc16**, so both games ran at ~52-55 fps and PC 1 dropped ~1 frame in 5 to keep pace. At delay 1 the early part (PC 2 warming up, PC 1 ahead) costs deep rollbacks that delay 2 hid.

### CHECKPOINT (2026-10-30 ~18:30) — M6 time sync
- Done: real holds (a refresh left out), the faster sync controller, the `net sync` lines; twopc16 (delay 2) 0 rollbacks, frame advantage 0; twopc17 (delay 1) in sync, rollbacks only in the first ~17 s. Launcher default delay is now 1 (`RB_DELAY`). Exe build 18:13 on both PCs; PC 2 has the new launcher.
- Open: (1) PC 2's speed: why it ran ~19-20 ms a frame all of twopc17 (17 by mid-game in twopc16); see whether its window lost focus, compare with the normal build there; it sets both games' pace. (2) The start of a match: PC 2's warm-up puts PC 1 ahead, deep rollbacks at delay 1; ideas: a higher delay for the first seconds is not possible mid-match (rbn_local), but holding PC 1 harder early (more than 8 in 32) or warming PC 2's renderer before the fight would. (3) Delay from the round trip, then internet play (port forwarding), audio during rollback, 3+ players.
- User's feel of delay 1 vs 2: not yet asked.

### twopc18 (2026-10-31, delay 1, two matches in one session): 50 fps, PC 2 stutters
- User: PC 2 stuttery at the start ("GO!" lingers), smooth-ish after 15-30 s but not 60; **match 2: visual stutter the whole match on PC 2, ~50 fps; PC 1 also 50 fps but smooth.** Both matches in sync on PC 2 (655 / 1537 hashes, 0 different); match 2 clean on PC 1 too (762, 0 different).
- PC 1 logged **one false desync in match 1** (logic frame 2100, 1 of 514 different): both PCs' fighter lines agree before and after (states, positions, the same KO and respawn), and PC 2 compared that frame equal: PC 1 sent the matching hash and later held another for the frame, so a rollback near a KO overwrote a frame already final. To find (the hash bookkeeping, not the fight).
- **PC 2 set the pace: frame gap ~19 ms all of match 2, spikes to 70-107 ms** (PC 1's worst ~33). Its saves cost ~9.3 ms (p50; 11.5 mean); at delay 1 it saves far more (match 1: 2023 saves in 4130 frames; twopc16 at delay 2: 32), since the other's next pad comes later. But match 2 had fewer saves (1050 in 7794) and was slower, so saves alone don't explain it. "GPU-resident buffers made" is a running total (made and retired all the time), the same on PC 1: churn, not the cause.
- Build 10:45: a `net work` line after each `net sync` line: the rollback's own work in the last second (fight hashes every 10 frames, saves, the once-a-second fighter line: count, ms in all, worst). Next run tells the rollback's cost on PC 2 from the game's. Also needed: **the normal build on PC 2** (does it hold 60 there at all?), and the same at delay 2.

### PC 2's lost frames: the fight hash (2026-10-31, twopc19, build 10:45)
- **The normal game (ssbu_nvfix.exe) on PC 2: 60 fps** (1801-1802 frames shown every 30 s; one shader load at a new move). So the ~10 fps PC 2 loses in net mode are the rollback build's. (pack_for_other_pc.bat now copies ssbu_nvfix.exe and its offline launcher too.)
- twopc19 (delay 1, the user on PC 2): 60 fps at times, back to ~50 whenever the fighters met. The `net work` lines: **a fight hash cost PC 2 ~40 ms** (6 a second: 240 ms a second, worst 59), late in the fight ~13 ms; the once-a-second fighter line 6-22 ms; against **~4 ms and 1-3 ms on PC 1, the same build, the same fight**. Saves on PC 2: 7-11 ms each, up to 53 a second late in the fight (the games ~2 frames apart at delay 1: the other's next pad rarely there).
- Suspect: `rng_base()` asked Windows (VirtualQuery) 4 times a call, twice a hash, plus every frame of a fight's first 5 s (the generator-calls diagnostic): VirtualQuery over this build's huge write-watched areas is slow, and maybe far slower on PC 2. **Build 11:05: the chain is found once a fight and then only followed (plain reads)**; the `net work` line now also says how much of it was the game's getters and how many look-ups still asked Windows. If the getters themselves are the cost on PC 2, the hash has to be made smaller or rarer instead.
- **Two "desyncs" in twopc19**, at logic frame 4430 on both PCs (PC 1: ours 6af50988, the peer's fc2fe795; PC 2: ours fc2fe795, the peer's 2a6f9b90: PC 1 had held two different hashes for the frame). Both at the fight's last KO, and both games played the same KO (P1 at 79.9 launched by P2's 0x2f attack, the same path after). twopc18's false one (frame 2100) was at a KO launch too, and the fight was the same after. **Pattern: a hash mismatch at a KO launch, the fight itself unchanged.** Suspects: the KO's camera zoom / slow-down being presentation work left out of re-run frames (`SSBU_RB_LITE` 57 includes the camera), or the bookkeeping's assumption that a frame's hash doesn't depend on that frame's own pad. Test on one PC: loop mode, a reference run and a lagged run, with LITE 57 and with the camera bit off.
- **twopc20 (build 11:05, delay 1, the user on PC 2): "the smoothest test yet ... ran at 60fps the whole time, felt like an offline match"** (only the stutter as the games line up in the first second). **VirtualQuery was the whole cost:** a fight hash now costs ~0.02 ms on both PCs (6 a second: 0.1 ms in all; PC 2's was 240 ms a second), the game's getters included; no look-up asked Windows after the first. Frame gap ~15.9-16.2 ms on both all match. In sync (1059 / 757 hashes, 0 different; no false desync this time). PC 1: 69 rollbacks, all depth 2 (catch-up 6.7 ms mean), waited 1 frame, 14 holds. PC 2: 0 rollbacks.
- What's left on PC 2: **saves, ~9 ms each (worst 17.8), on nearly every frame at delay 1** (5715 of 6095; ~500 ms a second), against ~4-5 ms on PC 1. They fit in a 16.7 ms frame here, but it's half the budget: the next thing to make cheaper (look for other VirtualQuery or Windows calls in the save path first). Also open: the false desyncs at KO launches (twopc18, twopc19).

### Saves on PC 2 (build 11:15, to test)
- twopc20's logged saves on PC 2 (only those over 8 ms are logged: 3330 of 5715): 9.25 ms on average = **"hold" 4.6 ms** (waking and parking ~23 threads, then `gpu_queue_drain()`: waiting until the GPU thread has run everything queued, this frame's whole submission, since the safe point is right after it) + **memory 4.4 ms** (alias stamps 1.8, alias pages 2.1: ~12.6 MB copied a save). PC 1's: ~5 ms (hold ~3.3).
- Each logged slow save also ran `present_runs()` for its line (a walk of every present page bit, ~500 000 pages) on top, outside the measured total: PC 2 logged over half its saves. **Now a slow save is logged at most once in 60.**
- **A `net saves` line once a second:** the saves of the last second, a save's average and its parts: parking the threads, GPU drain, memory, the rest. Next run says which part to attack. If the drain dominates: saving without waiting for the GPU thread needs its guest-memory writes (semaphores, sync points) accounted for, a design change. If parking: fewer threads woken, or a cheaper wait than SwitchToThread spinning. If memory: fewer alias pages (what the game writes each frame, much of it GPU data).
- **twopc21 (build 11:15, delay 1, the user on PC 2): "very smooth again, 60fps the whole time ... the GO fades cleanly"** (only the hitch as the games line up). In sync (719 / 363, 0 different). PC 1: 16 rollbacks (depth 2-3, catch-up 7.2 ms), waited 1 frame, 11 holds. PC 2's log 112 KB (twopc20: 1.7 MB).
- **The parts of a save** (every save, `net saves` lines): **PC 2: 8.94 ms = parking 0.09 + GPU drain 4.35 + memory 4.18 + rest 0.32; PC 1: 4.91 ms = 0.09 + 2.51 + 2.13 + 0.19.** Parking the threads costs nothing; **the GPU drain and the memory copy are half each.** At delay 1 PC 2 saves on ~98 % of frames: ~9 of every 16.7 ms, which fits now, with ~3 ms to spare on the busiest frames.
- The two ways to make saves cheaper, both bigger changes: (1) **no GPU drain at a save**: the safe point is right after the frame's submission, so the drain waits for the GPU thread to translate that whole frame. Saving without it means the state holds GPU work not yet run: its guest-memory writes (semaphore releases, sync point counts) land after the copy, and a load must account for them (run the queue to the save's mark before restoring, and restore sync points to what that work leaves). (2) **fewer memory pages**: ~12.6 MB a save is what the game writes each frame, much of it presentation/GPU data the fight doesn't need back; telling those apart is the hard part (the PR's crashes came from leaving out state).

## M6: play over the internet (build 11:27, to test)
- **The link (hle_rb_link.c):** the peer is a name or IPv4 address, `:port` when its router forwards another, or `*`: this game waits to be found (the host: its router forwards its UDP port to it). The peer's address is learned from its messages: once the core finds a message valid and the other player's (HELLO, PADS, CHOICE), `rbl_accept` makes its sender the peer, so a waiting game finds its joiner and a router that gives the other's messages another port is followed. Once there is a peer, messages from any other address are dropped unread. Windows' "port unreachable" errors (the other game not started yet) no longer stop a frame's reading (SIO_UDP_CONNRESET off).
- **HELLO version 2 (40 bytes):** each hello carries the sender's clock, an echo of the other's newest clock and how long it was held: the round trip, measured in the menus. **Player 1's game picks the input delay** from the least of the first 12 round trips (`rbn_delay_for_rtt`: 1 up to 50 ms, one more each 50 ms, at most 5), or takes `RB_DELAY` if set, and **sends no seed until it has**, so player 2's game takes the seed and the delay at once (player 2 always takes player 1's: the two can't differ). A version 1 game is told apart ("both PCs need the same build").
- **Launcher:** the address may be left empty (`*`: wait to be found); `RB_DELAY` defaults to `auto` (-1 in SSBU_RB_NET). Dry-run checked (waiting, `ip:port`, a fixed delay). TWO_PC_SETUP.md has a "Playing over the internet" section (forward UDP 7701/7702 on the host's router, the host leaves the address empty, the other gives the host's public address; without forwarding, both giving each other's public address may work; a phone hotspot for a real internet path at home).
- Tests: `rb_net_test` (HELLO v2 round trip, the v1 hello told apart, the delay rule) and the new **`rb_link_test`** (loopback: found while waiting and answering at the finder's port, following the peer to another port only once its message is valid, a stranger at 127.0.0.2 dropped, a bad name refused): all pass (`tests\run_rb_net_test.bat`).
- **To test:** (1) the home network as before, the address given as now: the logs should say the round trip (a few ms) and `input delay 1`, in sync. (2) The internet: PC 2 on a phone hotspot, PC 1 the host (its router forwarding UDP 7701 to it, the address left empty), PC 2 given PC 1's public address.
- **twopc22 (build 11:27, home network, delay auto): "felt smooth, 60 fps."** The handshake worked: player 1 measured, chose `input delay 1`, then proposed the seed; player 2 `took player 1's match seed ... and input delay 1`. In sync (789 / 393, 0 different), frame gap ~15.8-16.2 ms on both; PC 1 17 rollbacks (mostly depth 2), 1 frame waited, 11 holds.
- But player 1 measured **16.1 ms** every time while player 2's least was **1.0 ms** (the real trip): a game read its messages only at its safe points (or every 16 ms in the set-up wait), so an answer sat up to a frame on each side, and with both games at 60 Hz in step the least of many readings kept it. Harmless at home (delay 1 either way); over the internet up to ~33 ms more could cost a frame of delay. **Build 11:37: the link has a receive thread** that takes each message off the socket as it arrives and notes when; the game reads them from a queue. The round trip (and the ack round trip) now go by arrival times; the hold the other game reports also runs from when our hello arrived there. rb_link_test checks it (a message read 108 ms after it came: its time is 0.17 ms after it was sent).
- **The keyboard plays this PC's own player (build 11:43, to test).** In net mode the game read a controller slot only while something was connected to it on this PC, so the other player's slot needed a device for the other PC's moves to reach the game: that was the keyboard's real job in controllers_rollback_p1/p2.ini (`keyboard_player` = the *other* player), and it could not play. Now hle_hid.c keeps the other PC's player's slot connected as a Pro Controller by itself (`rb_slot_remote`, hle_rb_core.c), and both settings files put the keyboard on the PC's own player (p1: 1, p2: 2). The one-PC test file is unchanged (its keyboard is still player 1 in both windows: the remote slot in window 2, connected either way).

### **OVER THE INTERNET, IN SYNC (2026-10-31, inet1, build 11:43)**
PC 1 at home (the host: router forwarding UDP 7701, address left empty), PC 2 on a phone's USB tethering ("my reception is horrible"). **PC 1 found PC 2 at 198.51.100.20:3419** (the carrier's NAT, a port nobody chose: the address learning worked), the round trip measured (least of 12: 112.6 ms) -> **input delay 3**, taken by player 2 with the seed. **A whole fight in sync** (411 / 398 hashes, 0 different), no crash. User: "started very choppy, but then played at 60fps eventually."
- **The choppy start (the first ~7 s, logic frames 0-420):** both games waited for pads (~22 s each in all, nearly all then), and the ack round trip **climbed 120 -> 260 -> 430 -> 830 -> 1570 ms**, then fell to ~76 ms at logic ~450 and stayed there to the end (both games 2 frames apart, PC 1 45 rollbacks, PC 2 none). Of PC 2's 5378 messages PC 1 got 4555 (15 % lost, the phone's uplink); of PC 1's 5457 PC 2 got 5453.
- **Cause, likely: a spiral in the PADS messages.** Each message carries every pad the other hasn't acknowledged (up to 64, 20 bytes each): when the trip grows, so do the messages (28 + 64 x 20 = 1308 bytes, 60 a second: ~630 kbit/s), which a weak uplink can't carry, so its queue grows, the trip grows, and so on, until it drains. **Fix to make: run-length pads** (a pad held for many frames sent once with its count: most frames repeat the one before), so a message stays small however long the trip.
- The delay: the 12 samples came from that congested start (least 112.6 ms -> 3); the least later was ~85 ms (-> 2). Taking more samples, or choosing later, would pick better.
- **Build 12:02 (to test): PADS in runs, wire version 3.** A run is one pad held for up to 255 frames in a row (1 + 20 bytes); a message carries `runs` of them (byte 3), adding up to its count. rb_net_test: 64 pads alike are one run (49 bytes); 64 mixed pads in 28 runs, 616 bytes (were 1308); runs past the count, or of 0 frames, refused; in the simulated fights messages average **55.6 bytes against 107** (its pads change every 4-10 frames; a real game's are held longer, a stick swept changes every frame). Every buffer sized for the longest message (`RBN_MSG_MAX`). Both PCs need the build (a version 2 game says so and won't agree).
- **The delay is chosen from 30 round trips over 5 s at least** (was the first 12).
- The `net sync` line also says the PADS messages of the second: how many, their average and longest size.

### **inet2 (2026-10-31, build 12:02): over the internet, smooth from start to end**
Same set-up as inet1 (PC 1 the host, PC 2 on phone USB tethering, found at 198.51.100.20:3403). User: "smooth all throughout!" The delay: round trip **68.8 ms (the least of 30 over 6.1 s) -> input delay 2** (inet1: 3). **In sync** (357 / 328 hashes, 0 different), no crash. Round trip steady at **~70-77 ms** the whole fight (inet1 climbed to 1.5 s); waits for pads: PC 1 2 frames in all, PC 2 none (inet1: ~22 s each). PC 1: 205 rollbacks (depths 2: 161, 3: 40, 4: 3, 7: 1), catch-up 7.2 ms mean; PC 2 none. **PADS messages ~49-66 bytes on average** (runs). Losses this time ~0 (3479 sent by PC 2, 3477 got): the reception may have been better than in inet1 too, so the runs' share in the smooth start isn't measured alone; but the messages can no longer grow with the trip.

### Automatic port opening (UPnP), build 12:12, to test
- The waiting game (address left empty) asks its router to open its UDP port (7701 / 7702), on a thread of its own, through Windows' UPnP NAT API as WindowSmash's netplay relay does (runtime/net_relay.c: the mapping to this PC's LAN address, a stale one from an earlier run replaced, the router's outside address read back). It says in the log (`link: UPnP: ...`) and in `build\rollback_address.txt` either the address the other player types, or what to do instead: no UPnP / refused (forward by hand: one forwarded by hand already works), or the router behind another NAT (the provider's: let the other PC host). `SSBU_RB_UPNP=0`: the router left alone.
- The mapping is removed as the window closes (gpu_d3d11.c's WM_CLOSE calls `rbl_upnp_close`: the game ends with ExitProcess, which runs no atexit; WindowSmash's relay has the same gap), else replaced at the next start.
- The launcher, hosting, deletes the old address file, starts the game, waits for the file (up to 90 s: the link opens at the first safe point, after start-up) and shows it. Dry-run checked (host and joiner). TWO_PC_SETUP.md's internet section now starts with it; port forwarding by hand is the fallback. rb_link_test runs with UPnP off.
- Not yet run against a real router. This PC's router already has a hand-made forward of UDP 7701: UPnP may be refused for that port, which the message says works anyway.
- **inet3 (build 12:12): UPnP works.** The hand-made forward of UDP 7701 removed from this PC's router first. PC 1 (host, address empty): `link: UPnP: The other player joins: 203.0.113.7 (the router opened UDP port 7701 for this PC, 192.168.1.20)`; PC 2 (phone tethering) joined that address and was found at 198.51.100.20:9344. Round trip 71.3 ms (least of 30 over 6.1 s) -> delay 2. **In sync** (399 / 426 hashes, 0 different), "went smooth again"; PC 1 149 rollbacks (mostly depth 2, catch-up 7.4 ms), 3 frames waited; PC 2 none, 2 frames waited. On closing the window: `link: UPnP: the router's UDP port 7701 mapping removed`. **Hosting needs no router set-up now** (on a router with UPnP on).

### The false desyncs at KO launches: the finality rule (build 12:24, to test)
- Ruled out first: that the input is read before the frame's safe point. Old logs (`pad read: player 1, ... logic frame 11, fight frame 11`) show player 1's pad for logic frame k read after safe point k.
- But twopc19's numbers: PC 1 held two different hashes for frame 4430 (sent 2a6f9b90, later its own 6af50988), PC 2 had fc2fe795. A frame is sent only once final, which was "pads real up to the frame before it"; its hash can then change only if a wrong guess at that very frame rolled it back (the last frame of the catch-up is hashed again). With nothing before it changed, **part of a frame's state follows its own pads** (the other player's read, or some part of the fight, before the safe point). At a check frame (one in 10) where the guess at that frame was wrong, the guess-based hash was compared or sent: a false desync, the fight itself fine. Rare, and at moments of heavy input (a KO).
- **Fix:** a frame's hash is final once its own pads are real (`rbn_confirmed()`, was + 1); a wrong guess at frame m clears m's hash too (was from m + 1). Comparing waits one frame more; a real desync can't hide. Wire version 4 (a game with the old rule says so).
- **Self-check, kept to find the part:** each hashed check frame is kept (the fighters' fields and all 9 generators' words) with whether it was final by the OLD rule; one that was and comes out different when run again logs both passes field by field (`net check: logic frame ... came out DIFFERENT`), and the report counts them. A DESYNC report now also shows this game's fields for the frame (the first 3), to set beside the other PC's log.

### The false desyncs at KO launches: one suspect ruled out (2026-10-31)
- Suspect 1, the check's rule that a frame's hash is final once the other's pads are known up to the frame *before* it (`rbn_confirmed() + 1`): **ruled out.** play_rollback_chunk1.log: the fight's first safe point has logic frame 1 (`fight starts at logic frame 1`, S.frames 1), and the pad read for logic frame 1 comes *after* it (`pad read: ... logic frame 1, fight frame 1`, S.frames moving only at safe points). So a frame's pad is read after its safe point; the state at safe point N holds pads up to N - 1, and the rule is right.
- Left: the KO's camera zoom / slow-down as presentation work left out of re-run frames (`SSBU_RB_LITE` 57 includes the camera group). Test (one PC, needs a fight set up by hand): loop mode with lag against a reference run, LITE 57 against 56 (camera kept), with fights that have KOs. Parked: not seen in inet1-3 or twopc20-22, and the fights stayed the same each time; 3+ players first.
- **twopc23 (build 12:24, home network): smooth, in sync** (350 / 685 hashes, 0 different, no DESYNC). The round trip now reads **0.2 ms** (the least of 30 over 6.1 s; twopc22 read 16.1 ms before the arrival times) -> delay 1. Only 5 rollbacks (all depth 2) on PC 1, none on PC 2, so the self-check had nothing to see (0 frames hashed again with their pads real). The KO case needs a match with many rollbacks: over the internet.
- **inet4 (build 12:24, phone tethering, UPnP host): smooth, in sync** (349 / 348 hashes, 0 different, **no DESYNC**). UPnP opened UDP 7701 and removed it at close; PC 2 found at 198.51.100.21:9304 (another carrier address than before: the learning handled it); round trip 64.9 ms -> delay 2. PC 1 173 rollbacks (157 of depth 2), catch-up 6.7 ms; PC 2 none. The self-check: 0 frames hashed again with their pads real (the case needs a wrong guess AT a check frame noticed in that frame's own step, rare at delay 2), so no field evidence yet; it stays in, and the counts accumulate over later matches. The false alarm came in 2 of ~10 matches before; none since the fix (twopc23, inet4) - not yet proof.

## M6: audio during rollback (build 12:37, to hear)
- The game mixes its own sound (its thread, audout); ipc_audio.c queues each buffer and the device plays from the queue. A load put the mixer (guest memory) back by the rollback's depth and it mixed that stretch again, while the first pass's sound was still queued and partly played: **every rollback repeated a slice of sound** (33-117 ms; 150-200 a match on the PC ahead, so PC 1; the user mostly played on PC 2, which never rolls back).
- **The splice:** each savestate's audio part keeps the mixer's place on the queue's timeline (`wpos - skip`). A load drops what was queued since then and not yet played (the run again mixes it right), and leaves the part already played out of the run again's first appends (`skip`): the sound goes on from where the device is. A buffer left out whole is released at once, so the mixer catches up at once. The log says the first loads' and every 200th's amounts (`[audio] rollback N: ... dropped, ... left out`). `SSBU_RB_AUDIO_SPLICE=0`: as before.
- Model check (scratchpad splice_model.py, every byte tagged with its timeline place; saves every tick, loads 1-7 back at random, skips still owed at a load): 20 runs, every place played once, in order; without the splice, 616 repeats in one run. Not yet heard in the game.
- **To hear it:** on the PC that rolls back (PC 1), with many rollbacks: at home `set RB_DELAY=0` on player 1's PC (no delay: nearly every frame guessed) or over the internet. Listen to the music and hits, then the same with `set SSBU_RB_AUDIO_SPLICE=0` for the old way. A tiny dip at a rollback may remain (the queue refills from the mixer); a repeat should not.
- **Heard (build 12:37, home network, PC 1 at `RB_DELAY=0`, PC 2's player a bot: `SSBU_RB_NET_LIVE=0`, now honoured by the launcher):** with the splice, ~2 rollbacks a second all fight (125 in ~64 s), each spliced (e.g. 64 ms queued dropped, 13 ms heard left out): **"music and hits sound fine"**. With `SSBU_RB_AUDIO_SPLICE=0`: **"lots of doubling initially, then it resolves itself"** - the log shows why: 53 rollbacks in the first ~11 s, then none (PC 1 fell a frame behind, so PC 2's pads always came first). **The splice is needed and works.** Audio during rollback: done.

## M7: 3-4 players (2026-10-31: 3 and 4 players IN SYNC on 2 PCs, 3P1 and 4P2; friends to test)
The user's group plays 3+ (not yet more than 4). Design: **through the host** (player 1's game): every other game talks to the host only, and the host passes each player's PADS and CHOICE on to the rest, so only the host needs an open port (UPnP).
- **Netcode (wire version 5):** PADS carry an ack and a "frame heard" per player (`ack[8]`, `heard[8]`, 84-byte header) instead of one peer's; a game resends its pads from the oldest any player lacks; the frame advantage is sampled per player and a game waits for the one it is furthest ahead of (`rbn_sync_sample(p, ...)`). **rb_net_test now runs 3 and 4 players through a host that relays** (each hop lossy and reordering): every player's final states equal the all-pads-known run at delays 0, 2, 4, with one slow peer; the sabotaged 3-player run fails; 2-player numbers unchanged.
- **Link:** one address per player on the host (learned from each player's first valid message; 2 players at home may still be given player 2's address), `rbl_send` to all / to the host, `rbl_send_to(p)`; a message from an address that is another player's is refused; a player trading ports back and forth is reported ("two games set up as the same player?"). rb_link_test: the 3-player host (two found at one IP, told apart by port; send to all / to one; a late address refused).
- **Core:** `net_n` players (the host's `SSBU_RB_NET_PLAYERS`, 2-4; the others take it from the host's HELLO, byte 5); per-player acks, frames heard, hash checks (`DESYNC with player N`), round trips; the host relays; the delay from the longest round trip to any player; the host agrees once every player has the seed; CHOICE per player (each waits for every other's); the record builder handles N panels (each PC joins its own controller, CPUs in the other slots); every remote slot is a connected Pro Controller; the lazy save waits for every player's next pad.
- **Launcher:** player 1-4 (player 1 the host, asked how many play; 3-4 players: it waits for the others, UPnP); the others give the host's address; ports 7700 + player; `controllers_rollback_p3/p4.ini` (earlier players' slots pinned to "nopad", the keyboard on the PC's own player); **each player's log is `build\play_rollback_net_p<player>.log`** (two games can share a PC).
- **3P1 (build 13:07, 3 games on 2 PCs: players 1 and 3 on this PC as bots, `SSBU_RB_NET_LIVE=0`; player 2 on PC 2, the user): IT WORKS.** The host found player 3 at 127.0.0.1:7703 and player 2 at 192.168.0.x:7702; round trips 0.2 / 0.0 ms -> delay 1; "player 3 has the match seed", "player 2 has the match seed: every player has it, agreed"; each game took the other two's fighters (and player 1's stage). **A whole fight in sync on all three: 860 / 997 / 1011 hashes compared, 0 different**, no crash, no false desync. Rollbacks 79 / 102 / 49 (depths mostly 2, ~20 at 7), waits for pads ~5.4 s (host) and 5.8 s (PC 2), none on player 3; frame gaps ~16 ms mean with spikes of 130-175 ms (two whole games sharing this PC).
- Build 13:16: the round-trip lines logged once in 3000 (a long set-up wait logged thousands).
- (The old `play_rollback_net.log` on PC 2 is from before the per-player names: ignore it.)
- **4P1 (build 13:07, 4 games on 2 PCs: players 1, 3, 4 on this PC as bots, player 2 on PC 2, the user): a REAL desync on PC 2.** Set-up right (the host found 3 players, round trips 0.2/0.1/0.0 ms, delay 1, every player had the seed, every choice taken); players 1, 3, 4 agreed with each other all fight; **player 2 diverged at logic frame 440** (1544 of 1668 different). Cause in PC 2's log: **29 loads refused: "a guest thread is on another fiber than at the save"** (from logic ~112), each retried the next frame and refused again until the wrong guess was past the states kept ("5 rollbacks too deep"): a guess never corrected. None on this PC's three games (and none in any 2- or 3-player run; a few in old stress runs). Four fighters keep more jobs in flight, and PC 2's slower CPU catches a worker inside one more often. "Laggier" (user): three whole games on this PC, frame spikes to 1-4 s at the start.
- **Build 13:29:** (1) a load refused for that reason lets the threads go and holds them again, up to `SSBU_RB_LOAD_RETRY_MS` (20), the game not going on meanwhile (the memory then synced, not taken as the save's own); (2) if still refused, the core tries up to 3 older states (a frame or so further back each); (3) each refusal says which thread and where (its own fiber or a job's, at the save and now: the thread's first park marks its own); the report counts loads that went ahead after a wait, loads refused, and rollbacks from an older state.
- **4P2 (build 13:29): 4 PLAYERS IN SYNC.** Same set-up (this PC: players 1, 3, 4 as bots; PC 2: player 2, the user). **All four agreed all fight: 1196 / 1404 / 1486 / 1477 hashes compared, 0 different**, no crash. PC 2: 7 loads refused for a fiber, **all 7 "thread 7 was on a job's fiber at the save, is on its own fiber now"**: none went ahead after the 20 ms wait (a finished job doesn't come back), and **all 7 were done from an older state instead** (0 too deep). This PC's games: none. Rollbacks 260 / 558 / 202 / 147; waits for pads 18 s / 16 s / 4 s / 2 s (three whole games on this PC).
- **Found by accident:** PC 2's character select had only its own player and ONE CPU (panels 3 and 4 empty: "type 3, none"), yet the game built a record for every slot and the handshake made players 3 and 4 of the empty ones, with the right fighters. **A PC needs only its own player at character select**: no CPUs in the other slots. (For TWO_PC_SETUP and the streamlined launcher.)
- Build 13:38: the wait is only for a thread that is in a job NOW (that one may finish); one that was in a job at the save goes straight to an older state (no 20 ms lost).

## Streamlining, item 1: the Host/Join launcher (build 13:45, to test)
- **The launcher asks Host or Join.** Host: how many players (2-4); it waits (UPnP) and shows the addresses to give out, "on this network" (its LAN address) and "elsewhere" (the router's outside one). Join: the host's address only. No player numbers, no settings files per player (`controllers_rollback_p1.ini` on every PC), the delay always from the round trip (`RB_DELAY` / `SSBU_RB_NET_LIVE` still there for tests). Joining games bind any free port and log to `play_rollback_net_join.log` (`join2`... when another game on the PC still writes it); the host to `play_rollback_net_host.log`. Dry-run checked (host, join, a second joiner).
- **The host gives the numbers (HELLO version 6):** a joining game starts with no number (player -1, 0xFF on the wire) and a random join id in the seed field; the host gives it the lowest free player (`rbl_assign`), says which in every hello to it (`assign`, byte 6), and knows it again by its id when its address changes. A slot whose game has said nothing for 5 s outside a fight (pads count as heard) goes to a newcomer: a friend whose game started again gets back in. The host now reads a new address even with every player found; the core takes it in only as a valid joiner's hello.
- **A joiner's controller:** every PC's controller and keyboard are its player 1; once the host gives a game player N, hle_hid reads the game's slot N from this PC's slot 1 and the game's slot 1 (the host's player, remote) from the empty slot N (`rb_slot_phys`; rumble too).
- **Character select needs only your own player** (4P2's accident): TWO_PC_SETUP.md rewritten for Host/Join, 2-4 PCs, friends with their own game files.
- Tests: rb_net_test (a joiner's HELLO with no number and its id; the host's assigning HELLO; player -2 refused) and rb_link_test (`rbl_who`, `rbl_assign`, a joiner's restart from a new port followed; a joiner still drops strangers): all pass.
- **hj1 (build 13:45, Host/Join, 3 players: host on this PC (live), a joining bot on this PC, PC 2 joining live):** the host gave the numbers in join order: this PC's joiner (192.168.1.20:53739) player 2, **PC 2 (192.168.0.x:64127) player 3**; "every player has it, agreed"; each game took the others' fighters (PC 2's into panel 3, an empty slot on the host). Host and local joiner: **in sync** (1044 / 1097 hashes, 0 different), the joiner 1 load from an older state. PC 2's own log (`play_rollback_net_join.log` on PC 2) still to read: it shows whether its controller drove player 3 (the slot swap).
- **hj1, PC 2's log: Host/Join WORKS.** "the host made this game player 3 of 3"; at character select **PC 2's controller joined as controller slot 2 (player 3)**: the slot swap moved its player-1 controller onto the given number; player 3 moved around the stage all fight (the user's play); **in sync** (1089 hashes, 0 different). All three games: 0 different. Streamlining item 1 done; next: item 2, the ruleset in the handshake.
- **A window instead of a command prompt (no rebuild: launchers only).** `launchers\Rollback Netplay.bat` now opens `launchers\rollback_netplay.ps1` (PowerShell 5.1 + Windows Forms, both part of Windows) hidden: two big buttons, **Host a match** (players 2-4 from a list, Start hosting; then the "same network" and "internet" addresses with Copy buttons, from build\rollback_address.txt) and **Join a match** (the host's address, remembered in build\rollback_last_host.txt; Enter joins). It starts the game exactly as the text launcher, which stays as `Rollback Netplay (command prompt).bat` for tests (`RB_DELAY`, `SSBU_RB_NET_LIVE` are honoured by both). Checked: parses; `-Test host 3` / `-Test join <addr>` give the same SSBU_RB_NET as the text launcher; `-Shot <folder>` drew all five pages (choice, host, addresses, join, joined) and they look right. The pack script copies all three files.
- **The window, tried on both PCs (14:03, 2 players; "nice, that's cleaner"):** the host's window (now always on top: SSBU's window opened over it) gave the addresses; PC 2 joined through its window and was made player 2 of 2, its controller on slot 2; a ~26 s fight (1560 logic frames) with no desync before both windows were closed (no end report: the fight didn't end). UPnP mapping removed at close.

## Streamlining, item 2: the host's rules in the handshake (build 14:10, to test)
- **RULES messages (wire version 7):** at player 1's record (the match set-up) the host reads the game's **rule header** (the match block + 0, 0x588 bytes: 13.0.4 0x71052c41b0, the PR's MELEE_INFO, one page on: **0x71052c51b0**), the **stage set-up's inputs** (seed 0x710530e380, history 0x71050c6ad0, 0x60 bytes: the PR's hs12, a desync without them on stages with variations) and its first panel's **rule kind** (+5) and **stocks** (+0xa): a 0x5ee-byte blob, sent in two chunks (0x300 each, one UDP packet: the blob is over one) with its hash as id. Every joining game waits for it there (asking), then writes the header (but bytes 0-1, the mode) and the stage inputs over its own, and every panel's rule kind and stocks. The PR found the header pointer-free (two runs alike but for bytes 0-1); as a guard a header differing in over 256 bytes is left alone. The log says how many header bytes differed (and where), and whether the stage inputs did.
- rb_net_test: a RULES chunk round trip, an ask, a chunk past the blob refused.
- **To test:** set a different ruleset on PC 2 (e.g. 2 stocks against the host's 3, items off/on), host on this PC, join from PC 2: the match should play the host's rules on both, in sync; PC 2's log should show the header bytes that differed.

## The GO! freeze (queued by the user after the ruleset work; built 14:15 as build\ssbu_rollback_gofix.exe, NOT swapped in until the ruleset test is done)
Two causes (the other session's notes, checked in the logs):
- **(1) The fight's first save copies all of memory** on fight frame 1, as GO! appears: PC 2 2 GB in 565 ms (the layout walk); later fights worse, 935-1385 ms on PC 1, because the previous fight's states were still kept and the copy saved undo pages for all of memory (546 395). Fix: `rb_ring_drop()` (every state let go, the shadow copy kept) when a fight ends; and at the end of loading the whole copy is taken (a save, then let go), and through the entrances and countdown kept current every 4 frames, so the save at GO! copies a few frames' changes. Outside fights the game keeps to real time by itself (dropped refreshes not cleared there), so the copies don't make the entrance run late.
- **(2) The games start the entrances apart** (one loads faster), reach GO! apart, and the first waits for pads and holds: a stutter after GO!. Fix: a barrier at the end of loading: once a game's fighters are made (the FighterManager there, player 1's entry a battle object) and its other threads have been at a wait for 3 frames running, it says READY to the host (new message, wire version 8); the host waits for every player, then sends ALL READY; the entrances (logic frames, the same count everywhere) then run together. 30 s at most, then each goes on. The fight's start state doesn't depend on when the entrance began (logic frames), only the wall clock does.
- The entrance and countdown are NOT fight frames here (fight_on is the FighterManager's +0xd2, set at GO!: the fighters are 94 frames into their wait motion at fight frame 1), so the barrier sits before them.
- A timeline line every 30 frames from the set-up ("before the fight: frame N, fighters made / not yet, k threads busy") and one at the barrier (the copy's time, the wait), to see where loading really ends.
- **rules1 (build 14:10, the ruleset test; "smooth"):** PC 2 set to other rules. The host sent "rule kind 1, stocks 5, items 0"; PC 2: "the host's rules came", **"9 bytes of the rule header differed (from +0x4c to +0x502)", written over its own**; the stage seed differed and was taken (the history was alike). **In sync** (577 / 290 hashes, 0 different). Streamlining item 2 done.
- The GO! freeze build (14:15) is now build\ssbu_rollback.exe (it includes the rules).
- **hj2 (build 14:15): "still a bit of a stutter at GO!".** The barrier never came: **"fighters not yet" on every frame** from the set-up to GO! on both PCs (the game gives the players their fighters at GO!, though the fighters do their entrances before), so no copy was taken before and SAVE 1 stayed at GO! (358 ms here, 526 ms on PC 2). Both games reached GO! the same number of frames after the set-up (~460): the stutter is mostly PC 2's longer copy (it came to GO! 170 ms later; this PC waited a frame for its pads and held 5).
- **Build 14:29:** the whole copy is taken at the set-up (as the loading screen starts), then kept current every 4 frames through loading, entrances and countdown, each refresh waiting at most 4 ms for the threads (`rb_park_quick_ms`: a thread busy loading -> that refresh left out, no log line); the log counts refreshes and skips. The barrier stays in the code behind `SSBU_RB_PRE_BARRIER=1` until there is a real sign of loading done.
- **The results screen (user):** each game runs its menus alone after a fight, so the joining PC never sees the other players' cards ready. To pass ready presses on there (and stop before character select, where a forwarded A could join a panel and block the start), a sign of the change is needed: build 14:29 logs candidates after every fight until the next set-up (the fighter manager and its impl, its +0xd2, the logic frame, the stage choice, the rule header's first word), every 30 frames and on any change.
- **hj3 (build 14:29): "the GO! was smooth!"** (the copy at the set-up: 559 ms on PC 2 there, then refreshed 86 times with 4 left out by GO!, ~6-8 ms each). **But the results screen can't be passed:** on each PC the other player's card never readies (each game runs its menus alone). The research lines: through 1283 frames of results only the logic frame moved (the fighter manager and its impl stayed); nobody got to character select, so the change there is unseen.
- **Build 14:38: the results screen's A and B passed between the games** (MENU, wire version 9, through the host): from the fight's end each game sends its own player's A/B every frame and presses each other player's for it (rb_pad: this game's slot noted, the others' given; sticks and every other button never passed; a player silent for 0.5 s let go). It stops when the fight's fighter manager changes (the scene gone: to be seen), at the next set-up, or 3 minutes after the fight. The log says when it starts and stops (and why) and each player's A.
- **hj4 (build 14:38): PC 1 got through the results, PC 2 didn't.** PC 2's 34 A presses all reached the host ("player 2 pressed A"), so the host's results ended; the host's 3 presses never showed on PC 2 (sent, it seems, before PC 2's own results began, when it didn't keep them). And the host went on to character select with its **fighter manager unchanged**: no end of the passing there (a passed A at character select could add a player).
- **Build 14:44 (the user's idea): each game presses its own player's A and B on every other player's card too** (the others' passed presses still count): one press readies them all, each PC goes on when its own player does. **The end is now the logic frame standing still for 60 frames** (the results move it on: hj3 1747 -> 2823, never standing over ~30 frames; character select, it seems, doesn't), or the fighter manager changing, the next set-up, or 3 minutes. After the end the logic frame and the fighter manager are logged every second for 15 s, to check.
- **hj5 (build 14:44): neither PC got through.** The logic frame stood a second in the middle of the results animation (logic frame 2177, both PCs), so the mirroring ended before anyone readied; after it the logic frame went on one a frame. Not a sign of character select.
- **Build 14:50: the game's own word.** The game exports `app::lua_bind::FighterManager__is_result_mode_impl` (the fighters do their victory poses on the results screen, which is why the fighter manager lives through it): mirroring is on while it says the results show and ends when it stops (or the fighter manager changes, the next set-up, 3 minutes); called at the safe point with the main thread's registers kept, as the fight hash's getters; every change logged with the frames since the fight; no longer asked 15 s after the end (character select alone).
- **hj6 (build 14:50): "almost!"** The results can be passed now (is_result_mode came on 113 frames after the fight on both PCs: the mirroring started as the results showed; each PC went on with its own player's A), **but the mirroring followed into character select**: is_result_mode stays on there.
- **Build 14:56:** the end is now the FighterManager's **entry count or fighter count** (`entry_count_impl`, `total_fighter_num_impl`) changing from their values as the results began (the fighters let go, the manager staying), besides the fighter manager changing, the next set-up and 3 minutes. Logged on every change after a fight: result mode, entry count, fighters, is_ready_go, the stage choice, and a hash of the last set-up's first panel state (the record builder's x0, every 30 frames, read only if still committed), to see character select come if the counts don't move.
- **hj7 (build 14:56): the results screen WORKS.** Two matches in a row: "this game's A and B pressed on every player's card too (entry count 2, fighters 2)", then "the fighters are let go (character select): A and B no longer mirrored"; in sync (158 / 233 hashes, 0 different). (The entry/fighter count is the end signal.)

## The package for friends (2026-10-31)
- `patches\rollback\make_friend_package.bat` makes `build\friend_package\WindowSmash-Rollback-<date>\` and its zip (**315 MB**): the rollback exe and map, the launchers (window + command prompt), `controllers_rollback_p1.ini`, TWO_PC_SETUP.md, `PACKAGE_VERSION.txt` (the exe's build time and size), SDL3.dll (installed only where missing), and from `patches\rollback\friend\`: **`Install or Update Smash Bros.bat`** (asks for the WindowSmash folder - remembered - checks dump\modules\romfs.cfg, refuses while the game runs, copies; run again with a newer package to update), **`Remove Rollback.bat`** (asks Y/N; removes only the rollback files, logs and notes; game files, save, WindowSmash's launchers and SDL3.dll stay) and **`READ ME FIRST.txt`**. **No game files:** each friend uses their own dump (13.0.5) in their own WindowSmash. Install / update / remove dry-run on scratch folders: all as meant.
- First package: WindowSmash-Rollback-2026-10-31_1503 (exe 14:56).
- **Reformatted (the user's layout), package WindowSmash-Rollback-2026-10-31_1515 (same exe 14:56):** the installer is now `setup\install_rollback.ps1` (the .bat opens it): a folder picker (no typing), **renames the friend's WindowSmash folder to `SMASH BROS`** (rewriting dump\modules\romfs*.cfg's full paths to it; not if a SMASH BROS is already there or the package sits inside the folder; Retry/Cancel if something has it open), copies the files (and clears Windows' downloaded-file mark on them), makes **`SMASH BROS\MULTIPLAYER.lnk`** (powershell, hidden, straight to launchers\rollback_netplay.ps1; icon docs\assets\windowsmash.ico as launchers\multiplayer.ico), and remembers the folder in **%LOCALAPPDATA%\SMASH BROS Multiplayer\folder.txt**, so a newer package run from Downloads updates with no questions (lost note: it looks for a SMASH BROS with game files on the Desktop, Documents, Downloads, the user folder and drive roots, and asks). **`Remove Rollback.bat` now installs into `SMASH BROS\launchers\`** (not the package, not the root): it works on the folder it sits in, also removes MULTIPLAYER.lnk, the icon and the note, and deletes itself last; the folder keeps its name. `-Folder <path> -NoDialogs` for tests (answers every question no). Install, rename, update-by-note and remove dry-run on scratch folders: all as meant.
- **The .bat on its own, beside the zip (package 2026-10-31_1521):** friends download `build\friend_package\Install or Update Smash Bros.bat` and the zip and click the .bat, with nothing to unzip. The bat carries its PowerShell below a `##POWERSHELL##` line. It finds the newest `WindowSmash-Rollback-*.zip` (by time) beside it or in the Downloads folder (the shell's Downloads, so it follows a moved one), unpacks it to %TEMP%\SMASH BROS Multiplayer setup, runs the package's setup\install_rollback.ps1, and deletes the temp copy. Beside an unpacked package it uses that one. It's the same bat for every update. make_friend_package.bat puts it next to the zip. `RB_INSTALL_FOLDER=<folder>` is for tests. Tested on scratch: the bat with the zip alone installs; a newer zip beside the old one is picked.
