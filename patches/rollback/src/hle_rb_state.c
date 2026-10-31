/* Rollback research build, M2: a whole savestate, saved and loaded at the frame safe point
 * (hle_rb_core.c). See patches\rollback\README.md.
 *
 * The game's state is its guest memory plus host state that holds guest values:
 *  - every guest thread's host stack and registers. The recompiled code keeps guest registers in
 *    C locals, so a host stack is game state;
 *  - every switched-away fiber's host stack and Windows fiber block (hle_fiber.c);
 *  - the kernel's tables and every thread's guest registers (kernel.c);
 *  - the setjmp records (hle_setjmp.c).
 *
 * THE HOLD. rb_hold is set and every guest thread is woken from kwait; each one, at its next wait,
 * parks in rb_park_thread. There it records its registers (RtlCaptureContext) and its stack base,
 * then waits. Its stack above the recorded Rsp stays as it is while it waits, so a save copies it.
 *
 * A LOAD puts each parked thread back where it was at the save, even if it parked from another
 * wait now. Its stack may be in use (its waiting frames lie where the saved bytes go), so each
 * thread first moves onto a scratch stack of its own (a context restored with Rsp there). Then the
 * main thread restores memory, the kernel, the fibers and the setjmp records, and lets the threads
 * copy their saved stacks and restore their saved registers. They resume inside rb_park_thread,
 * as at the save, and wait again. The main thread does the same with its own stack and lands in
 * rb_save after its RtlCaptureContext; rb_resumed tells it a load brought it there.
 *
 * Refused (nothing changed) when: the guest threads differ from the save, a thread is on another
 * fiber than at the save, the safe point's stack pointer differs, or not every thread parks
 * within SSBU_RB_PARK_MS (default 500).
 *
 * Host state left out (M2 watches what that costs): GPU objects (caches follow the guest memory
 * through write-watch), audio queues, open files, nvmap handles, IPC sessions, the clock. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "host.h"
#include "gpu.h"

extern int rb_log;   /* (hle_rb_core.c) */

void kernel_rb_census(const void *self, u32 *alive, u32 *waiting, u32 *sleeping, u64 *busy, u32 max, u32 *nbusy);
void kernel_rb_wake_all(void);
void *kernel_rb_save(void);
const char *kernel_rb_check(const void *p);
void kernel_rb_load(const void *p);
u32 kernel_rb_regions(u64 (*out)[2], u32 max);
void *fiber_rb_save(u64 *bytes, u32 *suspended, const u64 *live, u32 nlive, u32 *in_use);
const char *fiber_rb_check(const void *p);
void fiber_rb_load(const void *p, const u64 *live, u32 nlive, u32 *skipped);
void fiber_rb_free(void *p);
void *setjmp_rb_save(u32 *count);
void setjmp_rb_load(const void *p);
void setjmp_rb_free(void *p);
void *vi_rb_save(void);
void vi_rb_load(const void *p);
void *audio_rb_save(void);
void audio_rb_load(const void *p);

volatile int rb_hold, rb_keep_fibers;

#define SCRATCH (256u << 10)
/* a thread's record in a saved state */
typedef struct ThrSave { CONTEXT saved; u64 saved_base; void *saved_fiber; u8 *stack; u64 n; int in_save; } ThrSave;
typedef struct RbThread {
    void *kthread;
    CONTEXT now;                 /* recorded at each park */
    u64 base;                    /* its stack's base (TEB) at that park */
    void *fiber;                 /* the fiber it is on then (NULL: not a fiber) */
    void *home;                  /* (M7) the fiber it was on at its first park: its own, outside any job */
    u8 *scratch;
    volatile int in_park, abort;  /* in_park: waiting in rb_park_thread now (it counts for a hold) */
    struct ThrSave *ld;          /* a load: what this thread goes back to (the state's record of it) */
} RbThread;

static RbThread *recs[1024];
static volatile LONG nrecs, on_scratch, copied;
static SRWLOCK recs_lock = SRWLOCK_INIT;
static HANDLE release_ev, load_ev, copy_ev;
static __declspec(thread) RbThread *mine;
static double park_limit_ms = 500;
double rb_park_quick_ms;   /* (the GO! freeze: > 0, a hold that waits only this long and says nothing when it gives up) */

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void *current_fiber(void) { return IsThreadAFiber() ? GetCurrentFiber() : NULL; }
static u64 stack_base(void) { return (u64)((NT_TIB *)NtCurrentTeb())->StackBase; }

/* A context that runs fn(arg) on the top of a scratch stack (as if just called: Rsp 8 mod 16, with
 * its home space). fn never returns. */
static void run_on_scratch(const CONTEXT *like, u8 *scratch, void (*fn)(void *), void *arg) {
    static __declspec(thread) CONTEXT c;   /* (not on the stack being left) */
    c = *like;
    u64 top = ((u64)(uintptr_t)(scratch + SCRATCH)) & ~15ull;
    c.Rsp = top - 0x38;
    c.Rip = (u64)(uintptr_t)fn;
    c.Rcx = (u64)(uintptr_t)arg;
    c.ContextFlags = CONTEXT_FULL;
    RtlRestoreContext(&c, NULL);
}

static void thread_on_scratch(void *p) {
    RbThread *t = p;
    InterlockedIncrement(&on_scratch);
    WaitForSingleObject(copy_ev, INFINITE);
    if (t->abort) { InterlockedIncrement(&copied); RtlRestoreContext(&t->now, NULL); }
    memcpy((void *)(uintptr_t)t->ld->saved.Rsp, t->ld->stack, t->ld->n);
    InterlockedIncrement(&copied);   /* (after this, copy_ev may be reset: this thread no longer waits on it) */
    RtlRestoreContext(&t->ld->saved, NULL);
}

