/* nn::os fibers, natively.
 *
 * On the console a fiber switch is a hand-written routine, DispatchFiberContext(save, load):
 * it pushes x0, x19-x30 and q8-q15 as a 0xF0-byte frame, stores the stack pointer in *save,
 * loads the stack pointer from *load, pops that frame and returns into it. Recompiled code
 * cannot do that - a guest call is a host call, so the host stack holds the real frames -
 * so each guest fiber runs on a Windows fiber with its own host stack and its own guest
 * register context, and this routine is the only part of the SDK's fiber code replaced.
 *
 * *save receives a token naming the suspended context. A load of anything else is a frame
 * prepared by nn::os::InitializeFiber on the new fiber's stack: a host fiber is created that
 * loads that frame exactly as the original would and jumps to the saved x30.
 *
 * A fiber nn::os::FinalizeFiber is done with gives its slot to the next new one: the game's
 * script coroutines come and go by the thousand in a match. Its host fiber is deleted only
 * then - it stays where the guest fiber last stopped, and is never switched to again. A fiber
 * finalized while it runs (the game's phx::Fiber can be destroyed from inside itself) gives its
 * slot up once it has been switched away from, never before: another thread could otherwise
 * delete its host fiber under it.
 *
 * A fiber the game drops without finalizing it goes the same way once nothing can reach it any
 * more (reclaim): a suspended fiber is reached only through the token in its own FiberType's
 * context, so one that is not running and whose context no longer holds its token - its
 * FiberType freed and the memory used again, or initialized again - is gone for good. Slots
 * that piled up stopped long sessions with "too many fibers".
 *
 * Fibers may resume on a different thread from the one they were suspended on, so the
 * thread's identity (kernel thread, thread-local region, TPIDR_EL0) moves with every switch. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"

#define TOKEN_TAG 0xF1BE000000000000ull
#define TOKEN_USE 0x0000FFFF00000000ull   /* how often the slot was used: an old fiber's token is not the new one's */
#define FRAME 0xF0
#define CONTEXT 0x50                      /* (nn::os::FiberType: where the context is kept) */
#define MAX_FIBERS 16384

typedef struct Fib {
    LPVOID host; Cpu *cpu; u64 token; u64 guest;
    int finalized;
    int running;        /* threads it runs on, or is being switched to or away from on (lock) */
    int spare_on_leave; /* finalized while running: spare once switched away from */
} Fib;

static Fib *fibs[MAX_FIBERS];
static u32 fib_count;
static Fib *spare[MAX_FIBERS];   /* finalized, not running: their slots go to new fibers */
static u32 nspare;
static u64 started, reclaim_at, reclaimed;
static u32 reclaim_from;   /* slots before reclaim looks: 1024 (SSBU_FIBER_RECLAIM_FROM for tests) */
static SRWLOCK lock = SRWLOCK_INIT;
static __declspec(thread) Fib *left;
extern volatile int rb_keep_fibers;   /* (rollback build, hle_rb_state.c) a savestate exists: host fibers are never deleted */   /* what this thread last switched away from: not running once the switch is done */

static Fib *by_token(u64 token) {
    if ((token & 0xFFFF000000000000ull) != TOKEN_TAG) return NULL;
    u64 i = token & 0xFFFFFFFF;
    Fib *f = i < fib_count ? fibs[i] : NULL;
    return f && f->token == token ? f : NULL;
}

static Fib *new_fib(Cpu *cpu) {
    Fib *f = calloc(1, sizeof *f);
    f->cpu = cpu;
    f->running = 1;   /* (made to run now) */
    AcquireSRWLockExclusive(&lock);
    if (fib_count == MAX_FIBERS) rt_fatal("too many fibers");
    f->token = TOKEN_TAG | fib_count;
    fibs[fib_count++] = f;
    ReleaseSRWLockExclusive(&lock);
    return f;
}

static void make_spare(Fib *f) {   /* (lock held; f not running) */
    f->finalized = 1;
    spare[nspare++] = f;
}

