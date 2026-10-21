/* Ahead-of-time shader compiler: the game's programs found in data.arc (tools/shader_aot.py
 * packs them) into the renderer's disk cache, translated and compiled exactly as the renderer
 * would, so that a program's first use loads its DXBC instead of compiling it.
 *
 * Usage: shaderaot <pack> <cache dir> <part> <parts> [--dry] [--keys <file>]
 *   Every process translates all vertex programs (their outputs are the paired pixel programs'
 *   inputs) and compiles the keys that are <part> modulo <parts> - by the key, not by where the
 *   program is in the pack (the same program is often packed more than once): each key is one
 *   part's, so the parts' counts add up to the distinct shaders. --dry translates only
 *   and prints the cache key of each program ("vs <hash>", "ps <hash> <vs outputs>"); --keys
 *   compiles only the keys listed in a file of such lines. On stderr, "progress <done> <of>" lines
 *   say how far it is, in keys, and a last line what it did.
 *
 * Pack: "SAOT", u32 count; then per program u32 stage (0 vertex, 1 pixel), u32 length, u32 pairs,
 * u32 pair[pairs] (the vertex programs found before it in its containers), then its bytes
 * (the header, its code, and possibly bytes after its end).
 *
 * Files are written dated 2001-01-01 (gpu_shader_cache.h): the start's prewarm leaves them out
 * until the game has used them. Programs with indirect branches are left out: their key depends
 * on the constant buffers of a draw. Build: python tools/shader_aot.py builds it. */
#define _CRT_SECURE_NO_WARNINGS
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#ifndef _WIN32   /* (Linux: windows.h is the runtime's own, for the types; the files go through POSIX) */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

#include "../runtime/gpu_shader_cache.h"

u32 shader_size(const u8 *program, u32 max);
#define MAX_SIZE 0x40000   /* (what the renderer reads of a program at most) */

static jmp_buf on_fatal;
static char fatal_text[256];
_Noreturn void rt_fatal(const char *fmt, ...) {   /* the translator gave up on a program */
    va_list a;
    va_start(a, fmt);
    vsnprintf(fatal_text, sizeof fatal_text, fmt, a);
    va_end(a);
    longjmp(on_fatal, 1);
}

static u32 zero_cbuf(void *user, u32 bank, u32 offset) { (void)user; (void)bank; (void)offset; return 0; }

typedef struct Program {
    int stage; u32 length, npairs; const u32 *pairs; const u8 *bytes; u32 size; u64 hash; int ok; u32 outputs, ntables;
    int mine;                  /* a vertex program this part compiles */
    u32 nmasks, masks[16];     /* a pixel program's keys: its vertex programs' distinct output masks */
} Program;

static char *translate(Program *p, u32 vs_outputs, ShaderInfo *info) {
    if (setjmp(on_fatal)) return NULL;
    return shader_hlsl_ex(p->bytes, p->size, p->stage, vs_outputs, info, zero_cbuf, NULL);
}

#ifdef _WIN32
static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static unsigned long process_id(void) { return GetCurrentProcessId(); }

/* A file written just now, dated as not yet used (SHADER_CACHE_UNPLAYED), then given its name. */
static void publish(const char *tmp, const char *path) {
    HANDLE h = CreateFileA(tmp, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME t = {(DWORD)SHADER_CACHE_UNPLAYED, (DWORD)(SHADER_CACHE_UNPLAYED >> 32)};
        SetFileTime(h, NULL, NULL, &t);
        CloseHandle(h);
    }
    if (!MoveFileExA(tmp, path, 0)) DeleteFileA(tmp);   /* (another process wrote it first) */
}
#else
static int exists(const char *path) { return access(path, F_OK) == 0; }
static unsigned long process_id(void) { return (unsigned long)getpid(); }

static void publish(const char *tmp, const char *path) {
    /* the same date as a Windows FILETIME gives (the renderer reads both alike) */
    struct timespec t[2] = {{0, UTIME_OMIT}, {(time_t)((SHADER_CACHE_UNPLAYED - 116444736000000000ull) / 10000000ull), 0}};
    utimensat(AT_FDCWD, tmp, t, 0);
    if (link(tmp, path) && errno != EEXIST) rename(tmp, path);   /* (a disk without links: renamed over a file another wrote) */
    unlink(tmp);                                                   /* (if another process wrote it first, its file stays) */
}
#endif

/* The distinct output masks of a pixel program's paired vertex programs (at most 16), into p->masks. */
static u32 pixel_masks(Program *p, const Program *prog, u32 total) {
    u32 n = 0;
    for (u32 k = 0; k < p->npairs && n < 16; k++) {
        const Program *v = p->pairs[k] < total ? &prog[p->pairs[k]] : NULL;
        if (!v || !v->ok) continue;
        u32 m = 0;
        for (; m < n && p->masks[m] != v->outputs; m++) {}
        if (m == n) p->masks[n++] = v->outputs;
    }
    return n;
}

static struct { u32 compiled, existing, tables, failed, translate_failed, keys; } count;