/* kernel.c kwait, during a hold (the kernel's lock not held). */
void rb_park_thread(void *kthread) {
    RbThread *t = mine;
    if (!t) {
        t = calloc(1, sizeof *t);
        t->kthread = kthread;
        t->scratch = VirtualAlloc(NULL, SCRATCH, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        AcquireSRWLockExclusive(&recs_lock);
        recs[nrecs] = t;
        InterlockedIncrement(&nrecs);
        ReleaseSRWLockExclusive(&recs_lock);
        mine = t;
    }
    t->base = stack_base();
    t->fiber = current_fiber();
    if (!t->home) t->home = t->fiber;
    RtlCaptureContext(&t->now);
    /* (a load resumes here, with this frame as at the save) */
    t = mine;
    t->in_park = 1;
    for (;;) {
        HANDLE h[2] = {release_ev, load_ev};
        DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        run_on_scratch(&t->now, t->scratch, thread_on_scratch, t);
    }
    t->in_park = 0;
}

/* ---------------------------------------------------------------- the hold */
static u32 others(void *self) {
    u32 alive, waiting, sleeping, nbusy; u64 busy[1];
    kernel_rb_census(self, &alive, &waiting, &sleeping, busy, 0, &nbusy);
    return alive;
}

/* Records of threads parked now, other than self's. */
static u32 parked_now(void *self) {
    u32 n = 0;
    for (LONG i = 0; i < nrecs; i++) if (recs[i]->kthread != self && recs[i]->in_park) n++;
    return n;
}

static void release(void) {
    rb_hold = 0;
    SetEvent(release_ev);
}

/* Every other guest thread parked, or 0 (released again) when not within the limit. */
static int hold(void *self, u32 *n) {
    if (!release_ev) {
        release_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        load_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        copy_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        const char *e = getenv("SSBU_RB_PARK_MS");
        if (e && *e) park_limit_ms = atof(e);
    }
    ResetEvent(release_ev);
    rb_hold = 1;
    kernel_rb_wake_all();
    *n = others(self);
    double t0 = now_ms();
    double limit = rb_park_quick_ms > 0 ? rb_park_quick_ms : park_limit_ms;
    while (parked_now(self) < *n) {
        if (now_ms() - t0 > limit) {
            if (rb_park_quick_ms <= 0) fprintf(stderr, "[rb] hold: %u of %u threads parked within %.0f ms: not now\n", parked_now(self), *n, limit);
            release();
            return 0;
        }
        SwitchToThread();
    }
    return 1;
}

/* ---------------------------------------------------------------- guest memory (M3: by chunks)
 * The savestate's copy of guest memory (the shadow) is kept from one save to the next, in 2 MB
 * chunks of the guest's address space, each with a bit per page: present, that is committed and
 * read-write in one of the guest's own regions (kernel.c's memory map) when memory last matched
 * the shadow. A save copies into the shadow, and a load copies back, only:
 *  - the present pages written since: the alias area (game heap, GPU memory) through the
 *    renderer's write-watch stamps (gpu_d3d11.c watch_rb_take / watch_rb_next), so its own news
 *    about textures stays its own; the heap, stack, thread-local and module areas through their own
 *    write watch (kernel.c and host.c reserve them with MEM_WRITE_WATCH in this build); anything
 *    else compared with the shadow;
 *  - the pages committed or decommitted since (kernel.c rb_mem_changed, which host.c's module
 *    loads note too: a page decommitted and committed again is zeros, with no write seen). A save
 *    takes their content and presence; a load puts both back: a page present in the shadow is
 *    committed again, one not present is decommitted, so a load gives memory its layout of then.
 * Windows is asked about the whole layout (some 16 ms) only at the first save and when the kernel's
 * map changed with nothing in the list to say where. SSBU_RB_CHECKLOAD=1: after each save and load,
 * every present page is compared with the shadow, and presence with the layout (2: loads only). */
#define CH_SHIFT 21
#define CH_PAGES 512
#define CH_COUNT (0x8000000000ull >> CH_SHIFT)
enum { K_ALIAS, K_WATCHED, K_COMPARE };
typedef struct Chunk { u8 *base, *copy; u64 present[CH_PAGES / 64], pushed[CH_PAGES / 64]; int kind; } Chunk;   /* pushed: an undo entry this save */
static Chunk **chunk_map;            /* by guest address >> 21 */
static u32 *chunk_ids, nchunks, chunks_cap;   /* the chunks made, in address order */
static int shadow_ok, check_after = -1;
static u64 shadow_seen;              /* the renderer's stamp clock when memory last matched the shadow */
static u64 kmap_sig;                 /* the kernel's memory map then */
static struct { u64 pages, whole_pages, undo_pages; u32 whole; int age; double ms, undo_ms, t[6]; } mstat;
/* t: layout, alias stamps, alias pages, watched, committed/decommitted, compared (ms) */
extern u64 rb_mem_changed[][2];
extern volatile LONG rb_mem_nchanged;
#define RB_CHANGES 256
u64 watch_rb_take(u64 lo, u64 hi);
u64 watch_rb_next(u64 p, u64 hi, u64 seen);
u64 watch_rb_begin(void);
void watch_rb_end(void);

/* (for the replay check and the logs: the present pages as runs) */
typedef struct Region { u8 *at; u64 size; } Region;
static Region *regions;
static u32 nregions, regions_cap;
static u64 mem_bytes;

static int kind_of(u64 g) {
    if (g >= ALIAS_BASE && g < ALIAS_BASE + ALIAS_SIZE) return K_ALIAS;
    if ((g >= HEAP_BASE && g < HEAP_BASE + HEAP_LIMIT) || (g >= STACK_BASE && g < STACK_BASE + STACK_SIZE) ||
        (g >= KERNEL_BASE && g < KERNEL_BASE + 0x1000000) || (g >= 0x7100000000ull && g < NATIVE_BASE)) return K_WATCHED;
    return K_COMPARE;
}

static int id_cmp(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return (x > y) - (x < y); }

static Chunk *chunk_of(const u8 *p, int make) {
    u64 id = (u64)(uintptr_t)p >> CH_SHIFT;
    if (id >= CH_COUNT) return NULL;
    if (!chunk_map) chunk_map = calloc(CH_COUNT, sizeof *chunk_map);
    Chunk *c = chunk_map[id];
    if (c || !make) return c;
    c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->base = (u8 *)(uintptr_t)(id << CH_SHIFT);
    c->kind = kind_of(id << CH_SHIFT);
    c->copy = VirtualAlloc(NULL, 1ull << CH_SHIFT, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!c->copy) { free(c); return NULL; }
    chunk_map[id] = c;
    if (nchunks == chunks_cap) chunk_ids = realloc(chunk_ids, sizeof *chunk_ids * (chunks_cap = chunks_cap ? chunks_cap * 2 : 1024));
    chunk_ids[nchunks++] = (u32)id;
    qsort(chunk_ids, nchunks, sizeof *chunk_ids, id_cmp);
    return c;
}
static int is_present(const Chunk *c, u32 i) { return c && (c->present[i >> 6] >> (i & 63) & 1); }
static void set_present(Chunk *c, u32 i, int on) { if (on) c->present[i >> 6] |= 1ull << (i & 63); else c->present[i >> 6] &= ~(1ull << (i & 63)); }
static u8 *shadow_page(Chunk *c, const u8 *p) { return c->copy + (p - c->base); }

/* An undo record (the ring, below): pages with what they were in an older state: their content,
 * committed or not, and the shadow page. A save fills undo_to with what it overwrites in the shadow. */
enum { U_CONTENT, U_COMMIT, U_ABSENT };   /* U_COMMIT: content, and the page was decommitted since */
typedef struct Undo { u8 **cur; u8 *how; u8 *data; u32 n, cap; } Undo;
static Undo *undo_to;
static int undo_lost;   /* (memory ran out for the record: the older states are dropped) */
#define NO_SLOT 0xFFFFFFFFu
/* An entry for the page in undo_to, its content not copied yet: its slot (NO_SLOT: none, as when the
 * page has one this save already). */
static u32 undo_reserve(Chunk *ch, u32 k, u8 *cur, int how) {
    Undo *u = undo_to;
    if (!u || (ch->pushed[k >> 6] >> (k & 63) & 1)) return NO_SLOT;   /* (one entry a page a save: the first, the content of then) */
    ch->pushed[k >> 6] |= 1ull << (k & 63);
    if (u->n == u->cap) {
        u32 cap = u->cap ? u->cap * 2 : 4096;
        u8 **c = realloc(u->cur, sizeof *c * cap), *h = c ? realloc(u->how, cap) : NULL;
        u8 *d = h ? realloc(u->data, (size_t)cap << 12) : NULL;
        if (c) u->cur = c;
        if (h) u->how = h;
        if (!d) { undo_lost = 1; return NO_SLOT; }
        u->data = d; u->cap = cap;
    }
    u->cur[u->n] = cur; u->how[u->n] = (u8)how;
    return u->n++;
}
static void undo_push(Chunk *ch, u32 k, u8 *cur, int how) {
    u32 slot = undo_reserve(ch, k, cur, how);
    if (slot != NO_SLOT && how != U_ABSENT) memcpy(undo_to->data + ((size_t)slot << 12), shadow_page(ch, cur), 0x1000);
}

/* ---- page copies on several threads (M4) ----
 * A save, a load and the undo records each copy some thousands of 4 KB pages: listed first (copy
 * jobs: s1 -> d1, then s2 -> d2; slot: d1 is that slot of undo_to, resolved when they run, as the
 * record may grow meanwhile), then copied by SSBU_RB_COPY_THREADS host threads (default 4) and this
 * one. Under 256 pages: here alone. A page is in one job at most within a batch. */
typedef struct CopyJob { u8 *d1; const u8 *s1; u8 *d2; const u8 *s2; u32 slot; } CopyJob;
static CopyJob *jobs;
static u32 njobs, jobs_cap;
static volatile LONG jobs_next, jobs_done;
static Undo *jobs_undo;
static HANDLE jobs_go[16];
static int nworkers = -1;
static double jobs_ms;
static void job_add(u8 *d1, const u8 *s1, u8 *d2, const u8 *s2, u32 slot) {
    if (njobs == jobs_cap) jobs = realloc(jobs, sizeof *jobs * (jobs_cap = jobs_cap ? jobs_cap * 2 : 8192));
    jobs[njobs++] = (CopyJob){d1, s1, d2, s2, slot};
}
/* (SSBU_RB_STAMPS=3: the alias stamps' ranges, asked on the copy threads too; see sync) */
void watch_rb_scan(u64 lo, u64 hi, u64 stamp, void **pages);
static u64 (*scan_ranges)[2];
static u32 nscan, scan_cap;
static u64 scan_stamp;
static void jobs_work(void) {
    if (nscan) {
        static __declspec(thread) void **buf;
        if (!buf) buf = malloc(sizeof *buf * 65536);
        for (LONG j; buf && (j = InterlockedIncrement(&jobs_next) - 1) < (LONG)nscan;)
            watch_rb_scan(scan_ranges[j][0], scan_ranges[j][1], scan_stamp, buf);
        return;
    }
    for (;;) {
        LONG j = InterlockedAdd(&jobs_next, 32) - 32;
        if (j >= (LONG)njobs) return;
        LONG e = j + 32 < (LONG)njobs ? j + 32 : (LONG)njobs;
        for (; j < e; j++) {
            CopyJob *q = &jobs[j];
            u8 *d1 = q->slot != NO_SLOT ? jobs_undo->data + ((size_t)q->slot << 12) : q->d1;
            if (d1) memcpy(d1, q->s1, 0x1000);
            if (q->d2) memcpy(q->d2, q->s2, 0x1000);
        }
    }
}
static DWORD WINAPI jobs_worker(void *p) {
    HANDLE go = p;
    for (;;) { WaitForSingleObject(go, INFINITE); jobs_work(); InterlockedIncrement(&jobs_done); }
}
static void jobs_run(void) {
    double t = now_ms();
    if (nworkers < 0) {
        const char *e = getenv("SSBU_RB_COPY_THREADS");
        nworkers = e && *e ? atoi(e) : 4;
        if (nworkers > 16) nworkers = 16;
        for (int i = 0; i < nworkers; i++) {
            jobs_go[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
            HANDLE h = CreateThread(NULL, 0, jobs_worker, jobs_go[i], 0, NULL);
            if (h) { SetThreadDescription(h, L"rollback copies"); CloseHandle(h); }
        }
    }
    jobs_undo = undo_to;
    jobs_next = 0; jobs_done = 0;
    int helpers = njobs >= 256 ? nworkers : 0;
    for (int i = 0; i < helpers; i++) SetEvent(jobs_go[i]);
    jobs_work();
    while (jobs_done < helpers) YieldProcessor();
    njobs = 0;
    jobs_ms += now_ms() - t;
}
/* The scan ranges listed (scan_ranges, nscan) on this thread and the copy threads at once. */
static void scan_run(void) {
    if (nworkers < 0) jobs_run();   /* (starts the copy threads: no jobs listed) */
    jobs_next = 0; jobs_done = 0;
    for (int i = 0; i < nworkers; i++) SetEvent(jobs_go[i]);
    jobs_work();
    while (jobs_done < nworkers) YieldProcessor();
    nscan = 0;
}
static void undo_apply(const Undo *u) {
    for (u32 i = 0; i < u->n; i++) {
        u8 *p = u->cur[i];
        Chunk *c = chunk_of(p, 1);
        if (!c) continue;
        u32 k = (u32)((p - c->base) >> 12);
        if (u->how[i] == U_ABSENT) {
            gpu_memory_unmapping((u64)(uintptr_t)p, 0x1000);
            VirtualFree(p, 0x1000, MEM_DECOMMIT);
            set_present(c, k, 0);
            continue;
        }
        if (u->how[i] == U_COMMIT) VirtualAlloc(p, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        job_add(p, u->data + ((size_t)i << 12), shadow_page(c, p), u->data + ((size_t)i << 12), NO_SLOT);
        set_present(c, k, 1);
    }
    jobs_run();
}

/* ---- the layout ---- */
static u64 kmap(u64 (*guest)[2], u32 *n) {
    *n = kernel_rb_regions(guest, 16384);
    if (*n > 16384) *n = 16384;
    u64 sig = 0xcbf29ce484222325ull;
    for (u32 g = 0; g < *n; g++) { sig = (sig ^ guest[g][0]) * 0x100000001b3ull; sig = (sig ^ guest[g][1]) * 0x100000001b3ull; }
    return sig;
}
static int base_cmp(const void *a, const void *b) { u64 x = ((const u64 *)a)[0], y = ((const u64 *)b)[0]; return (x > y) - (x < y); }
/* Is guest address a in one of the regions (sorted by base)? */
static int in_guest(u64 a, u64 (*guest)[2], u32 ng) {
    u32 lo = 0, hi = ng;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (guest[m][0] <= a) lo = m + 1; else hi = m; }
    return lo && a < guest[lo - 1][0] + guest[lo - 1][1];
}
static int rw_committed(const MEMORY_BASIC_INFORMATION *m) {
    return m->State == MEM_COMMIT && (m->Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(m->Protect & PAGE_GUARD);
}

/* A page's presence and content now differ from the shadow's, or may (committed or decommitted
 * since): to_shadow, the shadow takes them (with an undo entry); else memory takes the shadow's. */
static void settle_page(u8 *p, int now, int to_shadow) {
    Chunk *c = chunk_of(p, now && to_shadow);   /* (no chunk: no page of it present in the shadow) */
    u32 k = c ? (u32)((p - c->base) >> 12) : 0;
    int was = is_present(c, k);
    if (!c && (to_shadow || !now)) return;
    if (to_shadow) {
        if (now) {
            undo_push(c, k, p, was ? U_CONTENT : U_ABSENT);
            memcpy(shadow_page(c, p), p, 0x1000);
            set_present(c, k, 1);
        } else if (was) {
            undo_push(c, k, p, U_COMMIT);
            set_present(c, k, 0);
        } else return;
    } else {
        if (was) {
            if (!now) VirtualAlloc(p, 0x1000, MEM_COMMIT, PAGE_READWRITE);
            memcpy(p, shadow_page(c, p), 0x1000);
        } else if (now) {
            gpu_memory_unmapping((u64)(uintptr_t)p, 0x1000);
            VirtualFree(p, 0x1000, MEM_DECOMMIT);
        } else return;
    }
    mstat.whole_pages++;
}

/* Pages in [lo, hi) as they are now, against the shadow (Windows asked about that range only). */
static void settle_range(u8 *lo, u8 *hi, int to_shadow) {
    MEMORY_BASIC_INFORMATION m;
    for (u8 *p = lo; p < hi;) {
        if (!VirtualQuery(p, &m, sizeof m)) break;
        u8 *end = (u8 *)m.BaseAddress + m.RegionSize;
        if (end > hi) end = hi;
        int now = rw_committed(&m);
        if (now) { for (; p < end; p += 0x1000) settle_page(p, 1, to_shadow); continue; }
        /* not there now: only the pages the shadow has matter (a large range the game gave up or
         * never used is mostly chunks the shadow doesn't have: skipped whole) */
        while (p < end) {
            u8 *ce = (u8 *)(uintptr_t)((((u64)(uintptr_t)p >> CH_SHIFT) + 1) << CH_SHIFT);
            if (ce > end) ce = end;
            Chunk *c = chunk_of(p, 0);
            if (c) for (; p < ce; p += 0x1000) if (is_present(c, (u32)((p - c->base) >> 12))) settle_page(p, 0, to_shadow);
            p = ce;
        }
    }
}

/* The whole layout: every page of the guest's regions (some 16 ms), and every page the shadow has. */
static void settle_all(int to_shadow) {
    static u64 guest[16384][2];
    u32 ng;
    kmap_sig = kmap(guest, &ng);
    /* present pages outside the guest's regions now: gone (unmapped) */
    qsort(guest, ng, sizeof *guest, base_cmp);
    for (u32 i = 0; i < nchunks; i++) {
        Chunk *c = chunk_map[chunk_ids[i]];
        for (u32 k = 0; k < CH_PAGES; k++) {
            if (!is_present(c, k)) continue;
            if (!in_guest((u64)(uintptr_t)(c->base + ((u64)k << 12)), guest, ng)) settle_page(c->base + ((u64)k << 12), 0, to_shadow);
        }
    }
    for (u32 g = 0; g < ng; g++) settle_range(G2H(guest[g][0]), G2H(guest[g][0] + guest[g][1]), to_shadow);
}

/* The present pages as runs (regions[]), for the replay check and the logs. */
static void present_runs(void) {
    nregions = 0; mem_bytes = 0;
    for (u32 i = 0; i < nchunks; i++) {
        Chunk *c = chunk_map[chunk_ids[i]];
        for (u32 k = 0; k < CH_PAGES; k++) {
            if (!is_present(c, k)) continue;
            u8 *p = c->base + ((u64)k << 12);
            mem_bytes += 0x1000;
            if (nregions && regions[nregions - 1].at + regions[nregions - 1].size == p) { regions[nregions - 1].size += 0x1000; continue; }
            if (nregions == regions_cap) regions = realloc(regions, sizeof *regions * (regions_cap = regions_cap ? regions_cap * 2 : 1024));
            regions[nregions].at = p; regions[nregions].size = 0x1000; nregions++;
        }
    }
}

/* The write watches start again from now (memory matches the shadow): the renderer's stamps for the
 * alias area, the others reset (by reservation: a call may not span two). */
static void watches_reset(void) {
    for (u32 i = 0; i < nchunks; i++) {
        Chunk *c = chunk_map[chunk_ids[i]];
        if (c->kind == K_ALIAS) { shadow_seen = watch_rb_take((u64)c->base, (u64)c->base + (1ull << CH_SHIFT)); continue; }
        if (c->kind != K_WATCHED) continue;
        MEMORY_BASIC_INFORMATION m;
        for (u8 *p = c->base, *end = c->base + (1ull << CH_SHIFT); p < end && VirtualQuery(p, &m, sizeof m);) {
            u8 *e = (u8 *)m.BaseAddress + m.RegionSize;
            if (e > end) e = end;
            if (rw_committed(&m)) ResetWriteWatch(p, (SIZE_T)(e - p));
            p = e;
        }
    }
}

/* Called after a load put the kernel's tables back: the map signature of that state. */
static void kmap_refresh(void) {
    static u64 guest[16384][2];
    u32 ng;
    kmap_sig = kmap(guest, &ng);
}

/* (the stamp strategies, timed: see sync) */
static double stamp_ms[3];
static long long stamp_n[3], stamp_calls[3];
void rb_stamp_report(void) {
    for (int m = 0; m < 3; m++)
        if (stamp_n[m])
            fprintf(stderr, "[rb] alias stamps, %s: %.3f ms a save over %lld saves, %.1f calls a save\n", m == 2 ? "runs on the copy threads too" : m ? "one call over all of it" : "a call per run of chunks",
                    stamp_ms[m] / stamp_n[m], stamp_n[m], (double)stamp_calls[m] / stamp_n[m]);
}

/* ---- the sync ---- */
/* Brings memory and the shadow together: to_shadow (a save) copies memory into the shadow, else (a
 * load) the shadow into memory. The write watches then start again from here. */
static void sync(int to_shadow) {
    static void *pages[CH_PAGES];
    static u64 guest[16384][2];
    u64 n = 0, clock = 0;
    double t = now_ms();
    mstat.whole = 0; mstat.whole_pages = 0;
    if (to_shadow) for (u32 i = 0; i < nchunks; i++) memset(chunk_map[chunk_ids[i]]->pushed, 0, sizeof chunk_map[chunk_ids[i]]->pushed);
    /* the renderer's stamps first, for the alias area. SSBU_RB_STAMPS (measuring which is cheaper):
     * 1 a call per run of consecutive chunks, 2 one call from the first chunk to the last (the
     * holes too: the alias area is one reservation), 0 the two in turn, each timed. Default 1:
     * the stamps cost goes with the range scanned (stamps run: 1.27 ms a save against 13.6). */
    static int stamp_mode = -1, turn;
    if (stamp_mode < 0) { const char *e = getenv("SSBU_RB_STAMPS"); stamp_mode = e && *e ? atoi(e) : 1; }   /* (measured: 2 costs ten times 1) */
    int mode = stamp_mode ? stamp_mode : (turn++ & 1) ? 3 : 1;   /* (0: 1 and 3 in turn) */
    u32 calls = 0;
    if (mode == 2) {
        u64 lo = 0, hi = 0;
        for (u32 i = 0; i < nchunks; i++)
            if (chunk_map[chunk_ids[i]]->kind == K_ALIAS) { if (!lo) lo = (u64)chunk_ids[i] << CH_SHIFT; hi = ((u64)chunk_ids[i] + 1) << CH_SHIFT; }
        if (lo) { clock = watch_rb_take(lo, hi); calls = 1; }
    } else if (mode == 3) {
        /* the runs of consecutive alias chunks, cut to 16 chunks (32 MB) at most, asked on the copy
         * threads too, under the renderer's lock (its own questions wait: no news half stamped) */
        for (u32 i = 0; i < nchunks; i++) {
            if (chunk_map[chunk_ids[i]]->kind != K_ALIAS) continue;
            u32 j = i;
            while (j + 1 < nchunks && j - i < 15 && chunk_ids[j + 1] == chunk_ids[j] + 1 && chunk_map[chunk_ids[j + 1]]->kind == K_ALIAS) j++;
            if (nscan == scan_cap) scan_ranges = realloc(scan_ranges, sizeof *scan_ranges * (scan_cap = scan_cap ? scan_cap * 2 : 256));
            scan_ranges[nscan][0] = (u64)chunk_ids[i] << CH_SHIFT; scan_ranges[nscan][1] = ((u64)chunk_ids[j] + 1) << CH_SHIFT;
            nscan++;
            i = j;
        }
        calls = nscan;
        if (nscan) { scan_stamp = watch_rb_begin(); scan_run(); clock = scan_stamp; watch_rb_end(); }
    } else
        for (u32 i = 0; i < nchunks; i++) {
            u32 j = i;
            if (chunk_map[chunk_ids[i]]->kind != K_ALIAS) continue;
            while (j + 1 < nchunks && chunk_ids[j + 1] == chunk_ids[j] + 1 && chunk_map[chunk_ids[j + 1]]->kind == K_ALIAS) j++;
            clock = watch_rb_take((u64)chunk_ids[i] << CH_SHIFT, ((u64)chunk_ids[j] + 1) << CH_SHIFT);
            calls++;
            i = j;
        }
    mstat.t[1] = now_ms() - t;
    if (to_shadow) { stamp_ms[mode - 1] += mstat.t[1]; stamp_n[mode - 1]++; stamp_calls[mode - 1] += calls; }
    /* first the pages committed or decommitted since: their presence and content, whole (before the
     * written pages: a page written, then decommitted, is no longer there to copy) */
    t = now_ms();
    LONG nch = rb_mem_nchanged;
    u32 ng;
    u64 sig = kmap(guest, &ng);
    if (nch > RB_CHANGES || (sig != kmap_sig && !nch)) { settle_all(to_shadow); mstat.whole = 1; }
    else {
        for (LONG i = 0; i < nch; i++) {
            u8 *lo = (u8 *)(uintptr_t)(rb_mem_changed[i][0] & ~0xFFFull), *hi = (u8 *)(uintptr_t)((rb_mem_changed[i][0] + rb_mem_changed[i][1] + 0xFFF) & ~0xFFFull);
            settle_range(lo, hi, to_shadow);
        }
        kmap_sig = to_shadow ? sig : kmap_sig;
    }
    mstat.t[4] = now_ms() - t;
    /* the pages written since, where present then and now */
    for (u32 i = 0; i < nchunks; i++) {
        Chunk *c = chunk_map[chunk_ids[i]];
        u8 *end = c->base + (1ull << CH_SHIFT);
#define COPY_PAGE(p) do { u8 *cur_ = (p); u32 k_ = (u32)((cur_ - c->base) >> 12); \
            if (!is_present(c, k_)) break; u8 *sh_ = c->copy + (cur_ - c->base); \
            if (to_shadow) job_add(NULL, sh_, sh_, cur_, undo_reserve(c, k_, cur_, U_CONTENT)); else job_add(NULL, NULL, cur_, sh_, NO_SLOT); n++; } while (0)
        t = now_ms();
        if (c->kind == K_ALIAS) {
            for (u64 p = watch_rb_next((u64)c->base, (u64)end, shadow_seen); p < (u64)end; p = watch_rb_next(p + 0x1000, (u64)end, shadow_seen))
                COPY_PAGE((u8 *)p);
            mstat.t[2] += now_ms() - t;
        } else if (c->kind == K_WATCHED) {
            /* (by the runs of present pages: a call may not span two reservations; split where it fails) */
            for (u32 k = 0; k < CH_PAGES;) {
                if (!is_present(c, k)) { k++; continue; }
                u32 e = k;
                while (e < CH_PAGES && is_present(c, e)) e++;
                u8 *lo = c->base + ((u64)k << 12), *hi = c->base + ((u64)e << 12);
                while (lo < hi) {
                    u8 *part = hi;
                    ULONG_PTR count = CH_PAGES; ULONG gran;
                    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, lo, (SIZE_T)(hi - lo), pages, &count, &gran) != 0) {
                        MEMORY_BASIC_INFORMATION m;
                        if (!VirtualQuery(lo, &m, sizeof m)) break;
                        part = (u8 *)m.BaseAddress + m.RegionSize;
                        if (part > hi) part = hi;
                        count = CH_PAGES;
                        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, lo, (SIZE_T)(part - lo), pages, &count, &gran) != 0) count = 0;
                    }
                    for (ULONG_PTR q = 0; q < count; q++) COPY_PAGE((u8 *)pages[q]);
                    lo = part;
                }
                k = e;
            }
            mstat.t[3] += now_ms() - t;
        } else {
            for (u32 k = 0; k < CH_PAGES; k++)
                if (is_present(c, k) && memcmp(c->base + ((u64)k << 12), c->copy + ((u64)k << 12), 0x1000)) COPY_PAGE(c->base + ((u64)k << 12));
            mstat.t[5] += now_ms() - t;
        }
