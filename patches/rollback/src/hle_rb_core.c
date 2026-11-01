/* Rollback research build: the switch, the log and the frame safe point. See patches\rollback\README.md.
 *
 * SSBU_RB unset: nothing in the rollback files does anything; the exe behaves as ssbu_nvfix.exe
 * (the wrap below only compares an address and calls the original).
 * SSBU_RB=1: on. SSBU_RB_LOG=1: more lines in the log. SSBU_RB_QUIET_MS=<ms> (default 4): how
 * long the safe point waits, in a fight, for every other guest thread to be at a wait (M1 only
 * watches; nothing is held yet).
 *
 * THE SAFE POINT (M1). The game's main loop (13.0.5 FUN 0x7103747820) submits the command
 * buffers recorded in the previous iteration, then kicks the task workers: the call at
 * 0x710374a0d0 to FUN 0x7103549720. Right after that call returns (return address
 * 0x710374a0d4) is where ssbu-recomp PR #116 saves and loads its states (its 13.0.4
 * 0x7103749b24, mapped to 13.0.5 by that project's verified table, hostrt.c g_code_13_0_5).
 * FUN 0x7103549720 is called from other places too; only that return address counts.
 *
 * M1 measures, per fight frame, at that point: the host stack pointer (a state that includes
 * the host stack above it needs it to be the same every frame), how long until every other
 * guest thread is asleep in kwait (kernel_rb_census), which threads were not, and the fibers. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <time.h>

#include "host.h"

void kernel_rb_census(const void *self, u32 *alive, u32 *waiting, u32 *sleeping, u64 *busy, u32 max, u32 *nbusy);
void fiber_rb_census(u32 *live, u32 *running);

static void savestates(Cpu *c, u64 frame);

#define SAFE_POINT 0x710374A0D4ull
#define WINDOW 600   /* fight frames per summary line (10 s) */

int rb_on, rb_log;
static int clock_on = 1;   /* SSBU_RB_CLOCK=0: the real clock in fights too (kernel.c rt_ticks) */
void rb_clock_frame(int fight);
static double quiet_limit_ms = 4;

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

__attribute__((constructor)) static void rb_init(void) {
    const char *on = getenv("SSBU_RB"), *log = getenv("SSBU_RB_LOG"), *q = getenv("SSBU_RB_QUIET_MS");
    rb_on = on && *on && *on != '0';
    rb_log = log && *log && *log != '0';
    if (q && *q) quiet_limit_ms = atof(q);
    const char *ck = getenv("SSBU_RB_CLOCK");
    if (ck && *ck == '0') clock_on = 0;
    fprintf(stderr, "[rb] rollback research build (M2 savestates: F7 save, F8 load): %s\n", rb_on ? "on" : "off (SSBU_RB not set)");
}

/* lib::Singleton<app::FighterManager>::instance_ -> FighterManager -> its impl; +0xd2 is set while a fight
 * runs (ready-go to game set), as ssbu-recomp's run-ahead reads it (ra_fight_on). */
static int net_mode;   /* (the net mode: defined with its state below) */
static int fight_on(void) {
    static u64 inst = 1;
    if (inst == 1) {
        inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
        fprintf(stderr, "[rb] FighterManager instance: %#llx%s\n", inst, inst ? "" : " (not exported: fights can't be told apart)");
    }
    if (!inst) return 0;
    u64 mgr = ld64(inst), impl = mgr ? ld64(mgr) : 0;
    return impl && ld8(impl + 0xd2);
}

static struct {
    long long points, fights, frames;   /* safe points seen, fights started, fight frames */
    int in_fight;
    DWORD thread;                       /* the host thread the safe point runs on */
    int thread_changed;
    uintptr_t sp_ref;                   /* the fight's first frame's SP */
    long long sp_same, sp_other;
    uintptr_t sp_lo, sp_hi;
    long long quiet, not_quiet;         /* frames every other thread reached a wait within the limit, or not */
    double quiet_ms[WINDOW]; int nq;    /* time to quiet, for the percentiles */
    u32 alive_max, sleeping_max, fib_live_lo, fib_live_hi, fib_run_max;
    struct { u64 id; long long n; } busy[16]; int nb;
    double last_t, gap_lo, gap_hi, gap_sum; long long ngap;
    long long win;
} S;

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void summary(const char *why) {
    char busy[400] = ""; size_t n = 0;
    for (int i = 0; i < S.nb && n < sizeof busy - 40; i++)
        n += snprintf(busy + n, sizeof busy - n, "%s#%llu x%lld", i ? ", " : "", S.busy[i].id, S.busy[i].n);
    double p50 = 0, p95 = 0, pmax = 0;
    if (S.nq) {
        qsort(S.quiet_ms, S.nq, sizeof *S.quiet_ms, cmp_d);
        p50 = S.quiet_ms[S.nq / 2]; p95 = S.quiet_ms[S.nq * 95 / 100]; pmax = S.quiet_ms[S.nq - 1];
    }
    fprintf(stderr, "[rb] %s: fight %lld, %lld frames | SP %s (%lld same as frame 1 %#llx, %lld other, range %#llx..%#llx)%s | "
            "quiet within %.1f ms: %lld of %lld (p50 %.2f p95 %.2f max %.2f ms), threads %u, sleeping up to %u, busy: %s | "
            "fibers live %u..%u, running up to %u | frame gap %.1f..%.1f ms (mean %.2f)\n",
            why, S.fights, S.frames, S.sp_other ? "VARIES" : "steady", S.sp_same, (unsigned long long)S.sp_ref, S.sp_other,
            (unsigned long long)S.sp_lo, (unsigned long long)S.sp_hi, S.thread_changed ? " | SAFE POINT CHANGED THREAD" : "",
            quiet_limit_ms, S.quiet, S.quiet + S.not_quiet, p50, p95, pmax, S.alive_max, S.sleeping_max, S.nb ? busy : "none",
            S.fib_live_lo, S.fib_live_hi, S.fib_run_max, S.ngap ? S.gap_lo : 0, S.gap_hi, S.ngap ? S.gap_sum / S.ngap : 0);
    /* the window's figures start again; the SP reference and the fight stay */
    S.sp_same = S.sp_other = 0; S.sp_lo = S.sp_hi = S.sp_ref;
    S.quiet = S.not_quiet = 0; S.nq = 0; S.alive_max = S.sleeping_max = S.fib_run_max = 0;
    S.fib_live_lo = ~0u; S.fib_live_hi = 0; S.nb = 0; S.gap_lo = 1e9; S.gap_hi = 0; S.gap_sum = 0; S.ngap = 0; S.win = 0;
}

static void note_busy(u64 id) {
    for (int i = 0; i < S.nb; i++) if (S.busy[i].id == id) { S.busy[i].n++; return; }
    if (S.nb < 16) { S.busy[S.nb].id = id; S.busy[S.nb++].n = 1; }
}

/* The profile of re-run frames (SSBU_RB_RING): per frame run again, k = 0 for those whose
 * presentation work is left out (rb_fast > 1), 1 for the last one (all of it): the game's part
 * (from the safe point's return to the next one), the safe point's census wait, and the save's
 * parts (hold and GPU drain, memory, the rest). SSBU_RB_PROFILE=<s>: also the sampling profiler
 * (debug_profile.c) for <s> seconds from the first load, sampling only while frames run again. */
extern volatile LONG rb_fast;
static double fp_census_last, fp_hash_last, fp_in, fp_out, fp_sum[2][7];   /* (fp_hash_last: the test's own fight hash, this frame) */
static long long fp_n[2];
static int fp_fast, fp_loaded;
extern double rb_save_ms[3];
extern double rb_save_parts[4]; extern long long rb_save_count;   /* (hle_rb_state.c: parking, GPU drain, memory, in all) */
void profile_start_gated(double seconds);
static void tape_clear(void);
static void safe_point(Cpu *c, uintptr_t sp) {
    S.points++;
    int on = fight_on();
    if (!on) {
        if (S.in_fight) {
            summary("fight over"); S.in_fight = 0;
            /* A catch-up still running when the fight ended (a rollback at the last KO): its mode - presentation
             * work left out, the vsync thread unpaced - was only switched off at fight safe points, so it stayed on
             * through the results screen, which crashed in the job system (stress3, twopc13: main+0x35c4580, both
             * on the game that rolled back, both as the fight ended). Ended here, and any held threads let go. */
            if (rb_fast > 0) {
                fprintf(stderr, "[rb] the fight ended in the middle of a catch-up (%ld frames left): catch-up mode off\n", (long)rb_fast);
                InterlockedExchange(&rb_fast, 0);
            }
            { void rb_release_held(void); rb_release_held(); }
            if (net_mode == 2) { void rb_ring_drop(void); rb_ring_drop(); }   /* (the GO! freeze: no undo of the next loading at the next fight's first save) */
        }
        return;
    }
    double t = now_ms();
    if (!S.in_fight) {
        tape_clear();
        S.in_fight = 1; S.fights++; S.frames = 0; S.thread = GetCurrentThreadId(); S.thread_changed = 0;
        S.sp_ref = sp; S.last_t = 0;
        summary("fight start");   /* (clears the window; its numbers are all zero) */
        S.sp_same = 0;
    } else if (S.last_t) {
        double g = t - S.last_t;
        if (g < S.gap_lo) S.gap_lo = g;
        if (g > S.gap_hi) S.gap_hi = g;
        S.gap_sum += g; S.ngap++;
    }
    S.frames++;
    if (GetCurrentThreadId() != S.thread) S.thread_changed = 1;
    if (sp == S.sp_ref) S.sp_same++; else S.sp_other++;
    if (sp < S.sp_lo) S.sp_lo = sp;
    if (sp > S.sp_hi) S.sp_hi = sp;

    u32 alive, waiting, sleeping, nbusy; u64 busy[16];
    double t0 = now_ms(), waited;
    for (;;) {
        kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 16, &nbusy);
        waited = now_ms() - t0;
        if (!nbusy || waited >= quiet_limit_ms || rb_fast > 0) break;   /* (no waiting in the frames of a catch-up: M1's measure only) */
        SwitchToThread();
    }
    fp_census_last = waited;
    if (!nbusy) { S.quiet++; if (S.nq < WINDOW) S.quiet_ms[S.nq++] = waited; }
    else { S.not_quiet++; for (u32 i = 0; i < nbusy && i < 16; i++) note_busy(busy[i]); }
    if (alive > S.alive_max) S.alive_max = alive;
    if (sleeping > S.sleeping_max) S.sleeping_max = sleeping;
    u32 live, running;
    fiber_rb_census(&live, &running);
    if (live < S.fib_live_lo) S.fib_live_lo = live;
    if (live > S.fib_live_hi) S.fib_live_hi = live;
    if (running > S.fib_run_max) S.fib_run_max = running;
    if (rb_log && S.frames <= 5)
        fprintf(stderr, "[rb] fight frame %lld: SP %#llx, %u threads, %u waiting, %u sleeping, %u busy after %.2f ms, fibers %u (%u running)\n",
                S.frames, (unsigned long long)sp, alive, waiting, sleeping, nbusy, waited, live, running);
    S.last_t = now_ms();   /* (after the census: the gap is the game's own time) */
    if (++S.win == WINDOW) summary("in fight");
}

/* M2: savestates, at the safe point. F7 saves, F8 loads (F1-F6 are the runtime's own keys).
 * SSBU_RB_AUTO=<frame>,<every>[,<count>]: in a fight, save at fight frame <frame>, then load it
 * every <every> frames, <count> times (default 100): the stress test. A load comes back through
 * rb_save's call (hle_rb_state.c), so nothing may follow these calls but the return. */
int rb_save(Cpu *c, u64 frame);   /* 1 saved, 0 not, 2 a load came back here */
void rb_load(Cpu *c, u64 frame);
void rb_load_age(Cpu *c, u64 frame, int age);
void rb_ring_size(int n);
int rb_ring_count(void);
void rb_quiet(int q);
int gfx_window_focused(void);
static long long auto_at = -1, auto_every, auto_left = 100, auto_next;
static long long bench_from = -1, bench_n;

/* SSBU_RB_RING=<frame>,<every>,<depth>,<count>[,<states kept>] (M3, the ring): from fight frame
 * <frame>, a save at every frame; every <every> frames, a load of the state <depth> frames back,
 * <count> times, and the game goes on from there (the frames since run again, with their input
 * from the tape: at normal speed here; rollback proper runs them within one frame, M4). Each fight
 * frame's state (fight_frame) is kept by logic frame the first time it runs; when it runs again
 * after a load, it must be the same. */
static long long ring_from = -1, ring_every, ring_depth, ring_left, ring_since, ring_loads;
static long long ring_same, ring_diff, ring_saves;
static double ring_save_ms, ring_save_max, ring_load_ms, ring_load_max, ring_load_t0;
static struct FFrame *ring_first;   /* by logic frame */
/* M4: after a load, the frames up to where the game was run again without pacing and unseen
 * (ipc_vi.c's vsync thread neither waits for the refresh nor presents while rb_fast > 0), as
 * rollback must, within a frame. SSBU_RB_FAST=0: at normal speed, shown (the M3 test). */
volatile LONG rb_fast;
static int skip_mask = -1, lite_mask = -1;
static long long kicks_skipped, lite_skipped;
static int fast_on = -1;
static long long caught_up;
static double catchup_ms, catchup_max;
#define RING_FRAMES 65536
static void ring_test(Cpu *c, u64 frame);

/* SSBU_RB_VERIFY=<frame>,<k>[,<rounds>]: the replay check. In a fight, save at fight frame <frame>,
 * run <k> frames and keep a copy of guest memory (the reference), then <rounds> times (default 5):
 * load, run the same <k> frames, compare with the reference (hle_rb_state.c rb_ref_compare). The
 * pads must be the same each time: nobody touches the controls. */
void rb_ref_capture(Cpu *c);
void rb_ref_compare(Cpu *c, long long round);
static long long ver_at = -1, ver_k, ver_rounds = 5, ver_since, ver_round;
static int ver_phase;   /* 0 waiting, 1 running to the reference, 2 replaying, 3 done */

/* ---- the input tape --------------------------------------------------------------------
 * Rollback replays frames with the input they had. In a fight, each player's pad state
 * (hle_hid.c npad_state, after the pad, keyboard and SSBU_INPUT) is keyed by the logic frame:
 * the game's counter at 0x71052c5184, which moves once per logic frame and is guest memory, so
 * a load puts it back. The first read of a frame records what the pad gives; every later read of
 * that frame, in this pass or in a replay after a load, gets the recorded state. The sampling
 * number the game sees follows the same counter in a fight, so it is the same in a replay too.
 * SSBU_RB_TAPE=0: no tape (live input in replays: the control).
 * SSBU_RB_INPUT="<period>;<frame>:<button>[/<hold frames>],...": test input for player 1 on WALL
 * time (60ths of a second since start, modulo <period>), so a replay's live input differs from
 * the first pass's and only the tape can make them the same. Buttons: A B X Y L R ZL ZR, and
 * SLEFT SUP SRIGHT SDOWN for the left stick. */
typedef struct PadRec { u8 have; u64 buttons; i32 lx, ly, rx, ry, lt, rt; } PadRec;
static struct { PadRec *rec; u32 cap; } tape[8];
static SRWLOCK tape_lock = SRWLOCK_INIT;
static int tape_on = -1;
static long long tape_recorded, tape_replayed;
static volatile LONG64 rb_sampling, rb_sampling_base;
static struct { int at, hold; u64 button; int stick; } tin[64];
static int ntin = -1, tin_period;
#define LOGIC_FRAME 0x71052c5184ull

static void test_input_parse(void) {
    ntin = 0;
    const char *s = getenv("SSBU_RB_INPUT");
    if (!s || sscanf(s, "%d", &tin_period) != 1 || tin_period <= 0 || !(s = strchr(s, ';'))) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR"};
    static const char *const sticks[] = {"SLEFT", "SUP", "SRIGHT", "SDOWN"};
    for (s++; *s && ntin < 64;) {
        char name[16] = {0}; int at, hold = 5, n = 0;
        if (sscanf(s, "%d:%15[A-Z]%n", &at, name, &n) < 2) break;
        s += n;
        if (*s == '/') { n = 0; sscanf(s + 1, "%d%n", &hold, &n); s += 1 + n; }
        tin[ntin].at = at; tin[ntin].hold = hold; tin[ntin].button = 0; tin[ntin].stick = -1;
        for (int b = 0; b < 10; b++) if (!strcmp(name, names[b])) tin[ntin].button = 1ull << b;
        for (int k = 0; k < 4; k++) if (!strcmp(name, sticks[k])) tin[ntin].stick = k;
        if (tin[ntin].button || tin[ntin].stick >= 0) ntin++;
        if (*s == ',') s++;
    }
    fprintf(stderr, "[rb] test input on wall time: %d presses, every %d frames\n", ntin, tin_period);
}

static void tape_clear(void) {
    AcquireSRWLockExclusive(&tape_lock);
    for (int i = 0; i < 8; i++) if (tape[i].rec) memset(tape[i].rec, 0, sizeof(PadRec) * tape[i].cap);
    ReleaseSRWLockExclusive(&tape_lock);
    rb_sampling_base = rb_sampling + 1000;
}

static double seg_poll, seg_fe_in, seg_fe_out;   /* (the profile of re-run frames: this frame's marks) */
static int net_pad_for(u32 id, u32 key, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt);
/* (net mode: from a match set-up until the fight, players 1 and 2 hold nothing: in the entrance and countdown
 * each PC would read its own controllers in both slots - different devices, not exchanged.) */
static volatile LONG net_countdown;
static int net_n;   /* (players in net mode: defined with the net mode's state below) */
/* SSBU_RB_NET_EARLY=1: the extras made while two PCs' fights started apart for another reason (twopc2-5, the
 * lobbies wired differently): dropped refreshes cleared outside fights too, and the fixed clock from the match
 * set-up. Off by default from twopc13 on (to be shown unneeded). */
static int net_early = -1;
static int net_early_on(void) { if (net_early < 0) { const char *e = getenv("SSBU_RB_NET_EARLY"); net_early = e && *e == '1'; } return net_early; }
static int net_local;                 /* (this game's player: defined with the net mode's state below) */
volatile LONG net_results;            /* (the results screen: see net_results_frame) */
volatile LONG net_menu_mine;          /* (this game's player's A and B there, as rb_pad read them) */
volatile LONG net_menu_remote[8];     /* (each other player's, from its game) */
void rb_pad(u32 id, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt) {
    if (net_countdown && !S.in_fight && id < (u32)net_n) { *buttons = 0; *lx = *ly = *rx = *ry = *lt = *rt = 0; return; }
    if (net_results && !S.in_fight && id < (u32)net_n) {   /* (the results screen: this player's A and B on every card) */
        if ((int)id == net_local) InterlockedExchange(&net_menu_mine, (LONG)(*buttons & 3));
        else { *buttons = (u64)(net_menu_mine | net_menu_remote[id]); *lx = *ly = *rx = *ry = *lt = *rt = 0; }
        return;
    }
    if (!rb_on || !S.in_fight || id >= 8) return;
    if (id == 0 && !seg_poll) seg_poll = now_ms();
    if (ntin < 0) test_input_parse();
    if (tape_on < 0) { const char *e = getenv("SSBU_RB_TAPE"); tape_on = !(e && *e == '0'); }
    if (id == 0 && ntin > 0) {
        int f = (int)(rt_now_ns() / 16666667ull % (u64)tin_period);
        for (int i = 0; i < ntin; i++) {
            if (f < tin[i].at || f >= tin[i].at + tin[i].hold) continue;
            *buttons |= tin[i].button;
            switch (tin[i].stick) {
            case 0: *lx = -32767; break;
            case 1: *ly = 32767; break;
            case 2: *lx = 32767; break;
            case 3: *ly = -32767; break;
            }
        }
    }
    if (net_pad_for(id, ld32(LOGIC_FRAME), buttons, lx, ly, rx, ry, lt, rt)) return;   /* (loop mode: the bots, M5) */
    if (!tape_on) return;
    u32 key = ld32(LOGIC_FRAME);
    if (key >= (1u << 20)) return;
    AcquireSRWLockExclusive(&tape_lock);
    if (key >= tape[id].cap) {
        u32 cap = tape[id].cap ? tape[id].cap : 4096;
        while (cap <= key) cap *= 2;
        PadRec *r = realloc(tape[id].rec, sizeof(PadRec) * cap);
        if (r) { memset(r + tape[id].cap, 0, sizeof(PadRec) * (cap - tape[id].cap)); tape[id].rec = r; tape[id].cap = cap; }
    }
    if (key < tape[id].cap) {
        PadRec *r = &tape[id].rec[key];
        if (r->have) {
            *buttons = r->buttons; *lx = r->lx; *ly = r->ly; *rx = r->rx; *ry = r->ry; *lt = r->lt; *rt = r->rt;
            tape_replayed++;
        } else {
            r->have = 1; r->buttons = *buttons; r->lx = *lx; r->ly = *ly; r->rx = *rx; r->ry = *ry; r->lt = *lt; r->rt = *rt;
            tape_recorded++;
        }
    }
    ReleaseSRWLockExclusive(&tape_lock);
    {   /* the first reads in a fight: which thread, which logic frame, at which safe point */
        static int shown; static long long fight_shown;
        if (fight_shown != S.fights) { fight_shown = S.fights; shown = 0; }
        if (id == 0 && shown < 12) {
            shown++;
            fprintf(stderr, "[rb] pad read: player 1, thread %lu, logic frame %u, fight frame %lld, buttons %#llx\n",
                    GetCurrentThreadId(), key, S.frames, (unsigned long long)*buttons);
        }
    }
}

