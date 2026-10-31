/* A sampling profiler, for the debug keys (F6): for a few seconds, every millisecond, where each
 * of the process's threads is; then a report of the busiest functions (named from the exe's
 * linker map, <exe>.map), modules and threads, written to profile_<time>.txt beside the exe (and
 * its top lines to the log). Recompiled guest functions are named f_<guest address>.
 * Also: every thread's stack for freeze reports (profile_dump_threads), and the hitch log
 * (SSBU_HITCH=<ms>: what made each frame over that long late). */
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* (GetThreadDescription) */
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "debug_profile.h"

typedef unsigned long long U64;
typedef unsigned U32;

#define SLOTS (1u << 20)
typedef struct Sample { U64 rip; U32 count, name, thread; } Sample;   /* name: an offset into the name pool + 1; thread: an index into threads */
static Sample *table;
static U32 distinct;
static volatile LONG running;
static double duration;

typedef struct ThreadStat { DWORD tid; U32 samples, waiting, reported; WCHAR desc[48]; } ThreadStat;
static ThreadStat threads[512];
static U32 nthreads;

typedef struct Module { U64 base, size; RUNTIME_FUNCTION *pdata; U32 npdata; char name[64]; } Module;
static Module modules[512];
static U32 nmodules, exe_module;

/* Call stacks of busy samples (of every thread), unwound from the modules' own unwind tables
 * while the thread is suspended - without the system's lookup, which takes locks the
 * suspended thread may hold. */
#define STACK_DEPTH 24
#define MAX_STACKS 65536
typedef struct Stack { U32 thread, depth, waiting; U64 pc[STACK_DEPTH]; } Stack;   /* waiting: in a system call (only the guest main thread's) */
static Stack *stacks;
static U32 nstacks;

/* The program's symbols, from build\ssbu.map: addresses and names (offsets into sym_pool). */
static U64 *sym_va;
static U32 *sym_name, nsyms;
static char *sym_pool;

int profile_running(void) { return running != 0; }

static void add(U64 rip, U32 thread) {
    U32 h = (U32)(((rip ^ (U64)thread << 52) * 0x9E3779B97F4A7C15ull) >> 44) & (SLOTS - 1);
    while (table[h].rip && (table[h].rip != rip || table[h].thread != thread)) h = (h + 1) & (SLOTS - 1);
    if (!table[h].rip) { if (distinct >= SLOTS / 2) return; table[h].rip = rip; table[h].thread = thread; distinct++; }
    table[h].count++;
}

static int module_of(U64 rip) {
    for (U32 i = 0; i < nmodules; i++)
        if (rip >= modules[i].base && rip < modules[i].base + modules[i].size) return (int)i;
    return -1;
}

static void list_modules(void) {
    HMODULE mods[512];
    DWORD bytes = 0;
    nmodules = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return;
    HMODULE self = GetModuleHandleW(NULL);
    for (U32 i = 0; i < bytes / sizeof *mods && nmodules < 512; i++) {
        MODULEINFO mi;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        Module *m = &modules[nmodules];
        m->base = (U64)mi.lpBaseOfDll;
        m->size = mi.SizeOfImage;
        if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], m->name, sizeof m->name)) snprintf(m->name, sizeof m->name, "module %u", i);
        if (mods[i] == self) exe_module = nmodules;
        const BYTE *image = (const BYTE *)mi.lpBaseOfDll;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
        const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        m->pdata = nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXCEPTION && d->Size ? (RUNTIME_FUNCTION *)(image + d->VirtualAddress) : NULL;
        m->npdata = m->pdata ? d->Size / sizeof *m->pdata : 0;
        nmodules++;
    }
}

static RUNTIME_FUNCTION *function_entry(int m, U64 rip) {
    if (m < 0 || !modules[m].pdata) return NULL;
    DWORD rva = (DWORD)(rip - modules[m].base);
    U32 lo = 0, hi = modules[m].npdata;
    while (lo < hi) {   /* the table is sorted by address */
        U32 mid = (lo + hi) / 2;
        if (modules[m].pdata[mid].EndAddress <= rva) lo = mid + 1;
        else if (modules[m].pdata[mid].BeginAddress > rva) hi = mid;
        else return &modules[m].pdata[mid];
    }
    return NULL;
}

/* Walk a suspended thread's stack from its context. A function without unwind data is a leaf,
 * its return address on top of the stack; outside every module, the walk stops. A stack word
 * that cannot be read ends the walk (rt_probing tells the crash handler it is expected). */