#undef COPY_PAGE
    }
    t = now_ms();
    jobs_run();   /* (the copies of the pages listed above) */
    mstat.t[2] += now_ms() - t;
    if (clock) shadow_seen = clock;   /* (a page a load writes back is stamped later: copied again next time, the same bytes) */
    rb_mem_nchanged = 0;
    mstat.pages = n + mstat.whole_pages;
}

/* SSBU_RB_CHECKLOAD: present pages that differ from the shadow, and pages whose presence differs
 * from the layout (both 0 right after a save or a load). */
static u64 shadow_mismatches(u64 *first, u64 *layout_bad) {
    static u64 guest[16384][2];
    u32 ng;
    kmap(guest, &ng);
    qsort(guest, ng, sizeof *guest, base_cmp);
    u64 n = 0;
    *first = 0; *layout_bad = 0;
    for (u32 i = 0; i < nchunks; i++) {
        Chunk *c = chunk_map[chunk_ids[i]];
        MEMORY_BASIC_INFORMATION m; u8 *m_end = NULL; int now = 0;
        for (u32 k = 0; k < CH_PAGES; k++) {
            u8 *p = c->base + ((u64)k << 12);
            if (p >= m_end) { if (!VirtualQuery(p, &m, sizeof m)) break; m_end = (u8 *)m.BaseAddress + m.RegionSize; now = rw_committed(&m); }
            int was = is_present(c, k);
            if (was != (now && in_guest((u64)(uintptr_t)p, guest, ng))) { (*layout_bad)++; continue; }
            if (was && memcmp(p, c->copy + ((u64)k << 12), 0x1000)) { if (!n) *first = (u64)(uintptr_t)p; n++; }
        }
    }
    return n;
}