/* How far this part is, for shader_aot.py's progress bar: "progress <done> <of>" on stderr, counted
   in keys (one compile each, or a look in the cache): first the total, then every 16th and the last. */
static u32 progress_done, progress_of;
static void progress(void) {
    if (++progress_done % 16 == 0 || progress_done == progress_of) {
        fprintf(stderr, "progress %u %u\n", progress_done, progress_of);
        fflush(stderr);
    }
}

/* Compile and write one cache file, dated as not yet used. */
static void compile_to(const char *hlsl, const char *name, int stage, const char *path) {
    ID3DBlob *code, *errors;
    HRESULT hr = shader_cache_compile(hlsl, name, stage, &code, &errors);
    if (FAILED(hr)) {
        count.failed++;
        fprintf(stderr, "%s: shader compiler failed: %.300s\n", name, errors ? (const char *)errors->lpVtbl->GetBufferPointer(errors) : "?");
    } else {
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, process_id());
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fwrite(code->lpVtbl->GetBufferPointer(code), 1, code->lpVtbl->GetBufferSize(code), f);
            fclose(f);
            publish(tmp, path);
            count.compiled++;
        }
    }
    if (code) code->lpVtbl->Release(code);
    if (errors) errors->lpVtbl->Release(errors);
}

/* Sets of keys: the ones this process has seen (the same program is often packed more than
 * once), and --keys' list. */
typedef struct KeySet { u64 *slot; u32 cap; } KeySet;
static KeySet seen = {0, 1u << 18}, only = {0, 1u << 18}, counted = {0, 1u << 18};
/* (a slot holds its key, 0 marks it free: key 0 is kept as another value. Not key | 1, which made
   keys differing in bit 0 one - a pixel program's for vertex outputs 0x2 and 0x3, say - so one of
   them was never compiled, and which one depended on how the keys were split between parts) */
