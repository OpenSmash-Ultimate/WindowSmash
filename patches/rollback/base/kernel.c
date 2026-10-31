/* A native stand-in for the console's kernel, at the system-call boundary.
 *
 * The recompiled SDK reaches the kernel through a few dozen `svc` wrappers; implementing
 * those calls here lets the SDK's own threading, locking, timing and memory code run
 * unmodified on host threads. Register conventions follow the SDK's wrappers: arguments in
 * x0-x7, the result code in w0, further outputs in x1 onwards.
 *
 * Blocking is deliberately simple for now: one lock guards all kernel state and one
 * condition variable is broadcast on every change, so each waiter re-checks its own
 * condition. Correct, not fast.
 *
 * Calls that would talk to a system service (IPC) are not implemented here: they stop the
 * run and say so, since services are replaced at the SDK function level instead. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "gpu.h"   /* (gpu_memory_unmapping) */

#ifdef __APPLE__
void rt_upnp_remove(void);   /* (linux/upnp.c: the netplay relay's port mapping, if it made one) */
#endif

enum { R_OK = 0, R_TIMEOUT = 0xEA01, R_CANCELLED = 0xEC01, R_INVALID_HANDLE = 0xE401, R_INVALID_STATE = 0xFA01,
       R_INVALID_ADDRESS = 0xCC01, R_INVALID_ENUM = 0xF001, R_OUT_OF_MEMORY = 0xD001 };
enum { GM_FREE = 0, GM_CODE = 3, GM_CODE_DATA = 4, GM_HEAP = 5, GM_STACK = 0xB, GM_TLS = 0xC, GM_INACCESSIBLE = 0x10 };
enum { OBJ_FREE, OBJ_THREAD, OBJ_EVENT, OBJ_SESSION, OBJ_MEMORY };

#define WAITERS 0x40000000u          /* "has waiters" bit of a userspace mutex word */
#define CUR_THREAD 0xFFFF8000u
#define CUR_PROCESS 0xFFFF8001u
#define HOST_STACK (256ull << 20)    /* guest call depth is host call depth */
#define MAIN_STACK (1u << 20)     /* from the program's metadata */

typedef struct KThread {
    u32 handle;
    HANDLE host;
    Cpu *cpu;
    u64 entry, arg, stack_top, tlr, id;
    i32 priority, core;
    int started, exited, cancelled;
    u64 mutex_addr;     /* userspace mutex this thread is queued on, or 0 */
    u32 mutex_tag;      /* value to store in the mutex word when it is handed over */
    u64 cv_key;         /* condition variable this thread waits on, or 0 */
    u64 cv_mutex;       /* the mutex to re-acquire when that wait is signalled */
    u64 arb_addr;       /* address-arbiter address this thread waits on, or 0 */
    u64 order;          /* arrival order, so hand-over is first-come-first-served */
    CONDITION_VARIABLE woken;   /* what this thread sleeps on (kwait): wakes go to the threads concerned */
    u64 wait_handles;   /* while in WaitSynchronization: its handle array (guest) and count */
    u32 wait_count;
} KThread;

typedef struct KObject { int type, signaled; KThread *thread; void *session; } KObject;
typedef struct Region { u64 base, size; u32 state, perm; } Region;

static CRITICAL_SECTION klock;
/* Every thread the kernel made (an exited one stays: its handle may still be waited on). Waits
 * and hand-overs look through these rather than through every handle. */
static KThread *threads[1024];
static u32 nthreads;
static KObject objects[4096];
#define REGIONS 16384
static Region regions[REGIONS];
static u32 region_count;
static struct { u64 dst, src, size; } maps[1024];
static u64 heap_size, tlr_next = KERNEL_BASE, next_thread_id = 1, wait_order;

/* ---- time ------------------------------------------------------------------------ */
static u64 now_ns(void) { return rt_clock_ns(); }

u64 rt_ticks(void) { return rt_clock_ns() * 12 / 625; }   /* (19.2 MHz) */

static u64 deadline_of(i64 timeout_ns) { return timeout_ns < 0 ? ~0ull : now_ns() + (u64)timeout_ns; }

/* Sleep until something wakes this thread (kwake) or the deadline passes; returns 0 once it
 * has passed. Each thread sleeps on its own condition variable: a mutex handed over, a
 * condition variable signalled, an event set wakes just the threads waiting for it (one shared
 * one, woken in full, sent every sleeping thread round the kernel lock at every change). The
 * callers look at their condition again either way. */