extern __declspec(thread) int rt_probing;   /* host.c */
static U32 walk(CONTEXT *ctx, U64 *pc) {
    volatile U32 d = 0;
    pc[d++] = ctx->Rip;
    rt_probing = 1;
    __try {
        while (d < STACK_DEPTH) {
            int m = module_of(ctx->Rip);
            if (m < 0) break;
            RUNTIME_FUNCTION *rf = function_entry(m, ctx->Rip);
            U64 sp = ctx->Rsp;
            if (rf) {
                void *handler;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, modules[m].base, ctx->Rip, rf, ctx, &handler, &frame, NULL);
            } else {
                ctx->Rip = *(const U64 *)ctx->Rsp;
                ctx->Rsp += 8;
            }
            if (!ctx->Rip || ctx->Rsp <= sp) break;
            pc[d++] = ctx->Rip - 1;   /* (inside the call instruction, so a call at a function's end names that function) */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    rt_probing = 0;
    return d;
}

/* <exe folder>\<name>, or <exe path> with another extension (ext starting with '.') */
static void beside_exe(char *out, size_t n, const char *name) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    exe[len < MAX_PATH ? len : MAX_PATH - 1] = 0;
    if (name[0] == '.') {
        char *dot = strrchr(exe, '.');
        if (dot && dot > strrchr(exe, '\\')) *dot = 0;
        snprintf(out, n, "%s%s", exe, name);
    } else {
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(out, n, "%s\\%s", exe, name);
    }
}

/* Read once, by whichever thread asks first (the hitch log's reads it in the background at
 * start: names show up once it is done); the others wait. The symbols are published last. */
static volatile LONG symbols_state;   /* 0 not read, 1 being read, 2 read (or there is no map) */
static int symbols_found;
static int load_symbols(void) {
    if (InterlockedCompareExchange(&symbols_state, 1, 0) != 0) {
        while (symbols_state == 1) Sleep(10);
        return symbols_found;
    }
    char path[MAX_PATH];
    beside_exe(path, sizeof path, ".map");
    FILE *map = fopen(path, "r");
    if (!map) { symbols_state = 2; return 0; }
    U32 cap = 1 << 18, n = 0;
    size_t pool_used = 0, pool_cap = 1 << 22;
    U64 *va_of = malloc(sizeof *va_of * cap);
    U32 *name_of = malloc(sizeof *name_of * cap);
    char *pool = malloc(pool_cap);
    char line[512];
    while (fgets(line, sizeof line, map)) {   /* " 0001:00001234  f_7100000570  0000000140002234 f  g0000.obj" */
        unsigned sect, off;
        char name[256];
        U64 va;
        if (strncmp(line, " 0001:", 6) || sscanf(line, " %x:%x %255s %llx", &sect, &off, name, &va) != 4) continue;
        if (n && va < va_of[n - 1]) continue;
        if (n == cap) {
            cap *= 2;
            va_of = realloc(va_of, sizeof *va_of * cap);
            name_of = realloc(name_of, sizeof *name_of * cap);
        }
        size_t len = strlen(name) + 1;
        if (pool_used + len > pool_cap) { pool_cap *= 2; pool = realloc(pool, pool_cap); }
        memcpy(pool + pool_used, name, len);
        va_of[n] = va;
        name_of[n++] = (U32)pool_used;
        pool_used += len;
    }
    fclose(map);
    sym_va = va_of; sym_name = name_of; sym_pool = pool;
    __atomic_store_n(&nsyms, n, __ATOMIC_RELEASE);
    symbols_found = 1;
    symbols_state = 2;
    return 1;
}

/* The program's function at rip: an offset into sym_pool + 1, or 0. */
static U32 symbol_of(U64 rip) {
    U64 base = modules[exe_module].base;
    U32 count = __atomic_load_n(&nsyms, __ATOMIC_ACQUIRE);
    if (!count || rip < base || rip >= base + modules[exe_module].size) return 0;
    U64 va = rip - base + 0x140000000ull;   /* (the map's addresses are at the preferred base) */
    if (va < sym_va[0]) return 0;
    U32 lo = 0, hi = count;
    while (hi - lo > 1) {
        U32 mid = (lo + hi) / 2;
        if (sym_va[mid] <= va) lo = mid; else hi = mid;
    }
    return sym_name[lo] + 1;
}

static int is_wait_module(int m) {   /* system-call stubs: a thread here is (almost always) blocked */
    if (m < 0) return 0;
    return !_stricmp(modules[m].name, "ntdll.dll") || !_stricmp(modules[m].name, "win32u.dll");
}

static ThreadStat *thread_stat(DWORD tid) {
    for (U32 i = 0; i < nthreads; i++) if (threads[i].tid == tid) return &threads[i];
    if (nthreads == 512) return NULL;
    threads[nthreads] = (ThreadStat){.tid = tid};
    return &threads[nthreads++];
}