/* In a fight: the logic frame on from a base above every number given before (monotonic). */
int rb_pad_sampling(u64 *sampling) {
    if (!rb_on) return 0;
    if (!S.in_fight) { *sampling = (u64)InterlockedIncrement64(&rb_sampling); return 1; }
    *sampling = (u64)rb_sampling_base + ld32(LOGIC_FRAME);
    if ((LONG64)*sampling > rb_sampling) InterlockedExchange64(&rb_sampling, (LONG64)*sampling);   /* (after the fight: on from here) */
    return 1;
}

/* ---- the fight hash: what the fight itself is, per frame -------------------------------
 * At each safe point of the replay check (and of every fight frame with SSBU_RB_HASH=1, one
 * line per fighter in build\rb_hash.txt), each fighter's state is read through the game's own
 * exported getters, as ssbu-recomp's fighter_host.c SSBU_FIGHT_HASH reads it: entry slot ->
 * app::fighter::get_id_from_entry_id -> app::sv_battle_object::module_accessor (the "boma"),
 * then status kind, situation, motion kind and frame, position, facing, speed and damage. Plus
 * the 9 script random generators (app::sv_math::rand: 9 x 20 bytes, [[[0x71052c35b0]]], 13.0.4
 * 0x71052c25b0 one page on as all of main's data). The calls run on the main thread at the
 * safe point, its registers kept around them (they use the guest stack below its pointer). */
typedef struct FRec { int entry, st, sit, lr; u64 mot; float mf, x, y, sx, sy, dmg; } FRec;
typedef struct FFrame { u32 n; u32 rng_calls; u64 rng; u64 h; FRec f[8]; } FFrame;
#define LB(s) "_ZN3app8lua_bind" s "EPNS_26BattleObjectModuleAccessorE"
static u64 hfnv(u64 h, const void *p, size_t n) { const u8 *b = p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static int readable(u64 a, u64 n) {
    MEMORY_BASIC_INFORMATION mi;
    return a && VirtualQuery((void *)(uintptr_t)G2H(a), &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && G2H(a) + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
/* (M6: the chain is found once a fight, then only followed: each VirtualQuery costs PC 2 some 5 ms over this
 * build's write-watched areas, and a fight hash asked 8 of them, 40 ms a hash there against 4 here (twopc19).
 * The pointers read the same as when they were checked: the same memory, still there, since nothing in a fight
 * gives the generators up.) */
double rb_rng_slow_ms; long long rb_rng_slow_n;
double rb_ff_get_ms; long long rb_ff_get_n;   /* (fight_frame's calls into the game: the getters) */
static u64 rng_base(void) {
    static long long fight = -1;
    static u64 chain[3];
    u64 p = 0x71052c35b0ull;
    if (fight == S.fights && chain[2] && ld64(p) == chain[0] && ld64(chain[0]) == chain[1] && ld64(chain[1]) == chain[2]) return chain[2];
    LARGE_INTEGER q0, q1, fq; QueryPerformanceCounter(&q0);
    u64 out = 0;
    for (int i = 0; i < 3; i++) { if (!readable(p, 8)) goto done; p = ld64(p); chain[i] = p; }
    if (readable(p, 180)) out = p;
done:
    fight = out ? S.fights : -1;
    if (!out) chain[2] = 0;
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
    rb_rng_slow_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart; rb_rng_slow_n++;
    return out;
}
static float getf(Cpu *c, const char *sym, u64 boma, int n, u64 arg) {
    rt_callsym(c, sym, n, boma, arg);
    u32 b = (u32)c->v[0].d[0]; float f; memcpy(&f, &b, 4); return f;
}
static void fight_frame(Cpu *c, FFrame *o) {
    static Cpu keep;   /* (the main thread's only) */
    keep = *c;
    memset(o, 0, sizeof *o);
    double t0 = now_ms();
    for (int e = 0; e < 8; e++) {
        u32 id = (u32)rt_callsym(c, "_ZN3app7fighter20get_id_from_entry_idEi", 1, (u64)e);
        if (id == 0x50000000u) continue;
        u64 boma = rt_callsym(c, "_ZN3app16sv_battle_object15module_accessorEj", 1, (u64)id);
        if (!boma) continue;
        FRec *r = &o->f[o->n++];
        r->entry = e;
        r->st = (int)rt_callsym(c, LB("30StatusModule__status_kind_impl"), 1, boma);
        r->sit = (int)rt_callsym(c, LB("33StatusModule__situation_kind_impl"), 1, boma);
        r->mot = rt_callsym(c, LB("30MotionModule__motion_kind_impl"), 1, boma);
        r->mf = getf(c, LB("24MotionModule__frame_impl"), boma, 1, 0);
        r->x = getf(c, LB("25PostureModule__pos_x_impl"), boma, 1, 0);
        r->y = getf(c, LB("25PostureModule__pos_y_impl"), boma, 1, 0);
        float lr = getf(c, LB("22PostureModule__lr_impl"), boma, 1, 0); r->lr = lr < 0 ? -1 : 1;
        r->sx = getf(c, LB("35KineticModule__get_sum_speed_x_impl") "i", boma, 2, (u64)(u32)-1);
        r->sy = getf(c, LB("35KineticModule__get_sum_speed_y_impl") "i", boma, 2, (u64)(u32)-1);
        r->dmg = getf(c, LB("25DamageModule__damage_impl") "i", boma, 2, 0);
        if (o->n == 8) break;
    }
    rb_ff_get_ms += now_ms() - t0; rb_ff_get_n++;
    *c = keep;
    u64 rb = rng_base();
    o->rng = 0xcbf29ce484222325ull;
    if (rb) { u32 w[45]; for (int i = 0; i < 45; i++) w[i] = ld32(rb + 4ull * i); for (int i = 0; i < 9; i++) o->rng_calls += w[i * 5 + 4]; o->rng = hfnv(o->rng, w, sizeof w); }
    o->h = hfnv(hfnv(0xcbf29ce484222325ull, o->f, sizeof(FRec) * o->n), &o->rng, 8);
}
static void frec_text(char *s, size_t n, const FRec *r) {
    snprintf(s, n, "P%d st %#x sit %d mot %#llx mf %.3f pos %.4f,%.4f lr %d speed %.4f,%.4f dmg %.2f%%",
             r->entry + 1, r->st, r->sit, (unsigned long long)r->mot, r->mf, r->x, r->y, r->lr, r->sx, r->sy, r->dmg);
}
static FFrame *ref_frames;      /* the reference run's frames 1..k */
static long long hash_first_bad;
static FILE *hash_out;
static void fight_hash(Cpu *c, long long verify_frame) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("SSBU_RB_HASH"); all = e && *e == '1'; }
    if (!all && verify_frame <= 0) return;
    FFrame f;
    fight_frame(c, &f);
    if (all) {
        if (!hash_out) {
            hash_out = fopen("build\\rb_hash.txt", "w");
            fprintf(stderr, "[rb] fight hash: every fight frame, one line per fighter, in build\\rb_hash.txt; script random generators at %#llx\n", rng_base());
        }
        if (hash_out) {
            char t[256];
            for (u32 i = 0; i < f.n; i++) { frec_text(t, sizeof t, &f.f[i]); fprintf(hash_out, "F %lld %s rng %u.%08llx H %016llx\n", S.frames, t, f.rng_calls, (unsigned long long)(f.rng & 0xffffffff), (unsigned long long)f.h); }
            if (!(S.frames & 63)) fflush(hash_out);
        }
    }
    if (verify_frame <= 0 || verify_frame > ver_k) return;
    if (ver_phase == 1) {
        if (!ref_frames) ref_frames = calloc((size_t)ver_k + 1, sizeof *ref_frames);
        if (ref_frames) ref_frames[verify_frame] = f;
        if (verify_frame == 1) fprintf(stderr, "[rb] fight hash: %u fighters, script random generators %s\n", f.n, rng_base() ? "found" : "NOT found");
        return;
    }
    if (!ref_frames || hash_first_bad) return;
    const FFrame *r = &ref_frames[verify_frame];
    if (r->h == f.h) return;
    hash_first_bad = verify_frame;
    fprintf(stderr, "[rb] fight hash: FIRST DIFFERENCE at frame %lld after the load (reference %016llx, replay %016llx; generators %u calls %s)\n",
            verify_frame, (unsigned long long)r->h, (unsigned long long)f.h, f.rng_calls, r->rng == f.rng ? "same" : "DIFFER");
    char a[256], b[256];
    for (u32 i = 0; i < f.n || i < r->n; i++) {
        frec_text(a, sizeof a, &r->f[i]); frec_text(b, sizeof b, &f.f[i]);
        if (memcmp(&r->f[i], &f.f[i], sizeof(FRec))) fprintf(stderr, "[rb]   reference: %s\n[rb]   replay:    %s\n", a, b);
    }
}

extern void *rb_main_kthread;
extern volatile u64 rb_main_wait_ns, rb_main_waits, rb_vsync_gpu_wait_qpc, rb_vsync_gpu_waits;
static void tree_report(void);
static double seg_sum[4];
static long long seg_n;
static void seg_report(void) {
    if (!seg_n) return;
    fprintf(stderr, "[rb] profile, re-run frames by part (%lld): safe point -> pad poll %.2f ms, poll -> frame end (logic) %.2f, "
            "frame end call %.2f, frame end -> safe point %.2f (means)\n", seg_n, seg_sum[0] / seg_n, seg_sum[1] / seg_n, seg_sum[2] / seg_n, seg_sum[3] / seg_n);
}
static void fp_waits_report(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    long long frames = fp_n[0] + fp_n[1];
    if (!frames) return;
    fprintf(stderr, "[rb] profile, frames run again (%lld): the main thread asleep in kwait %.2f ms a frame (%.1f waits); "
            "the vsync thread waiting for the GPU %.2f ms a frame (%llu waits)\n", frames,
            rb_main_wait_ns / 1e6 / frames, (double)rb_main_waits / frames,
            rb_vsync_gpu_wait_qpc * 1000.0 / (double)f.QuadPart / frames, (unsigned long long)rb_vsync_gpu_waits);
}
static void fp_report(int k) {
    double n = (double)fp_n[k];
    fprintf(stderr, "[rb] profile, %s frames run again (%lld): game %.2f ms, census wait %.2f, save: hold + GPU drain %.2f, memory %.2f, "
            "the rest %.2f; the test's fight hash %.2f; other safe point work %.2f (means)\n", k ? "the last" : "lite", fp_n[k],
            fp_sum[k][0] / n, fp_sum[k][1] / n, fp_sum[k][2] / n, fp_sum[k][3] / n, fp_sum[k][4] / n, fp_sum[k][6] / n, fp_sum[k][5] / n);
}
/* SSBU_RB_RING_ROTATE=1: the loads go back 1, 2 .. <depth> frames in turn, with the catch-up
 * timed for each depth. */
static int ring_rotate = -1, cur_depth;
static double depth_catch[65], depth_load[65], depth_rerun[65];
static long long depth_n[65];
/* SSBU_RB_RERUN_SAVE=<n>: of the frames a catch-up runs again, every n-th is saved, and the last
 * (default 3; 1: all of them; 0: only the last). A later load back to an unsaved one takes the newest
 * state before it and runs the rest again: at most n - 1 frames more. States are found by their
 * logic frame (rb_save_tag / rb_ring_find), not by how many saves back. */
static int rerun_save = -1;
static LONG pending_fast, catch_total;
extern u32 rb_save_tag;
int rb_ring_find(u32 target, u32 *tag);
u32 rb_ring_tag(int age);
extern long long rb_fiber_refused, rb_fiber_retried_ok;   /* (hle_rb_state.c, M7) */
void rb_keep_hold_next(void);
void rb_release_held(void);
static void ring_test(Cpu *c, u64 frame) {
    if (fast_on < 0) { const char *e = getenv("SSBU_RB_FAST"); fast_on = !(e && *e == '0'); }
    if (ring_rotate < 0) { const char *e = getenv("SSBU_RB_RING_ROTATE"); ring_rotate = e && *e == '1'; }
    if (rb_fast > 0 && InterlockedDecrement(&rb_fast) == 0) {   /* caught up: the load and every frame since */
        double ms = now_ms() - ring_load_t0;
        caught_up++; catchup_ms += ms; if (ms > catchup_max) catchup_max = ms;
        if (cur_depth > 0 && cur_depth <= 64) { depth_catch[cur_depth] += ms; depth_n[cur_depth]++; }
    }
    /* this frame's state, against its first pass */
    if (!ring_first) ring_first = calloc(RING_FRAMES, sizeof *ring_first);
    u32 key = ld32(LOGIC_FRAME);
    FFrame f;
    /* (its getter calls cost some 5 ms a frame: in the middle of a catch-up only with
     * SSBU_RB_HASHALL=1; by default the last re-run frame and every normal one are checked) */
    static int hash_all = -1;
    if (hash_all < 0) { const char *e = getenv("SSBU_RB_HASHALL"); hash_all = e && *e == '1'; }
    int hash_now = hash_all || rb_fast <= 0;
    double th = now_ms();
    if (hash_now) fight_frame(c, &f);
    fp_hash_last = now_ms() - th;
    if (hash_now && ring_first && key < RING_FRAMES) {
        FFrame *r = &ring_first[key];
        if (!r->h) *r = f;
        else if (r->h == f.h) ring_same++;
        else if (++ring_diff <= 5) {
            char a[256], b[256];
            fprintf(stderr, "[rb] ring: logic frame %u runs again DIFFERENT (first %016llx, now %016llx; generators %s)\n",
                    key, (unsigned long long)r->h, (unsigned long long)f.h, r->rng == f.rng ? "same" : "DIFFER");
            for (u32 i = 0; i < f.n || i < r->n; i++) {
                frec_text(a, sizeof a, &r->f[i]); frec_text(b, sizeof b, &f.f[i]);
                if (memcmp(&r->f[i], &f.f[i], sizeof(FRec))) fprintf(stderr, "[rb]   first: %s\n[rb]   again: %s\n", a, b);
            }
        }
    }
    if (ring_left <= 0) {   /* done: a line, then nothing more */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[rb] ring test done: %lld loads, %lld saves; frames run again: %lld the same, %lld DIFFERENT; "
                    "save %.2f ms mean (max %.1f), load %.2f ms mean (max %.1f)\n", ring_loads, ring_saves, ring_same, ring_diff,
                    ring_saves ? ring_save_ms / ring_saves : 0, ring_save_max, ring_loads ? ring_load_ms / ring_loads : 0, ring_load_max);
            if (caught_up)
                fprintf(stderr, "[rb] ring test: back %lld frames and caught up again, unseen: %.1f ms mean (max %.1f) over %lld loads, "
                        "%.1f ms a frame run again (with its save); left out of the re-run frames: %lld kicks (mask %d), %lld presentation calls (mask %d)\n",
                        ring_depth, catchup_ms / caught_up, catchup_max, caught_up,
                        (catchup_ms / caught_up - (ring_loads ? ring_load_ms / ring_loads : 0)) / ring_depth, kicks_skipped, skip_mask, lite_skipped, lite_mask);
            for (int k = 0; k < 2; k++)
                if (fp_n[k])
                    fp_report(k);
            fp_waits_report();
            tree_report();
            seg_report();
            { void rb_stamp_report(void); rb_stamp_report(); }
            for (int d = 1; d <= 64; d++)
                if (depth_n[d])
                    fprintf(stderr, "[rb] ring test: back %d and caught up: %.1f ms mean over %lld loads (the load %.1f ms; %.1f frames run again)\n",
                            d, depth_catch[d] / depth_n[d], depth_n[d], depth_load[d] / depth_n[d], depth_rerun[d] / depth_n[d]);
        }
        return;
    }
    if (rerun_save < 0) { const char *e = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = e && *e ? atoi(e) : 3; }
    if (rb_fast > 0) {   /* a frame in the middle of a catch-up: saved if it is an n-th; no load */
        LONG done = catch_total - rb_fast;   /* (re-run frames finished, this one included) */
        if (rerun_save != 1 && (rerun_save == 0 || done % rerun_save)) { rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; return; }
    }
    double t0 = now_ms();
    rb_save_tag = key;
    /* (a load will follow this save at once if it is time and no catch-up runs: keep the hold) */
    static int keep_hold_on = -1;
    if (keep_hold_on < 0) { const char *e = getenv("SSBU_RB_KEEPHOLD"); keep_hold_on = !(e && *e == '0'); }
    if (keep_hold_on && ring_since + 1 >= ring_every && rb_fast <= 0) rb_keep_hold_next();
    int r = rb_save(c, frame);
    if (r == 2) {   /* a load came back here: the state `ring_depth` frames back */
        double ms = now_ms() - ring_load_t0;
        ring_load_ms += ms; if (ms > ring_load_max) ring_load_max = ms;
        if (cur_depth > 0 && cur_depth <= 64) depth_load[cur_depth] += ms;
        if (cur_depth > 0 && cur_depth <= 64) depth_rerun[cur_depth] += pending_fast;
        catch_total = pending_fast;
        if (fast_on) rb_fast = pending_fast;   /* the frames back to where the game was: fast and unseen */
        return;
    }
    if (r == 1) {
        double ms = now_ms() - t0;
        ring_saves++; ring_save_ms += ms; if (ms > ring_save_max) ring_save_max = ms;
    }
    int depth = ring_rotate ? 1 + (int)(ring_loads % ring_depth) : (int)ring_depth;
    u32 found = 0;
    int age = key >= (u32)depth ? rb_ring_find(key - (u32)depth, &found) : -1;
    int loading = ++ring_since >= ring_every && age >= 0 && rb_fast <= 0;
    if (!loading) rb_release_held();
    if (loading) {
        ring_since = 0; ring_left--; ring_loads++;
        cur_depth = depth;
        if (!(ring_loads % 25))
            fprintf(stderr, "[rb] ring: %lld loads; frames run again so far: %lld the same, %lld DIFFERENT; save %.2f ms mean (max %.1f), load %.2f ms mean (max %.1f); "
                    "caught up %.1f ms mean (max %.1f)\n",
                    ring_loads, ring_same, ring_diff, ring_save_ms / ring_saves, ring_save_max, ring_loads > 1 ? ring_load_ms / (ring_loads - 1) : 0, ring_load_max,
                    caught_up ? catchup_ms / caught_up : 0, catchup_max);
        ring_load_t0 = now_ms();
        fp_loaded = 1;
        {   static int profiled; const char *e = getenv("SSBU_RB_PROFILE");
            if (!profiled && e && atof(e) > 0) { profiled = 1; profile_start_gated(atof(e)); } }
        pending_fast = (LONG)(key - found);
        rb_load_age(c, frame, age);   /* (returns only if refused) */
        rb_release_held();
        ring_loads--; ring_left++;
    }
}