static int kwait(KThread *me, u64 deadline) {
    DWORD ms = INFINITE;
    if (deadline != ~0ull) {
        u64 now = now_ns();
        if (now >= deadline) return 0;
        ms = (DWORD)((deadline - now + 999999) / 1000000);
    }
    SleepConditionVariableCS(&me->woken, &klock, ms);
    return 1;
}

static void kwake(KThread *t) { WakeConditionVariable(&t->woken); }

/* Object `handle` became signalled: wake the threads waiting for it in WaitSynchronization. */
static void kwake_handle(u32 handle) {
    for (u32 i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        for (u32 k = 0; k < t->wait_count; k++)
            if (ld32(t->wait_handles + 4ull * k) == handle) { kwake(t); break; }
    }
}

/* ---- handles --------------------------------------------------------------------- */
static u32 new_handle(int type, KThread *t) {
    for (u32 i = 1; i < 4096; i++)
        if (objects[i].type == OBJ_FREE) {
            objects[i] = (KObject){type, 0, t};
            return 0xA000 + i;
        }
    rt_fatal("kernel: out of handles");
}

static KObject *object_of(Cpu *c, u32 handle, int type) {
    if (handle == CUR_THREAD && type == OBJ_THREAD) return &objects[((KThread *)c->thread)->handle - 0xA000];
    u32 i = handle - 0xA000;
    return i < 4096 && objects[i].type == type ? &objects[i] : NULL;
}

/* ---- memory map ------------------------------------------------------------------ */
/* The regions are sorted and apart. Memory the game maps and unmaps as it goes (heap, alias
 * memory, stacks, thread-local regions) is merged with a neighbour of the same kind, as the
 * console's kernel does: the game grows its pools a piece at a time, and with one region per
 * piece a long session of matches ran out of them ("too many memory regions", ten minutes and
 * ~90 fighters in). */
static int merges(u32 state) { return state == GM_HEAP || state == GM_STACK || state == GM_TLS; }

static u32 insert_region(u64 base, u64 size, u32 state, u32 perm) {
    if (region_count == REGIONS) rt_fatal("kernel: too many memory regions");
    u32 i = region_count++;
    for (; i > 0 && regions[i - 1].base > base; i--) regions[i] = regions[i - 1];
    regions[i] = (Region){base, size, state, perm};
    return i;
}

static void erase_region(u32 i) { memmove(&regions[i], &regions[i + 1], (--region_count - i) * sizeof *regions); }

/* Forget [base, base + size): regions inside it go, regions straddling its ends are trimmed. */
static void unmap_range(u64 base, u64 size) {
    u64 end = base + size;
    for (u32 i = 0; i < region_count;) {
        Region r = regions[i];
        u64 rend = r.base + r.size;
        if (rend <= base || r.base >= end || !merges(r.state)) { i++; continue; }
        erase_region(i);
        if (r.base < base) insert_region(r.base, base - r.base, r.state, r.perm);
        if (rend > end) insert_region(end, rend - end, r.state, r.perm);
        i = 0;
    }
}

/* (also from the loader, outside system calls: the lock keeps QueryMemory's search off a
 * half-moved table) */
void kernel_add_region(u64 base, u64 size, u32 state, u32 perm) {
    if (!size) return;
    EnterCriticalSection(&klock);
    if (!merges(state)) insert_region(base, size, state, perm);
    else {
        unmap_range(base, size);   /* (mapped again over itself: the new mapping is the one there) */
        u32 i = insert_region(base, size, state, perm);
        if (i + 1 < region_count && regions[i + 1].base == base + size && regions[i + 1].state == state && regions[i + 1].perm == perm) {
            regions[i].size += regions[i + 1].size;
            erase_region(i + 1);
        }
        if (i > 0 && regions[i - 1].base + regions[i - 1].size == base && regions[i - 1].state == state && regions[i - 1].perm == perm) {
            regions[i - 1].size += regions[i].size;
            erase_region(i);
        }
    }
    LeaveCriticalSection(&klock);
}