/* ---- the report -------------------------------------------------------------------- */
typedef struct Entry { const char *name; U64 rip; U32 count; } Entry;
static int by_count(const void *a, const void *b) { return (int)((const Entry *)b)->count - (int)((const Entry *)a)->count; }
static int by_rip(const void *a, const void *b) {
    U64 x = ((const Sample *)a)->rip, y = ((const Sample *)b)->rip;
    return x < y ? -1 : x > y;
}

/* The busiest functions among samples s (sorted by address and named), of one thread or (thread
 * -1) all; e is room for n entries. */
static void busiest(FILE *f, const Sample *s, U32 n, const char *pool, Entry *e, int thread, U64 busy, U32 limit) {
    U32 ne = 0;
    for (U32 i = 0; i < n; i++) {
        int m = module_of(s[i].rip);
        if (is_wait_module(m) || (thread >= 0 && s[i].thread != (U32)thread)) continue;
        const char *name = s[i].name ? pool + s[i].name - 1 : NULL;
        /* merge same-named entries (a function's samples are adjacent once sorted by address) */
        if (name && ne && e[ne - 1].name && !strcmp(e[ne - 1].name, name)) { e[ne - 1].count += s[i].count; continue; }
        if (!name && ne && !e[ne - 1].name && e[ne - 1].rip == s[i].rip) { e[ne - 1].count += s[i].count; continue; }
        e[ne++] = (Entry){name, s[i].rip, s[i].count};
    }
    qsort(e, ne, sizeof *e, by_count);
    for (U32 i = 0; i < ne && i < limit; i++) {
        int m = module_of(e[i].rip);
        if (e[i].name) fprintf(f, "  %5.2f%%  %s\n", 100.0 * e[i].count / (busy ? busy : 1), e[i].name);
        else fprintf(f, "  %5.2f%%  %s+%#llx\n", 100.0 * e[i].count / (busy ? busy : 1), m >= 0 ? modules[m].name : "?",
                     m >= 0 ? e[i].rip - modules[m].base : e[i].rip);
    }
}