/* ---- M5 step 2: loop mode -----------------------------------------------------------------
 * SSBU_RB_NET=loop[,<delay>,<lag min>,<lag max>,<loss %>,<frames>] (defaults 2,1,6,10,1800): one game,
 * both players driven by bots by logic frame (net_pad: the same pads every run), player 2's arriving
 * through rb_net.c as if from another PC: the pad for frame F is read there at F - <delay> and gets
 * here <lag> frames later (each frame's lag drawn from F, between min and max; <loss> % lost, so the
 * next message brings it, 1-3 frames on), on wall time from the fight's start. Until then player 2
 * plays a guess (its last pad, held); a wrong guess loads the newest state at or before the frame
 * before it and runs the frames since again, fast and unseen, with the pads known now. A frame past
 * the 7 a rollback reaches waits for the pads instead. <lag max> -1: every pad known at once (no
 * rollbacks: the reference run). For the first <frames> fight frames each frame's fight hash, as it
 * stands once its pads are final, goes to build\rb_loop_<lag max>.txt: two runs (one with lag, one
 * with -1) compared frame by frame say whether rolling back gave the same fight. Both players must
 * be humans at character select (port 2 too: any controller), not CPUs; hands off. */
#include "hle_rb_net.h"
static int net_mode = -1, net_delay = 2, net_lag_lo = 1, net_lag_hi = 6, net_loss = 10, net_frames = 1800;
static u32 net_key0;            /* the logic frame the fight started at */
static double net_t0;           /* and when (ms) */
static u8 *net_got;             /* by frame - net_key0: delivered */
static u32 net_low;             /* every frame below this delivered */
static u64 *net_final;          /* by frame - net_key0: the fight hash once final (0: none) */
static long long net_rollbacks, net_rerun, net_waits, net_too_deep, net_depth[16], net_saves_made, net_saves_skipped;
static long long net_older;   /* (M7: rollbacks done from an older state, the newest refused) */
static double net_catch_ms, net_catch_max, net_wait_ms;
static int net_done;
static long long net_reads[8];   /* (pad reads by slot since the last line, the diagnostic once a second) */
static u32 net_last_key[8];
static RbnPad net_last_pad[RBN_PLAYERS];
/* ---- M5 step 3: net mode, two games ---------------------------------------------------------
 * SSBU_RB_NET=net,<local player 0|1>,<my port>,<peer port>[,<delay>[,<frames>[,<peer host>]]]: two games
 * (on one PC: two instances, ports swapped), each driving its own player with its bot (net_pad) and
 * getting the other's pads from the other game: rb_net PADS messages over UDP (hle_rb_link.c), every
 * normal frame, with every pad the peer hasn't acknowledged. Each frame's fight hash, once its pads
 * are final, goes into the messages, and the peer's are compared with this game's own: a desync is
 * found, not assumed away. A frame too far past the peer's pads waits for them (up to 10 min: the other
 * game may still be loading, or its fight not started yet). Both games need the same seed (SSBU_RB_SEED), the same characters in
 * the same ports, the same stage; the starting sides are still random (step 3b): a start on different
 * sides shows up as a desync at once. */
static int net_local, net_port, net_peer_port;
static u64 hs_seed;    /* (the match seed of the handshake, below savestates) */
/* SSBU_RB_NET_CHECK_EVERY=<n>: the fight hash (some 5 ms of getter calls) only on logic frames
 * that are multiples of n (default 10), in both games alike; the others go unhashed and uncompared. The hash every
 * frame made the test's window play in slow motion ("responsive once the logger stopped"). */
static int net_every = -1;
/* SSBU_RB_NET_LIVE=1 (M5 step 4): this game's player is played by its own controllers (the pad of its
 * slot as hle_hid.c reads it: pad, keyboard), not by the bot: the newest live read is this game's pad for
 * the frame `delay` on, sent to the peer like the bot's. The other player's slot ignores the controllers
 * here (they may reach both windows): only the peer's pads count. */
static int net_live = -1;
static RbnPad net_live_pad;
static char net_host[64] = "127.0.0.1";
/* M7, 3+ players: what is kept of each other player (by player index; this game's own unused). Through the host:
 * player 1's game is the host, every other game talks to it only, and it passes each player's PADS and CHOICE on to
 * the rest (net_receive, net_choice_take). */
static int net_n = 2;                       /* players (the host's SSBU_RB_NET_PLAYERS; the others take it from its hello) */
static u32 net_acked_by[RBN_PLAYERS];      /* the newest frame of our pads each player has all of */
static u32 net_heard[RBN_PLAYERS];         /* the newest frame each player said it was running */
static u32 net_checked;                    /* (frames compared up to here) */
static u32 *net_peer_check[RBN_PLAYERS];   /* by frame - net_key0: each player's hash (low 32 bits); 0: none */
static long long net_checks, net_desyncs, net_msgs_in, net_msgs_out, net_sync_waits;
static long long net_first_desync = -1;
static long long net_link_base[3];   /* (the link's counts at the fight's start: the report is per fight) */
/* M6, the false desyncs at KO launches (twopc18 frame 2100, twopc19 frame 4430): PC 1 held two different hashes for
 * one frame (one sent, one its own later), neither the other PC's, while both fights went on alike. So a frame run
 * again can come out different from its first pass even with every pad the same: which part, is to see. Each
 * hashed frame is kept (its fighters and the generators' words) with whether its pads were all real then
 * (final); when a final frame is hashed again after a rollback and differs, both passes go to the log, field by
 * field. A DESYNC report shows the frame's fields too, to set beside the other PC's log. */
typedef struct CheckRec { u32 key; int final; u64 h; FFrame f; u32 w[45]; } CheckRec;
static CheckRec check_recs[64];
static long long net_again, net_again_bad;
static void check_words(u32 *w) { u64 rb = rng_base(); for (int k = 0; k < 45; k++) w[k] = rb ? ld32(rb + 4ull * k) : 0; }
static void check_show(const char *what, const CheckRec *r) {
    char t[256];
    for (u32 k = 0; k < r->f.n; k++) { frec_text(t, sizeof t, &r->f.f[k]); fprintf(stderr, "[rb]   %s: %s\n", what, t); }
    char g[512]; size_t n = 0;
    for (int q = 0; q < 9; q++)
        n += snprintf(g + n, sizeof g - n, " %d:%08x %08x %08x %08x #%u%s", q, r->w[q * 5], r->w[q * 5 + 1], r->w[q * 5 + 2], r->w[q * 5 + 3], r->w[q * 5 + 4],
                      q == 1 ? "(not hashed)" : "");
    fprintf(stderr, "[rb]   %s: generators%s\n", what, g);
}
/* the frame `key` was just hashed (h): kept, or, if it was final before and differs now, both passes logged */
static void check_again(u32 key, const FFrame *f, u64 h) {
    CheckRec *r = &check_recs[(key / (u32)(net_every > 0 ? net_every : 10)) % 64];
    if (r->key == key && r->final) {
        net_again++;
        if (r->h != h && ++net_again_bad <= 8) {
            CheckRec now = {key, 1, h, *f, {0}};
            check_words(now.w);
            fprintf(stderr, "[rb] net check: logic frame %u, its pads all real, ran again after a rollback and came out DIFFERENT (%08x, first %08x):\n",
                    key, (u32)h, (u32)r->h);
            check_show("first pass", r);
            check_show("run again ", &now);
        }
        return;   /* (the first final pass is what the other PC got) */
    }
    r->key = key; r->h = h; r->f = *f;
    r->final = rbn_confirmed() + 1 >= key;   /* (the old rule, on purpose: a frame final by it that changes shows what follows its own pads) */
    check_words(r->w);
}
static void check_desync_show(u32 key) {
    CheckRec *r = &check_recs[(key / (u32)(net_every > 0 ? net_every : 10)) % 64];
    if (r->key == key) { fprintf(stderr, "[rb]   (this game's frame %u, %s when hashed:)\n", key, r->final ? "its pads all real" : "on a guess"); check_show("ours", r); }
}
/* M6, time sync: a hold for the frame advantage leaves one refresh out (ipc_vi.c), so it costs the game a frame
 * for sure; SSBU_RB_NET_HOLD=sleep: Sleep(16) at the safe point, as before (twopc15: it did not hold). The
 * round trip comes from the acks: when the peer first says it has our pad for frame f, against when that pad
 * first went out (on top of the real trip: up to a frame of the peer's sending and of ours polling, so the
 * second's least is the one to read). Once a second the log says where both games are (net sync lines). */
extern volatile LONG rb_vsync_skip;
extern volatile LONG64 rb_vsync_skipped;
static int net_hold_sleep = -1;
static double net_sent_ms[RBN_RING]; static u32 net_sent_at[RBN_RING];
static double net_rtt_min_s = 1e9, net_rtt_sum; static long long net_rtt_n;
static int32_t net_adv_ours, net_adv_peer;
static long long net_bytes_s, net_msgs_s, net_bytes_max_s;   /* (the PADS messages sent this second: bytes, count, the longest) */
/* (M6: the rollback's own work on the frames of this second, to tell it from the game's: the fight hash every
 * net_every frames, the saves, the once-a-second fighter line; sum, worst, count) */
static double net_w_ms[3], net_w_max[3]; static long long net_w_n[3];
static void net_work(int k, double t0) { double ms = now_ms() - t0; net_w_ms[k] += ms; if (ms > net_w_max[k]) net_w_max[k] = ms; net_w_n[k]++; }
int rbl_open(int local, int my_port, const char *host, int peer_port);
void rbl_players(int n);
void rbl_send(const void *buf, int len);
void rbl_send_to(int p, const void *buf, int len);
int rbl_recv(void *buf, int cap);
void rbl_stats(long long *s, long long *r, long long *e);
int rbl_accept(int p);
int rbl_has_peer(int p);
/* While a set-up waits for the other games the main thread draws nothing, so the game looks frozen: say what it
 * waits for (the game window's title, and build\rollback_status.txt, which the Host/Join window shows, on top) and
 * keep the freeze watchdog quiet (a known wait, not a hang). NULL: the wait is over. */
void watchdog_present(void);    /* debug_watchdog.c */
void gfx_set_status(const char *text);   /* gpu_d3d11.c */
static void net_wait_say(const char *what) {
    static double last; static int shown;
    double t = now_ms();
    if (what) watchdog_present();
    if (what && t - last < 500) return;
    if (!what && !shown) return;
    last = t; shown = what != NULL;
    char s[160] = "";
    if (what && net_local == 0) {
        int k = 1;
        for (int p = 1; p < net_n; p++) k += rbl_has_peer(p) ? 1 : 0;
        snprintf(s, sizeof s, "%s (%d of %d players here)", what, k, net_n);
    } else if (what) snprintf(s, sizeof s, "%s", what);
    gfx_set_status(s);
    FILE *f = fopen("build\\rollback_status.txt", "w");
    if (f) { fputs(s, f); fclose(f); }
}
int rbl_who(void);
void rbl_assign(int p);
long long rbl_dropped(void);
double rbl_last_arrival_ms(void);
static void net_hello_send(void);
/* M6, the round trip and the delay agreed in the handshake (HELLO version 2). SSBU_RB_NET's delay -1 (the
 * launcher's RB_DELAY=auto): player 1's game picks it from the round trip (the least of the first
 * HS_RTT_SAMPLES, taken over HS_RTT_MS at least), and only then proposes the seed; player 2's game takes
 * player 1's delay always, so the two never differ. (inet1: the first 12, 2 s of a phone link just woken, had
 * 112.6 ms at least and gave delay 3; later the least was ~85 ms, delay 2.) */
#define HS_RTT_SAMPLES 30
#define HS_RTT_MS 5000.0
static int hs_delay_ready;                 /* (player 1: net_delay is chosen; player 2: taken from player 1) */
static u64 hs_peer_t[RBN_PLAYERS], hs_peer_t_at[RBN_PLAYERS];   /* each player's newest hello clock, and when (our us) it came */
static double hs_rtt_min[RBN_PLAYERS], hs_rtt_t0[RBN_PLAYERS]; static int hs_rtt_n[RBN_PLAYERS];   /* (the least: once n > 0) */
static int hs_agreed_by[RBN_PLAYERS];      /* (the host: which players have the seed) */
static u64 now_us(void) { return (u64)(now_ms() * 1000.0); }
/* (hle_hid.c) the other PC's player's controller slot, in net mode: connected here as a Pro Controller always */
int rb_slot_remote(u32 id) { return net_mode == 2 && net_local >= 0 && id < (u32)net_n && id != (u32)net_local; }
/* (hle_hid.c, Host/Join) which of this PC's controller slots the game's slot `id` reads: a joining game's controller and
 * keyboard are its player 1 here, as on every PC; once the host gives it player N, the game's slot N reads them, and
 * its slot 1 (the host's player: a remote slot) the empty slot N */
u32 rb_slot_phys(u32 id) {
    if (net_mode != 2 || net_local <= 0) return id;
    if (id == (u32)net_local) return 0;
    if (id == 0) return (u32)net_local;
    return id;
}
/* M7, Host/Join: a joining game starts with no number (net_local -1) and a random join id; it says hello with player
 * -1 and the id, and the host gives it the lowest free player (rbl_assign), says which in every hello to it after (assign),
 * and knows it again by the id when its address changes. A slot whose game has said nothing for 5 s outside a fight goes
 * to a newcomer (a friend whose game started again: another port, another id). */
static u64 hs_join_id;                     /* (a joiner: its id) */
static u64 hs_slot_id[RBN_PLAYERS];        /* (the host: each player's join id) */
static double hs_heard_ms[RBN_PLAYERS];    /* (the host: when each player was last heard) */
static int hs_agreed;
static void net_forget_player(int p) {      /* (the host: a slot given to another game) */
    hs_rtt_n[p] = 0; hs_rtt_min[p] = 0; hs_peer_t[p] = 0; hs_agreed_by[p] = 0; hs_slot_id[p] = 0;
    hs_agreed = 0;   /* (the newcomer must take the seed too) */
}
static int net_join(u64 id) {
    int p = rbl_who();
    if (p >= 1 && p < net_n) { if (id) hs_slot_id[p] = id; return p; }   /* (this address's already) */
    for (int q = 1; q < net_n; q++)
        if (id && hs_slot_id[q] == id) { rbl_assign(q); return q; }      /* (the same game from another port) */
    for (int q = 1; q < net_n; q++)
        if (!rbl_has_peer(q)) {
            rbl_assign(q); net_forget_player(q); hs_slot_id[q] = id; hs_heard_ms[q] = now_ms();
            fprintf(stderr, "[rb] handshake: a game joined: it is player %d of %d\n", q + 1, net_n);
            return q;
        }
    if (!S.in_fight)
        for (int q = 1; q < net_n; q++)
            if (now_ms() - hs_heard_ms[q] > 5000) {
                rbl_assign(q); net_forget_player(q); hs_slot_id[q] = id; hs_heard_ms[q] = now_ms();
                fprintf(stderr, "[rb] handshake: player %d's game went quiet: a new joiner takes its place\n", q + 1);
                return q;
            }
    static int shown;
    if (!shown++) fprintf(stderr, "[rb] handshake: a game asked to join, but all %d players are here\n", net_n);
    return -1;
}
/* (a joiner) the host says this game is player p (of `players`) */
static void net_take_number(int p, int players) {
    if (p < 1 || p >= RBN_PLAYERS || S.in_fight) return;
    if (players >= 2 && players <= RBN_PLAYERS) net_n = players;
    if (net_local == p) return;
    if (net_local > 0) fprintf(stderr, "[rb] handshake: the host now says this game is player %d (was %d)\n", p + 1, net_local + 1);
    else fprintf(stderr, "[rb] handshake: the host made this game player %d of %d\n", p + 1, net_n);
    net_local = p;
}
/* A hello came: 1 if it is the other game's and speaks our version; then its sender is the peer (rbl_accept),
 * its clock is kept for our next hello's echo, and its echo of ours is a round trip. All by when it arrived
 * (the link's receive thread), not when the game read it: a frame's wait on each side is no part of the trip. */
static int net_hello_heard(const RbnHello *h) {
    int p = h->player;
    if (h->version != RBN_VERSION) return 0;
    if (net_local == 0 && p == -1) {                 /* (the host: a joiner asking for its number) */
        if ((p = net_join(h->seed)) < 0) return 0;
    } else {
        if (p < 0 || p >= RBN_PLAYERS || p == net_local) return 0;
        if (net_local != 0 && p != 0) return 0;      /* (M7: the others talk to the host only) */
        if (net_local == 0 && p >= net_n) return 0;  /* (the host: a player past the count) */
        if (rbl_accept(p) < 0) return 0;
    }
    if (net_local == 0) hs_heard_ms[p] = now_ms();
    if (net_local != 0 && h->assign) net_take_number(h->assign - 1, h->players);
    u64 now = (u64)(rbl_last_arrival_ms() * 1000.0);
    if (h->t) { hs_peer_t[p] = h->t; hs_peer_t_at[p] = now; }
    if (h->echo && h->echo + h->hold <= now) {
        double rtt = (double)(now - h->echo - h->hold) / 1000.0;
        if (!hs_rtt_n[p] || rtt < hs_rtt_min[p]) hs_rtt_min[p] = rtt;
        if (!hs_rtt_n[p]) hs_rtt_t0[p] = now_ms();
        if (hs_rtt_n[p]++ < 3 || !(hs_rtt_n[p] % 3000))   /* (a wait at the set-up measures thousands: M7 logs fewer) */
            fprintf(stderr, "[rb] handshake: round trip to player %d %.1f ms (least so far %.1f, %d measured)\n", p + 1, rtt, hs_rtt_min[p], hs_rtt_n[p]);
    }
    return 1;
}
/* the oldest frame any other player's pads are known up to (net mode); loop mode: player 2's */
static u32 net_remote_low(void) {
    if (net_mode != 2) return rbn_remote_newest(1);
    u32 low = ~0u;
    for (int p = 0; p < net_n; p++) if (p != net_local && rbn_remote_newest(p) < low) low = rbn_remote_newest(p);
    return low == ~0u ? 0 : low;
}
/* M5 step 6, the match's set-up agreed (CHOICE messages): each game's own player's fighter and colour,
 * player 1's game adds the stage; applied at the player record builder (HLE_WRAP 0x71017880c0). */
static RbnChoice ch_mine, ch_peer[RBN_PLAYERS];
static int ch_have_mine, ch_have_peer[RBN_PLAYERS];
static void net_choice_send(int ask);
static int net_choice_take(const u8 *buf, int len);
static int net_rules_take(const u8 *buf, int len);
static int net_ready_take(const u8 *buf, int len);
static int net_menu_take(const u8 *buf, int len);
static u64 net_mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdull; }
/* the bots: a stick direction and a button held a few frames at a time, as a frame's own function */
static RbnPad net_pad(int who, u32 f) {
    u64 h = net_mix((u64)who * 7919 + 1, f / (u32)(5 + who * 4));
    RbnPad p = {0};
    static const u64 press[8] = {0, 0, 1ull << 0, 1ull << 1, 1ull << 2, 0, 1ull << 0, 1ull << 7};   /* -, -, A, B, X (jump), -, A, R (shield) */
    p.buttons = press[h & 7];
    p.lx = (int16_t)((int)((h >> 8) % 3) - 1) * 32767;
    p.ly = (h >> 16) % 6 == 0 ? 32767 : (h >> 16) % 6 == 1 ? -32767 : 0;
    return rbn_rest(p);
}
/* the logic frame at which player 2's pad for frame F gets here. (In this game's frames, not on
 * wall time: the bot keeps pace with this game as a real peer would through the frame advantage;
 * on wall time, a hitch at the fight's start left this game behind for good, every pad came
 * early and nothing was ever guessed: loop1.) */
static double net_arrival(u32 f) {
    if (net_lag_hi < 0) return -1e18;
    u64 h = net_mix(0xa11, f);
    int lag = net_lag_lo + (int)(h % (u64)(net_lag_hi - net_lag_lo + 1));
    if ((int)((h >> 20) % 100) < net_loss) lag += 1 + (int)((h >> 28) % 3);
    return (double)f - net_delay + lag;
}
/* every pad come by logic frame `now`; force: also the oldest missing ones until frame now + 1 may run
 * (the wait a game makes for a peer that is too far behind: here the peer is this game's own bot) */