static void flags_once(void) {
    if (check_after < 0) { const char *e = getenv("SSBU_RB_CHECKLOAD"); check_after = e ? atoi(e) : 0;   /* 1: after saves and loads, 2: after loads only */ }
}

/* *lost: the older states can't be rebuilt from this save on (memory ran out for the undo record). */
static int memory_save(int *lost) {
    double t0 = now_ms();
    flags_once();
    memset(mstat.t, 0, sizeof mstat.t);
    undo_lost = 0;
    if (!shadow_ok) {   /* the first save: every page of the guest's regions, then the write watches from here */
        Undo *keep = undo_to;
        undo_to = NULL;
        mstat.whole_pages = 0;
        settle_all(1);
        undo_to = keep;
        watches_reset();
        rb_mem_nchanged = 0;
        mstat.t[0] = now_ms() - t0;
        mstat.whole = 1; mstat.pages = mstat.whole_pages;
        undo_lost = 1;
        shadow_ok = 1;
    } else sync(1);
    mstat.ms = now_ms() - t0;
    *lost = undo_lost;
    if (check_after == 1) {
        u64 first, lay, bad = shadow_mismatches(&first, &lay);
        fprintf(stderr, "[rb] check after the save: %llu pages differ from the shadow (first %#llx), %llu pages' presence differs\n", bad, first, lay);
    }
    return 1;
}