/* ---- call paths ------------------------------------------------------------------------ */
typedef struct Count { U32 key[3], count; } Count;
static int count_by_key(const void *a, const void *b) { return memcmp(((const Count *)a)->key, ((const Count *)b)->key, sizeof ((const Count *)a)->key); }
static int count_by_count(const void *a, const void *b) {
    U32 x = ((const Count *)a)->count, y = ((const Count *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* A frame's function: the program's own by name, any other by its module (all of a module's
 * functions as one). */
static U32 frame_key(U64 pc) {
    U32 s = symbol_of(pc);
    if (s) return s;
    int m = module_of(pc);
    return m < 0 ? 0xFFFFFFFFu : 0x80000000u | (U32)m;
}
static const char *key_name(U32 key) {
    if (key == 0xFFFFFFFFu) return "?";
    if (key & 0x80000000u) return modules[key & 0x7FFFFFFFu].name;
    return sym_pool + key - 1;
}

/* Sort counts by key, merge equal keys, then sort by count: the number left. */
static U32 tally(Count *c, U32 n) {
    if (!n) return 0;
    qsort(c, n, sizeof *c, count_by_key);
    U32 out = 0;
    for (U32 i = 0; i < n; i++) {
        if (out && !count_by_key(&c[out - 1], &c[i])) c[out - 1].count += c[i].count;
        else c[out++] = c[i];
    }
    qsort(c, out, sizeof *c, count_by_count);
    return out;
}

/* For the busiest thread: the functions it spends its time in or under (which shows what
 * its time goes to as a whole), and how it reaches the functions it spends the most in. */
static void call_paths(FILE *f) {
    U32 t = 0, t_busy = 0, n = 0;
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples - threads[i].waiting > t_busy) { t = i; t_busy = threads[i].samples - threads[i].waiting; }
    U32 nwait = 0;
    for (U32 i = 0; i < nstacks; i++) { n += stacks[i].thread == t && !stacks[i].waiting; nwait += stacks[i].thread == t && stacks[i].waiting; }
    if (!n) return;
    Count *c = malloc(sizeof *c * (size_t)(n + nwait) * STACK_DEPTH);
    U32 nc = 0;
    for (U32 i = 0; i < nstacks; i++) {
        if (stacks[i].thread != t || stacks[i].waiting) continue;
        U32 keys[STACK_DEPTH], nk = 0;
        for (U32 d = 0; d < stacks[i].depth; d++) {   /* each function once per sample (recursion) */
            U32 k = frame_key(stacks[i].pc[d]), seen = 0;
            for (U32 j = 0; j < nk && !seen; j++) seen = keys[j] == k;
            if (!seen) { keys[nk++] = k; c[nc++] = (Count){{k, 0, 0}, 1}; }
        }
    }
    nc = tally(c, nc);
    fprintf(f, "\nThread %lu %ls: time in or under each function (%% of %u sampled stacks, at most %u frames deep):\n",
            threads[t].tid, threads[t].desc, n, STACK_DEPTH);
    for (U32 i = 0; i < nc && i < 60; i++) fprintf(f, "  %5.1f%%  %s\n", 100.0 * c[i].count / n, key_name(c[i].key[0]));

    /* its busiest functions, and the three callers above each */
    nc = 0;
    for (U32 i = 0; i < nstacks; i++)
        if (stacks[i].thread == t && !stacks[i].waiting) c[nc++] = (Count){{frame_key(stacks[i].pc[0]), 0, 0}, 1};
    nc = tally(c, nc);
    U32 top = nc < 12 ? nc : 12;
    Count *callee = malloc(sizeof *callee * (top ? top : 1)), *paths = c + nc;
    memcpy(callee, c, sizeof *callee * top);
    fprintf(f, "\nThread %lu: how it reaches its busiest functions (%% of that function's samples):\n", threads[t].tid);
    for (U32 j = 0; j < top; j++) {
        U32 np = 0;
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != t || k->waiting || frame_key(k->pc[0]) != callee[j].key[0]) continue;
            Count p = {{0, 0, 0}, 1};
            for (U32 d = 1; d < 4 && d < k->depth; d++) p.key[d - 1] = frame_key(k->pc[d]);
            paths[np++] = p;
        }
        np = tally(paths, np);
        fprintf(f, "  %s (%.1f%% of the thread):\n", key_name(callee[j].key[0]), 100.0 * callee[j].count / n);
        for (U32 i = 0; i < np && i < 5; i++) {
            fprintf(f, "    %5.1f%%  <-", 100.0 * paths[i].count / callee[j].count);
            for (U32 d = 0; d < 3 && paths[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(paths[i].key[d]));
            fprintf(f, "\n");
        }
    }

    /* the guest main thread's waits: from the first frame above the system-call stubs and
     * system libraries, what it was waiting in */
    for (U32 m = 0; m < nthreads; m++) {
        if (!wcsstr(threads[m].desc, L"main thread")) continue;
        U32 nw = 0;
        Count *w = malloc(sizeof *w * (nstacks ? nstacks : 1));
        for (U32 i = 0; i < nstacks; i++) {
            const Stack *k = &stacks[i];
            if (k->thread != m || !k->waiting) continue;
            U32 d = 0;
            while (d < k->depth) {
                int mod = module_of(k->pc[d]);
                if (mod >= 0 && !is_wait_module(mod) && _stricmp(modules[mod].name, "KERNELBASE.dll") && _stricmp(modules[mod].name, "KERNEL32.DLL")) break;
                d++;
            }
            Count p = {{0, 0, 0}, 1};
            for (U32 j = 0; j < 3 && d + j < k->depth; j++) p.key[j] = frame_key(k->pc[d + j]);
            if (!p.key[0]) p.key[0] = 0xFFFFFFFFu;
            w[nw++] = p;
        }
        nw = tally(w, nw);
        fprintf(f, "\nThread %lu %ls: where it waits (%u of its samples were waiting; %% of those with stacks):\n",
                threads[m].tid, threads[m].desc, threads[m].waiting);
        U32 sum = 0;
        for (U32 i = 0; i < nw; i++) sum += w[i].count;
        for (U32 i = 0; i < nw && i < 15; i++) {
            fprintf(f, "  %5.1f%%  in", 100.0 * w[i].count / (sum ? sum : 1));
            for (U32 d = 0; d < 3 && w[i].key[d]; d++) fprintf(f, " %s%s", d ? "<- " : "", key_name(w[i].key[d]));
            fprintf(f, "\n");
        }
        free(w);
    }
    free(callee);
    free(c);
}

static void report(U64 total, U64 busy) {
    char path[MAX_PATH], name[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(name, sizeof name, "profile_%04u%02u%02u_%02u%02u%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    beside_exe(path, sizeof path, name);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[profile] cannot write %s (errno %d)\n", path, errno); f = stderr; }
    fprintf(f, "Sampling profile: %.0f s, %llu samples (%llu busy, %llu in system calls - mostly waiting)\n\n", duration, total, busy, total - busy);

    fprintf(f, "Threads (samples, busy %%):\n");
    for (U32 i = 0; i < nthreads; i++)
        if (threads[i].samples > threads[i].waiting)
            fprintf(f, "  %6lu %-32ls %6u samples, %5.1f%% busy\n", threads[i].tid, threads[i].desc[0] ? threads[i].desc : L"",
                    threads[i].samples, 100.0 * (threads[i].samples - threads[i].waiting) / threads[i].samples);

    /* the samples, sorted by address */
    Sample *s = malloc(sizeof *s * (distinct ? distinct : 1));
    U32 n = 0;
    for (U32 i = 0; i < SLOTS; i++) if (table[i].rip) s[n++] = table[i];
    qsort(s, n, sizeof *s, by_rip);

    fprintf(f, "\nBusy time by module:\n");
    U64 *per_module = calloc(nmodules + 1, sizeof *per_module);
    for (U32 i = 0; i < n; i++) {
        int m = module_of(s[i].rip);
        if (!is_wait_module(m)) per_module[m < 0 ? nmodules : (U32)m] += s[i].count;
    }
    for (U32 m = 0; m <= nmodules; m++)
        if (per_module[m] * 1000 >= busy && busy)
            fprintf(f, "  %5.1f%%  %s\n", 100.0 * per_module[m] / busy, m < nmodules ? modules[m].name : "(no module: generated code?)");

    /* name the program's own samples from the map */
    if (!load_symbols()) fprintf(f, "\n(the exe's .map not found: functions unnamed)\n");
    for (U32 i = 0; i < n; i++) s[i].name = symbol_of(s[i].rip);
    const char *pool = sym_pool;

    /* busiest functions, everywhere and then in each of the busiest threads */
    Entry *e = malloc(sizeof *e * (n ? n : 1));
    fprintf(f, "\nBusiest code (%% of busy samples):\n");
    busiest(f, s, n, pool, e, -1, busy, 80);
    for (int round = 0; round < 4; round++) {
        U32 best = 0, best_busy = 0;   /* the next busiest thread not yet reported */
        for (U32 i = 0; i < nthreads; i++) {
            U32 b = threads[i].samples - threads[i].waiting;
            if (!threads[i].reported && b > best_busy) { best = i; best_busy = b; }
        }
        if (best_busy * 50 < busy) break;
        threads[best].reported = 1;
        fprintf(f, "\nThread %lu %ls (%.1f%% busy) - its busiest code (%% of its busy samples):\n", threads[best].tid,
                threads[best].desc, 100.0 * best_busy / threads[best].samples);
        busiest(f, s, n, pool, e, (int)best, best_busy, 25);
    }
    call_paths(f);
    if (f != stderr) {
        fclose(f);
        fprintf(stderr, "[profile] written to %s\n", path);
    }
    free(e); free(s); free(per_module);
}

/* ---- sampling ------------------------------------------------------------------------ */
static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    list_modules();
    memset(table, 0, sizeof *table * SLOTS);
    distinct = 0;
    nstacks = 0;
    nthreads = 0;
    HANDLE handles[512];
    DWORD tids[512];
    U32 nh = 0;
    U64 total = 0, busy = 0;
    LARGE_INTEGER freq, t0, now, refreshed = {0};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    timeBeginPeriod(1);
    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (t >= duration) break;
        if (!nh || (double)(now.QuadPart - refreshed.QuadPart) / (double)freq.QuadPart > 0.25) {   /* threads come and go */
            for (U32 i = 0; i < nh; i++) CloseHandle(handles[i]);
            nh = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok && nh < 512; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (h) { handles[nh] = h; tids[nh++] = te.th32ThreadID; }
            }
            CloseHandle(snap);
            refreshed = now;
        }
        for (U32 i = 0; i < nh; i++) {
            if (SuspendThread(handles[i]) == (DWORD)-1) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            BOOL got = GetThreadContext(handles[i], &ctx);
            ThreadStat *ts = got ? thread_stat(tids[i]) : NULL;
            int waiting = got && is_wait_module(module_of(ctx.Rip));
            U64 rip = ctx.Rip;
            if (got && ts && (!waiting || wcsstr(ts->desc, L"main thread")) && nstacks < MAX_STACKS) {   /* (while it is still suspended) */
                Stack *k = &stacks[nstacks++];
                k->thread = (U32)(ts - threads);
                k->waiting = waiting;
                k->depth = walk(&ctx, k->pc);
            }
            ResumeThread(handles[i]);
            if (!got) continue;
            total++;
            add(rip, ts ? (U32)(ts - threads) : 511);
            busy += !waiting;
            if (ts) {
                ts->samples++;
                ts->waiting += waiting;
                if (!ts->desc[0]) {
                    PWSTR d = NULL;
                    if (SUCCEEDED(GetThreadDescription(handles[i], &d)) && d) { _snwprintf(ts->desc, 47, L"%ls", d); LocalFree(d); }
                }
            }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    for (U32 i = 0; i < nh; i++) CloseHandle(handles[i]);
    report(total, busy);
    InterlockedExchange(&running, 0);
    return 0;
}