static void query_memory(u64 info, u64 addr) {
    Region r = {0, 0, GM_FREE, 0};
    u64 end = ASPACE_BASE + ASPACE_SIZE, prev = 0;
    if (addr >= end) r = (Region){end, 0 - end, GM_INACCESSIBLE, 0};
    else {
        /* the first region ending above addr: the regions are sorted and apart, so their ends
         * are in order too (thousands of them, and C++ exceptions ask tens of thousands of
         * times a second - libunwind finds each frame's module through QueryMemory) */
        u32 i = 0, hi = region_count;
        while (i < hi) {
            u32 mid = (i + hi) / 2;
            if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
            else hi = mid;
        }
        if (i) prev = regions[i - 1].base + regions[i - 1].size;
        if (i < region_count && regions[i].base <= addr) r = regions[i];
        else r = (Region){prev, (i < region_count ? regions[i].base : end) - prev, GM_FREE, 0};
    }
    st64(info, r.base);
    st64(info + 8, r.size);
    st32(info + 16, r.state);
    st32(info + 20, 0);
    st32(info + 24, r.perm);
    st32(info + 28, 0);
    st64(info + 32, 0);
}

/* 8 bytes of guest memory at addr if a region holds them: 0 if that memory is not mapped (hle_fiber.c
 * looks into fiber objects the game may have freed). Under the lock, so it cannot be unmapped
 * between the look and the read. */
int kernel_read64(u64 addr, u64 *out) {
    int ok = 0;
    EnterCriticalSection(&klock);
    u32 i = 0, hi = region_count;
    while (i < hi) {
        u32 mid = (i + hi) / 2;
        if (regions[mid].base + regions[mid].size <= addr) i = mid + 1;
        else hi = mid;
    }
    if (i < region_count && regions[i].base <= addr && addr + 8 <= regions[i].base + regions[i].size && (regions[i].perm & 1)) {
        *out = ld64(addr);
        ok = 1;
    }
    LeaveCriticalSection(&klock);
    return ok;
}

static void *commit(u64 at, u64 size, const char *what) {
    void *p = VirtualAlloc(G2H(at), size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) rt_fatal("kernel: cannot commit %llu KiB at %#llx for %s (error %lu)", size >> 10, at, what, (unsigned long)GetLastError());
    return p;
}

/* ---- threads --------------------------------------------------------------------- */
static KThread *new_thread(u64 entry, u64 arg, u64 stack_top, i32 priority, i32 core) {
    KThread *t = calloc(1, sizeof *t);
    if (nthreads == sizeof threads / sizeof *threads) rt_fatal("kernel: too many threads");
    threads[nthreads++] = t;
    InitializeConditionVariable(&t->woken);
    t->entry = entry; t->arg = arg; t->stack_top = stack_top; t->priority = priority; t->core = core;
    t->id = next_thread_id++;
    t->tlr = tlr_next;
    tlr_next += 0x1000;
    commit(t->tlr, 0x1000, "a thread-local region");
    kernel_add_region(t->tlr, 0x1000, GM_TLS, 3);
    t->cpu = _aligned_malloc(sizeof *t->cpu, 64);
    memset(t->cpu, 0, sizeof *t->cpu);
    t->cpu->thread = t;
    t->cpu->tpidrro = t->tlr;
    t->cpu->sp = stack_top;
    t->cpu->x[0] = arg;
    t->handle = new_handle(OBJ_THREAD, t);
    return t;
}

#ifndef _WIN32
/* Linux and macOS: a thread leaves from wherever it is by jumping back to its start (ExitThread
 * would be pthread_exit, which unwinds every guest frame on the way out). (RT_SETJMP: recomp.h's,
 * which on macOS leaves the signal mask alone - nothing changes it on a guest thread.) */
static __thread jmp_buf *exit_point;
#endif

static _Noreturn void exit_thread(KThread *t) {
    EnterCriticalSection(&klock);
    t->exited = 1;
    objects[t->handle - 0xA000].signaled = 1;
    kwake_handle(t->handle);
    LeaveCriticalSection(&klock);
#ifndef _WIN32
    if (exit_point) RT_LONGJMP(*exit_point, 1);
#endif
    ExitThread(0);
}

static DWORD WINAPI thread_main(LPVOID param) {
    KThread *t = param;
    char where[96];
    WCHAR desc[128];   /* for the profiler and debuggers */
#ifdef _WIN32
    _snwprintf(desc, 127, L"guest thread %llu (%hs, priority %d)", t->id, rt_describe(t->entry, where, sizeof where), t->priority);
#else
    swprintf(desc, 127, L"guest thread %llu (%s, priority %d)", (unsigned long long)t->id, rt_describe(t->entry, where, sizeof where), t->priority);
#endif
    desc[127] = 0;
    SetThreadDescription(GetCurrentThread(), desc);
#ifndef _WIN32
    jmp_buf out;
    exit_point = &out;
    if (RT_SETJMP(out)) return 0;
#endif
    rt_call(t->cpu, t->entry);
    exit_thread(t);
}