/* (a load straight after a save that kept the threads held: memory is the shadow, nothing ran) */
static int memory_untouched;
static void memory_load(void) {
    if (memory_untouched) {
        memset(mstat.t, 0, sizeof mstat.t);
        mstat.pages = mstat.whole_pages = 0; mstat.whole = 0; mstat.ms = 0;
        memory_untouched = 0;
        if (check_after) {
            u64 first, lay, bad = shadow_mismatches(&first, &lay);
            fprintf(stderr, "[rb] check after the load (straight after its save): %llu pages differ from the shadow (first %#llx), %llu pages' presence differs\n", bad, first, lay);
        }
        return;
    }
    double t0 = now_ms();
    flags_once();
    memset(mstat.t, 0, sizeof mstat.t);
    sync(0);
    mstat.ms = now_ms() - t0;
    if (check_after) {
        u64 first, lay, bad = shadow_mismatches(&first, &lay);
        fprintf(stderr, "[rb] check after the load: %llu pages differ from the shadow (first %#llx), %llu pages' presence differs\n", bad, first, lay);
    }
}

/* ---------------------------------------------------------------- save / load: the ring (M3)
 * The last N states (rb_ring_size, default 1). Guest memory has one copy, the shadow, which is the
 * newest state; each older state keeps, as its undo record, the pages a later save overwrote in
 * the shadow (their content in that state). A load of the state `age` saves back: memory goes back
 * to the newest state (memory_load), then each undo record from the newest older state to that one
 * is put back, in memory and in the shadow; the newer states are dropped. The host state (kernel,
 * fibers, setjmp records, display and audio queues, every thread's stack and registers) is kept
 * whole per state. A save that finds memory runs the shadow did not have (new mappings) cannot
 * record their old content: the older states are dropped there (a barrier). */
typedef struct Slot {
    int have;
    long long seq;                   /* the save's number */
    u32 tag;                         /* the caller's name for it (hle_rb_core.c: the logic frame) */
    void *kernel, *fibers, *setjmps, *vi, *audio;
    u32 threads;                     /* other threads in the save */
    CONTEXT main;                    /* the main thread's registers in rb_save */
    u64 main_base, main_frame;
    void *main_fiber;
    u8 *main_stack; u64 main_n;
    ThrSave *thr; u32 nthr;          /* per thread record (recs[] order) */
    Undo undo;                       /* this state's pages the next save overwrote in the shadow */
} Slot;
#define RING_MAX 64
static Slot ring[RING_MAX];
static int ring_size = 1, ring_count, ring_head;   /* ring_head: the newest */
static Slot *loading;                /* the state a load is putting back */
static u8 *main_scratch;
static long long saves, loads, refused, barriers;
static volatile int rb_resumed;
static double load_t0;
static int quiet;
double rb_save_ms[3];   /* the last save: hold and GPU drain, memory, in all (ms) */
double rb_save_parts[4]; long long rb_save_count;   /* (M6: every save's parking, GPU drain, memory, in all (ms), summed; hle_rb_core.c reads and clears them) */