void profile_start(double seconds) {
    if (InterlockedCompareExchange(&running, 1, 0)) return;   /* one at a time */
    if (!table) table = malloc(sizeof *table * SLOTS);
    if (!stacks) stacks = malloc(sizeof *stacks * MAX_STACKS);
    duration = seconds;
    fprintf(stderr, "[profile] sampling for %.0f s\n", seconds);
    CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
}

/* ---- thread dumps, for freeze reports --------------------------------------------------- */
const char *rt_describe_host(unsigned long long pc, char *buf, size_t n);   /* host.c: a recompiled function's guest place */

static U64 filetime(FILETIME t) { return (U64)t.dwHighDateTime << 32 | t.dwLowDateTime; }

/* A code address for a report: the guest function of recompiled code, else the program's own
 * function (once the map is read: by F6, or the hitch log at start) or module + offset
 * (tools/dev/hostsym.py names those). */
static void describe_pc(U64 pc, char *buf, size_t n) {
    char guest[96];
    U32 sym = symbol_of(pc);
    int m = module_of(pc);
    if (rt_describe_host(pc, guest, sizeof guest)) snprintf(buf, n, "%s", guest);
    else if (sym) snprintf(buf, n, "%s", sym_pool + sym - 1);
    else if (m >= 0) snprintf(buf, n, "%s+%#llx", modules[m].name, pc - modules[m].base);
    else snprintf(buf, n, "%#llx", pc);
}