void fiber_counts(u32 *slots, u32 *finalized, u64 *made) {
    AcquireSRWLockShared(&lock);
    *slots = fib_count;
    *finalized = nspare;
    *made = started;
    ReleaseSRWLockShared(&lock);
}

/* (rollback build, M1) Live fibers (not finalized) and how many of them run now. */
void fiber_rb_census(u32 *live, u32 *running) {
    *live = *running = 0;
    AcquireSRWLockShared(&lock);
    for (u32 i = 0; i < fib_count; i++)
        if (fibs[i] && !fibs[i]->finalized) { ++*live; if (fibs[i]->running) ++*running; }
    ReleaseSRWLockShared(&lock);
}

/* (rollback build, M2) The fibers for a savestate. Every slot's record and guest registers; for
 * a fiber that is switched away (not running on a thread), its Windows fiber block (saved
 * registers: CONTEXT at +0x30, its Rsp at +0x30+0x98, StackBase at +0x10 - the layout
 * ssbu-recomp's rollback relies on too) and its host stack from that Rsp to StackBase. A running
 * fiber is a thread's: its stack is saved with that thread (hle_rb_state.c). */
#define RB_FIBER_BLOCK 0x500
typedef struct FSave { Fib f; Cpu cpu; u8 blk[RB_FIBER_BLOCK]; u8 *stack; u64 rsp, n; } FSave;
typedef struct FSnap { u32 fib_count, nspare; Fib **spare; u64 started, reclaim_at, reclaimed; FSave *v; u64 bytes; } FSnap;

/* A fiber block whose host stack (DeallocationStack +0x20 .. StackBase +0x10) holds one of the stack
 * pointers in live[]: some thread runs on that stack now, whatever the fiber's record says (seen:
 * a thread's own fiber, marked switched away, whose stack was the thread's running one). Its
 * state is that thread's, saved with the thread; copying the block's stale view back would write
 * over frames in use. ssbu-recomp's rollback keeps the same rule (threads.c live_on_stack). */
static int stack_in_use(const u8 *blk, const u64 *live, u32 nlive) {
    u64 lo = *(const u64 *)(blk + 0x20), hi = *(const u64 *)(blk + 0x10);
    for (u32 i = 0; i < nlive; i++) if (live[i] >= lo && live[i] < hi) return 1;
    return 0;
}

void *fiber_rb_save(u64 *bytes, u32 *suspended, const u64 *live, u32 nlive, u32 *in_use) {
    FSnap *s = calloc(1, sizeof *s);
    AcquireSRWLockShared(&lock);
    s->fib_count = fib_count; s->nspare = nspare; s->started = started; s->reclaim_at = reclaim_at; s->reclaimed = reclaimed;
    s->spare = malloc(sizeof *s->spare * (nspare + 1));
    memcpy(s->spare, spare, sizeof *spare * nspare);
    s->v = calloc(fib_count + 1, sizeof *s->v);
    *suspended = *in_use = 0;
    for (u32 i = 0; i < fib_count; i++) {
        FSave *v = &s->v[i];
        Fib *f = fibs[i];
        v->f = *f; v->cpu = *f->cpu;
        if (!f->host || f->running) continue;
        memcpy(v->blk, f->host, RB_FIBER_BLOCK);
        if (stack_in_use(v->blk, live, nlive)) {
            ++*in_use;
            fprintf(stderr, "[rb] save: fiber %u is marked switched away, but its host stack %#llx..%#llx is in use: saved with its thread\n",
                    i, *(u64 *)(v->blk + 0x20), *(u64 *)(v->blk + 0x10));
            continue;
        }
        u64 base = *(u64 *)(v->blk + 0x10), rsp = *(u64 *)(v->blk + 0x30 + 0x98);
        if (rsp >= base || base - rsp > (64ull << 20)) { fprintf(stderr, "[rb] fiber %u: odd host stack (rsp %#llx, base %#llx): not saved\n", i, rsp, base); continue; }
        v->rsp = rsp; v->n = base - rsp;
        v->stack = malloc(v->n);
        memcpy(v->stack, (void *)(uintptr_t)rsp, v->n);
        s->bytes += v->n + RB_FIBER_BLOCK;
        ++*suspended;
    }
    ReleaseSRWLockShared(&lock);
    *bytes = s->bytes;
    return s;
}