static Slot *slot_at(int age) { return &ring[(ring_head - age + ring_size * 2) % ring_size]; }

static void slot_free(Slot *s) {
    for (u32 i = 0; i < s->nthr; i++) free(s->thr[i].stack);
    free(s->thr); s->thr = NULL; s->nthr = 0;
    free(s->kernel); fiber_rb_free(s->fibers); setjmp_rb_free(s->setjmps); free(s->vi); free(s->audio); free(s->main_stack);
    s->kernel = s->fibers = s->setjmps = s->vi = s->audio = NULL; s->main_stack = NULL;
    s->undo.n = 0;   /* (its buffers stay, for the next state in this slot) */
    s->have = 0;
}

/* Keeps the newest `keep` states (the rest dropped). */
static void ring_keep(int keep) {
    while (ring_count > keep) { slot_free(slot_at(ring_count - 1)); ring_count--; }
}

/* N states (1..64), before the first save. */
void rb_ring_size(int n) {
    if (n < 1) n = 1;
    if (n > RING_MAX) n = RING_MAX;
    ring_keep(0);
    ring_size = n; ring_head = 0;
}
int rb_ring_count(void) { return ring_count; }
/* (set before rb_save: the tag the state gets) */
u32 rb_save_tag;
/* The newest state tagged at or before `target` (tags rise with age going down): its age, and its
 * tag in *tag; -1 if none. */
int rb_ring_find(u32 target, u32 *tag) {
    for (int a = 0; a < ring_count; a++)
        if (slot_at(a)->tag <= target) { *tag = slot_at(a)->tag; return a; }
    return -1;
}
/* (the GO! freeze) every saved state let go; the copy of memory (the shadow) stays as at the last save, so the next save
 * copies only what changed since. A fight's states are of no use once it is over, and a state kept from before the
 * fight made the first save of the next one copy undo pages for all of memory (twopc14: 546 395 of them, 935 ms). */
void rb_ring_drop(void) { while (ring_count) { slot_free(slot_at(ring_count - 1)); ring_count--; } }
/* (M7) the logic frame of the state `age` saves back; 0: none */
u32 rb_ring_tag(int age) { return age >= 0 && age < ring_count ? slot_at(age)->tag : 0; }
void rb_quiet(int q) { quiet = q; }

static void finish_load(void);

/* M4: a rollback saves the frame it is in and loads an older one at once. rb_keep_hold_next():
 * the next save leaves every guest thread held, and the load that follows takes them as they
 * are: no second hold, and no memory sync (nothing ran between). rb_release_held(): no load after
 * all. */
static int keep_hold, held;
static u32 held_n;
void rb_keep_hold_next(void) { keep_hold = 1; }
void rb_release_held(void) { memory_untouched = 0; if (held) { held = 0; release(); } }   /* (memory_untouched: also after a refused load) */

/* The stack pointers in use now: this thread's and every parked thread's (where it parked). */
static u32 live_stacks(void *self, u64 *live) {
    volatile u64 here = (u64)(uintptr_t)&here;
    u32 n = 0;
    live[n++] = here;
    for (LONG i = 0; i < nrecs; i++)
        if (recs[i]->kthread != self && recs[i]->in_park) live[n++] = recs[i]->now.Rsp;
    return n;
}

static void step(const char *what) {   /* (a load's progress, for a crash in the middle of one) */
    if (quiet) return;
    fprintf(stderr, "[rb] load: %s (%.0f ms)\n", what, now_ms() - load_t0);
    fflush(stderr);
}

/* A save at the safe point: 1 saved, 0 not (no hold), 2 a load came back here (to the state this
 * call saved). (noinline: its frame is the one a load returns into) */
