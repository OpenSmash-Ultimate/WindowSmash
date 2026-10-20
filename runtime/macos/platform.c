/* What the macOS build of the Windows API (runtime/linux/win32.c) needs from the system that
 * Linux has and macOS spells differently: waiting on an address (Linux's futex), the clock and
 * sleeping until a time on it, the program's own path, and walking a thread's stack (another's
 * too, stopped for it: as the Windows build does, with no signal).
 *
 * Waiting on an address is os_sync_wait_on_address, from macOS 14.4; on 14.0 - 14.3 (the
 * oldest this build runs on) the same kernel call under its older, private name __ulock_wait2.
 * SSBU_FUTEX=ulock takes that one anywhere (for testing). Timed sleeps are on critical timers,
 * which the system does not coalesce. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#include <windows.h>

typedef uint64_t u64;

/* ---- waiting on an address ---------------------------------------------------------------- */
/* (libsystem_kernel's, since macOS 11: what os_sync_wait_on_address calls) */
extern int __ulock_wait2(uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100
#define ULF_NO_ERRNO 0x01000000

static int os_sync = -1;   /* 1: os_sync_*, 0: __ulock_* */

static int use_os_sync(void) {
    if (os_sync < 0) {
        const char *force = getenv("SSBU_FUTEX");
        int have = 0;
        if (__builtin_available(macOS 14.4, *)) have = 1;
        os_sync = have && !(force && !strcmp(force, "ulock"));
    }
    return os_sync;
}

const char *rt_futex_mode(void) { return use_os_sync() ? "os_sync_wait_on_address" : "__ulock_wait2"; }

static int wait_once(volatile int *addr, int value, u64 ns) {   /* ns 0: no limit. 1 woken (or not holding value), 0 timed out */
    if (use_os_sync()) {
        if (__builtin_available(macOS 14.4, *)) {
            int r = !ns ? os_sync_wait_on_address((void *)addr, (uint32_t)value, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE)
                        : os_sync_wait_on_address_with_timeout((void *)addr, (uint32_t)value, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
            return !(r < 0 && errno == ETIMEDOUT);
        }
    }
    return __ulock_wait2(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, (void *)addr, (uint32_t)value, ns, 0) != -ETIMEDOUT;
}

u64 rt_uptime_ns(void);

/* Sleep while *addr holds value, ms at most (INFINITE: no limit): 1 when woken (or spuriously,
 * or it held something else), 0 when the time ran out. (macOS lets a timed wait run late by a
 * share of its length - a tenth, measured: so half of what is left at a time, until under a
 * millisecond is.) */
int rt_futex_wait(volatile int *addr, int value, DWORD ms) {
    if (!ms) return *addr != value;
    if (ms == INFINITE) return wait_once(addr, value, 0);
    u64 until = rt_uptime_ns() + (u64)ms * 1000000ull;
    for (u64 now; (now = rt_uptime_ns()) < until;) {
        u64 left = until - now;
        if (wait_once(addr, value, left > 1000000 ? left / 2 : left)) return 1;
    }
    return *addr != value;
}

/* Wake one thread waiting on addr, or all of them. */
void rt_futex_wake(volatile int *addr, int all) {
    if (use_os_sync()) {
        if (__builtin_available(macOS 14.4, *)) {
            if (all) os_sync_wake_by_address_all((void *)addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
            else os_sync_wake_by_address_any((void *)addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
            return;
        }
    }
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | (all ? ULF_WAKE_ALL : 0), (void *)addr, 0);
}

/* ---- the clock ---------------------------------------------------------------------------- */
/* mach_absolute_time: the processor's counter, as Linux's CLOCK_MONOTONIC stopping while the
 * Mac sleeps (24 MHz on Apple silicon); a few nanoseconds a read. */
static mach_timebase_info_data_t timebase;   /* (read before main: no thread asks before it is) */
static u64 counter_hz;

__attribute__((constructor)) static void timebase_init(void) {
    mach_timebase_info(&timebase);
    u64 scaled = 1000000000ull * timebase.denom;
    counter_hz = scaled % timebase.numer ? 0 : scaled / timebase.numer;
}

u64 rt_uptime_ns(void) { return (u64)((unsigned __int128)mach_absolute_time() * timebase.numer / timebase.denom); }

/* Counter ticks a second, when that is a whole number (it is on every Mac): QueryPerformanceFrequency. */
u64 rt_uptime_hz(void) { return counter_hz; }

/* Sleep until rt_uptime_ns reaches t: on a timer of the thread's own kqueue marked critical,
 * which the system never puts off to wake with others (a plain sleep's may come several
 * milliseconds late - tens, while the system coalesces timers - which a frame's pacing cannot
 * take: Windows' high-resolution waitable timers are this kind). */
static pthread_key_t sleep_key;
static pthread_once_t sleep_once = PTHREAD_ONCE_INIT;
static void close_queue(void *kq) { close((int)(intptr_t)kq - 1); }
static void sleep_key_init(void) { pthread_key_create(&sleep_key, close_queue); }

static int thread_queue(void) {
    pthread_once(&sleep_once, sleep_key_init);
    intptr_t v = (intptr_t)pthread_getspecific(sleep_key);
    if (!v) {
        int kq = kqueue();
        if (kq < 0) return -1;
        fcntl(kq, F_SETFD, FD_CLOEXEC);
        v = kq + 1;
        pthread_setspecific(sleep_key, (void *)v);
    }
    return (int)v - 1;
}

void rt_sleep_until_ns(u64 t) {
    u64 at = (u64)((unsigned __int128)t * timebase.denom / timebase.numer);
    int kq = thread_queue();
    for (u64 now; (now = mach_absolute_time()) < at;) {   /* (again if a signal cut it short) */
        struct kevent64_s ev, out;
        u64 left = (u64)((unsigned __int128)(at - now) * timebase.numer / timebase.denom);
        EV_SET64(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (int64_t)(left ? left : 1), 0, 0, 0);
        if (kq < 0 || (kevent64(kq, &ev, 1, &out, 1, 0, NULL) < 0 && errno != EINTR)) mach_wait_until(at);
    }
}

/* ---- the program -------------------------------------------------------------------------- */
static char exe_path[PATH_MAX];
static pthread_once_t exe_once = PTHREAD_ONCE_INIT;

static void find_exe(void) {
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n)) return;   /* (longer than PATH_MAX: left empty) */
    if (!realpath(raw, exe_path)) snprintf(exe_path, sizeof exe_path, "%s", raw);
}

const char *rt_exe_path(void) {
    pthread_once(&exe_once, find_exe);
    return exe_path;
}

/* ---- stack walks -------------------------------------------------------------------------- */
/* Frame records ({caller's x29, return address}, x29 pointing at them) are copied off the stack
 * by the kernel (mach_vm_read_overwrite: an address that cannot be read fails the copy, never
 * faults), up to a host page's end at a time - so a walk usually takes a call or two, not one a
 * frame. A record that cannot be read, or that is not above the last, ends the walk. (arm64
 * macOS code keeps x29 a frame pointer; the fiber trampoline zeroes it, ending a fiber's chain.) */
int rt_frame_walk(uint64_t pc, uint64_t lr, uint64_t fp, void **frames, int max) {
    u64 copy[2048];   /* (16 KiB: a host page) */
    u64 from = 0, to = 0;   /* what copy holds */
    int n = 0, first = 1;
    if (max <= 0) return 0;
    frames[n++] = (void *)(uintptr_t)pc;
    for (; n < max && fp && !(fp & 7);) {
        if (fp < from || fp + 16 > to) {
            u64 end = (fp + 16 + 0x3FFF) & ~0x3FFFull;
            mach_vm_size_t got = 0;
            if (end - fp > sizeof copy) end = fp + sizeof copy;
            if (mach_vm_read_overwrite(mach_task_self(), fp, end - fp, (mach_vm_address_t)(uintptr_t)copy, &got) != KERN_SUCCESS || got < 16) break;
            from = fp;
            to = fp + got;
        }
        const u64 *record = copy + (fp - from) / 8;
        u64 next = record[0], ra = record[1];
        if (first && lr && lr != ra) frames[n++] = (void *)(uintptr_t)lr;   /* (not saved yet: a leaf, or it called since) */
        first = 0;   /* (a record read: lr is dealt with, even if the walk ends here) */
        if (!ra || n == max) break;
        frames[n++] = (void *)(uintptr_t)ra;
        if (next <= fp) break;
        fp = next;
    }
    if (first && lr && n < max) frames[n++] = (void *)(uintptr_t)lr;   /* (no record at all) */
    return n;
}

/* Another thread's frames: stopped, its registers read and its stack walked, then let go on -
 * nothing between that allocates or locks, which the stopped thread might hold. pc and lr: its
 * registers (when not NULL). 0 frames if it could not be stopped (it ended). One thread at a
 * time stops another, in the whole process: two stopping each other at once would both stay
 * stopped (host.c's thread dumps and the profiler both come here). */
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;

int rt_thread_frames(unsigned port, void **frames, int max, uint64_t *pc, uint64_t *lr) {
    int n = 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (!port || port == pthread_mach_thread_np(pthread_self())) return 0;
    pthread_mutex_lock(&stop_lock);
    if (thread_suspend(port) == KERN_SUCCESS) {
        if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
            u64 p = arm_thread_state64_get_pc(st), l = arm_thread_state64_get_lr(st);
            n = max > 0 ? rt_frame_walk(p, l, arm_thread_state64_get_fp(st), frames, max) : 1;
            if (pc) *pc = p;
            if (lr) *lr = l;
        }
        thread_resume(port);
    }
    pthread_mutex_unlock(&stop_lock);
    return n;
}