static void net_deliver(u32 now, int force) {
    /* (at most 64 frames ahead of now: with every pad known at once (lag max -1), going 128 past
     * net_low each frame overran rb_net's 256-frame ring within two frames, and player 2 got
     * nothing but empty pads: seed2) */
    for (u32 f = net_low; f < net_low + 128 && f <= now + 64 && f - net_key0 < 1u << 20; f++) {
        u32 i = f - net_key0;
        if (net_got[i] || (net_arrival(f) > now && !(force && !rbn_can_run(now + 1)))) continue;
        rbn_remote(1, f, net_pad(1, f));
        net_got[i] = 1;
    }
    while (net_low - net_key0 < 1u << 20 && net_got[net_low - net_key0]) net_low++;
}
static void net_report(void) {
    char name[64];
    if (net_mode == 2) snprintf(name, sizeof name, "build\\rb_net_%d.txt", net_local);
    else snprintf(name, sizeof name, "build\\rb_loop_%d.txt", net_lag_hi);
    FILE *o = fopen(name, "w");
    long long n = 0;
    if (o) {
        for (u32 i = 0; i < (u32)net_frames + 600 && i < 1u << 20; i++) if (net_final[i]) { fprintf(o, "%u %016llx\n", i, (unsigned long long)net_final[i]); n++; }
        fclose(o);
    }
    char hist[200] = ""; size_t k = 0;
    for (int d = 1; d < 16; d++) if (net_depth[d]) k += snprintf(hist + k, sizeof hist - k, " %d:%lld", d, net_depth[d]);
    fprintf(stderr, "[rb] saves: %lld made, %lld left out (the other's next pad already there)\n", net_saves_made, net_saves_skipped);
    fprintf(stderr, "[rb] loads held up by a thread on another fiber: %lld went ahead after a wait, %lld refused; %lld rollbacks from an older state instead\n",
            rb_fiber_retried_ok, rb_fiber_refused, net_older);
    fprintf(stderr, "[rb] net check: %lld frames hashed again after a rollback with their pads all real, %lld of them DIFFERENT%s\n", net_again, net_again_bad,
            net_again_bad ? " (the passes are in the log: net check lines)" : "");
    fprintf(stderr, "[rb] loop mode done (delay %d, lag %d..%d, loss %d%%): %lld rollbacks (depths%s), %lld frames run again, caught up %.1f ms mean (max %.1f); "
            "%lld frames waited for pads (%.1f ms), %lld rollbacks too deep for the ring; %lld final hashes in %s\n",
            net_delay, net_lag_lo, net_lag_hi, net_loss, net_rollbacks, k ? hist : " none", net_rerun,
            net_rollbacks ? net_catch_ms / net_rollbacks : 0, net_catch_max, net_waits, net_wait_ms, net_too_deep, n, name);
    if (net_mode == 2) {
        long long s, r, e; rbl_stats(&s, &r, &e);
        s -= net_link_base[0]; r -= net_link_base[1]; e -= net_link_base[2];   /* (this fight's) */
        fprintf(stderr, "[rb] net mode (player %d): %lld hashes compared with the peer's, %lld DIFFERENT%s; messages %lld sent (%lld failed), %lld received; "
                "%lld frames waited for the peer's pads (%.1f ms), %lld frames held back for the frame advantage (%s); ack round trip %.1f ms mean\n",
                net_local + 1, net_checks, net_desyncs, net_desyncs ? "" : " (in sync)", s, e, r, net_waits, net_wait_ms, net_sync_waits,
                net_hold_sleep > 0 ? "slept" : "refreshes left out", net_rtt_n ? net_rtt_sum / net_rtt_n : -1.0);
    }
}
/* this game's newest frame whose hash is final (its pads all known), with that hash; 0: none yet.
 * (M6: final once the frame's OWN pads are real, not the frame before's. The false desyncs at KO launches
 * (twopc18, twopc19): a frame final by the old rule held one hash, then a wrong guess at that very frame rolled it
 * back and it came out another; with nothing before it changed, part of a frame's state follows its own pads
 * after all (player 1's pad is read after the safe point; the other's, or some part, not). A frame's hash
 * now waits one frame more, and a wrong guess at a frame clears its own hash too. check_again still flags
 * frames final by the old rule that change, to find which part.) */