__declspec(noinline) int rb_save(Cpu *c, u64 frame) {
    u32 n;
    double t0 = now_ms();
    if (!hold(c->thread, &n)) { refused++; return 0; }
    double tp = now_ms();
    gpu_queue_drain();
    double t1 = now_ms();
    rb_save_parts[0] += tp - t0; rb_save_parts[1] += t1 - tp;
    if (ring_count == ring_size) { slot_free(slot_at(ring_count - 1)); ring_count--; }   /* (the oldest) */
    Slot *prev = ring_count ? slot_at(0) : NULL;
    Slot *s = &ring[ring_count ? (ring_head + 1) % ring_size : ring_head];
    RtlCaptureContext(&s->main);
    if (rb_resumed) { rb_resumed = 0; finish_load(); return 2; }
    if (prev) prev->undo.n = 0;
    undo_to = prev ? &prev->undo : NULL;
    int barrier = 0;
    if (!memory_save(&barrier)) { undo_to = NULL; release(); return 0; }
    undo_to = NULL;
    if (barrier && ring_count) { ring_keep(0); barriers++; }   /* (s is not among them: it was free) */
    u64 fib_bytes, live[1025]; u32 suspended, jmps, in_use;
    u32 nlive = live_stacks(c->thread, live);
    s->kernel = kernel_rb_save();
    s->fibers = fiber_rb_save(&fib_bytes, &suspended, live, nlive, &in_use);
    s->setjmps = setjmp_rb_save(&jmps);
    s->vi = vi_rb_save();
    s->audio = audio_rb_save();
    u64 thread_bytes = 0;
    s->threads = 0;
    s->nthr = (u32)nrecs;
    s->thr = calloc(s->nthr ? s->nthr : 1, sizeof *s->thr);
    for (u32 i = 0; i < s->nthr; i++) {
        RbThread *t = recs[i];
        if (t->kthread == c->thread || !t->in_park) continue;
        ThrSave *ts = &s->thr[i];
        ts->saved = t->now; ts->saved_base = t->base; ts->saved_fiber = t->fiber;
        ts->n = t->base - t->now.Rsp;
        ts->stack = malloc(ts->n);
        memcpy(ts->stack, (void *)(uintptr_t)t->now.Rsp, ts->n);
        ts->in_save = 1;
        thread_bytes += ts->n;
        s->threads++;
    }
    s->main_base = stack_base(); s->main_frame = frame; s->main_fiber = current_fiber();
    s->main_n = s->main_base - s->main.Rsp;
    s->main_stack = malloc(s->main_n);
    memcpy(s->main_stack, (void *)(uintptr_t)s->main.Rsp, s->main_n);
    if (!main_scratch) main_scratch = VirtualAlloc(NULL, SCRATCH, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s->have = 1; s->seq = ++saves; s->tag = rb_save_tag;
    ring_head = (int)(s - ring); ring_count++;
    rb_keep_fibers = 1;
    if (keep_hold) { keep_hold = 0; held = 1; held_n = n; memory_untouched = 1; }   /* (a load follows at once: rb_load_age) */
    else release();
    double total = now_ms() - t0;
    rb_save_ms[0] = t1 - t0; rb_save_ms[1] = mstat.ms; rb_save_ms[2] = total;   /* (hle_rb_core.c's profile of re-run frames) */
    rb_save_parts[2] += mstat.ms; rb_save_parts[3] += total; rb_save_count++;
    /* (M6: a slow save logged at most once in 60. PC 2's saves were nearly all over 8 ms at delay 1, and each
     * logged one walked every page bit for the line (present_runs, ~500 000 pages) on top: twopc20, 3330 of
     * 5715 saves logged. The net work line has every save's parts, a second at a time.) */
    static long long slow_logged;
    int slow = total > 8 && saves - slow_logged >= 60;
    if (slow) slow_logged = saves;
    if (!quiet || slow || barrier || !(saves % 600)) {
        present_runs();
        fprintf(stderr, "[rb] SAVE %lld: memory: %llu pages copied (%llu MB; layout walk %u, %llu pages committed or decommitted; undo %u pages) in %.1f ms "
                "[layout %.1f, alias stamps %.1f, alias pages %.1f, watched %.1f, (de)commits %.1f, compared %.1f]%s\n", saves,
                mstat.pages, mstat.pages >> 8, mstat.whole, mstat.whole_pages, prev && !barrier ? prev->undo.n : 0, mstat.ms,
                mstat.t[0], mstat.t[1], mstat.t[2], mstat.t[3], mstat.t[4], mstat.t[5], barrier ? " (new memory runs: older states dropped)" : "");
        fprintf(stderr, "[rb] SAVE %lld: %.1f ms (hold %.1f ms): guest memory %llu MB in %u regions; %u other threads (%llu KB of host stack), "
                "main stack %llu KB (%#llx..%#llx, host thread %lu); %u switched-away fibers (%llu KB), %u more on stacks in use; %u setjmp records; "
                "%d of %d states kept\n",
                saves, total, t1 - t0, mem_bytes >> 20, nregions, s->threads, thread_bytes >> 10, s->main_n >> 10,
                s->main.Rsp, s->main_base, GetCurrentThreadId(), suspended, fib_bytes >> 10, in_use, jmps, ring_count, ring_size);
    }
    return 1;
}

static void main_on_scratch(void *unused) {
    (void)unused;
    memcpy((void *)(uintptr_t)loading->main.Rsp, loading->main_stack, loading->main_n);
    rb_resumed = 1;
    RtlRestoreContext(&loading->main, NULL);
}

static const char FIBER_MISMATCH[] = "a guest thread is on another fiber than at the save";
static char fiber_why[200];   /* (M7: which thread, and where it was and is: its own fiber or a job's) */
static int fiber_now_in_job;  /* (that thread is in a job now: waiting may see it out; one in a job at the save is past) */
static const char *check(Cpu *c, u64 frame, u32 n, const Slot *s) {
    if (frame != s->main_frame) return "the safe point's stack pointer differs from the save";
    if (current_fiber() != s->main_fiber || stack_base() != s->main_base) return "the main thread is on another fiber than at the save";
    if (n != s->threads) return "another number of guest threads than at the save";
    const char *why = kernel_rb_check(s->kernel);
    if (why) return why;
    for (LONG i = 0; i < nrecs; i++) {
        RbThread *t = recs[i];
        if (t->kthread == c->thread || !t->in_park) continue;
        if ((u32)i >= s->nthr || !s->thr[i].in_save) return "a guest thread that was not in the save";
        if (t->fiber != s->thr[i].saved_fiber || t->base != s->thr[i].saved_base) {
            snprintf(fiber_why, sizeof fiber_why, "thread %ld was on %s at the save, is on %s now", (long)i,
                     s->thr[i].saved_fiber == t->home ? "its own fiber" : "a job's fiber", t->fiber == t->home ? "its own fiber" : "a job's fiber");
            fiber_now_in_job = t->fiber != t->home;
            return FIBER_MISMATCH;
        }
    }
    return fiber_rb_check(s->fibers);
}
/* (M7) loads refused because a thread was on another fiber, and how many a retry saved; hle_rb_core.c reports them */
long long rb_fiber_refused, rb_fiber_retried_ok;

/* Back to the state saved `age` saves ago (0: the newest). Returns only if refused; otherwise the
 * game goes on from that save's rb_save call. */
void rb_load_age(Cpu *c, u64 frame, int age) {
    if (age < 0 || age >= ring_count) { fprintf(stderr, "[rb] load: no state %d saves back (%d kept)\n", age, ring_count); return; }
    Slot *s = slot_at(age);
    u32 n;
    load_t0 = now_ms();
    if (held) { held = 0; n = held_n; }   /* (straight after a save that kept them held) */
    else if (!hold(c->thread, &n)) { refused++; return; }
    const char *why = check(c, frame, n, s);
    /* M7, 4 players (4P1: 29 loads refused on PC 2, every retry the next frame too, until the guess was past the
     * states kept: a desync): a thread on another fiber than at the save may be in the middle of a job that ends in a
     * moment. The threads are let go and held again, up to SSBU_RB_LOAD_RETRY_MS (20), the game not going on
     * meanwhile. (They ran: the memory is no longer the save's own, so the load syncs it.) Only while the thread is
     * in a job now: in 4P2 all 7 were the other way (in a job at the save, on its own fiber now), which no wait mends;
     * the core then takes an older state at once. */
    if (why == FIBER_MISMATCH && !fiber_now_in_job) {
        rb_fiber_refused++;
        if (rb_fiber_refused <= 10) fprintf(stderr, "[rb] LOAD refused: %s (%s: an older state is tried)\n", why, fiber_why);
        refused++; release(); return;
    }
    if (why == FIBER_MISMATCH) {
        static double retry_ms = -1;
        if (retry_ms < 0) { const char *e = getenv("SSBU_RB_LOAD_RETRY_MS"); retry_ms = e && *e ? atof(e) : 20; }
        double tr = now_ms();
        char first[200];
        snprintf(first, sizeof first, "%s", fiber_why);
        while (why == FIBER_MISMATCH && now_ms() - tr < retry_ms) {
            memory_untouched = 0;
            release();
            SwitchToThread();
            if (!hold(c->thread, &n)) { refused++; rb_fiber_refused++; return; }
            why = check(c, frame, n, s);
        }
        if (!why) {
            rb_fiber_retried_ok++;
            if (rb_fiber_retried_ok <= 5) fprintf(stderr, "[rb] load: %s; let go and held again, it went ahead after %.1f ms\n", first, now_ms() - tr);
        } else if (why == FIBER_MISMATCH) {
            rb_fiber_refused++;
            if (rb_fiber_refused <= 10) fprintf(stderr, "[rb] LOAD refused: %s (still after %.0f ms; %s)\n", why, retry_ms, fiber_why);
            refused++; release(); return;
        }
    }
    if (why) { refused++; fprintf(stderr, "[rb] LOAD refused: %s\n", why); release(); return; }
    /* every other thread onto its scratch stack */
    ResetEvent(copy_ev);
    on_scratch = 0;
    for (LONG i = 0; i < nrecs; i++) { recs[i]->abort = 0; recs[i]->ld = (u32)i < s->nthr ? &s->thr[i] : NULL; }
    step("threads to their scratch stacks");
    SetEvent(load_ev);
    while ((u32)on_scratch < n) SwitchToThread();
    for (LONG i = 0; i < nrecs; i++)
        if (recs[i]->in_park && recs[i]->kthread != c->thread && !(recs[i]->ld && recs[i]->ld->in_save)) recs[i]->abort = 1;
    ResetEvent(load_ev);
    u64 live[1025]; u32 skipped;
    u32 nlive = live_stacks(c->thread, live);
    gpu_queue_drain();
    step("memory");
    memory_load();
    double tm = now_ms();
    u64 undone = 0;
    for (int a = 1; a <= age; a++) { undo_apply(&slot_at(a)->undo); undone += slot_at(a)->undo.n; }
    mstat.undo_pages = undone; mstat.undo_ms = now_ms() - tm;
    for (int a = 0; a < age; a++) slot_free(slot_at(a));   /* (the newer states) */
    ring_head = (int)(s - ring); ring_count -= age;
    s->undo.n = 0;
    step("kernel");
    kernel_rb_load(s->kernel);
    kmap_refresh();
    if (check_after && age) {   /* (memory and the shadow, after the undo records: contents and presence; after the kernel's map is that state's) */
        u64 first, lay, bad = shadow_mismatches(&first, &lay);
        fprintf(stderr, "[rb] check after the undo records: %llu pages differ from the shadow (first %#llx), %llu pages' presence differs\n", bad, first, lay);
    }
    step("fibers");
    fiber_rb_load(s->fibers, live, nlive, &skipped);
    step("setjmp records");
    setjmp_rb_load(s->setjmps);
    step("display buffer queue");
    vi_rb_load(s->vi);
    audio_rb_load(s->audio);
    step("threads' stacks, then this thread's");
    copied = 0;
    loading = s;
    mstat.age = age;
    SetEvent(copy_ev);   /* they copy their stacks back and park again, as at the save */
    run_on_scratch(&s->main, main_scratch, main_on_scratch, NULL);
}

void rb_load(Cpu *c, u64 frame) {
    if (!ring_count) { fprintf(stderr, "[rb] load: no savestate yet (F7 saves)\n"); return; }
    rb_load_age(c, frame, 0);
}

/* In rb_save's frame as at the save, after a load. */
static void finish_load(void) {
    step("back in the save's frame");
    while ((u32)copied < loading->threads) SwitchToThread();
    ResetEvent(copy_ev);
    loads++;
    release();
    double total = now_ms() - load_t0;
    if (!quiet || total > 12 || !(loads % 100))
        fprintf(stderr, "[rb] LOAD %lld: %.1f ms (save %lld, %d back); memory: %llu pages copied back (%llu MB; layout walk %u, %llu pages committed or decommitted) in %.1f ms "
                "[layout %.1f, alias stamps %.1f, alias pages %.1f, watched %.1f, (de)commits %.1f, compared %.1f], undo %llu pages in %.1f ms\n",
                loads, total, loading->seq, mstat.age, mstat.pages, mstat.pages >> 8, mstat.whole, mstat.whole_pages, mstat.ms,
                mstat.t[0], mstat.t[1], mstat.t[2], mstat.t[3], mstat.t[4], mstat.t[5], mstat.undo_pages, mstat.undo_ms);
}

/* ---------------------------------------------------------------- the replay check (M2: determinism)
 * rb_ref_capture keeps a copy of the saved regions as they are now (some frames after the save);
 * after a load and the same number of frames, rb_ref_compare says which pages differ, by area. With
 * the same pads, a deterministic game differs nowhere that is simulation. Both hold the threads, so
 * nothing changes memory while it is read. */
static u8 **ref;
static u8 **ref_absent;   /* per region, per page: 1 = not committed when the reference was taken */
static u32 nref;

/* Whether the page at p is committed and readable now (the game decommits GPU memory as it goes:
 * the saved region list is the save's, not today's). */
static const u8 *pt_lo, *pt_hi;   /* (the last committed run found: most pages fall in it; reset per pass) */
static int page_there(const u8 *p) {
    if (p >= pt_lo && p < pt_hi) return 1;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    pt_lo = m.BaseAddress; pt_hi = (const u8 *)m.BaseAddress + m.RegionSize;
    return 1;
}

static const char *area(u64 a) {
    if (a >= ALIAS_BASE && a < ALIAS_BASE + ALIAS_SIZE) return "alias (GPU memory)";
    if (a >= HEAP_BASE && a < HEAP_BASE + HEAP_LIMIT) return "heap";
    if (a >= KERNEL_BASE && a < KERNEL_BASE + 0x1000000) return "thread-local";
    if (a >= STACK_BASE && a < STACK_BASE + STACK_SIZE) return "guest stacks";
    if (a >= 0x7100000000ull && a < 0x7300000000ull) return "module data";
    return "other";
}

void rb_ref_capture(Cpu *c) {
    u32 n;
    if (!ring_count || !hold(c->thread, &n)) return;
    gpu_queue_drain();
    present_runs();
    if (!ref || nref != nregions) {
        for (u32 i = 0; ref && i < nref; i++) { VirtualFree(ref[i], 0, MEM_RELEASE); free(ref_absent[i]); }
        free(ref); free(ref_absent);
        ref = calloc(nregions, sizeof *ref);
        ref_absent = calloc(nregions, sizeof *ref_absent);
        nref = nregions;
        for (u32 i = 0; i < nref; i++) {
            ref[i] = VirtualAlloc(NULL, regions[i].size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
            ref_absent[i] = calloc(regions[i].size >> 12, 1);
        }
    }
    u64 absent = 0;
    pt_lo = pt_hi = NULL;
    for (u32 i = 0; i < nref; i++) {
        if (!ref[i]) continue;
        for (u64 off = 0; off < regions[i].size; off += 0x1000) {
            ref_absent[i][off >> 12] = !page_there(regions[i].at + off);
            if (ref_absent[i][off >> 12]) absent++;
            else memcpy(ref[i] + off, regions[i].at + off, 0x1000);
        }
    }
    release();
    if (absent) fprintf(stderr, "[rb] verify: %llu pages of the save's regions are not committed now\n", absent);
    fprintf(stderr, "[rb] verify: reference taken (%llu MB)\n", mem_bytes >> 20);
}

void rb_ref_compare(Cpu *c, long long round) {
    u32 n;
    if (!ref || !hold(c->thread, &n)) return;
    gpu_queue_drain();
    static const char *names[] = {"module data", "heap", "alias (game heap + GPU)", "guest stacks", "thread-local", "other"};
    static const char *areas[] = {"module data", "heap", "alias (GPU memory)", "guest stacks", "thread-local", "other"};
    /* the previous replay, to tell "the replays differ from the first run" from "every run differs" */
    static u8 **prev;
    static int have_prev;
    if (!prev) {
        prev = calloc(nref, sizeof *prev);
        for (u32 i = 0; i < nref; i++) prev[i] = VirtualAlloc(NULL, regions[i].size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    }
    /* every differing page, for reading: build\rb_verify_<round>.txt */
    char path[64];
    snprintf(path, sizeof path, "build\\rb_verify_%lld.txt", round);
    FILE *f = fopen(path, "w");
    u64 pages[6] = {0}, diff[6] = {0}, diff_prev[6] = {0}, bytes[6] = {0};
    char first[6][160] = {{0}};
    pt_lo = pt_hi = NULL;
    for (u32 i = 0; i < nref; i++) {
        if (!ref[i]) continue;
        const char *a = area((u64)(uintptr_t)regions[i].at);
        int k = 0;
        while (k < 5 && strcmp(areas[k], a)) k++;
        for (u64 off = 0; off < regions[i].size; off += 0x1000) {
            pages[k]++;
            const u8 *now = regions[i].at + off, *was_p = ref[i] + off;
            int there = page_there(now), was = !ref_absent[i][off >> 12];
            if (!there) { if (was) diff[k]++; continue; }
            if (have_prev && prev[i] && memcmp(now, prev[i] + off, 0x1000)) diff_prev[k]++;
            if (prev[i]) memcpy(prev[i] + off, now, 0x1000);
            if (!was || !memcmp(now, was_p, 0x1000)) continue;
            u32 nb = 0, at = 0x1000;
            for (u32 b = 0; b < 0x1000; b++) if (now[b] != was_p[b]) { if (at == 0x1000) at = b; nb++; }
            bytes[k] += nb;
            if (diff[k]++ < 4) {
                size_t used = strlen(first[k]);
                snprintf(first[k] + used, sizeof first[k] - used, "%s%#llx", used ? " " : "", (unsigned long long)(uintptr_t)now);
            }
            if (f) {
                u32 q = at & ~7u;
                fprintf(f, "%#llx %-24s %4u bytes differ, first at +%#05x: was %016llx now %016llx\n", (unsigned long long)(uintptr_t)now, a, nb, at,
                        *(const unsigned long long *)(was_p + q), *(const unsigned long long *)(now + q));
            }
        }
    }
    release();
    if (f) fclose(f);
    u64 total = 0, total_prev = 0;
    for (int k = 0; k < 6; k++) { total += diff[k]; total_prev += diff_prev[k]; }
    fprintf(stderr, "[rb] verify round %lld: %llu pages differ from the reference", round, total);
    if (have_prev) fprintf(stderr, ", %llu from the previous replay", total_prev);
    fprintf(stderr, " (each page: %s)\n", path);
    for (int k = 0; k < 6; k++)
        if (pages[k]) fprintf(stderr, "[rb]   %-24s %7llu of %7llu pages differ (%llu bytes)%s%s%s\n", names[k], diff[k], pages[k], bytes[k],
                              have_prev ? "" : "", diff[k] ? ", first: " : "", first[k]);
    if (have_prev)
        for (int k = 0; k < 6; k++)
            if (diff_prev[k]) fprintf(stderr, "[rb]   vs the previous replay: %-24s %7llu pages\n", names[k], diff_prev[k]);
    have_prev = 1;
}