/* Every thread's stack but the caller's, a line at a time to line. Each thread is suspended only
 * while its stack is read; everything else (names, descriptions) after all run again. */
int profile_dump_threads(void (*line)(const char *text)) {
    if (InterlockedCompareExchange(&running, 1, 0)) { line("  (the profiler is running: no stacks)\n"); return 0; }
    static struct { DWORD tid; U32 depth; U64 pc[STACK_DEPTH], cpu_ms; HANDLE h; } t[256];
    U32 n = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    list_modules();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && n < 256; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        t[n].tid = te.th32ThreadID;
        t[n].h = h;
        t[n].depth = 0;
        FILETIME c, e, k, u;
        t[n].cpu_ms = GetThreadTimes(h, &c, &e, &k, &u) ? (filetime(k) + filetime(u)) / 10000 : 0;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &ctx)) t[n].depth = walk(&ctx, t[n].pc);
            ResumeThread(h);
        }
        n++;
    }
    CloseHandle(snap);
    char text[400], where[160];
    for (U32 i = 0; i < n; i++) {
        WCHAR desc[64] = L"";
        PWSTR d = NULL;
        if (SUCCEEDED(GetThreadDescription(t[i].h, &d)) && d) { _snwprintf(desc, 63, L"%ls", d); LocalFree(d); }
        snprintf(text, sizeof text, "thread %lu %ls (CPU %.1f s)%s\n", t[i].tid, desc, t[i].cpu_ms / 1000.0, t[i].depth ? "" : ": no stack");
        line(text);
        for (U32 f = 0; f < t[i].depth; f++) {
            describe_pc(t[i].pc[f], where, sizeof where);
            snprintf(text, sizeof text, "    %s\n", where);
            line(text);
        }
        CloseHandle(t[i].h);
    }
    InterlockedExchange(&running, 0);
    return 1;
}

/* ---- the hitch log (SSBU_HITCH=<ms>) ------------------------------------------------------
 * A sampler wakes about every millisecond and notes where three threads are: the guest's main
 * thread, the vsync thread (it presents), and the thread that last entered the renderer. At each
 * present the samples since the last one are the frame's: a frame over the threshold is logged
 * with the functions each thread spent it in (a sample is about a millisecond), the process's
 * page faults and the whole system's CPU use during it (to tell other programs' load from ours);
 * a table of the causes so far every minute. Nothing runs without SSBU_HITCH. */
DWORD gfx_last_enter_thread(void);   /* gpu_d3d11.c */

enum { ROLE_MAIN, ROLE_VSYNC, ROLE_RENDER, ROLES };
static const char *const ROLE_NAMES[ROLES] = {"main", "vsync", "renderer"};
typedef struct HitchSample { U64 key, via; unsigned char role, waiting; } HitchSample;   /* via: see hitch_sample */
#define HITCH_RING 8192
static HitchSample hitch_ring[HITCH_RING];
static U32 hitch_head, hitch_frame_start;
static SRWLOCK hitch_lock = SRWLOCK_INIT;
static int hitch_ms = -1;
typedef struct HitchCause { U64 key, via; U32 role, frames; double ms; } HitchCause;
static HitchCause hitch_causes[512];
static U32 nhitch_causes, hitch_count;