const char *fiber_rb_check(const void *p) {
    const FSnap *s = p;
    if (fib_count < s->fib_count) return "fewer fiber slots than at the save";
    for (u32 i = 0; i < fib_count; i++) {
        if (i < s->fib_count && fibs[i]->running && !s->v[i].f.running) return "a fiber runs now that was switched away at the save";
        if (i >= s->fib_count && fibs[i]->running) return "a fiber made since the save runs now";
    }
    return NULL;
}

void fiber_rb_load(const void *p, const u64 *live, u32 nlive, u32 *skipped) {
    const FSnap *s = p;
    *skipped = 0;
    AcquireSRWLockExclusive(&lock);
    for (u32 i = 0; i < s->fib_count; i++) {
        const FSave *v = &s->v[i];
        Fib *f = fibs[i];
        *f = v->f;
        *f->cpu = v->cpu;
        if (v->stack && stack_in_use(v->blk, live, nlive)) {
            ++*skipped;
            fprintf(stderr, "[rb] load: fiber %u's saved host stack %#llx..%#llx is in use now: not copied back\n",
                    i, *(u64 *)(v->blk + 0x20), *(u64 *)(v->blk + 0x10));
        } else if (v->stack) {
            memcpy(f->host, v->blk, RB_FIBER_BLOCK);
            memcpy((void *)(uintptr_t)v->rsp, v->stack, v->n);
        }
    }
    fib_count = s->fib_count; nspare = s->nspare; started = s->started; reclaim_at = s->reclaim_at; reclaimed = s->reclaimed;
    memcpy(spare, s->spare, sizeof *spare * nspare);
    ReleaseSRWLockExclusive(&lock);
}

void fiber_rb_free(void *p) {
    FSnap *s = p;
    if (!s) return;
    for (u32 i = 0; i < s->fib_count; i++) free(s->v[i].stack);
    free(s->v); free(s->spare); free(s);
}

/* Fibers the game dropped without FinalizeFiber, made spare. Called with the lock held, when no
 * spare is left: only once there are over a thousand slots (a few dozen in normal play), and at
 * most once per 256 fibers started. A FiberType whose memory is gone counts as dropped too - the
 * console would fault on it. */
static void reclaim(void) {
    if (!reclaim_from) reclaim_from = getenv("SSBU_FIBER_RECLAIM_FROM") ? (u32)atoi(getenv("SSBU_FIBER_RECLAIM_FROM")) : 1024;
    if (fib_count < reclaim_from || started < reclaim_at) return;
    reclaim_at = started + (reclaim_from < 256 ? reclaim_from : 256);
    u32 n = 0;
    for (u32 i = 0; i < fib_count; i++) {
        Fib *f = fibs[i];
        u64 context;
        if (!f->guest || f->finalized || f->running || f->spare_on_leave) continue;   /* (guest 0: a thread made a fiber of) */
        if (kernel_read64(f->guest + CONTEXT, &context) && context == f->token) continue;
        make_spare(f);
        n++;
    }
    u64 before = reclaimed;
    reclaimed += n;
    if (n && (before == 0 || (before ^ reclaimed) > before))   /* (the first time, then at each power of two) */
        fprintf(stderr, "[fiber] %llu fibers the game left unfinished taken back so far (%u slots)\n", (unsigned long long)reclaimed, fib_count);
}

/* On the fiber just switched to: the one switched away from has stopped now. (Not inlined: the
 * thread-local must be found again after a switch, which may have moved the fiber to another
 * thread.) */
static __declspec(noinline) void arrived(void) {
    Fib *l = left;
    left = NULL;
    if (!l) return;
    AcquireSRWLockExclusive(&lock);
    if (--l->running == 0 && l->spare_on_leave) {
        l->spare_on_leave = 0;
        make_spare(l);
    }
    ReleaseSRWLockExclusive(&lock);
}