static u32 net_final_frame(u32 key, u64 *h) {
    u32 c = rbn_confirmed();
    if (c > key) c = key;
    for (u32 f = c; f >= net_key0 && f + 8 + (u32)net_every > c; f--) {
        u32 i = f - net_key0;
        if (i < 1u << 20 && net_final[i]) { *h = net_final[i]; return f; }
    }
    return 0;
}
static void net_compare(u32 key) {
    u32 c = rbn_confirmed();   /* (final: see net_final_frame) */
    if (c > key) c = key;
    for (u32 f = net_checked + 1; f <= c; f++) {
        u32 i = f - net_key0;
        if (i >= 1u << 20 || !net_final[i]) continue;
        for (int p = 0; p < net_n; p++) {
            if (p == net_local || !net_peer_check[p] || !net_peer_check[p][i]) continue;
            net_checks++;
            if ((u32)net_final[i] != net_peer_check[p][i]) {
                if (!net_desyncs) net_first_desync = f;
                if (net_desyncs++ < 3) {   /* (the first few, each with this game's fields: M6) */
                    fprintf(stderr, "[rb] net mode: DESYNC with player %d at logic frame %u (ours %08x, theirs %08x)%s\n", p + 1, f, (u32)net_final[i],
                            net_peer_check[p][i], net_checks <= 1 ? ": the games did not start the same match (characters, colours, stage, rules or the seed differ)" : "");
                    check_desync_show(f);
                }
            }
        }
    }
    if (c > net_checked) net_checked = c;
}
static void net_receive(u32 key) {
    static u8 buf[2048];
    int n;
    while ((n = rbl_recv(buf, sizeof buf)) > 0) {
        RbnMsg m;
        RbnHello hh;
        if (rbn_hello_parse(buf, (size_t)n, &hh)) {   /* (a game still at its set-up: it waits for our answer) */
            if (net_hello_heard(&hh) && net_local != 0 && hs_agreed && hh.seed == hs_seed) net_hello_send();
            continue;
        }
        if (net_rules_take(buf, n)) continue;
        if (net_ready_take(buf, n)) continue;
        if (net_menu_take(buf, n)) continue;
        if (net_choice_take(buf, n)) continue;
        if (!rbn_parse(buf, (size_t)n, &m) || m.player == net_local || m.player >= net_n) continue;
        if (rbl_accept(m.player) < 0) continue;
        int p = m.player;
        if (net_local == 0) hs_heard_ms[p] = now_ms();   /* (Host/Join: pads count as heard: a fight's players are no quiet slots) */
        if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != p) rbl_send_to(k, buf, n);   /* (the host passes it on: M7) */
        net_msgs_in++;
        for (int k = 0; k < m.count; k++) rbn_remote(p, m.first + (u32)k, m.pads[k]);
        u32 ack = m.ack[net_local];
        if (ack > net_acked_by[p] && net_sent_at[ack & (RBN_RING - 1)] == ack) {
            double rtt = rbl_last_arrival_ms() - net_sent_ms[ack & (RBN_RING - 1)];   /* (by arrival: the link's thread) */
            if (rtt < net_rtt_min_s) net_rtt_min_s = rtt;
            net_rtt_sum += rtt; net_rtt_n++;
        }
        if (ack > net_acked_by[p]) net_acked_by[p] = ack;
        if (m.frame > net_heard[p]) net_heard[p] = m.frame;
        if (m.check_frame >= net_key0 && m.check_frame - net_key0 < 1u << 20 && net_peer_check[p]) {
            u32 i = m.check_frame - net_key0;
            net_peer_check[p][i] = m.check ? m.check : 1;
            static u32 last_arrival[RBN_PLAYERS];   /* (a player repeats its newest final frame in several messages: counted once) */
            if (m.check_frame <= net_checked && net_final[i] && m.check_frame != last_arrival[p]) {   /* (a frame compared already passed here: at once) */
                last_arrival[p] = m.check_frame;
                net_checks++;
                if ((u32)net_final[i] != net_peer_check[p][i]) {
                    if (!net_desyncs) net_first_desync = m.check_frame;
                    if (net_desyncs++ < 3) {
                        fprintf(stderr, "[rb] net mode: DESYNC with player %d at logic frame %u (ours %08x, theirs %08x)\n", p + 1, m.check_frame, (u32)net_final[i],
                                net_peer_check[p][i]);
                        check_desync_show(m.check_frame);
                    }
                }
                net_peer_check[p][i] = 0;
            }
        }
        /* (each side's advantage as each measures it, late alike: the difference cancels the trip) */
        net_adv_ours = (int32_t)key - (int32_t)(net_heard[p] + 1); net_adv_peer = (int32_t)m.frame - (int32_t)(m.heard[net_local] + 1);
        rbn_sync_sample(p, net_adv_ours, net_adv_peer);
    }
    /* a wrong guess these pads showed: the frames after it are not what they will be once run again,
     * so their hashes go now, before anything is compared or sent (net3: the hashes of a pass about to
     * be rolled back went out as final and were "different" at frame 88, the rollback in the same
     * step still to come). The rollback itself comes at the end of net_step. */
    u32 bad = rbn_take_mismatch();
    if (bad) {
        for (u32 f = bad; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;   /* (bad too: M6, net_final_frame) */
        rbn_put_mismatch(bad);
    }
}
static void net_send(u32 key) {
    static RbnMsg m;
    static u8 buf[RBN_MSG_MAX];
    m.type = RBN_MSG_PADS; m.player = net_local; m.count = 0;
    memset(m.ack, 0, sizeof m.ack); memset(m.heard, 0, sizeof m.heard);
    u32 low = ~0u;   /* (from the oldest frame of ours any player lacks: M7) */
    for (int p = 0; p < net_n; p++) if (p != net_local) {
        if (net_acked_by[p] < low) low = net_acked_by[p];
        m.ack[p] = rbn_remote_newest(p); m.heard[p] = net_heard[p];
    }
    m.first = (low == ~0u ? 0 : low) + 1;
    for (u32 f = m.first; f <= rbn_local_newest() && m.count < RBN_MAX_PADS; f++) rbn_local_get(f, &m.pads[m.count++]);
    m.frame = key;
    u64 h = 0;
    m.check_frame = net_final_frame(key, &h); m.check = (u32)h;
    size_t len = rbn_build(buf, sizeof buf, &m);
    if (len) { rbl_send(buf, (int)len); net_msgs_out++; net_bytes_s += (long long)len; net_msgs_s++; if ((long long)len > net_bytes_max_s) net_bytes_max_s = (long long)len; }
}
static void net_step(Cpu *c, u64 frame) {
    u32 key = ld32(LOGIC_FRAME);
    static long long net_fight = -1;
    if (net_fight != S.fights || !net_got) {   /* a fight starts (by its number: S.frames == 1 came twice once) */
        net_fight = S.fights;
        if (!net_got) { net_got = calloc(1u << 20, 1); net_final = calloc(1u << 20, sizeof *net_final); }
        else { memset(net_got, 0, 1u << 20); memset(net_final, 0, (1u << 20) * sizeof *net_final); }
        if (net_delay < 0) { net_delay = 2; fprintf(stderr, "[rb] net mode: no input delay agreed (no handshake): 2\n"); }
        if (net_mode == 2) {
            int measured = 0;
            for (int p = 0; p < RBN_PLAYERS; p++) measured |= hs_rtt_n[p] > 0;
            fprintf(stderr, "[rb] net mode: %d players, this game player %d; input delay %d this fight%s\n", net_n, net_local + 1, net_delay,
                    measured ? "" : " (no round trip measured)");
        }
        rbn_reset(net_mode == 2 ? net_n : 2, net_mode == 2 ? net_local : 0, net_delay);
        rbn_sync_reset();
        for (int p = 0; p < net_n; p++) {
            if (p == net_local || net_mode != 2) continue;
            if (!net_peer_check[p]) net_peer_check[p] = calloc(1u << 20, sizeof *net_peer_check[p]);
            else memset(net_peer_check[p], 0, (1u << 20) * sizeof *net_peer_check[p]);
        }
        memset(net_acked_by, 0, sizeof net_acked_by); memset(net_heard, 0, sizeof net_heard); net_checked = key;
        net_countdown = 0;
        /* (every count of the report from zero: it added up over a session's fights, twopc14) */
        net_rollbacks = net_rerun = net_waits = net_too_deep = net_saves_made = net_saves_skipped = net_older = 0;
        rb_fiber_refused = rb_fiber_retried_ok = 0;
        memset(net_depth, 0, sizeof net_depth);
        net_catch_ms = net_catch_max = net_wait_ms = 0;
        net_checks = net_desyncs = net_msgs_in = net_msgs_out = net_sync_waits = 0;
        net_first_desync = -1;
        memset(check_recs, 0, sizeof check_recs); net_again = net_again_bad = 0;
        net_rtt_min_s = 1e9; net_rtt_sum = 0; net_rtt_n = 0; net_adv_ours = net_adv_peer = 0;
        memset(net_sent_at, 0, sizeof net_sent_at);
        rbl_stats(&net_link_base[0], &net_link_base[1], &net_link_base[2]);
        net_key0 = net_low = key; net_t0 = now_ms(); net_done = 0;
        fprintf(stderr, "[rb] loop mode: fight starts at logic frame %u\n", key);
        if (net_mode == 2) fprintf(stderr, "[rb] net mode: the match seed %s (%016llx)\n", hs_agreed ? "was agreed" : "was NOT agreed: the start may differ", (unsigned long long)hs_seed);
        /* SSBU_RB_SEED=<n>: the 9 script random generators (4 state words and a call count each) set
         * from <n> now, so two fights can start alike (two runs differed from frame 0: loop2). A
         * stand-in for step 3's handshake, which agrees on a seed between two PCs. */
        const char *sd = getenv("SSBU_RB_SEED"), *sf = getenv("SSBU_RB_SEED_FIGHT");
        u64 rb = rng_base();
        if (sf && *sf == '0') sd = NULL;   /* (step 3b's test: the match seed alone, net mode's global generator, outside fights) */
        if (sd && *sd && rb) {
            u64 z = strtoull(sd, NULL, 0);
            for (int g = 0; g < 9; g++) {
                for (int w = 0; w < 4; w++) {   /* (splitmix64: never all zero) */
                    z += 0x9e3779b97f4a7c15ull;
                    u64 x = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull; x ^= x >> 31;
                    st32(rb + 20ull * g + 4ull * w, (u32)x | 1);
                }
                st32(rb + 20ull * g + 16, 0);
            }
            fprintf(stderr, "[rb] loop mode: script random generators seeded from %s\n", sd);
        }
    }
    if (S.frames > 8 && S.frames <= 300) {   /* (then, to 5 s: each generator's calls whenever they change) */
        static u32 prev[9];
        u64 rb = rng_base();
        int changed = 0;
        char g[200] = ""; size_t k = 0;
        for (int q = 0; q < 9 && rb; q++) { u32 v = ld32(rb + 20ull * q + 16); if (v != prev[q]) changed = 1; prev[q] = v; k += snprintf(g + k, sizeof g - k, " %u", v); }
        if (changed) fprintf(stderr, "[rb] loop mode, fight frame %lld (logic %u): generator calls now%s\n", S.frames, key, g);
    }
    if (S.frames <= 8) {   /* (what two runs' first frames hold, to see what differs: fighters, each generator's calls, the pads) */
        FFrame f; fight_frame(c, &f);
        char t[256];
        for (u32 k = 0; k < f.n; k++) { frec_text(t, sizeof t, &f.f[k]); fprintf(stderr, "[rb] loop mode, fight frame %lld: %s\n", S.frames, t); }
        char g[200] = ""; size_t k = 0;
        u64 rb = rng_base();
        for (int q = 0; q < 9 && rb; q++) k += snprintf(g + k, sizeof g - k, " %u", ld32(rb + 20ull * q + 16));
        fprintf(stderr, "[rb] loop mode, fight frame %lld (logic %u): generators %u calls (each:%s), %016llx; pads slot 0 %#llx %d,%d, slot 1 %#llx %d,%d\n",
                S.frames, key, f.rng_calls, g, (unsigned long long)f.rng, (unsigned long long)net_last_pad[0].buttons, net_last_pad[0].lx, net_last_pad[0].ly,
                (unsigned long long)net_last_pad[1].buttons, net_last_pad[1].lx, net_last_pad[1].ly);
    }
    if (rb_fast > 0 && InterlockedDecrement(&rb_fast) == 0) {
        double ms = now_ms() - ring_load_t0;
        net_catch_ms += ms; if (ms > net_catch_max) net_catch_max = ms;
    }
    u32 i = key - net_key0;
    if (i < 1u << 20) {
        if (net_every < 0) { const char *e = getenv("SSBU_RB_NET_CHECK_EVERY"); net_every = e && atoi(e) > 0 ? atoi(e) : 10; }
        if (rb_fast <= 0 && !net_done && key % (u32)net_every == 0) {
            /* the fighters and the generators, all but generator 1: something outside the fight (on Final
             * Destination; its background?) draws from it every other frame, starting at a different frame
             * each run, while the fighters stay the same to the last bit (dbg1-3, seed6: 30 s alike) */
            double t0 = now_ms();
            FFrame f; fight_frame(c, &f);
            u64 h = hfnv(0xcbf29ce484222325ull, f.f, sizeof(FRec) * f.n), rb = rng_base();
            for (int q = 0; q < 9 && rb; q++) if (q != 1) { u32 w[5]; for (int k = 0; k < 5; k++) w[k] = ld32(rb + 20ull * q + 4ull * k); h = hfnv(h, w, sizeof w); }
            net_final[i] = h ? h : 1;
            net_work(0, t0);
            check_again(key, &f, net_final[i]);
        }
        else net_final[i] = 0;   /* (run again in the middle of a catch-up: not hashed) */
    }
    if (!net_done && S.frames >= net_frames) { net_done = 1; net_report(); }
    if (rb_fast <= 0 && S.frames % 60 == 0 && S.frames <= net_frames) {   /* (once a second: who reads which pad, where the fighters are) */
        double t0 = now_ms();
        FFrame f; fight_frame(c, &f);
        char t[1024]; size_t k = 0;
        for (int p = 0; p < 8; p++) if (net_reads[p])
            k += snprintf(t + k, sizeof t - k, " | slot %d: %lld reads (key %u), last %#llx %d,%d", p, net_reads[p], net_last_key[p],
                          p < 2 ? (unsigned long long)net_last_pad[p].buttons : 0ull, p < 2 ? net_last_pad[p].lx : 0, p < 2 ? net_last_pad[p].ly : 0);
        for (u32 q = 0; q < f.n; q++) k += snprintf(t + k, sizeof t - k, " | P%d st %#x pos %.1f,%.1f", f.f[q].entry + 1, f.f[q].st, f.f[q].x, f.f[q].y);
        fprintf(stderr, "[rb] loop mode, fight frame %lld:%s\n", S.frames, t);
        memset(net_reads, 0, sizeof net_reads);
        net_work(2, t0);
    }
    if (rb_fast > 0) {   /* the middle of a catch-up: a save every n-th frame, nothing else */
        LONG done = catch_total - rb_fast;
        if (rerun_save != 1 && (rerun_save == 0 || done % rerun_save)) { rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; return; }
    }
    /* A save only where a rollback could come back to it: a wrong guess at frame m loads a state tagged m - 1 or
     * before, so a state is needed at `key` only while the other player's pad for key + 1 hasn't come (it will be
     * guessed). When it has, no guess can go wrong before key + 2, whose own safe point saves if needed. The game
     * behind never guesses and saved every frame for nothing: some 9 ms a frame on PC 2 (GTX 1660 Ti), which ran at
     * about 20 ms a frame and held PC 1 back (twopc10, twopc14). SSBU_RB_NET_LAZY=0: a save every frame. */
    static int lazy = -1;
    if (lazy < 0) { const char *e = getenv("SSBU_RB_NET_LAZY"); lazy = !(e && *e == '0'); }
    int skip_save = lazy && rb_fast <= 0 && net_remote_low() >= key + 1;   /* (every other player's: M7) */
    if (skip_save) { net_saves_skipped++; rb_save_ms[0] = rb_save_ms[1] = rb_save_ms[2] = 0; }
    else {
        rb_save_tag = key;
        double t0 = now_ms();
        if (rb_save(c, frame) == 2) { catch_total = pending_fast; rb_fast = pending_fast; return; }
        if (rb_fast <= 0) net_work(1, t0);
        net_saves_made++;
    }
    if (rb_fast > 0) return;
    if (net_mode == 2) {
        /* this game's pad for the frame `delay` on; the peer's that came; ours to the peer; then the next
         * frame waits while it would run too far ahead of the peer's pads, or for the frame advantage */
        rbn_local(key, net_live == 1 ? net_live_pad : net_pad(net_local, key));
        u32 nw = rbn_local_newest();
        if (net_sent_at[nw & (RBN_RING - 1)] != nw) { net_sent_at[nw & (RBN_RING - 1)] = nw; net_sent_ms[nw & (RBN_RING - 1)] = now_ms(); }
        net_receive(key);
        net_compare(key);
        net_send(key);
        if (!rbn_can_run(key + 1)) {
            net_waits++;
            double w0 = now_ms(), last = w0;
            while (!rbn_can_run(key + 1) && now_ms() - w0 < 600000) {   /* (10 min: two PCs a room apart, started by one person) */
                Sleep(1);
                net_receive(key);
                if (now_ms() - last > 16) { net_send(key); last = now_ms(); }   /* (our pads and acks again: the peer may be waiting too) */
            }
            net_wait_ms += now_ms() - w0;
            if (!rbn_can_run(key + 1)) fprintf(stderr, "[rb] net mode: no pads from the peer for 10 min (logic frame %u): going on with guesses\n", key);
        }
        if (net_hold_sleep < 0) { const char *e = getenv("SSBU_RB_NET_HOLD"); net_hold_sleep = e && !strcmp(e, "sleep"); }
        if (rbn_sync_wait(key + 1)) {
            net_sync_waits++;
            if (net_hold_sleep) Sleep(16); else InterlockedIncrement(&rb_vsync_skip);
        }
        if (S.frames % 60 == 0) {
            fprintf(stderr, "[rb] net sync, fight frame %lld (logic %u): frame advantage ours %d, the peer's %d; %lld holds (%lld refreshes left out), "
                    "%lld rollbacks, %lld waits for pads; ack round trip %.1f ms least this second, %.1f mean; pads sent in %lld messages, %.0f bytes on average, %lld at most\n",
                    S.frames, key, net_adv_ours, net_adv_peer, net_sync_waits, (long long)rb_vsync_skipped, net_rollbacks, net_waits,
                    net_rtt_min_s < 1e9 ? net_rtt_min_s : -1.0, net_rtt_n ? net_rtt_sum / net_rtt_n : -1.0,
                    net_msgs_s, net_msgs_s ? (double)net_bytes_s / net_msgs_s : 0.0, net_bytes_max_s);
            net_bytes_s = net_msgs_s = net_bytes_max_s = 0;
            fprintf(stderr, "[rb] net work, the last second: hashes %lld (%.1f ms, worst %.1f), saves %lld (%.1f ms, worst %.1f), fighter line %lld (%.1f ms, worst %.1f); "
                    "of them the game's getters %lld (%.1f ms), generator look-ups that asked Windows %lld (%.1f ms)\n",
                    net_w_n[0], net_w_ms[0], net_w_max[0], net_w_n[1], net_w_ms[1], net_w_max[1], net_w_n[2], net_w_ms[2], net_w_max[2],
                    rb_ff_get_n, rb_ff_get_ms, rb_rng_slow_n, rb_rng_slow_ms);
            if (rb_save_count) {
                double k = (double)rb_save_count;
                fprintf(stderr, "[rb] net saves, the last second: %lld, a save %.2f ms on average: parking the threads %.2f, GPU drain %.2f, memory %.2f, the rest %.2f\n",
                        rb_save_count, rb_save_parts[3] / k, rb_save_parts[0] / k, rb_save_parts[1] / k, rb_save_parts[2] / k,
                        (rb_save_parts[3] - rb_save_parts[0] - rb_save_parts[1] - rb_save_parts[2]) / k);
            }
            memset(rb_save_parts, 0, sizeof rb_save_parts); rb_save_count = 0;
            memset(net_w_ms, 0, sizeof net_w_ms); memset(net_w_max, 0, sizeof net_w_max); memset(net_w_n, 0, sizeof net_w_n);
            rb_ff_get_ms = rb_rng_slow_ms = 0; rb_ff_get_n = rb_rng_slow_n = 0;
            net_rtt_min_s = 1e9;
        }
    } else {
        /* the pads that came; the next frame waits while it would run too far ahead of them */
        net_deliver(key, 0);
        if (!rbn_can_run(key + 1)) { net_waits++; net_deliver(key, 1); }
    }
    u32 m = rbn_take_mismatch();
    if (!m || m > key) { rb_release_held(); return; }
    u32 found = 0;
    int age = rb_ring_find(m - 1, &found);
    if (age < 0) { net_too_deep++; rb_release_held(); return; }
    int depth = (int)(key - found);
    net_rollbacks++; net_rerun += depth; net_depth[depth < 15 ? depth : 15]++;
    for (u32 f = found + 1; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;
    pending_fast = depth;
    ring_load_t0 = now_ms();
    rb_load_age(c, frame, age);   /* (returns only if refused) */
    net_rollbacks--; net_rerun -= depth; net_depth[depth < 15 ? depth : 15]--;
    /* M7: refused (a thread on another fiber than at that save, even after waiting): an older state may have had
     * every thread where it is now. Up to 3 more, each a frame or so further back; the frames since go again. */
    for (int older = age + 1; older <= age + 3; older++) {
        u32 tag = rb_ring_tag(older);
        if (!tag || tag < net_key0) break;
        int d = (int)(key - tag);
        net_rollbacks++; net_rerun += d; net_depth[d < 15 ? d : 15]++; net_older++;
        for (u32 f = tag + 1; f <= key; f++) if (f - net_key0 < 1u << 20) net_final[f - net_key0] = 0;
        pending_fast = d;
        rb_load_age(c, frame, older);   /* (returns only if refused) */
        net_rollbacks--; net_rerun -= d; net_depth[d < 15 ? d : 15]--; net_older--;
    }
    rb_release_held();
    rbn_put_mismatch(m);
}
/* (rb_pad, in loop mode: both players' pads by logic frame) */
static int net_pad_for(u32 id, u32 key, u64 *buttons, i32 *lx, i32 *ly, i32 *rx, i32 *ry, i32 *lt, i32 *rt) {
    if (net_mode != 1 && net_mode != 2) return 0;
    if (!net_got) {   /* a fight's reads before its first safe point: they were live; now the bots' too (both from net_pad) */
        static int shown;
        if (shown < 16 && id < 2) {
            shown++;
            fprintf(stderr, "[rb] loop mode: pad read before the fight's first safe point: slot %u, logic frame %u, live %#llx %d,%d (lt %d rt %d)\n",
                    id, key, (unsigned long long)*buttons, *lx, *ly, *lt, *rt);
        }
        if ((int)id >= (net_mode == 2 ? net_n : 2)) return 0;
        RbnPad p = net_live == 1 ? (RbnPad){0} : net_pad((int)id, key);   /* (live: nothing held yet, in both games) */
        *buttons = p.buttons; *lx = p.lx; *ly = p.ly; *rx = p.rx; *ry = p.ry; *lt = p.lt; *rt = p.rt;
        return 1;
    }
    if (id < 8) { net_reads[id]++; net_last_key[id] = key; }
    if (net_mode == 2 && net_live == 1 && (int)id == net_local && rb_fast <= 0) {   /* (the controllers now: this game's next pad) */
        RbnPad l = {*buttons, (int16_t)*lx, (int16_t)*ly, (int16_t)*rx, (int16_t)*ry, (int16_t)*lt, (int16_t)*rt};
        net_live_pad = rbn_rest(l);
    }
    if ((int)id >= (net_mode == 2 ? net_n : 2)) return 0;
    RbnPad p = net_mode == 2 ? rbn_get((int)id, key, NULL) : id == 0 ? net_pad(0, key) : rbn_get(1, key, NULL);
    *buttons = p.buttons; *lx = p.lx; *ly = p.ly; *rx = p.rx; *ry = p.ry; *lt = p.lt; *rt = p.rt;
    net_last_pad[id] = p;
    return 1;
}

/* ---- M5 step 5: the handshake, in the menus (net mode) --------------------------------------------
 * Player 1's game proposes the match seed (SSBU_RB_SEED if set, for tests that must repeat; else from
 * the clock); player 2's takes the first one it hears and answers with it; once both have it, both set
 * the game's global generator from it at every safe point outside fights (savestates), so the match
 * set-up draws the same in both (step 3b). HELLO messages every 10 safe points until the fight; other
 * messages heard in the menus are dropped (the peer's pads come again in every message: nothing is
 * lost). The characters and the stage are still chosen by hand in both games; a match set up
 * differently shows as a desync on the fight's first frames, said as such (net_compare). */
/* The game's global xorshift128 (13.0.5 0x71052391c0) set from the agreed seed. */
static void net_seed_global(const char *why) {
    const u64 g = 0x71052391c0ull;
    if (!hs_agreed || !readable(g, 16)) return;
    static int shown;
    if (shown < 4) {
        shown++;
        fprintf(stderr, "[rb] net mode: the global generator %#llx was %08x %08x %08x %08x; set from the agreed seed (%s)\n",
                (unsigned long long)g, ld32(g), ld32(g + 4), ld32(g + 8), ld32(g + 12), why);
    }
    u64 z = hs_seed ^ 0x5eed5eedull;
    for (int k = 0; k < 4; k++) {
        z += 0x9e3779b97f4a7c15ull;
        u64 y = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; y = (y ^ (y >> 27)) * 0x94d049bb133111ebull; y ^= y >> 31;
        st32(g + 4ull * k, (u32)y | (k == 0));
    }
}
/* (M7: one hello to each game this one talks to - the host to each player, the others to the host - each with the
 * echo of that game's clock) */
static void net_hello_send(void) {
    for (int p = 0; p < net_n; p++) {
        if (p == net_local || (net_local != 0 && p != 0)) continue;
        RbnHello h = {net_local, RBN_VERSION, hs_agreed, 0, (net_local == 0 && hs_delay_ready) || hs_agreed ? hs_seed : 0};
        if (net_local == 0) { h.players = net_n; h.assign = p + 1; }   /* (Host/Join: its number, every time) */
        if (net_local < 0) {   /* (a joiner with no number yet: its join id) */
            if (!hs_join_id) { LARGE_INTEGER t; QueryPerformanceCounter(&t); hs_join_id = net_mix((u64)t.QuadPart, (u64)GetCurrentProcessId()) | 1; }
            h.seed = hs_join_id;
        }
        if (net_local == 0 && hs_delay_ready) h.input_delay = net_delay + 1;
        u64 now = now_us();
        h.t = now;
        if (hs_peer_t[p]) { h.echo = hs_peer_t[p]; h.hold = (u32)(now - hs_peer_t_at[p]); }
        u8 out[RBN_HELLO_BYTES];
        size_t k = rbn_hello_build(out, sizeof out, &h);
        if (k) rbl_send_to(p, out, (int)k);
    }
}
static void net_hello(int now) {
    static long long n;
    static int bad_version;
    if (!hs_seed && net_local == 0) {
        const char *sd = getenv("SSBU_RB_SEED");
        if (sd && *sd) hs_seed = strtoull(sd, NULL, 0);
        else {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            hs_seed = net_mix((u64)t.QuadPart, ((u64)GetCurrentProcessId() << 32) ^ (u64)time(NULL));
        }
        if (!hs_seed) hs_seed = 1;
        fprintf(stderr, "[rb] handshake: this game (player 1) proposes the match seed %016llx\n", (unsigned long long)hs_seed);
    }
    if (net_local == 0 && !hs_delay_ready) {   /* (the seed goes out with the delay, not before) */
        /* (M7: every other player measured, each over HS_RTT_MS at least; the delay from the longest of their trips:
         * two players' pads meet at the host, each about half its own trip away from it) */
        int ready = net_delay >= 0;
        double worst = 0;
        if (!ready) {
            ready = 1;
            for (int p = 1; p < net_n; p++) {
                if (hs_rtt_n[p] < HS_RTT_SAMPLES || now_ms() - hs_rtt_t0[p] < HS_RTT_MS) { ready = 0; break; }
                if (hs_rtt_min[p] > worst) worst = hs_rtt_min[p];
            }
        }
        if (ready) {
            if (net_delay < 0) {
                net_delay = rbn_delay_for_rtt(worst);
                char t[200] = ""; size_t k = 0;
                for (int p = 1; p < net_n; p++) k += snprintf(t + k, sizeof t - k, "%s player %d %.1f ms", p > 1 ? "," : "", p + 1, hs_rtt_min[p]);
                fprintf(stderr, "[rb] handshake: round trips (the least of %d over 5 s or more):%s; the longest %.1f ms: input delay %d\n", HS_RTT_SAMPLES, t, worst, net_delay);
            } else fprintf(stderr, "[rb] handshake: input delay %d (set)\n", net_delay);
            hs_delay_ready = 1;
        }
    }
    static u8 buf[2048];
    int len;
    while ((len = rbl_recv(buf, sizeof buf)) > 0) {
        RbnHello h;
        if (net_rules_take(buf, len)) continue;
        if (net_ready_take(buf, len)) continue;
        if (net_menu_take(buf, len)) continue;
        if (net_choice_take(buf, len)) continue;
        if (!rbn_hello_parse(buf, (size_t)len, &h)) continue;   /* (the peer's pads from a fight it is already in: they come again) */
        if (h.version != RBN_VERSION) {
            if (!bad_version++) fprintf(stderr, "[rb] handshake: the peer speaks version %d, this game %d: no agreement (both PCs need the same build)\n", h.version, RBN_VERSION);
            continue;
        }
        if (!net_hello_heard(&h)) continue;
        if (net_local > 0 && h.player == 0 && h.seed && h.seed != hs_seed && h.input_delay) {
            int was = net_delay;
            hs_seed = h.seed; hs_agreed = 1;
            net_delay = h.input_delay - 1; hs_delay_ready = 1;
            if (h.players >= 2 && h.players <= RBN_PLAYERS) net_n = h.players;   /* (M7: how many play, from the host) */
            if (net_local >= net_n) fprintf(stderr, "[rb] handshake: the host plays %d players, but this PC is set up as player %d: start it as player 2-%d\n",
                                            net_n, net_local + 1, net_n);
            fprintf(stderr, "[rb] handshake: took player 1's match seed %016llx, input delay %d, %d players%s\n", (unsigned long long)hs_seed, net_delay, net_n,
                    was >= 0 && was != net_delay ? " (this PC was set to another delay: player 1's counts)" : "");
        }
        if (net_local == 0 && h.player >= 1 && h.player < net_n && h.seed && h.seed == hs_seed && !hs_agreed_by[h.player]) {
            hs_agreed_by[h.player] = 1;
            int all = 1;
            for (int p = 1; p < net_n; p++) all &= hs_agreed_by[p];
            fprintf(stderr, "[rb] handshake: player %d has the match seed%s\n", h.player + 1, all ? ": every player has it, agreed" : "");
            if (all) hs_agreed = 1;
        }
    }
    if (n++ % 10 && !now) return;
    net_hello_send();
}
/* At a match set-up (player 1's record built): wait for the seed to be agreed, up to 10 min, hellos going
 * out and coming in. The handshake ran only while both games were in the menus: a game whose fight was
 * set up before the other had started never agreed (twopc4-7: one PC set up first, the other started a
 * room away), so the two began from different seeds and on different sides. */
/* A CHOICE from the peer: kept (the newest), and ours sent back (it may be waiting for it). 1: it was one. */
static int net_choice_take(const u8 *buf, int len) {
    RbnChoice c;
    if (!rbn_choice_parse(buf, (size_t)len, &c)) return 0;
    if (c.player == net_local || c.player >= net_n) return 1;
    if (rbl_accept(c.player) < 0) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != c.player) rbl_send_to(k, buf, len);   /* (the host passes it on: M7) */
    RbnChoice cmp = c; cmp.ask = 0;
    if (!ch_have_peer[c.player] || memcmp(&cmp, &ch_peer[c.player], sizeof cmp))
        fprintf(stderr, "[rb] handshake: player %d's choice came: ui_chara %016llx, colour %d%s\n", c.player + 1, (unsigned long long)c.chara, c.colour,
                c.has_stage ? ", and the stage" : "");
    int ask = c.ask;
    c.ask = 0;
    ch_peer[c.player] = c; ch_have_peer[c.player] = 1;
    if (ask) net_choice_send(0);   /* (only when asked: two games answering every answer ping-ponged some 30 000 messages, twopc11) */
    return 1;
}
static void net_choice_send(int ask) {
    if (!ch_have_mine) return;
    u8 out[RBN_CHOICE_BYTES];
    ch_mine.ask = ask;
    size_t k = rbn_choice_build(out, sizeof out, &ch_mine);
    if (k) rbl_send(out, (int)k);
}
/* ---- streamlining, item 2: the host's rules (RULES messages) ----------------------------------------------------
 * As the match is set up (player 1's record), the host reads the game's rule header (the match block + 0, 0x588 bytes:
 * stocks, time, items and the rest; 13.0.4 0x71052c41b0, ssbu-recomp PR #116's MELEE_INFO, one page on as all of
 * main's data), the stage set-up's inputs (its seed and the history it picks stage variations from: the PR's hs12, a
 * desync without them) and its first panel's rule kind and stocks, and sends them; every joining game waits for them
 * there and writes them over its own, so a friend's save needs no matching ruleset. The header holds no pointers: the PR
 * found two runs' headers alike but for bytes 0 and 1 (the mode), which each game keeps. */