Cpu *kernel_main_thread(u32 *handle) {
    u64 top = STACK_BASE + STACK_SIZE - 0x10000;
    commit(top - MAIN_STACK, MAIN_STACK, "the main stack");
    EnterCriticalSection(&klock);
    kernel_add_region(top - MAIN_STACK, MAIN_STACK, GM_STACK, 3);
    KThread *t = new_thread(0, 0, top, 44, 0);
    t->started = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
    LeaveCriticalSection(&klock);
    *handle = t->handle;
    return t->cpu;
}

/* ---- userspace mutexes (svcArbitrateLock / Unlock) --------------------------------
 * The mutex word holds the owner's thread handle, plus WAITERS when the kernel has queued
 * threads on it. Unlocking hands the mutex directly to the longest-waiting thread. */
static KThread *threads_waiting(u64 KThread_field_offset, u64 addr, u32 *count) {
    KThread *first = NULL;
    *count = 0;
    for (u32 i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        if (t->exited || *(u64 *)((u8 *)t + KThread_field_offset) != addr) continue;
        ++*count;
        if (!first || t->order < first->order) first = t;
    }
    return first;
}
#define WAITING(field, addr, count) threads_waiting(offsetof(KThread, field), addr, count)

static void mutex_release(u64 addr) {
    u32 count;
    KThread *next = WAITING(mutex_addr, addr, &count);
    if (!next) { st32(addr, 0); return; }
    next->mutex_addr = 0;   /* this is what the waiter is watching for */
    st32(addr, next->mutex_tag | (count > 1 ? WAITERS : 0));
    kwake(next);
}

/* Give `t` the mutex if it is free, otherwise queue it behind the current owner. */
static void mutex_acquire_or_queue(KThread *t, u64 addr, u32 tag) {
    for (;;) {
        u32 v = ld32(addr);
        if (v == 0) {
            if (cas32(addr, 0, tag)) return;
        } else if (cas32(addr, v, v | WAITERS)) {
            t->mutex_tag = tag;
            t->order = ++wait_order;
            t->mutex_addr = addr;
            return;
        }
    }
}

/* ---- the system calls ------------------------------------------------------------ */
static u32 get_info(Cpu *c, u32 id, u64 sub, u64 *out) {
    (void)c;
    u64 image = 0;
    for (u32 i = 0; i < region_count; i++)
        if (regions[i].state == GM_CODE || regions[i].state == GM_CODE_DATA) image += regions[i].size;
    switch (id) {
    case 0: *out = 0x7; break;                          /* cores an application may use */
    case 1: *out = 0x0FFFFFFFF0000000ull; break;        /* priorities 28..59 */
    case 2: *out = ALIAS_BASE; break;
    case 3: *out = ALIAS_SIZE; break;
    case 4: *out = HEAP_BASE; break;
    case 5: *out = HEAP_LIMIT; break;
    case 6: case 21: *out = 0xCD500000ull; break;       /* memory an application gets on the console */
    case 7: case 22: *out = image + heap_size; break;
    case 8: *out = 0; break;                            /* no debugger */
    case 11: *out = 0x9E3779B97F4A7C15ull * (sub + 1) ^ now_ns(); break;   /* entropy */
    case 12: *out = ASPACE_BASE; break;
    case 13: *out = ASPACE_SIZE; break;
    case 14: *out = STACK_BASE; break;
    case 15: *out = STACK_SIZE; break;
    case 16: *out = 0x1000000; break;                   /* system resource size, from the program's metadata */
    case 17: *out = 0; break;
    case 18: *out = 0x01006A800016E000ull; break;       /* program id */
    case 20: *out = 0; break;
    case 23: *out = 1; break;                           /* is an application */
    default:
        fprintf(stderr, "[kernel] svcGetInfo: unknown id %u (sub %llu)\n", id, sub);
        return R_INVALID_ENUM;
    }
    return R_OK;
}

static u32 wait_synchronization(Cpu *c, u64 handles, u32 count, i64 timeout, u32 *index) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    for (;;) {
        for (u32 i = 0; i < count; i++) {
            u32 h = ld32(handles + 4 * i), k = h - 0xA000;
            if (k >= 4096 || objects[k].type == OBJ_FREE) return R_INVALID_HANDLE;
            if (objects[k].signaled) { *index = i; return R_OK; }
        }
        if (me->cancelled) { me->cancelled = 0; return R_CANCELLED; }
        if (timeout == 0) return R_TIMEOUT;
        me->wait_handles = handles;
        me->wait_count = count;
        int more = kwait(me, deadline);
        me->wait_count = 0;
        if (!more) return R_TIMEOUT;
    }
}