static int is_system_module(int m) {
    return is_wait_module(m) || (m >= 0 && (!_stricmp(modules[m].name, "KERNELBASE.dll") || !_stricmp(modules[m].name, "KERNEL32.DLL")));
}

/* The sample's key: the function (its start) of the innermost frame outside system code; and
 * when that is a driver's or library's (a wait in the graphics driver), via: the program's own
 * function that called into it. */
static void hitch_sample(HANDLE h, int role) {
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    U64 pc[STACK_DEPTH];
    U32 depth = GetThreadContext(h, &ctx) ? walk(&ctx, pc) : 0;
    ResumeThread(h);
    if (!depth) return;
    HitchSample s = {0, 0, (unsigned char)role, (unsigned char)is_wait_module(module_of(pc[0]))};
    U32 i = 0;
    for (; i < depth; i++) {
        int m = module_of(pc[i]);
        if (is_system_module(m)) continue;
        RUNTIME_FUNCTION *rf = function_entry(m, pc[i]);
        s.key = rf ? modules[m].base + rf->BeginAddress : pc[i];
        break;
    }
    if (i < depth && module_of(pc[i]) != (int)exe_module)
        for (U32 j = i + 1; j < depth; j++)
            if (module_of(pc[j]) == (int)exe_module) {
                RUNTIME_FUNCTION *rf = function_entry((int)exe_module, pc[j]);
                s.via = rf ? modules[exe_module].base + rf->BeginAddress : pc[j];
                break;
            }
    AcquireSRWLockExclusive(&hitch_lock);
    hitch_ring[hitch_head++ % HITCH_RING] = s;
    ReleaseSRWLockExclusive(&hitch_lock);
}

static DWORD WINAPI hitch_sampler(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"hitch sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    list_modules();
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    HANDLE main_h = NULL, vsync_h = NULL, render_h = NULL;
    DWORD render_tid = 0, pid = GetCurrentProcessId();
    for (U32 tick = 0;; tick++) {
        if ((!main_h || !vsync_h) && tick % 500 == 0) {   /* find the two named threads */
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                PWSTR d = NULL;
                int keep = 0;
                if (h && SUCCEEDED(GetThreadDescription(h, &d)) && d) {
                    if (!main_h && !wcscmp(d, L"guest main thread")) { main_h = h; keep = 1; }
                    else if (!vsync_h && !wcscmp(d, L"vsync")) { vsync_h = h; keep = 1; }
                    LocalFree(d);
                }
                if (h && !keep) CloseHandle(h);
            }
            CloseHandle(snap);
        }
        DWORD r = gfx_last_enter_thread();
        if (r && r != render_tid) {
            if (render_h) CloseHandle(render_h);
            render_tid = r;
            render_h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, r);
        }
        if (main_h) hitch_sample(main_h, ROLE_MAIN);
        if (vsync_h) hitch_sample(vsync_h, ROLE_VSYNC);
        if (render_h && render_tid != GetThreadId(main_h) && render_tid != GetThreadId(vsync_h)) hitch_sample(render_h, ROLE_RENDER);
        LARGE_INTEGER due = {.QuadPart = -10000};
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 20);
        else Sleep(1);
    }
}

static void hitch_add_cause(U64 key, U64 via, U32 role, double ms) {
    for (U32 i = 0; i < nhitch_causes; i++)
        if (hitch_causes[i].key == key && hitch_causes[i].via == via && hitch_causes[i].role == role) { hitch_causes[i].frames++; hitch_causes[i].ms += ms; return; }
    if (nhitch_causes < 512) hitch_causes[nhitch_causes++] = (HitchCause){key, via, role, 1, ms};
}

/* "function", or "driver function via the program's function" */
static void describe_hitch(U64 key, U64 via, char *buf, size_t n) {
    describe_pc(key, buf, n);
    if (via) {
        size_t len = strlen(buf);
        if (len + 6 < n) { memcpy(buf + len, " via ", 5); describe_pc(via, buf + len + 5, n - len - 5); }
    }
}

/* The map read in the background (seconds for a big one), so the log names the program's functions. */
static DWORD WINAPI symbols_main(void *unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ULONGLONG t0 = GetTickCount64();
    if (!load_symbols()) fprintf(stderr, "[hitch] the exe's .map not found: the program's functions unnamed\n");
    else fprintf(stderr, "[hitch] the program's functions named from here on (the map took %.1f s to read)\n", (GetTickCount64() - t0) / 1000.0);
    return 0;
}