#define MELEE_INFO 0x71052c51b0ull
#define RULE_BYTES 0x588
#define STAGE_SEED 0x710530e380ull          /* (13.0.4 0x710530d380) */
#define STAGE_HIST 0x71050c6ad0ull          /* (13.0.4 0x71050c5ad0) */
#define STAGE_HIST_BYTES 0x60
#define RULES_TOTAL (RULE_BYTES + STAGE_HIST_BYTES + 4 + 2)   /* header | history | seed u32 | rule kind | stocks */
static u8 rules_blob[RULES_TOTAL];
static u32 rules_id;
static int rules_have;                       /* (the host: read at this set-up; a joiner: every chunk of rules_id come) */
static unsigned rules_chunks;                /* (a joiner: which chunks of rules_id came, a bit each) */
static void net_rules_send(int ask) {
    static u8 out[RBN_RULES_HEAD + RBN_RULES_CHUNK];
    static RbnRules r;
    memset(&r, 0, sizeof r);
    r.player = net_local < 0 ? 0 : net_local;
    if (ask) { r.ask = 1; size_t k = rbn_rules_build(out, sizeof out, &r); if (k) rbl_send(out, (int)k); return; }
    if (net_local != 0 || !rules_have) return;
    r.id = rules_id; r.total = RULES_TOTAL;
    for (int off = 0; off < RULES_TOTAL; off += RBN_RULES_CHUNK) {
        r.off = off; r.len = RULES_TOTAL - off < RBN_RULES_CHUNK ? RULES_TOTAL - off : RBN_RULES_CHUNK;
        memcpy(r.data, rules_blob + off, (size_t)r.len);
        size_t k = rbn_rules_build(out, sizeof out, &r);
        if (k) rbl_send(out, (int)k);
    }
}
/* a RULES message: the host answers an ask with every chunk; a joiner keeps the host's chunks. 1: it was one. */
static int net_rules_take(const u8 *buf, int len) {
    static RbnRules r;
    if (!rbn_rules_parse(buf, (size_t)len, &r)) return 0;
    if (net_local == 0) {
        if (r.ask && r.player > 0 && r.player < net_n && rbl_accept(r.player) >= 0) net_rules_send(0);
        return 1;
    }
    if (r.player != 0 || r.ask || r.total != RULES_TOTAL || rbl_accept(0) < 0) return 1;
    if (r.id != rules_id) { rules_id = r.id; rules_chunks = 0; rules_have = 0; }
    memcpy(rules_blob + r.off, r.data, (size_t)r.len);
    rules_chunks |= 1u << (r.off / RBN_RULES_CHUNK);
    unsigned all = (1u << ((RULES_TOTAL + RBN_RULES_CHUNK - 1) / RBN_RULES_CHUNK)) - 1;
    if (rules_chunks == all && !rules_have) {
        rules_have = 1;
        fprintf(stderr, "[rb] handshake: the host's rules came (rule kind %d, stocks %d)\n", rules_blob[RULES_TOTAL - 2], rules_blob[RULES_TOTAL - 1]);
    }
    return 1;
}
/* at player 1's record: the host reads and sends its rules; a joiner waits for them and writes them over its own */
static void net_rules_setup(u64 st) {
    if (net_local == 0) {
        rules_have = 0;
        if (!readable(MELEE_INFO, RULE_BYTES) || !readable(STAGE_HIST, STAGE_HIST_BYTES) || !readable(STAGE_SEED, 4)) {
            fprintf(stderr, "[rb] handshake: the rules aren't readable here: each game keeps its own\n");
            return;
        }
        for (u32 k = 0; k < RULE_BYTES; k++) rules_blob[k] = ld8(MELEE_INFO + k);
        for (u32 k = 0; k < STAGE_HIST_BYTES; k++) rules_blob[RULE_BYTES + k] = ld8(STAGE_HIST + k);
        u32 seed = ld32(STAGE_SEED);
        memcpy(rules_blob + RULE_BYTES + STAGE_HIST_BYTES, &seed, 4);
        rules_blob[RULES_TOTAL - 2] = ld8(st + 5); rules_blob[RULES_TOTAL - 1] = ld8(st + 0xa);
        rules_id = (u32)hfnv(0xcbf29ce484222325ull, rules_blob, RULES_TOTAL) | 1;
        rules_have = 1;
        fprintf(stderr, "[rb] handshake: this game's rules go to the others (rule kind %d, stocks %d, items %#x)\n",
                rules_blob[RULES_TOTAL - 2], rules_blob[RULES_TOTAL - 1], ld32(MELEE_INFO + 0x54));
        for (int k = 0; k < 3; k++) { net_rules_send(0); Sleep(5); }
        return;
    }
    if (!rules_have) {
        fprintf(stderr, "[rb] handshake: waiting for the host's rules\n");
        double t0 = now_ms();
        while (!rules_have && now_ms() - t0 < 600000) { net_hello(1); net_rules_send(1); net_wait_say("Waiting for the host's rules..."); Sleep(16); }   /* (ours asks for them) */
        net_wait_say(NULL);
    }
    if (!rules_have) { fprintf(stderr, "[rb] handshake: no rules from the host after 10 min: this game's own (the match may differ)\n"); return; }
    if (!readable(MELEE_INFO, RULE_BYTES) || !readable(STAGE_HIST, STAGE_HIST_BYTES) || !readable(STAGE_SEED, 4)) {
        fprintf(stderr, "[rb] handshake: the rules aren't writable here: this game's own (the match may differ)\n");
        return;
    }
    u32 differ = 0, first = 0, last = 0;
    for (u32 k = 2; k < RULE_BYTES; k++)   /* (bytes 0 and 1, the mode, stay this game's) */
        if (ld8(MELEE_INFO + k) != rules_blob[k]) { if (!differ++) first = k; last = k; }
    /* (two rulesets differ in their settings - stocks, time, item switches - not in most of the header: a header far from
     * this game's is no rule header, and stays unwritten) */
    if (differ > 256) {
        fprintf(stderr, "[rb] handshake: the host's rule header differs from this game's in %u of %u bytes: not rules as known, left alone\n",
                differ, RULE_BYTES);
        return;
    }
    for (u32 k = 2; k < RULE_BYTES; k++) if (ld8(MELEE_INFO + k) != rules_blob[k]) st8(MELEE_INFO + k, rules_blob[k]);
    u32 hist_differ = 0;
    for (u32 k = 0; k < STAGE_HIST_BYTES; k++)
        if (ld8(STAGE_HIST + k) != rules_blob[RULE_BYTES + k]) { hist_differ++; st8(STAGE_HIST + k, rules_blob[RULE_BYTES + k]); }
    u32 seed; memcpy(&seed, rules_blob + RULE_BYTES + STAGE_HIST_BYTES, 4);
    u32 seed_was = ld32(STAGE_SEED);
    st32(STAGE_SEED, seed);
    if (differ) fprintf(stderr, "[rb] handshake: the host's rules written over this game's: %u bytes of the rule header differed (from +%#x to +%#x)\n", differ, first, last);
    else fprintf(stderr, "[rb] handshake: the host's rules are this game's already\n");
    if (hist_differ || seed_was != seed)
        fprintf(stderr, "[rb] handshake: the stage set-up's inputs taken from the host (%u history bytes differed, seed %s)\n", hist_differ, seed_was != seed ? "differed" : "the same");
}

/* ---- the GO! freeze -----------------------------------------------------------------------------------------------
 * Two causes, both moved to the end of loading:
 * (1) A fight's first save copied all of memory (PC 2: 2 GB, 565 ms; later fights 935-1385 ms here, undo pages for all of
 *     it on top) on fight frame 1, as GO! appears. Now: at the end of loading the copy is taken (rb_save, every state let
 *     go after: rb_ring_drop), and through the entrances and the countdown kept current every PRE_EVERY frames, so the
 *     save at GO! copies a few frames' changes. (Outside fights the game keeps to real time by itself, catching up for
 *     frames that ran long: the entrance doesn't run late on a slower PC for it.)
 * (2) The games began the entrances apart (one loads faster), so the first came to GO! first and waited for the others'
 *     pads, then the frame advantage held it back: a stutter after GO!. Now a barrier at the end of loading: each game,
 *     once its fighters exist and its other threads have been at a wait for PRE_QUIET frames running (loading done),
 *     says READY to the host; the host waits for every player's and then tells them all to go on. The entrances run on
 *     logic frames after it, the same number on every game: they reach GO! together. (Not all at the barrier after
 *     PRE_WAIT_MS: each goes on, as before.)
 * Until that point, a line every 30 frames says what the frames before the fight hold (fighters, busy threads). */
#define PRE_EVERY 4
#define PRE_QUIET 3
#define PRE_WAIT_MS 30000.0
static int pre_done, pre_quiet;
static long long pre_frames;
static int ready_from[RBN_PLAYERS], all_ready;
static void net_pre_reset(void) { pre_done = 0; pre_quiet = 0; pre_frames = 0; memset(ready_from, 0, sizeof ready_from); all_ready = 0; }
static void net_ready_send(int all) {
    u8 out[RBN_READY_BYTES];
    RbnReady r = {net_local < 0 ? 0 : net_local, all};
    size_t k = rbn_ready_build(out, sizeof out, &r);
    if (k) rbl_send(out, (int)k);
}
static int net_ready_take(const u8 *buf, int len) {
    RbnReady r;
    if (!rbn_ready_parse(buf, (size_t)len, &r)) return 0;
    if (net_local == 0) { if (!r.all && r.player > 0 && r.player < net_n && rbl_accept(r.player) >= 0) ready_from[r.player] = 1; }
    else if (r.all && r.player == 0 && rbl_accept(0) >= 0) all_ready = 1;
    return 1;
}
/* the fighters are made (the fighter manager there, player 1's entry a battle object) */
static int fighters_made(Cpu *c) {
    static u64 inst = 1;
    if (inst == 1) inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    if (!inst) return 0;
    u64 mgr = ld64(inst), impl = mgr ? ld64(mgr) : 0;
    if (!impl) return 0;
    static Cpu keep;
    keep = *c;
    u32 id = (u32)rt_callsym(c, "_ZN3app7fighter20get_id_from_entry_idEi", 1, (u64)0);
    *c = keep;
    return id != 0x50000000u && id != 0;
}
extern double rb_park_quick_ms;   /* (hle_rb_state.c) */
static long long pre_refreshed, pre_skipped;
static void net_pre_fight(Cpu *c, u64 frame) {
    pre_frames++;
    /* (hj2: "fighters not yet" on every frame to GO! - the game gives the players their fighters at GO!, though the
     * fighters do their entrances before - so the barrier never came, and the whole copy stayed at GO!: 358 ms here,
     * 526 ms on PC 2, whose GO! then came that much later: the stutter. The two games had come to GO! the same number
     * of frames after the set-up. So now the whole copy is taken at the set-up, as the loading screen starts, and kept
     * current every PRE_EVERY frames through loading, the entrances and the countdown, each refresh waiting a few ms
     * at most for the threads (a thread busy loading: that refresh left out). The barrier waits on a better sign of
     * loading done: SSBU_RB_PRE_BARRIER=1 tries it as it is.) */
    static int barrier = -1;
    if (barrier < 0) { const char *e = getenv("SSBU_RB_PRE_BARRIER"); barrier = e && *e == '1'; }
    if (!barrier) {
        void rb_ring_drop(void);
        if (pre_frames == 1) {
            double t0 = now_ms();
            rb_save_tag = 0xFFFFFFFFu;
            int saved = rb_save(c, frame);
            rb_ring_drop();
            pre_refreshed = pre_skipped = 0;
            fprintf(stderr, "[rb] before the fight: the whole copy at the set-up %s in %.0f ms\n", saved == 1 ? "taken" : "NOT taken (the threads didn't stop)", now_ms() - t0);
            return;
        }
        if (pre_frames % PRE_EVERY) return;
        rb_park_quick_ms = 4;
        rb_save_tag = 0xFFFFFFFFu;
        double t0 = now_ms();
        int saved = rb_save(c, frame);
        rb_park_quick_ms = 0;
        if (saved == 1) { rb_ring_drop(); pre_refreshed++; } else pre_skipped++;
        if (pre_frames % 120 == 0)
            fprintf(stderr, "[rb] before the fight: frame %lld since the set-up: the copy refreshed %lld times, %lld left out (threads busy); this one %.1f ms\n",
                    pre_frames, pre_refreshed, pre_skipped, now_ms() - t0);
        return;
    }
    if (pre_done) {   /* the entrances and the countdown: the copy kept current */
        if (pre_frames % PRE_EVERY == 0) { rb_save_tag = 0xFFFFFFFFu; if (rb_save(c, frame) == 1) { void rb_ring_drop(void); rb_ring_drop(); } }
        return;
    }
    u32 alive, waiting, sleeping, nbusy; u64 busy[1];
    kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 0, &nbusy);
    int made = fighters_made(c);
    pre_quiet = made && !nbusy ? pre_quiet + 1 : 0;
    if (pre_frames % 30 == 1 || pre_quiet == PRE_QUIET)
        fprintf(stderr, "[rb] before the fight: frame %lld since the set-up, fighters %s, %u threads busy%s\n", pre_frames, made ? "made" : "not yet", nbusy,
                pre_quiet == PRE_QUIET ? ": loading done" : "");
    if (pre_quiet < PRE_QUIET) return;
    pre_done = 1;
    /* (1) the whole copy, here at the end of loading */
    double t0 = now_ms();
    rb_save_tag = 0xFFFFFFFFu;   /* (no fight frame's: let go at once anyway) */
    int saved = rb_save(c, frame);
    { void rb_ring_drop(void); rb_ring_drop(); }
    double copy_ms = now_ms() - t0;
    /* (2) every game here before the entrances */
    double t1 = now_ms();
    if (net_local == 0) {
        for (;;) {
            int all = 1;
            for (int p = 1; p < net_n; p++) all &= ready_from[p];
            if (all || now_ms() - t1 > PRE_WAIT_MS) break;
            net_hello(0); Sleep(2);   /* (hellos at their own pace: one call in 10) */
        }
        for (int k = 0; k < 5; k++) { net_ready_send(1); Sleep(2); }
    } else {
        double last = 0;
        while (!all_ready && now_ms() - t1 < PRE_WAIT_MS) {
            if (now_ms() - last > 16) { net_ready_send(0); last = now_ms(); }
            net_hello(0); Sleep(2);
        }
    }
    int ok = net_local == 0 ? 1 : all_ready;
    if (net_local == 0) for (int p = 1; p < net_n; p++) ok &= ready_from[p];
    fprintf(stderr, "[rb] before the fight: loading done at frame %lld; the whole copy %s in %.0f ms; %s after %.0f ms\n", pre_frames,
            saved == 1 ? "taken" : "NOT taken (the threads didn't stop)", copy_ms, ok ? "every game ready, the entrances start" : "NOT every game came: going on", now_ms() - t1);
}

/* (the results screen, research: each game runs its menus alone after a fight, so the other players' cards never show
 * ready on it. To pass ready presses on there and stop before character select, a sign of the change is needed: after a
 * fight, until the next set-up, these candidates go to the log every 30 frames and whenever one changes) */
/* ---- the results screen -------------------------------------------------------------------------------------------
 * Each game runs its menus alone after a fight, so on a PC the other players' cards never showed ready and its results
 * screen never ended (hj3: stuck on it). hj4: passing each player's presses to the others got the host through, but the
 * host's presses never showed on PC 2 (sent before PC 2's own results began, it seems). So now (the user's idea) each
 * game also presses its own player's A and B on every other player's card: one press readies them all, each PC going on
 * when its own player does; the presses from the others still count too. Now from the fight's end each game sends its own player's A and B every frame
 * (MENU, through the host), and presses each other player's for it (rb_pad): a friend readies on its PC and its card is
 * ready on all of them; the screen goes on once every player has, as at one console. It stops (to keep character select
 * alone: a passed A there could add a player) when the fighter manager the fight left changes (the fight's scene gone:
 * hj3 saw it stay the same through the results), at the next match set-up, or MENU_MAX_MS after the fight; a player
 * not heard from for half a second has its buttons let go. Every change of the fighter manager is logged, to see
 * when the scene goes. */
#define MENU_MAX_MS 180000.0
static double menu_heard_ms[RBN_PLAYERS];
static int net_menu_take(const u8 *buf, int len) {
    RbnMenu m;
    if (!rbn_menu_parse(buf, (size_t)len, &m)) return 0;
    if (m.player == net_local || m.player >= net_n || rbl_accept(m.player) < 0) return 1;
    if (net_local == 0) for (int k = 1; k < net_n; k++) if (k != m.player) rbl_send_to(k, buf, len);   /* (the host passes it on) */
    if (net_results) {
        LONG was = net_menu_remote[m.player];
        InterlockedExchange(&net_menu_remote[m.player], m.buttons);
        menu_heard_ms[m.player] = now_ms();
        if (m.buttons & ~was & 1) fprintf(stderr, "[rb] results: player %d pressed A\n", m.player + 1);
    }
    return 1;
}
/* (hj5: the logic frame stood a second in the middle of the results animation - the mirroring ended before anyone
 * readied, and neither PC got through. The game says itself whether it shows the results:
 * app::lua_bind::FighterManager__is_result_mode_impl (the fighters do their victory poses there, so the fighter manager
 * lives on through it). Mirroring is on while it says so, and ends when it stops; the next set-up and 3 minutes end it too.
 * Its changes are logged.) */
static u64 fm_ask(Cpu *c, u64 mgr, const char *sym) {   /* (a FighterManager query, the main thread's registers kept) */
    if (!mgr) return 0;
    static Cpu keep;
    keep = *c;
    u64 v = rt_callsym(c, sym, 1, mgr);
    *c = keep;
    return v;
}
static int net_result_mode(Cpu *c, u64 mgr) { return (int)(fm_ask(c, mgr, "_ZN3app8lua_bind35FighterManager__is_result_mode_implEPNS_14FighterManagerE") & 0xff); }
/* (hj6: is_result_mode came on 113 frames after the fight - mirroring began right as the results showed - but stayed on
 * in character select, so the mirroring went on there. The fighters are let go after the results, the manager staying:
 * the end is now its entry count or its fighter count changing from their values as the results began. Those, the
 * result mode, is_ready_go, the stage choice and the last set-up's first panel (a hash, every 30 frames) are logged
 * whenever one changes, to see character select come.) */
u64 net_panel0;   /* (the first panel state at the last set-up: the record builder's x0) */
static void net_results_frame(Cpu *c) {
    static long long fight_seen = -1, frames;
    static u64 mgr0;
    static double t0;
    static int ended, rm_last;
    static double t_end;
    static u64 cnt0, num0, seen[6];
    static u64 inst = 1;
    if (inst == 1) inst = rt_symbol("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    u64 mgr = inst ? ld64(inst) : 0, impl = mgr ? ld64(mgr) : 0;
    if (fight_seen != S.fights) {   /* a fight just ended */
        fight_seen = S.fights; mgr0 = mgr; t0 = now_ms(); frames = 0; ended = 0; rm_last = -1; memset(seen, 0xff, sizeof seen);
        memset((void *)net_menu_remote, 0, sizeof net_menu_remote); InterlockedExchange(&net_menu_mine, 0);
        InterlockedExchange(&net_results, 0);
    }
    frames++;
    if (ended && now_ms() - t_end > 15000) return;   /* (then character select alone: the fighter manager no longer asked) */
    int rm = impl ? net_result_mode(c, mgr) : 0;
    u64 cnt = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind32FighterManager__entry_count_implEPNS_14FighterManagerE") & 0xffffffff : 0, num = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind38FighterManager__total_fighter_num_implEPNS_14FighterManagerE") & 0xffffffff : 0;
    u64 rg = impl ? fm_ask(c, mgr, "_ZN3app8lua_bind32FighterManager__is_ready_go_implEPNS_14FighterManagerE") & 0xff : 0;
    static u64 ph;
    if (frames % 30 == 1) ph = net_panel0 && readable(net_panel0, 0x460) ? hfnv(0xcbf29ce484222325ull, G2H(net_panel0), 0x460) : 0;
    u64 v[6] = {(u64)rm, cnt, num, rg, ld32(0x710530a968ull), ph};
    if (memcmp(v, seen, sizeof v) && frames < 20000)
        fprintf(stderr, "[rb] results: frame %lld after the fight: result mode %llu, entry count %llu, fighters %llu, ready-go %llu, stage choice %llu, panel 1 %016llx\n",
                frames, v[0], v[1], v[2], v[3], v[4], v[5]);
    memcpy(seen, v, sizeof v);
    rm_last = rm;
    if (!net_results) {
        if (!ended && rm) {
            InterlockedExchange(&net_results, 1); cnt0 = cnt; num0 = num;
            fprintf(stderr, "[rb] results: this game's A and B pressed on every player's card too (entry count %llu, fighters %llu)\n", cnt, num);
        }
        return;
    }
    const char *end = !rm ? "the results are over" : cnt != cnt0 || num != num0 ? "the fighters are let go (character select)" : mgr != mgr0 ? "the fight's scene is gone"
                    : now_ms() - t0 > MENU_MAX_MS ? "3 minutes on" : NULL;
    if (end) {
        InterlockedExchange(&net_results, 0); ended = 1; t_end = now_ms();
        fprintf(stderr, "[rb] results: %s: A and B no longer mirrored\n", end);
        return;
    }
    for (int p = 0; p < net_n; p++)
        if (p != net_local && net_menu_remote[p] && now_ms() - menu_heard_ms[p] > 500) InterlockedExchange(&net_menu_remote[p], 0);
    RbnMenu m = {net_local < 0 ? 0 : net_local, (int)net_menu_mine};
    u8 out[RBN_MENU_BYTES];
    size_t k = rbn_menu_build(out, sizeof out, &m);
    if (k && net_local >= 0) rbl_send(out, (int)k);
    static LONG mine_was;
    if (m.buttons & ~mine_was & 1) fprintf(stderr, "[rb] results: this game's player pressed A\n");
    mine_was = m.buttons;
}