static u64 slot_key(u64 key) { return key ? key : 0x9E3779B97F4A7C15ull; }
static int add_key(KeySet *ks, u64 key) {   /* 1 if it was not there */
    if (!ks->slot) ks->slot = calloc(ks->cap, sizeof *ks->slot);
    u64 k = slot_key(key);
    for (u32 i = (u32)(k * 0x9E3779B97F4A7C15ull >> 47) & (ks->cap - 1);; i = (i + 1) & (ks->cap - 1)) {
        if (ks->slot[i] == k) return 0;
        if (!ks->slot[i]) { ks->slot[i] = k; return 1; }
    }
}
static int has_key(const KeySet *ks, u64 key) {
    u64 k = slot_key(key);
    for (u32 i = (u32)(k * 0x9E3779B97F4A7C15ull >> 47) & (ks->cap - 1);; i = (i + 1) & (ks->cap - 1)) {
        if (ks->slot[i] == k) return 1;
        if (!ks->slot[i]) return 0;
    }
}
static u32 part, parts;
static int ours(u64 key) { return key % parts == part; }   /* (a key is one part's) */
static int first_time(u64 key) { return ours(key) && add_key(&seen, key) && (!only.slot || has_key(&only, key)); }
/* the same, ahead of the compiling: for counting the keys to do (seen in the same order) */
static int to_count(u64 key) { return ours(key) && add_key(&counted, key) && (!only.slot || has_key(&only, key)); }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: shaderaot <pack> <cache dir> <part> <parts> [--dry]\n"); return 2; }
    const char *dir = argv[2];
    part = (u32)atoi(argv[3]);
    parts = (u32)atoi(argv[4]);
    if (!parts || part >= parts) { fprintf(stderr, "bad part %u of %u\n", part, parts); return 2; }
    int dry = 0;
    for (int a = 5; a < argc; a++) {
        if (!strcmp(argv[a], "--dry")) dry = 1;
        else if (!strcmp(argv[a], "--keys") && a + 1 < argc) {
            FILE *k = fopen(argv[++a], "r");
            char kind[4];
            unsigned long long hash;
            unsigned mask = 0;
            char line[128];
            while (k && fgets(line, sizeof line, k)) {
                int n = sscanf(line, "%3s %llx %x", kind, &hash, &mask);
                if (n >= 2 && !strcmp(kind, "vs")) add_key(&only, hash * 3);
                else if (n == 3 && !strcmp(kind, "ps")) add_key(&only, hash * 5 ^ mask);
            }
            if (k) fclose(k);
            if (!only.slot) add_key(&only, 0);   /* (an empty list: nothing) */
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    u8 *pack = malloc((size_t)n);
    if (!pack || fread(pack, 1, (size_t)n, f) != (size_t)n || memcmp(pack, "SAOT", 4)) { fprintf(stderr, "bad pack\n"); return 1; }
    fclose(f);
    u32 total;
    memcpy(&total, pack + 4, 4);
    Program *prog = calloc(total, sizeof *prog);
    const u8 *q = pack + 8;
    for (u32 i = 0; i < total; i++) {
        u32 w[3];
        memcpy(w, q, 12);
        prog[i].stage = w[0] ? SHADER_PIXEL : SHADER_VERTEX;
        prog[i].length = w[1];
        prog[i].npairs = w[2];
        prog[i].pairs = (const u32 *)(q + 12);
        prog[i].bytes = q + 12 + 4ull * w[2];
        q = prog[i].bytes + w[1];
    }
    ShaderInfo info;
    char path[1200], name[64];
    /* every vertex program: its key and outputs (what the pixel programs paired with it read);
       which of them are this part's to compile */
    for (u32 i = 0; i < total; i++) {
        Program *p = &prog[i];
        if (p->stage != SHADER_VERTEX || p->length <= 0x50) continue;
        if (setjmp(on_fatal)) { count.translate_failed += part == 0; continue; }   /* (every part translates them: counted once) */
        p->size = shader_size(p->bytes, p->length);
        if (p->size <= 0x50 || p->size >= MAX_SIZE) continue;   /* (no end found: not a program) */
        p->hash = shader_cache_hash(shader_program_hash(p->bytes, p->size), p->bytes, p->size, SHADER_VERTEX);
        char *hlsl = translate(p, 0, &info);
        if (!hlsl) { count.translate_failed += part == 0; continue; }
        free(hlsl);
        p->ok = 1;
        p->outputs = info.outputs;
        p->ntables = info.ntables;
        p->mine = first_time(p->hash * 3);
        progress_of += p->mine;
    }
    /* the pixel programs' keys, this part's counted: one per distinct output mask of their vertex
       programs (or, with none, what the program reads) */
    for (u32 i = 0; i < total; i++) {
        Program *p = &prog[i];
        if (p->stage != SHADER_PIXEL || p->length <= 0x50) continue;
        if (setjmp(on_fatal)) { count.translate_failed += part == 0; continue; }
        p->size = shader_size(p->bytes, p->length);
        if (p->size <= 0x50 || p->size >= MAX_SIZE) continue;   /* (no end found: not a program) */
        p->hash = shader_cache_hash(shader_program_hash(p->bytes, p->size), p->bytes, p->size, SHADER_PIXEL);
        p->nmasks = pixel_masks(p, prog, total);
        if (!p->nmasks) {   /* no vertex program known: what it reads */
            char *probe = translate(p, 0xFFFFFFFFu, &info);
            if (!probe) { count.translate_failed += ours(p->hash); p->size = 0; continue; }
            free(probe);
            p->masks[p->nmasks++] = info.inputs;
        }
        for (u32 m = 0; m < p->nmasks; m++) progress_of += to_count(p->hash * 5 ^ p->masks[m]);
    }
    fprintf(stderr, "progress 0 %u\n", progress_of);   /* (the total, before the first compile) */
    fflush(stderr);
    /* this part's keys, in the pack's order: vertex and pixel ones come mixed, so the pace stays
       even and the time left holds from the start. A pixel program: once per mask. */
    for (u32 i = 0; i < total; i++) {
        Program *p = &prog[i];
        if (p->stage == SHADER_VERTEX && p->mine) {
            progress();
            count.keys++;
            snprintf(name, sizeof name, "vs_%016llx", (unsigned long long)p->hash);
            shader_cache_file(path, sizeof path, dir, SHADER_VERTEX, p->hash, 0, 0);
            if (dry) { printf("vs %016llx\n", (unsigned long long)p->hash); continue; }
            if (p->ntables) { count.tables++; continue; }
            if (exists(path)) { count.existing++; continue; }
            char *hlsl = translate(p, 0, &info);
            if (!hlsl) { count.translate_failed++; continue; }
            compile_to(hlsl, name, SHADER_VERTEX, path);
            free(hlsl);
            continue;
        }
        if (p->stage != SHADER_PIXEL || p->size <= 0x50) continue;
        for (u32 m = 0; m < p->nmasks; m++) {
            if (!first_time(p->hash * 5 ^ p->masks[m])) continue;
            progress();
            count.keys++;
            snprintf(name, sizeof name, "ps_%016llx", (unsigned long long)p->hash);
            shader_cache_file(path, sizeof path, dir, SHADER_PIXEL, p->hash, p->masks[m], 0);
            if (dry) { printf("ps %016llx %08x\n", (unsigned long long)p->hash, p->masks[m]); continue; }
            if (exists(path)) { count.existing++; continue; }
            char *hlsl = translate(p, p->masks[m], &info);
            if (!hlsl) { count.translate_failed++; continue; }
            if (info.ntables) count.tables++;
            else compile_to(hlsl, name, SHADER_PIXEL, path);
            free(hlsl);
        }
    }
    fprintf(stderr, "progress %u %u\n", progress_done, progress_of);
    fprintf(stderr, "part %u/%u: %u keys; %u compiled, %u already cached, %u with jump tables (left out), %u fxc failures, %u not translated\n",
            part, parts, count.keys, count.compiled, count.existing, count.tables, count.failed, count.translate_failed);
    return 0;
}