static void WINAPI fiber_main(void *param) {
    Fib *f = param;
    arrived();
    rt_lookup(f->cpu->x[30])(f->cpu);
    rt_fatal("a fiber's entry function returned");
}

/* nn::os::FinalizeFiber(fiber). */
/* signature: f81e0ff3 a9017bfd 910043fd 39408408 aa0003f3 34.....8 a9438261 f9402662 9....... f9401e68 3900867f f9002268 3900827f a9417bfd f84207f3 d65f03c0 */
HLE_WRAP(710932f130) {
    u64 guest = c->x[0];
    f_710932f130_orig(c);
    Fib *f = by_token(ld64(guest + CONTEXT));   /* (none if it never stopped) */
    if (!f || f->guest != guest) {   /* finalized from inside itself before it ever stopped: its context still holds InitializeFiber's frame */
        Fib *cur = IsThreadAFiber() ? GetFiberData() : NULL;
        if (!cur || cur->guest != guest) return;   /* (never ran) */
        f = cur;
    }
    AcquireSRWLockExclusive(&lock);
    if (!f->finalized && !f->spare_on_leave) {
        if (f->running) f->spare_on_leave = 1;
        else make_spare(f);
    }
    ReleaseSRWLockExclusive(&lock);
}

HLE(_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_) {
    u64 save = c->x[0], load = c->x[1], target = ld64(load);

    /* The context being left: this host fiber (converting the thread on first use). */
    Fib *me;
    if (IsThreadAFiber() && GetFiberData()) me = GetFiberData();
    else {
        me = new_fib(c);
        me->host = ConvertThreadToFiberEx(me, FIBER_FLAG_FLOAT_SWITCH);
        if (!me->host) rt_fatal("cannot make a fiber of a guest thread (error %lu)", (unsigned long)GetLastError());
    }
    me->cpu = c;
    st64(save, me->token);

    AcquireSRWLockExclusive(&lock);
    Fib *next = by_token(target);
    int fresh = !next;
    if (fresh) {   /* a fresh fiber: the frame InitializeFiber prepared at `target` - in a spare slot if there is one */
        if (!nspare) reclaim();
        next = nspare ? spare[--nspare] : NULL;   /* (never `me`: spares are not running) */
        if (next) {
            next->token = TOKEN_TAG | ((next->token + (1ull << 32)) & TOKEN_USE) | (next->token & 0xFFFFFFFF);
            next->finalized = 0;
        }
        started++;
    }
    if (next) next->running++;
    ReleaseSRWLockExclusive(&lock);
    if (fresh) {
        Cpu *n;
        if (next) {
            if (!rb_keep_fibers) DeleteFiber(next->host);   /* (rollback build: a savestate may need it again; kept, leaked) */
            n = next->cpu;
        } else {
            n = _aligned_malloc(sizeof *n, 64);
            next = new_fib(n);
        }
        memset(n, 0, sizeof *n);
        n->x[0] = ld64(target);
        for (int r = 19; r <= 30; r++) n->x[r] = ld64(target + 8ull * (r - 18));
        for (int q = 8; q <= 15; q++) memcpy(&n->v[q], P(void, target + 0x70 + 16ull * (q - 8)), 16);
        n->sp = target + FRAME;
        next->guest = load - CONTEXT;
        next->host = CreateFiberEx(64 << 10, 64 << 20, FIBER_FLAG_FLOAT_SWITCH, fiber_main, next);
        if (!next->host) rt_fatal("cannot create a host fiber (error %lu)", (unsigned long)GetLastError());
    } else {
        next->cpu->x[0] = 0;   /* what the frame of a suspended context holds in x0 */
    }
    Cpu *n = next->cpu;
    n->thread = c->thread; n->tpidrro = c->tpidrro; n->tpidr = c->tpidr; n->fpcr = c->fpcr; n->fpsr = c->fpsr;
    left = me;
    SwitchToFiber(next->host);
    /* Resumed - possibly on another thread, whose identity the switcher handed us. */
    arrived();
}