static void net_hello_wait(void) {
    if (hs_agreed) return;
    fprintf(stderr, "[rb] handshake: the match is being set up but the seed is not agreed: waiting for the other games (up to 10 min)\n");
    double t0 = now_ms();
    while (!hs_agreed && now_ms() - t0 < 600000) { net_hello(1); net_wait_say(net_local == 0 ? "Waiting for players to join..." : "Waiting for the host..."); Sleep(16); }
    net_wait_say(NULL);
    if (hs_agreed) {
        fprintf(stderr, "[rb] handshake: agreed after %.1f s of waiting\n", (now_ms() - t0) / 1000);
        for (int k = 0; k < 5; k++) { net_hello_send(); Sleep(20); }   /* (player 2's answer, again: one lost would leave player 1 waiting) */
    } else fprintf(stderr, "[rb] handshake: no agreement after 10 min: going on (the start may differ)\n");
}

static void savestates(Cpu *c, u64 frame) {
    static int ready, prev_save, prev_load;
    if (!ready) {
        ready = 1;
        const char *e = getenv("SSBU_RB_AUTO");
        if (e && sscanf(e, "%lld,%lld,%lld", &auto_at, &auto_every, &auto_left) >= 2)
            fprintf(stderr, "[rb] auto test: save at fight frame %lld, then load every %lld frames, %lld times\n", auto_at, auto_every, auto_left);
        else auto_at = -1;
        e = getenv("SSBU_RB_VERIFY");
        if (e && sscanf(e, "%lld,%lld,%lld", &ver_at, &ver_k, &ver_rounds) >= 2)
            fprintf(stderr, "[rb] replay check: save at fight frame %lld, reference %lld frames later, %lld replays\n", ver_at, ver_k, ver_rounds);
        else ver_at = -1;
        e = getenv("SSBU_RB_BENCH");
        if (e && sscanf(e, "%lld,%lld", &bench_from, &bench_n) == 2)
            fprintf(stderr, "[rb] save benchmark: a save at every fight frame from %lld, %lld times\n", bench_from, bench_n);
        else bench_from = -1;
        e = getenv("SSBU_RB_RING");
        int size = 0;
        if (e && sscanf(e, "%lld,%lld,%lld,%lld,%d", &ring_from, &ring_every, &ring_depth, &ring_left, &size) >= 4 && ring_depth >= 1) {
            if (size <= ring_depth) size = (int)ring_depth + 2;
            rb_ring_size(size);
            rb_quiet(1);
            fprintf(stderr, "[rb] ring test: from fight frame %lld a save every frame (%d kept); every %lld frames back %lld, %lld times; "
                    "each frame's fight hash checked against its first pass\n", ring_from, size, ring_every, ring_depth, ring_left);
        } else ring_from = -1;
        e = getenv("SSBU_RB_NET");
        net_mode = e && !strncmp(e, "loop", 4) ? 1 : e && !strncmp(e, "net,", 4) ? 2 : 0;
        if (net_mode == 2) {
            net_lag_hi = -2;
            /* (M7: SSBU_RB_NET_PLAYERS=<n>, the host's: how many play, 2-4; the others take it from the host's hello) */
            const char *np = getenv("SSBU_RB_NET_PLAYERS");
            if (np && atoi(np) >= 2 && atoi(np) <= 4) net_n = atoi(np);
            /* (Host/Join: player -1, a joining game: its number comes from the host; it needs the host's address) */
            if (sscanf(e + 4, "%d,%d,%d,%d,%d,%63s", &net_local, &net_port, &net_peer_port, &net_delay, &net_frames, net_host) < 3 || net_local < -1 ||
                net_local > 3 || (net_local < 0 && !strcmp(net_host, "*")) || !rbl_open(net_local, net_port, net_host, net_peer_port)) {
                fprintf(stderr, "[rb] net mode: SSBU_RB_NET=net,<player 0-3, -1 to join>,<my port>,<peer port>[,<delay>,<frames>,<host>] not usable (%s): off\n", e);
                net_mode = 0;
            } else {
                if (rerun_save < 0) { const char *r = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = r && *r ? atoi(r) : 3; }
                rb_ring_size(RBN_MAX_BACK + 3);
                rb_quiet(1);
                if (net_frames < (1 << 19)) net_frames = 1 << 19;   /* (the whole fight: some 2.4 hours of frames) */
                { const char *l = getenv("SSBU_RB_NET_LIVE"); net_live = l && *l == '1'; }
                fprintf(stderr, "[rb] net mode: this game's player plays with %s\n", net_live ? "its controllers (live)" : "its bot");
                char dl[48];
                if (net_delay < 0) snprintf(dl, sizeof dl, "%s", net_local == 0 ? "from the round trip" : "the host's");
                else snprintf(dl, sizeof dl, "%d%s", net_delay, net_local != 0 ? " (the host's counts)" : "");
                if (net_local == 0) rbl_players(net_n);
                if (net_local < 0) fprintf(stderr, "[rb] net mode: this game joins: its player number comes from the host; input delay %s\n", dl);
                else fprintf(stderr, "[rb] net mode: this game plays player %d%s; input delay %s; fight hashes of the first %d frames compared\n",
                             net_local + 1, net_local == 0 ? " (the host)" : "", dl, net_frames);
                if (net_local == 0) fprintf(stderr, "[rb] net mode: %d players (SSBU_RB_NET_PLAYERS)\n", net_n);
            }
        }
        if (net_mode == 1) {
            if (e[4] == ',') sscanf(e + 5, "%d,%d,%d,%d,%d", &net_delay, &net_lag_lo, &net_lag_hi, &net_loss, &net_frames);
            if (rerun_save < 0) { const char *r = getenv("SSBU_RB_RERUN_SAVE"); rerun_save = r && *r ? atoi(r) : 3; }
            rb_ring_size(RBN_MAX_BACK + 3);
            rb_quiet(1);
            fprintf(stderr, "[rb] loop mode (M5): player 2's pads through rb_net, delay %d, lag %d..%d frames, %d%% lost; fight hashes of the first %d frames\n",
                    net_delay, net_lag_lo, net_lag_hi, net_loss, net_frames);
        }
    }
    if (net_mode && S.in_fight) { net_step(c, frame); return; }
    /* (net mode: a fight that ended before <frames> still gets its report, and the whole fight is checked:
     * the launcher's 7200 is a floor, not a window - twopc8's report never came, its fight ending sooner) */
    if (net_mode == 2 && !S.in_fight && net_got && !net_done) {
        net_done = 1; net_report(); ch_have_mine = 0; memset(ch_have_peer, 0, sizeof ch_have_peer);
        rules_have = 0; rules_id = 0; rules_chunks = 0;   /* (the next set-up's rules again: the host's may change) */
    }
    /* M5 step 3b: the same start on both games. The match set-up (13.0.4 0x71014d8620) stores the next
     * value of the game's global xorshift128 (4 words, 13.0.5 0x71052391c0: 13.0.4's 0x71052381c0 one page
     * on, as all of main's data) in the match block, and the fight's random set-up, the starting sides
     * among it, comes from that; the menus advance the generator, so two games had different starts
     * about half the time (ssbu-recomp PR #116's handshake found the store: debug_mode.c). Outside fights,
     * with SSBU_RB_SEED, it is set from the seed at every safe point, so the set-up draws the same in both
     * games. (Until a handshake agrees on the seed between two PCs: the same SSBU_RB_SEED on both.) */
    if (net_mode == 2 && !S.in_fight) net_hello(0);
    if (net_mode == 2 && !S.in_fight) net_seed_global("menus");
    /* (net mode, from a match set-up until the fight: each frame waits for the game's other threads to be at a
     * wait - loading done - up to SSBU_RB_NET_SETTLE_MS (default 500). The fighters' entrances begin once their
     * files are loaded, checked each frame, so the frame they began on followed each PC's disk: the slower PC
     * had Steve's entrance 90 frames later and Kazuya's a frame apart, the same in three runs (twopc2-4).) */
    static double settle_limit = -1;
    if (settle_limit < 0) { const char *e = getenv("SSBU_RB_NET_SETTLE_MS"); settle_limit = e && *e ? atof(e) : 0; }   /* (off by default from twopc13: it made the countdown stutter and didn't help) */
    if (net_mode == 2 && !S.in_fight && net_countdown && settle_limit > 0) {
        double limit = settle_limit;
        static long long settled, timed_out;
        static double settle_ms;
        u32 alive, waiting, sleeping, nbusy; u64 busy[16];
        double t0 = now_ms();
        for (;;) {
            kernel_rb_census(c->thread, &alive, &waiting, &sleeping, busy, 16, &nbusy);
            if (!nbusy || now_ms() - t0 >= limit) break;
            Sleep(0);
        }
        settle_ms += now_ms() - t0;
        if (nbusy) timed_out++; else settled++;
        if ((settled + timed_out) % 60 == 1 || nbusy)
            if (timed_out <= 20)
                fprintf(stderr, "[rb] net mode, before the fight: frame %lld settled, %lld timed out (%.0f ms waited in all)%s\n",
                        settled, timed_out, settle_ms, nbusy ? ": a thread stayed busy" : "");
    }
    if (net_mode == 2 && !S.in_fight && net_countdown) net_pre_fight(c, frame);   /* (the GO! freeze) */
    if (net_mode == 2 && !S.in_fight && !net_countdown && net_got) net_results_frame(c);   /* (the results screen) */
    if (ring_from >= 0 && S.in_fight && S.frames >= ring_from) { ring_test(c, frame); return; }
    /* SSBU_RB_BENCH=<frame>,<count> (M3): a save at every fight frame from <frame>, <count> of them,
     * as rollback's per-frame capture would do; before a replay check's save, that save is a
     * page-by-page one too, which the check then tests. */
    if (bench_from >= 0 && S.in_fight && S.frames >= bench_from && S.frames < bench_from + bench_n) {
        rb_save(c, frame);
        return;
    }
    if (ver_at >= 0 && S.in_fight && ver_phase < 3) {
        if (ver_phase) ver_since++;
        /* A game counter, 0x71052c5184, that moves once per logic frame: which safe points ran more
         * than one (the game catching up), as a list of "frame:+n" after each save or load. */
        static u32 prev_count; static char steps[512]; static size_t ns;
        u32 count = ld32(0x71052c5184ull);
        if (ver_phase && ver_since > 0 && count - prev_count != 1 && ns < sizeof steps - 16)
            ns += snprintf(steps + ns, sizeof steps - ns, " %lld:+%d", ver_since, (int)(count - prev_count));
        prev_count = count;
        if (ver_phase && ver_since == ver_k) {
            fprintf(stderr, "[rb] verify phase %d: counter %u after %lld frames; safe points that were not one logic frame:%s\n",
                    ver_phase, count, ver_since, ns ? steps : " none");
            ns = 0; steps[0] = 0;
        }
        fight_hash(c, ver_phase ? ver_since : 0);
        if (ver_phase == 2 && ver_since == ver_k) {
            if (!hash_first_bad) fprintf(stderr, "[rb] fight hash: replay %lld matches the reference on all %lld frames\n", ver_round + 1, ver_k);
            fprintf(stderr, "[rb] input tape: %lld pad reads recorded, %lld played back so far\n", tape_recorded, tape_replayed);
            hash_first_bad = 0;
        }
        if (ver_phase == 0 && S.frames == ver_at) { ver_phase = 1; ver_since = 0; rb_save(c, frame); return; }
        if (ver_phase == 1 && ver_since == ver_k) { rb_ref_capture(c); ver_phase = 2; ver_since = 0; rb_load(c, frame); return; }
        if (ver_phase == 2 && ver_since == ver_k) {
            rb_ref_compare(c, ++ver_round);
            if (ver_round >= ver_rounds) { ver_phase = 3; return; }
            ver_since = 0;
            rb_load(c, frame);
            return;
        }
        return;   /* (no hotkeys while it runs) */
    }
    if (S.in_fight) fight_hash(c, 0);
    if (auto_at >= 0 && S.in_fight) {
        if (S.frames == auto_at) { auto_next = S.frames + auto_every; rb_save(c, frame); return; }
        if (S.frames > auto_at && S.frames >= auto_next && auto_left > 0) { auto_left--; auto_next = S.frames + auto_every; rb_load(c, frame); return; }
    }
    int focused = gfx_window_focused();
    int save = focused && (GetAsyncKeyState(VK_F7) & 0x8000), load = focused && (GetAsyncKeyState(VK_F8) & 0x8000);
    int new_save = save && !prev_save, new_load = load && !prev_load;
    prev_save = save; prev_load = load;
    if (new_save) rb_save(c, frame);
    else if (new_load) rb_load(c, frame);
}

/* FUN 0x71038701e0: the graphics module's frame end (13.0.4 0x710386fc30, as ssbu-recomp's
 * fighter_host.c fh_frame_end, SSBU_DETERMINISTIC). It compares the refresh count of the game's
 * vsync count thread with the count at the last frame and reports the difference less one,
 * "dropped" refreshes, through its third argument (result code 0xF0044007 / 0xF0044005): the game
 * then runs that many EXTRA logic frames. That count follows wall time, so a replay ran 0-3 more
 * logic frames than the run it repeats. In a fight the report is cleared: one logic frame per
 * main loop iteration, always. SSBU_RB_DROPS=1 leaves it (the game's own catching up). */
static int keep_drops = -1;
static long long drops_cleared, drop_reports;
/* signature: f81b0ff9 a9015ff8 a90257f6 a9034ff4 a9047bfd 910103fd f900005f f9405437 3943b828 aa0203f5 aa0103f3 aa0003f4 34.....8 9100c2f6 1....... aa1703f6 */
HLE_WRAP(71038701e0) {
    u64 p1 = c->x[0], p3 = c->x[2];
    seg_fe_in = now_ms();
    f_71038701e0_orig(c);
    seg_fe_out = now_ms();
    /* (net mode: outside fights too: the entrance and the countdown before "GO" run on logic frames as well,
     * and a slower PC caught up there with extra ones: two PCs began the fight a frame apart, animations and
     * an idle variant's random draw out of step (twopc3: Kazuya's wait motion at frame 63 on one PC, 64 on the
     * other, Steve on another motion and one draw more). In the menus a lagging PC then runs slower instead.) */
    if (!rb_on || !p3 || !(S.in_fight || (net_mode == 2 && net_early_on()))) return;
    if (keep_drops < 0) { const char *e = getenv("SSBU_RB_DROPS"); keep_drops = e && *e == '1'; }
    u64 n = ld64(p3);
    if (!n || keep_drops) return;
    drop_reports++; drops_cleared += (long long)n;
    if (rb_log || drop_reports <= 5) fprintf(stderr, "[rb] frame end reported %llu dropped refreshes (fight frame %lld): cleared (%lld reports, %lld frames so far)\n",
                                             (unsigned long long)n, S.frames, drop_reports, drops_cleared);
    st64(p3, 0);
    if (p1 && (ld32(p1) == 0xF0044007u || ld32(p1) == 0xF0044005u)) st32(p1, 0);
}

/* ---- M4: re-run frames without their drawing --------------------------------------------
 * While rb_fast > 1 (the frames a load runs again, all but the last, whose picture is shown next),
 * the main loop's presentation work is left out, as ssbu-recomp's rollback re-run passes do
 * (runahead.c ra_kick2 and ring_simulation.c's "lite" list, 13.0.4 addresses mapped by that
 * project's verified table, hostrt.c g_code_13_0_5):
 *  - SSBU_RB_SKIP=<mask> (default 0; was 49, the PR's, which crashed long fights): the job-group kicks (FUN 0x7103549720) called
 *    from: 1 the render preparation (0x710374a0d4 - the safe point's own call - 0x710374baa8,
 *    0x710374bad8, 0x710374bb08); 16 preparation list A (0x7103749aac); 32 list B (0x7103749b48);
 *  - SSBU_RB_LITE=<mask> (default 57: all but 2, 4 and 64, see skip_flags): the presentation callees of the main loop (FUN
 *    0x7103747820, 0x6000 bytes), by group: 1 camera, 2 UI, 4 lighting, 8 render layers,
 *    16 recorder, 32 dynamic resolution, 64 effect emitter tick; each returns a fixed value.
 * The fight hash then says whether the frames after are still the same. */
#define MAIN_LOOP_LO 0x7103747820ull
#define MAIN_LOOP_HI 0x710374d820ull
static void skip_flags(void) {
    const char *e = getenv("SSBU_RB_SKIP"), *l = getenv("SSBU_RB_LITE");
    skip_mask = e && *e ? atoi(e) : 0;   /* (was 49, the PR's: leaving the job-group kicks out of re-run frames crashed long
                                          * fights - a null read in the job system or the game after some 4 000 - 14 000 logic
                                          * frames of rollbacks, often as the fight ended; the very same fight (stress3 / stress4,
                                          * bots, 6359 frames) crashed at the winner screen with 49 and finished with 0) */
    lite_mask = l && *l ? atoi(l) : 57;    /* (not 64 either, the effect emitter tick: effects made in re-run frames and never ticked crashed the
                                            * fight's end after many rollbacks - main+0x35c4580 - the same fight crashing with it skipped (stress5)
                                            * and finishing twice without (stress7) and with no lite skips at all (stress6)) */   /* (not 4, lighting: fighters and items drew flat green from then on, and it saved nothing measurable;
                                            * not 2, UI: the HUD's animations ("GO!", the stock counter after a KO) jittered and stuck in the game
                                            * that rolled back most, every load putting them back and the re-run frames not moving them on: ui1) */
}
static int kick_skipped(u64 lr) {
    if (rb_fast <= 1) return 0;
    if (skip_mask < 0) skip_flags();
    int prep = lr == SAFE_POINT || lr == 0x710374baa8ull || lr == 0x710374bad8ull || lr == 0x710374bb08ull;
    int la = lr == 0x7103749aacull, lb = lr == 0x7103749b48ull;
    if (((skip_mask & 1) && prep) || ((skip_mask & 16) && la) || ((skip_mask & 32) && lb)) { kicks_skipped++; return 1; }
    /* 2: the render list's kick (0x710374c688). Its culling job sets a byte the main loop spins on
     * (13.0.4 0x7103749f48), [0x71053375a8] + 0x140 (13.0.4 0x71053365a8, data one page on): set
     * here instead, as the PR's runahead.c ra_kick2 does. The PR measured 0.3% of the picture changed.
     * HERE IT CRASHES (rend_crash: a task worker calls a stale pointer from the job runner in the
     * first rollbacks): not in the default mask. */
    if ((skip_mask & 2) && lr == 0x710374c688ull) {
        u64 p = ld64(0x71053375a8ull);
        if (p) st8(p + 0x140, 1);
        kicks_skipped++;
        return 1;
    }
    return 0;
}
static int lite_skipped_now(Cpu *c, int grp) {
    if (rb_fast <= 1) return 0;
    if (lite_mask < 0) skip_flags();
    u64 lr = c->x[30];
    if (!(lite_mask & grp) || lr < MAIN_LOOP_LO || lr >= MAIN_LOOP_HI) return 0;
    lite_skipped++;
    return 1;
}
#define LITE(a, grp, ret) do { if (lite_skipped_now(c, grp)) { c->x[0] = (ret); return; } f_##a##_orig(c); } while (0)