static u32 wait_for_address(Cpu *c, u64 addr, u32 type, i32 value, i64 timeout) {
    KThread *me = c->thread;
    i32 v = (i32)ld32(addr);
    if (type == 0 || type == 1) { if (!(v < value)) return R_INVALID_STATE; }
    else if (type == 2) { if (v != value) return R_INVALID_STATE; }
    else return R_INVALID_ENUM;
    if (type == 1) st32(addr, (u32)(v - 1));
    if (timeout == 0) return R_TIMEOUT;
    u64 deadline = deadline_of(timeout);
    me->order = ++wait_order;
    me->arb_addr = addr;
    while (me->arb_addr)
        if (!kwait(me, deadline)) { me->arb_addr = 0; return R_TIMEOUT; }
    return R_OK;
}

static u32 signal_to_address(u64 addr, u32 type, i32 value, i32 count) {
    u32 waiting;
    KThread *t = WAITING(arb_addr, addr, &waiting);
    if (type == 1) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        st32(addr, (u32)(value + 1));
    } else if (type == 2) {
        if ((i32)ld32(addr) != value) return R_INVALID_STATE;
        i32 next = value + 1;
        if (waiting) next = count <= 0 ? value - 2 : ((i32)waiting <= count ? value - 1 : value);
        st32(addr, (u32)next);
    } else if (type != 0) return R_INVALID_ENUM;
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        t->arb_addr = 0;
        kwake(t);
        t = WAITING(arb_addr, addr, &waiting);
    }
    return R_OK;
}

static u32 wait_condition(Cpu *c, u64 mutex, u64 key, u32 tag, i64 timeout) {
    KThread *me = c->thread;
    u64 deadline = deadline_of(timeout);
    u32 others;
    mutex_release(mutex);
    st32(key, 1);
    me->mutex_tag = tag;
    me->cv_mutex = mutex;
    me->order = ++wait_order;
    me->cv_key = key;
    while (me->cv_key)
        if (!kwait(me, deadline)) {
            if (!me->cv_key) break;                 /* signalled just as the wait expired */
            me->cv_key = 0;
            WAITING(cv_key, key, &others);
            if (!others) st32(key, 0);
            return R_TIMEOUT;                       /* the caller re-locks the mutex itself */
        }
    while (me->mutex_addr) kwait(me, ~0ull);        /* the signaller queued us on the mutex */
    return R_OK;
}

static void signal_condition(u64 key, i32 count) {
    u32 waiting;
    KThread *t = WAITING(cv_key, key, &waiting);
    for (i32 n = 0; t && (count <= 0 || n < count); n++) {
        t->cv_key = 0;
        /* the woken thread must own the mutex again before it returns */
        mutex_acquire_or_queue(t, t->cv_mutex, t->mutex_tag);
        kwake(t);   /* (queued on the mutex instead, it waits on there) */
        t = WAITING(cv_key, key, &waiting);
    }
    if (!t) st32(key, 0);
}