static int by_cause_ms(const void *a, const void *b) {
    double x = ((const HitchCause *)a)->ms, y = ((const HitchCause *)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* (gpu_d3d11.c, at each present) */
void hitch_present(void) {
    if (hitch_ms < 0) {
        const char *v = getenv("SSBU_HITCH");
        hitch_ms = v ? atoi(v) : 0;
        if (hitch_ms > 0) {
            fprintf(stderr, "[hitch] logging frames over %d ms\n", hitch_ms);
            CloseHandle(CreateThread(NULL, 0, symbols_main, NULL, 0, NULL));
            CloseHandle(CreateThread(NULL, 0, hitch_sampler, NULL, 0, NULL));
        }
    }
    if (hitch_ms <= 0) return;
    static LARGE_INTEGER freq, last;
    static U64 last_faults, last_idle, last_total, frame, last_table;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    double ms = last.QuadPart ? (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart : 0;
    last = now;
    frame++;
    PROCESS_MEMORY_COUNTERS pmc = {sizeof pmc};
    U64 faults = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PageFaultCount : 0;
    FILETIME fi, fk, fu;
    U64 idle = 0, total = 0;
    if (GetSystemTimes(&fi, &fk, &fu)) { idle = filetime(fi); total = filetime(fk) + filetime(fu); }   /* (kernel time includes idle) */
    AcquireSRWLockExclusive(&hitch_lock);
    U32 from = hitch_frame_start, to = hitch_head;
    hitch_frame_start = hitch_head;
    if (ms > hitch_ms && to - from > 0) {
        if (to - from > HITCH_RING) from = to - HITCH_RING;
        /* per role, the most sampled functions of the frame */
        struct { U64 key, via; U32 n, waiting; } top[ROLES][64];
        U32 ntop[ROLES] = {0}, total_role[ROLES] = {0};
        for (U32 i = from; i != to; i++) {
            const HitchSample *s = &hitch_ring[i % HITCH_RING];
            total_role[s->role]++;
            U32 j = 0;
            for (; j < ntop[s->role] && (top[s->role][j].key != s->key || top[s->role][j].via != s->via); j++) {}
            if (j == ntop[s->role]) {
                if (j == 64) continue;
                top[s->role][j].key = s->key; top[s->role][j].via = s->via; top[s->role][j].n = top[s->role][j].waiting = 0; ntop[s->role]++;
            }
            top[s->role][j].n++;
            top[s->role][j].waiting += s->waiting;
        }
        char text[2400], where[320];
        int len = snprintf(text, sizeof text, "[hitch] frame %llu: %.1f ms; page faults +%llu; system CPU %.0f%%", frame, ms,
                           faults - last_faults, total > last_total ? 100.0 * (1.0 - (double)(idle - last_idle) / (double)(total - last_total)) : 0.0);

        for (int r = 0; r < ROLES; r++) {
            if (!total_role[r]) continue;
            len += snprintf(text + len, sizeof text - len, "\n[hitch]   %s:", ROLE_NAMES[r]);
            for (int k = 0; k < 3; k++) {   /* the three biggest */
                U32 best = 0, bn = 0;
                for (U32 j = 0; j < ntop[r]; j++) if (top[r][j].n > bn) { bn = top[r][j].n; best = j; }
                if (!bn) break;
                describe_hitch(top[r][best].key, top[r][best].via, where, sizeof where);
                double est = ms * bn / total_role[r];   /* the role's samples cover the frame */
                len += snprintf(text + len, sizeof text - len, " %.1f ms %s%s;", est, where, top[r][best].waiting * 2 > top[r][best].n ? " (waiting)" : "");
                if (k == 0) hitch_add_cause(top[r][best].key, top[r][best].via, (U32)r, est);
                top[r][best].n = 0;
            }
        }
        hitch_count++;
        fprintf(stderr, "%s\n", text);
    }
    ReleaseSRWLockExclusive(&hitch_lock);
    last_faults = faults; last_idle = idle; last_total = total;
    if (hitch_count && frame - last_table >= 3600 && nhitch_causes) {   /* every minute: the causes so far */
        last_table = frame;
        qsort(hitch_causes, nhitch_causes, sizeof *hitch_causes, by_cause_ms);
        fprintf(stderr, "[hitch] %u frames over %d ms so far; the largest part of each, by total time:\n", hitch_count, hitch_ms);
        char where[320];
        for (U32 i = 0; i < nhitch_causes && i < 10; i++) {
            describe_hitch(hitch_causes[i].key, hitch_causes[i].via, where, sizeof where);
            fprintf(stderr, "[hitch]   %7.1f ms in %4u frames  %s: %s\n", hitch_causes[i].ms, hitch_causes[i].frames, ROLE_NAMES[hitch_causes[i].role], where);
        }
    }
}