/* signature: d10483ff fd0063ec 6d0d2beb 6d0e23e9 a90f57fc a9104ff4 a9117bfd 910443fd f9424808 a94e5514 aa0003f3 eb15029f 54.....0 39468288 35.....8 39468688 */
HLE_WRAP(710361d5f0) { LITE(710361d5f0, 1, 0); }
/* signature: fc1b0fe8 a9015ff8 a90257f6 a9034ff4 a9047bfd 910103fd f9400418 f9401f08 aa0003f3 b4.....8 f9400709 b941ad2a 34.....a bd41c121 bd41c522 .0.....a */
HLE_WRAP(7103618c80) { LITE(7103618c80, 1, 0); }
/* signature: d102c3ff a9094ff4 a90a7bfd 910283fd aa0003f3 9....... 9....... f9402c08 f9402d14 f9400288 f9401508 aa1403e0 d63f0100 f9400288 f9401908 aa1403e0 */
HLE_WRAP(7103864cb0) { LITE(7103864cb0, 1, 0); }
/* signature: d101c3ff a9016ffc a90267fa a9035ff8 a90457f6 a9054ff4 a9067bfd 910183fd f9428016 f9428417 eb1702df 54.....0 aa0003f3 321c03f9 .0.....a 91....5a */
HLE_WRAP(7103619630) { LITE(7103619630, 1, 0); }
/* signature: f81d0ff5 a9014ff4 a9027bfd 910083fd f9400408 f9400114 .0.....8 f9....08 f9....08 f9....08 f9....08 39....09 1e2703e1 aa0003f3 34.....9 3d....00 */
HLE_WRAP(7103619130) { LITE(7103619130, 1, 0); }
/* signature: d10443ff a90b6ffc a90c67fa a90d5ff8 a90e57f6 a90f4ff4 a9107bfd 910403fd 910ca014 aa0003f3 aa1403e0 9....... f9417a68 b940151c b4.....8 b9400111 */
HLE_WRAP(7103632e00) { LITE(7103632e00, 2, 0); }
/* signature: a9be4ff4 a9017bfd 910043fd .0.....8 f9....08 .0.....9 f9....21 91....00 9....... b4.....0 f9401408 f9....13 f942e268 b4.....8 b9....09 8b091669 */
HLE_WRAP(710361ed90) { LITE(710361ed90, 2, 0); }
/* signature: d10243ff a9036ffc a90467fa a9055ff8 a90657f6 a9074ff4 a9087bfd 910203fd 39458008 aa0003f3 35.....8 39458668 34.....8 .0.....8 f9....08 f9....08 */
HLE_WRAP(71035c1980) { LITE(71035c1980, 2, 0); }
/* signature: fc1c0fe8 a90157f6 a9024ff4 a9037bfd 9100c3fd b9400808 aa0003f3 7100091f 54.....3 7100011f 54.....d f9400a6a b940426b 2a1f03e9 2a0803ec 0b0c012d */
HLE_WRAP(7103576550) { LITE(7103576550, 4, 0); }
/* signature: b9400808 34.....8 d10343ff a90a57f6 a90b4ff4 a90c7bfd 910303fd ad461003 4e0c0401 4f849026 4f849821 4e140402 6e20dc65 4e0404a5 6e20dc91 4e0c0631 */
HLE_WRAP(7103576210) { LITE(7103576210, 4, 0); }
/* signature: bd405800 bd407801 1e210800 aa0003e8 37f....2 b9405d09 6b02013f 54.....d b9805109 93407c4a 9b0a7d29 7940050a 370....a 371....a 371....a 320003e0 */
HLE_WRAP(7103576970) { LITE(7103576970, 4, 0); }
/* signature: d101c3ff f90023f5 a9054ff4 a9067bfd 910183fd 5293f908 72a000c8 5293f989 72a000c9 78686808 78296808 5293f8c9 72a000c9 7869680a 5293f94b 72a000cb */
HLE_WRAP(71035941f0) { LITE(71035941f0, 4, 0); }
/* signature: d10183ff a90357f6 a9044ff4 a9057bfd 910143fd 7100283f 54.....2 93407c28 321d07e9 9b090109 a941a52a aa0003f5 eb09015f 54.....0 .0.....9 f9....29 */
HLE_WRAP(7103785da0) { LITE(7103785da0, 8, 0); }
/* signature: f81c0ff7 a90157f6 a9024ff4 a9037bfd 9100c3fd 91198013 aa0003f5 aa1303e0 9....... 911902b4 aa1403e0 9....... b9400ab6 aa1403e0 9....... 71001adf */
HLE_WRAP(71036f31b0) { LITE(71036f31b0, 16, 0); }
/* signature: f81c0ff7 a90157f6 a9024ff4 a9037bfd 9100c3fd 91198013 aa0003f5 aa1303e0 9....... 911902b4 aa1403e0 9....... b9400ab6 aa1403e0 9....... 710012df */
HLE_WRAP(71036f32f0) { LITE(71036f32f0, 16, 0); }
/* signature: f81d0ff5 a9014ff4 a9027bfd 910083fd 39404008 aa0003f3 34.....8 9102a274 aa1403e0 9....... 9101a275 aa1503e0 9....... aa1403e0 9....... b4.....0 */
HLE_WRAP(71036e73b0) { LITE(71036e73b0, 16, 0); }
/* signature: d104c3ff fd0063e8 a90d6ffc a90e67fa a90f5ff8 a91057f6 a9114ff4 a9127bfd 910483fd 39406008 34.....8 39406408 aa0003f3 34.....8 321403f5 1....... */
HLE_WRAP(71036f79c0) { LITE(71036f79c0, 16, 1); }
/* signature: .0.....8 bd....02 .0.....9 bd....23 1e202040 1e210460 54.....0 bd....00 .0.....8 f9....08 bd....21 f9....08 .0.....9 2d461504 .0.....8 7e21d8a5 */
HLE_WRAP(71036194a0) { LITE(71036194a0, 32, 0); }
/* signature: d10603ff 6d102beb 6d1123e9 a9126ffc a91367fa a9145ff8 a91557f6 a9164ff4 a9177bfd 9105c3fd bd401401 bd426800 1e202028 aa0003f3 54.....0 1e212800 */
HLE_WRAP(710356dd50) { LITE(710356dd50, 64, 0); }

/* ---- M4 profile: the game's task tree (SSBU_RB_TREE=1) --------------------------------------
 * FUN 0x7103725030 (13.0.4 0x7103724a80, ssbu-recomp's ring_simulation.c profile 2) walks the
 * game's task tree once per logic step: node->vt[0x28], its children (re-entering here), node->vt[0x30].
 * Timed per node class (its vtable), exclusive of its children, on the guest main thread, apart for
 * normal frames and re-run ones; the busiest classes are listed at the end of the ring test. */
typedef struct TreeE { u64 vt; double incl[2], excl[2]; long long n[2]; int dmin; } TreeE;
#define TREE_N 2048
static TreeE tree_e[TREE_N];
static int tree_on = -1, tree_depth;
static double tree_child[256];
static TreeE *tree_slot(u64 vt) {
    u32 h = (u32)((vt * 0x9e3779b97f4a7c15ull) >> 52) % TREE_N;
    for (int i = 0; i < TREE_N; i++) { TreeE *e = &tree_e[(h + (u32)i) % TREE_N]; if (e->vt == vt) return e; if (!e->vt) { e->vt = vt; e->dmin = 999; return e; } }
    return NULL;
}
static int tree_cmp(const void *a, const void *b) { double x = ((const TreeE *)a)->excl[1], y = ((const TreeE *)b)->excl[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void tree_report(void) {
    if (tree_on != 1) return;
    TreeE *v = malloc(sizeof tree_e);
    int n = 0;
    for (int i = 0; i < TREE_N; i++) if (tree_e[i].vt) v[n++] = tree_e[i];
    qsort(v, n, sizeof *v, tree_cmp);
    double tot[2] = {0, 0};
    for (int i = 0; i < n; i++) { tot[0] += v[i].excl[0]; tot[1] += v[i].excl[1]; }
    fprintf(stderr, "[rb] task tree: %d node classes; exclusive time in all: normal frames %.1f ms, re-run frames %.1f ms (sums)\n", n, tot[0], tot[1]);
    for (int i = 0; i < n && i < 30; i++)
        fprintf(stderr, "[rb]   vt %#llx depth %d: re-run %6.1f ms excl (%5.1f%%) %6.1f incl, %lld calls | normal %6.1f ms excl, %lld calls\n",
                (unsigned long long)v[i].vt, v[i].dmin, v[i].excl[1], tot[1] ? 100 * v[i].excl[1] / tot[1] : 0, v[i].incl[1], v[i].n[1], v[i].excl[0], v[i].n[0]);
    free(v);
}
/* signature: d10243ff a9036ffc a90467fa a9055ff8 a90657f6 a9074ff4 a9087bfd 910203fd b9403008 aa0003f3 7100051f 54.....0 7100091f 54.....1 aa1303f4 f8438e80 */
HLE_WRAP(7103725030) {
    if (tree_on < 0) { const char *e = getenv("SSBU_RB_TREE"); tree_on = e && *e == '1'; }
    if (tree_on != 1 || !S.in_fight || c->thread != rb_main_kthread || tree_depth >= 255) { f_7103725030_orig(c); return; }
    u64 node = c->x[0], vt = node ? ld64(node) : 0;
    int kd = rb_fast > 0, d = tree_depth;
    tree_child[d + 1] = 0; tree_depth++;
    double t0 = now_ms();
    f_7103725030_orig(c);
    double dt = now_ms() - t0;
    tree_depth--;
    TreeE *e = tree_slot(vt ? vt : 1);
    if (e) { e->n[kd]++; e->incl[kd] += dt; e->excl[kd] += dt - tree_child[d + 1]; if (d < e->dmin) e->dmin = d; }
    tree_child[d] += dt;
}

/* ---- M5: who draws from the script random generators at a fight's start ----------------------
 * app::sv_math::rand(Hash40, int) and randf(Hash40, float): the Hash40 picks one of the 9 generators.
 * Two runs differed in when generator 1 was drawn (every other frame, on even frames in one run and
 * odd in another: dbg1). In loop mode, each draw of the first 12 fight frames is logged with its
 * caller, its Hash40 and which generator moved. */
static void rand_log(Cpu *c, const char *fn, u64 hash, u64 lr, const u32 *before) {
    u64 rb = rng_base();
    int moved = -1;
    for (int q = 0; q < 9 && rb; q++) if (ld32(rb + 20ull * q + 16) != before[q]) moved = q;
    fprintf(stderr, "[rb] draw: %s(%#llx) from %#llx, generator %d, logic frame %u, fight frame %lld%s\n", fn, (unsigned long long)hash,
            (unsigned long long)lr, moved, ld32(LOGIC_FRAME), S.frames, rb_fast > 0 ? " (re-run)" : "");
    (void)c;
}
#define RAND_WRAP(addr, name) do { \
        static int logged_; \
        int log_ = net_mode >= 1 && S.in_fight && S.frames <= 300 && logged_ < 60; \
        if (log_) logged_++; \
        u64 hash_ = c->x[0] & 0xFFFFFFFFFFull, lr_ = c->x[30]; u32 before_[9] = {0}; u64 rb_ = log_ ? rng_base() : 0; \
        for (int q = 0; q < 9 && rb_; q++) before_[q] = ld32(rb_ + 20ull * q + 16); \
        f_##addr##_orig(c); \
        if (log_) rand_log(c, name, hash_, lr_, before_); \
    } while (0)
/* signature: d28b5509 f2bf73e9 92409c08 f2c000a9 eb09011f 54.....c d2926d09 f2b84f89 f2c000a9 eb09011f 54.....c d2923ca9 f2a12a29 f2c00049 eb09011f 54.....0 */
HLE_WRAP(7102275320) { RAND_WRAP(7102275320, "rand"); }
/* signature: d28b5509 f2bf73e9 92409c08 f2c000a9 eb09011f 54.....c d2926d09 f2b84f89 f2c000a9 eb09011f 54.....c d2923ca9 f2a12a29 f2c00049 eb09011f 54.....0 */
HLE_WRAP(71022756c0) { RAND_WRAP(71022756c0, "randf"); }

/* M5: the player record builder (13.0.5 0x71017880c0, 13.0.4 0x7101788260: ssbu-recomp PR #116's
 * FN_PLAYER_RECORD; x0 the player's panel state, x1 its index). The match set-up draws from the global
 * generator right after the records are built; setting it only at the menus' safe points left a gap: two
 * PCs agreed on the seed and still started on different sides (twopc2, desync at the first check) - a
 * slower PC's menus run extra logic frames between safe points (dropped refreshes), and those can draw.
 * So in net mode it is set here too, as player 1's record is built, as the PR's handshake does. */
/* signature: d105c3ff a9116ffc a91267fa a9135ff8 a91457f6 a9154ff4 a9167bfd 910583fd .0.....8 f9....1a aa0003f4 f9404698 b940781b aa0103f7 f9400756 910582c0 */
/* The stage choice (13.0.4 0x71053097c0: three entries of 0x20 bytes, the current one's index at
 * 0x7105309968; the hazards byte 0x7105309985: the PR's debug_mode.c), one page on in 13.0.5. */
#define SEL_BASE 0x710530a7c0ull
#define SEL_CUR  0x710530a968ull
#define SEL_HAZ  0x710530a985ull
HLE_WRAP(71017880c0) {
    int idx = (int)c->x[1];
    u64 st = c->x[0];
    if (net_mode == 2 && idx == 0) {
        fprintf(stderr, "[rb] net mode: player 1's record is built (a match set-up): the %d players hold nothing until the fight\n", net_n);
        net_hello_wait();
        net_seed_global("match set-up");
        net_rules_setup(st);   /* (streamlining 2: the host's rules on every game) */
        net_countdown = 1;
        net_pre_reset();       /* (the GO! freeze: a new set-up's loading watched again) */
        if (net_results) { InterlockedExchange(&net_results, 0); fprintf(stderr, "[rb] results: a new match set up: A and B no longer passed\n"); }
    }
    if (net_mode == 2 && idx == 0) net_panel0 = st;   /* (the results' research: character select's panel, watched after the fight) */
    if (net_mode == 2 && idx >= 0 && idx < net_n && readable(st, 0x460)) {
        /* (the panel state, read off the PR's runs: + 0x78 type (0 human), + 0x88 ui_chara, + 0x90 colour,
         * + 0x210 the controller slot, + 0x450 "human with a name tag") */
        u32 cur = ld32(SEL_CUR);
        u64 sel = SEL_BASE + 0x20ull * (cur < 3 ? cur : 0);
        u32 type = ld32(st + 0x78), slot = ld32(st + 0x210);
        fprintf(stderr, "[rb] handshake: panel %d as the select screen left it: type %u (0 human, 1 CPU, 3 none), controller slot %d, ui_chara %016llx, colour %d\n",
                idx + 1, type, (int)slot, (unsigned long long)ld64(st + 0x88), ld8(st + 0x90));
        if (idx == 0) ch_have_mine = 0;   /* (a new set-up: this game's own choice read again) */
        if (net_local != 0 && rules_have) { st8(st + 5, rules_blob[RULES_TOTAL - 2]); st8(st + 0xa, rules_blob[RULES_TOTAL - 1]); }   /* (the host's rule kind and stocks) */
        /* This game's own player is the human panel its own controller joined (controller slot = this game's
         * player, as controllers_rollback_p<n>.ini puts it), at whichever index: the other slot may be a CPU,
         * added first or after (no keyboard stand-in needed). Its choice goes to the peer. */
        if (!ch_have_mine && type == 0 && slot == (u32)net_local) {
            memset(&ch_mine, 0, sizeof ch_mine);
            ch_mine.player = net_local; ch_mine.chara = ld64(st + 0x88); ch_mine.colour = ld8(st + 0x90);
            if (net_local == 0) {
                ch_mine.has_stage = 1; ch_mine.hazards = ld8(SEL_HAZ);
                for (int k = 0; k < 0x20; k++) ch_mine.stage[k] = ld8(sel + (u64)k);
            }
            ch_have_mine = 1;
            fprintf(stderr, "[rb] handshake: this game's player %d (panel %d): ui_chara %016llx, colour %d%s (stage entry %u: %016llx, hazards %d)\n", net_local + 1, idx + 1,
                    (unsigned long long)ch_mine.chara, ch_mine.colour, net_local == 0 ? ", sent with the stage" : "", cur, (unsigned long long)ld64(sel), ld8(SEL_HAZ));
            for (int k = 0; k < 3; k++) { net_choice_send(0); Sleep(5); }
        }
        if (idx == net_local) {   /* this game's own player's slot: its own choice */
            if (ch_have_mine) {
                st64(st + 0x88, ch_mine.chara); st8(st + 0x90, (u8)ch_mine.colour);
            } else fprintf(stderr, "[rb] handshake: player %d's own controller hasn't joined before this panel: join with your own controller FIRST (this match will not be the agreed one)\n", idx + 1);
        } else {   /* the other player: its game's choice, waited for */
            if (!ch_have_peer[idx]) {
                fprintf(stderr, "[rb] handshake: waiting for player %d's choice from its game\n", idx + 1);
                double t0 = now_ms();
                while (!ch_have_peer[idx] && now_ms() - t0 < 600000) { net_hello(1); net_choice_send(1); net_wait_say("Waiting for the others to pick their fighters..."); Sleep(16); }   /* (ours asks for theirs) */
                net_wait_say(NULL);
            }
            if (ch_have_peer[idx]) {
                const RbnChoice *pc = &ch_peer[idx];
                u64 was = ld64(st + 0x88);
                st64(st + 0x88, pc->chara); st8(st + 0x90, (u8)pc->colour);
                if (pc->has_stage) {
                    for (int k = 0; k < 0x20; k++) st8(sel + (u64)k, pc->stage[k]);
                    st8(SEL_HAZ, (u8)pc->hazards);
                }
                fprintf(stderr, "[rb] handshake: player %d set from its game's choice (ui_chara %016llx, was %016llx; colour %d)%s\n", idx + 1,
                        (unsigned long long)pc->chara, (unsigned long long)was, pc->colour, pc->has_stage ? "; the stage too" : "");
            } else fprintf(stderr, "[rb] handshake: no choice from the other game after 10 min: player %d as this game has it\n", idx + 1);
        }
        /* both games build the same two players: human, each on its own controller slot (whatever joined the
         * panel first), no name tag (a tag brings its own controls) */
        st32(st + 0x78, 0); st32(st + 0x210, (u32)idx); st8(st + 0x450, 0);
        st8(st + 0x454, 0); st8(st + 0x455, 0); st32(st + 0x214, 0xffffffffu);   /* (no name-tag profile: the game's default controls, as the PR's handshake) */
    }
    f_71017880c0_orig(c);
}

/* FUN 0x7103549720: kicks the task workers (see the top of this file). */
/* signature: a9bb67fa a9015ff8 a90257f6 a9034ff4 a9047bfd 910103fd b9401008 34.....8 a9447bfd a9434ff4 a94257f6 a9415ff8 a8c567fa d65f03c0 2a0303f4 aa0003f3 */
HLE_WRAP(7103549720) {
    u64 lr = c->x[30];
    if (!kick_skipped(lr)) f_7103549720_orig(c);
    if (rb_on && lr == SAFE_POINT) {
        uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
        rb_main_kthread = c->thread;
        {   /* the frame now ending: safe point -> poll -> frame end -> frame end's return -> here */
            double now = now_ms();
            if (rb_fast > 0 && fp_out && seg_poll > fp_out && seg_fe_in > seg_poll && seg_fe_out >= seg_fe_in) {
                seg_sum[0] += seg_poll - fp_out; seg_sum[1] += seg_fe_in - seg_poll;
                seg_sum[2] += seg_fe_out - seg_fe_in; seg_sum[3] += now - seg_fe_out; seg_n++;
            }
            seg_poll = 0;
        }
        fp_in = now_ms(); fp_fast = rb_fast > 0 ? (rb_fast > 1 ? 1 : 2) : 0; fp_loaded = 0;
        double game = fp_out ? fp_in - fp_out : 0;
        safe_point(c, frame);
        double census = fp_census_last;
        /* (net mode: from a match set-up, the entrance and countdown too: on real time there, the two PCs
         * began the fight with the fighters at different points of their entrances (twopc2-5)) */
        if (clock_on) rb_clock_frame(S.in_fight || (net_countdown && net_early_on()));
        savestates(c, frame);
        /* (after a load this runs in the save's frame: the statics, not the locals of then, are now's) */
        double out = now_ms();
        if (fp_fast && !fp_loaded) {
            int k = fp_fast - 1;
            fp_n[k]++;
            fp_sum[k][0] += game; fp_sum[k][1] += census;
            fp_sum[k][2] += rb_save_ms[0]; fp_sum[k][3] += rb_save_ms[1]; fp_sum[k][4] += rb_save_ms[2] - rb_save_ms[0] - rb_save_ms[1];
            fp_sum[k][5] += out - fp_in - census - rb_save_ms[2] - fp_hash_last;
            fp_sum[k][6] += fp_hash_last;
        }
        fp_out = out;
    }
}