void rt_svc(Cpu *c, u32 n) {
    KThread *me = c->thread;
    KObject *o;
    u64 *x = c->x;
    if (rt_trace) fprintf(stderr, "[svc %#04x] thread %llu x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", n, me->id, x[0], x[1], x[2], x[3]);

    /* calls that never touch shared kernel state */
    switch (n) {
    case 0x0B:   /* SleepThread(ns): non-positive values are yield requests */
        if ((i64)x[0] > 0) Sleep((DWORD)((x[0] + 999999) / 1000000)); else SwitchToThread();
        return;
    case 0x10: x[0] = 0; return;                         /* GetCurrentProcessorNumber */
    case 0x1E: x[0] = rt_ticks(); return;                /* GetSystemTick */
    case 0x27:                                           /* OutputDebugString(ptr, len) */
        fprintf(stderr, "[guest] %.*s%s", (int)x[1], P(const char, x[0]), x[1] && P(const char, x[0])[x[1] - 1] == '\n' ? "" : "\n");
        x[0] = R_OK;
        return;
    case 0x26:                                           /* Break(reason, address, size) */
        if (x[2] == 4 && x[1])   /* the info is a failed result */
            rt_fatal("the guest aborted on result %#x (module %u, description %u)", ld32(x[1]), ld32(x[1]) & 0x1FF, (ld32(x[1]) >> 9) & 0x1FFF);
        rt_fatal("the guest called svcBreak (reason %#llx, info %#llx, size %#llx)", x[0], x[1], x[2]);
#ifdef __APPLE__   /* (not exit(): atexit handlers and C++ destructors under the threads still running - Metal's, AppKit's - can crash on the way out) */
    case 0x07: fprintf(stderr, "guest process exited\n"); rt_upnp_remove(); ExitProcess(0);
#else
    case 0x07: fprintf(stderr, "guest process exited\n"); exit(0);
#endif
    case 0x0A: exit_thread(me);
    case 0x1F: {   /* ConnectToNamedPort(name) -> session handle */
        void *s = ipc_connect(P(const char, x[1]));
        if (!s) rt_fatal("svcConnectToNamedPort(\"%s\"): no such port here", P(const char, x[1]));
        x[0] = R_OK; x[1] = kernel_new_session(s);
        return;
    }
    case 0x21: case 0x22: {   /* SendSyncRequest(handle) / ...WithUserBuffer(buffer, size, handle) */
        u32 h = (u32)(n == 0x21 ? x[0] : x[2]);
        void *s = kernel_session_of(h);
        if (!s) { x[0] = R_INVALID_HANDLE; return; }
        x[0] = ipc_request(c, s, n == 0x21 ? c->tpidrro : x[0], n == 0x21 ? 0x100 : x[1]);
        return;
    }
    }

    EnterCriticalSection(&klock);
    switch (n) {
    case 0x01: {   /* SetHeapSize(size) -> address */
        u64 size = x[1];
        if (size > HEAP_LIMIT) { x[0] = R_OUT_OF_MEMORY; break; }
        if (size > heap_size) commit(HEAP_BASE + heap_size, size - heap_size, "the heap");
        if (heap_size) unmap_range(HEAP_BASE, heap_size);
        heap_size = size;
        if (size) kernel_add_region(HEAP_BASE, size, GM_HEAP, 3);
        x[0] = R_OK; x[1] = HEAP_BASE;
        break;
    }
    case 0x02: case 0x03: x[0] = R_OK; break;            /* SetMemoryPermission / SetMemoryAttribute */
    case 0x04: {   /* MapMemory(dst, src, size): the pages appear at dst; here, as a copy */
        u32 i = 0;
        while (i < 1024 && maps[i].size) i++;
        if (i == 1024) rt_fatal("kernel: too many memory mappings");
        commit(x[0], x[2], "a memory mapping");
        memcpy(G2H(x[0]), G2H(x[1]), x[2]);
        maps[i].dst = x[0]; maps[i].src = x[1]; maps[i].size = x[2];
        kernel_add_region(x[0], x[2], GM_STACK, 3);
        x[0] = R_OK;
        break;
    }
    case 0x05: {   /* UnmapMemory(dst, src, size) */
        for (u32 i = 0; i < 1024; i++)
            if (maps[i].size && maps[i].dst == x[0]) {
                memcpy(G2H(maps[i].src), G2H(maps[i].dst), maps[i].size);
                gpu_memory_unmapping(maps[i].dst, maps[i].size);   /* (queued GPU work and the renderer may read it) */
                VirtualFree(G2H(maps[i].dst), maps[i].size, MEM_DECOMMIT);
                unmap_range(maps[i].dst, maps[i].size);
                maps[i].size = 0;
            }
        x[0] = R_OK;
        break;
    }
    case 0x06: query_memory(x[0], x[2]); x[0] = R_OK; x[1] = 0; break;
    case 0x08: {   /* CreateThread(entry, arg, stack top, priority, core) -> handle */
        KThread *t = new_thread(x[1], x[2], x[3], (i32)x[4], (i32)x[5]);
        x[0] = R_OK; x[1] = t->handle;
        break;
    }
    case 0x09:     /* StartThread(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->started = 1;
        o->thread->host = CreateThread(NULL, HOST_STACK, thread_main, o->thread, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!o->thread->host) rt_fatal("kernel: cannot start a host thread (error %lu)", (unsigned long)GetLastError());
        x[0] = R_OK;
        break;
    case 0x0C:     /* GetThreadPriority(handle) -> priority */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->priority;
        break;
    case 0x0D:     /* SetThreadPriority(handle, priority) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->priority = (i32)x[1];
        x[0] = R_OK;
        break;
    case 0x0E:     /* GetThreadCoreMask(handle) -> core, mask */
        if (!(o = object_of(c, (u32)x[2], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = (u32)o->thread->core; x[2] = 0x7;
        break;
    case 0x0F: x[0] = R_OK; break;                       /* SetThreadCoreMask */
    case 0x11:     /* SignalEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 1;
        kwake_handle((u32)x[0]);
        x[0] = R_OK;
        break;
    case 0x12:     /* ClearEvent(handle) */
        if (!(o = object_of(c, (u32)x[0], OBJ_EVENT))) { x[0] = R_INVALID_HANDLE; break; }
        o->signaled = 0;
        x[0] = R_OK;
        break;
    case 0x16: {   /* CloseHandle(handle) */
        u32 k = (u32)x[0] - 0xA000;
        if (k < 4096 && objects[k].type == OBJ_SESSION) { ipc_close(objects[k].session); objects[k].type = OBJ_FREE; }
        if (k < 4096 && (objects[k].type == OBJ_EVENT || objects[k].type == OBJ_MEMORY)) objects[k].type = OBJ_FREE;
        x[0] = R_OK;   /* thread objects stay: their thread may still be running */
        break;
    }
    case 0x17: {   /* ResetSignal(handle) */
        u32 k = (u32)x[0] - 0xA000;
        if (k >= 4096 || objects[k].type == OBJ_FREE) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = objects[k].signaled && objects[k].type == OBJ_EVENT ? R_OK : R_INVALID_STATE;
        if (objects[k].type == OBJ_EVENT) objects[k].signaled = 0;
        break;
    }
    case 0x18: {   /* WaitSynchronization(handles, count, timeout) -> index */
        u32 index = 0;
        x[0] = wait_synchronization(c, x[1], (u32)x[2], (i64)x[3], &index);
        x[1] = index;
        break;
    }
    case 0x19:     /* CancelSynchronization(thread) */
        if (!(o = object_of(c, (u32)x[0], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        o->thread->cancelled = 1;
        kwake(o->thread);
        x[0] = R_OK;
        break;
    case 0x1A:     /* ArbitrateLock(owner handle, mutex address, own handle) */
        if (ld32(x[1]) == ((u32)x[0] | WAITERS)) {
            me->mutex_tag = (u32)x[2];
            me->order = ++wait_order;
            me->mutex_addr = x[1];
            while (me->mutex_addr) kwait(me, ~0ull);
        }
        x[0] = R_OK;
        break;
    case 0x1B: mutex_release(x[0]); x[0] = R_OK; break;  /* ArbitrateUnlock(mutex address) */
    case 0x1C: x[0] = wait_condition(c, x[0], x[1], (u32)x[2], (i64)x[3]); break;
    case 0x1D: signal_condition(x[0], (i32)x[1]); x[0] = R_OK; break;
    case 0x25:     /* GetThreadId(handle) -> id */
        if (!(o = object_of(c, (u32)x[1], OBJ_THREAD))) { x[0] = R_INVALID_HANDLE; break; }
        x[0] = R_OK; x[1] = o->thread->id;
        break;
    case 0x24: x[0] = R_OK; x[1] = 0x51; break;          /* GetProcessId */
    case 0x29: {   /* GetInfo(id, handle, sub id) -> value */
        u64 value = 0;
        x[0] = get_info(c, (u32)x[1], x[3], &value);
        x[1] = value;
        break;
    }
    case 0x2C:     /* MapPhysicalMemory(address, size): back part of the alias area with memory */
        commit(x[0], x[1], "physical memory");
        kernel_add_region(x[0], x[1], GM_HEAP, 3);
        x[0] = R_OK;
        break;
    case 0x2D:     /* UnmapPhysicalMemory(address, size) */
        gpu_memory_unmapping(x[0], x[1]);   /* (queued GPU work and the renderer may read it) */
        VirtualFree(G2H(x[0]), x[1], MEM_DECOMMIT);
        unmap_range(x[0], x[1]);
        x[0] = R_OK;
        break;
    case 0x34: x[0] = wait_for_address(c, x[0], (u32)x[1], (i32)x[2], (i64)x[3]); break;
    case 0x35: x[0] = signal_to_address(x[0], (u32)x[1], (i32)x[2], (i32)x[3]); break;
    case 0x15: {   /* CreateTransferMemory(address, size, permission) -> handle: lent memory stays where it is */
        x[1] = new_handle(OBJ_MEMORY, NULL);
        x[0] = R_OK;
        break;
    }
    case 0x45: {   /* CreateEvent() -> writable handle, readable handle (one object here) */
        u32 h = new_handle(OBJ_EVENT, NULL);
        x[0] = R_OK; x[1] = h; x[2] = h;
        break;
    }
    default:
        LeaveCriticalSection(&klock);
        rt_fatal("system call %#x is not implemented by the native kernel", n);
    }
    LeaveCriticalSection(&klock);
    if (rt_trace) fprintf(stderr, "[svc %#04x] -> %#llx (x1=%#llx)\n", n, x[0], x[1]);
}

/* For diagnosis: where is every guest thread right now? */
DWORD kernel_lock_owner(void) { return (DWORD)(uintptr_t)klock.OwningThread; }

/* For a freeze report: every guest thread's state and host thread id (its stack is in the host
 * threads' part of the report). Without waiting for the kernel's lock: says so when it is held. */
void kernel_thread_states(void (*line)(const char *text)) {
    char text[200];
    if (!TryEnterCriticalSection(&klock)) {
        snprintf(text, sizeof text, "  (the kernel's lock is held by thread %lu: guest thread states not read)\n", (unsigned long)kernel_lock_owner());
        line(text);
        return;
    }
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        const char *state = t->mutex_addr ? "waiting for a mutex" : t->cv_key ? "waiting on a condition" :
                            t->arb_addr ? "waiting on an address" : "running or in a system call";
        snprintf(text, sizeof text, "  guest thread %llu (host thread %lu, priority %d): %s\n", t->id, (unsigned long)GetThreadId(t->host), t->priority, state);
        line(text);
    }
    LeaveCriticalSection(&klock);
}

void kernel_dump_threads(void) {
    EnterCriticalSection(&klock);
    for (u32 i = 1; i < 4096; i++) {
        KThread *t = objects[i].type == OBJ_THREAD ? objects[i].thread : NULL;
        if (!t || !t->started || t->exited || !t->host) continue;
        const char *state = t->mutex_addr ? "waiting for a mutex" : t->cv_key ? "waiting on a condition" :
                            t->arb_addr ? "waiting on an address" : "running or in a system call";
        fprintf(stderr, "  thread %llu (priority %d): %s\n", t->id, t->priority, state);
        rt_backtrace_thread(t->host, 6);
    }
    LeaveCriticalSection(&klock);
}

u32 kernel_new_session(void *session) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_SESSION, NULL);
    objects[h - 0xA000].session = session;
    LeaveCriticalSection(&klock);
    return h;
}

void *kernel_session_of(u32 handle) {
    u32 k = handle - 0xA000;
    return k < 4096 && objects[k].type == OBJ_SESSION ? objects[k].session : NULL;
}

u32 kernel_create_event(void) {
    EnterCriticalSection(&klock);
    u32 h = new_handle(OBJ_EVENT, NULL);
    LeaveCriticalSection(&klock);
    return h;
}

void kernel_signal_event(u32 handle) {
    EnterCriticalSection(&klock);
    u32 k = handle - 0xA000;
    if (k < 4096 && objects[k].type == OBJ_EVENT) { objects[k].signaled = 1; kwake_handle(handle); }
    LeaveCriticalSection(&klock);
}

u32 rt_attach_system_event(Cpu *c, u64 system_event, int auto_clear) {
    u32 h = kernel_create_event();
    /* nn::os::AttachReadableHandleToSystemEvent(event, handle, manage the handle, clear mode) */
    rt_callsym(c, "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE", 4,
               system_event, (u64)h, (u64)1, (u64)(auto_clear ? 1 : 0));
    return h;
}

void kernel_init(void) {
    InitializeCriticalSection(&klock);
    rt_clock_ns();   /* (the clock set up - and its line logged - before the game asks it) */
    /* Reserve the areas the guest maps into; pages are committed as they are used. The alias
     * area, where the game keeps its GPU memory, has its writes watched so that the renderer
     * learns which textures the CPU changed (GetWriteWatch). */
    static const struct { u64 base, size; const char *what; DWORD flags; } areas[] = {
        {HEAP_BASE, HEAP_LIMIT, "heap", 0}, {ALIAS_BASE, ALIAS_SIZE, "alias", MEM_WRITE_WATCH}, {STACK_BASE, STACK_SIZE, "stack", 0},
        {KERNEL_BASE, 0x1000000, "thread-local", 0},
    };
    for (u32 i = 0; i < sizeof areas / sizeof *areas; i++)
        if (VirtualAlloc(G2H(areas[i].base), areas[i].size, MEM_RESERVE | areas[i].flags, PAGE_READWRITE) != G2H(areas[i].base))
            rt_fatal("kernel: cannot reserve the %s area at %#llx", areas[i].what, areas[i].base);
}
