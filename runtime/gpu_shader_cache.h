/* The disk cache of compiled shaders (DXBC files), shared by the renderer (gpu_d3d11.c) and the
 * ahead-of-time compiler (tools/shaderaot.c), so that both key, name and compile a program alike.
 *
 * A file is "<dir>/v<version>_<vs|ps|cs>_<program hash>_<vs outputs>_<jump tables' hash>.dxbc".
 * The ahead-of-time compiler dates its files 2001-01-01 until the game first uses them (the
 * renderer then dates them now): the start's prewarm makes only shaders the game has used.
 *
 * The Metal renderer (macOS) keeps MSL source under the same key, named apart so that the two
 * never mix: "<dir>/m<MSL version>_<vs|ps|cs>_<...the same...>.metal" (shader_cache_file_msl),
 * dated the same way. There is no fxc there: the D3D part below is left out. */
#pragma once
#include <stdio.h>
#include <string.h>
#ifndef __APPLE__
#include <d3dcompiler.h>
#endif

#include "gpu_shader.h"

/* Bump when the translator changes what it generates: older files are then ignored. */
#define SHADER_CACHE_VERSION 10
/* MSL files' version: the translator's, and the MSL dialect's own revision (bump the last
 * digits for a change to MSL alone). */
#define SHADER_CACHE_MSL_VERSION (SHADER_CACHE_VERSION * 100 + 3)
#define SHADER_CACHE_UNPLAYED 0x01C07385C89DC000ull   /* FILETIME 2001-01-01 */
#define SHADER_CACHE_PLAYED   0x01C1925741648000ull   /* written after 2002-01-01: the game used it */
#define SHADER_CACHE_UNPLAYED_UNIX 978307200ll        /* the same two, as Unix times */
#define SHADER_CACHE_PLAYED_UNIX   1009843200ll

/* FNV-1a over a program's bytes (a vertex or pixel program: its header and code). */
static inline u64 shader_program_hash(const u8 *p, u32 size) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* The hash a program's cache files are named by: shader_program_hash's (and a compute
 * program's group size), mixed with a salt for the few programs whose translation takes the
 * render scale into account (shader_uses_pixels), and another for those with SSY/PBK stacks,
 * translated without them since (shader_uses_stacks), so that their files from before are not
 * used. Every other program keeps its files. */
#define SHADER_SCALE_SALT 0x7273636C30303031ull   /* "rscl0001" */
#define SHADER_STACK_SALT 0x7374636B30303031ull   /* "stck0001" */
static inline u64 shader_cache_hash(u64 hash, const u8 *program, u32 size, int stage) {
    if (shader_uses_pixels(program, size, stage)) hash = (hash ^ SHADER_SCALE_SALT) * 1099511628211ull;
    if (shader_uses_stacks(program, size)) hash = (hash ^ SHADER_STACK_SALT) * 1099511628211ull;
    return hash;
}

static inline void shader_cache_file(char *out, size_t n, const char *dir, int stage, u64 hash, u32 vs_outputs, u64 table_hash) {
    snprintf(out, n, "%s/v%u_%s_%016llx_%08x_%016llx.dxbc", dir, SHADER_CACHE_VERSION,
             stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)hash, vs_outputs,
             (unsigned long long)table_hash);
}

static inline void shader_cache_file_msl(char *out, size_t n, const char *dir, int stage, u64 hash, u32 vs_outputs, u64 table_hash) {
    snprintf(out, n, "%s/m%u_%s_%016llx_%08x_%016llx.metal", dir, SHADER_CACHE_MSL_VERSION,
             stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)hash, vs_outputs,
             (unsigned long long)table_hash);
}

#ifdef __APPLE__
#include <sys/stat.h>
#include <sys/time.h>

/* (macOS) A file's date: written after 2002 (the game used it), or not. */
static inline int shader_cache_played(const char *path) {
    struct stat st;
    return !stat(path, &st) && (long long)st.st_mtime >= SHADER_CACHE_PLAYED_UNIX;
}

/* A file the ahead-of-time translator wrote, now used by the game: dated now. */
static inline void shader_cache_touch(const char *path) {
    struct stat st;
    if (!stat(path, &st) && (long long)st.st_mtime < SHADER_CACHE_PLAYED_UNIX) utimes(path, NULL);
}

/* How the ahead-of-time translator dates what it writes: 2001-01-01, not yet used. */
static inline int shader_cache_unplayed(const char *path) {
    struct timeval t[2] = {{(time_t)SHADER_CACHE_UNPLAYED_UNIX, 0}, {(time_t)SHADER_CACHE_UNPLAYED_UNIX, 0}};
    return utimes(path, t);
}
#else
/* fxc as the renderer runs it: dispatch-loop programs ("npc_") without fxc's optimiser (they
 * compile much faster, and the driver optimises the bytecode anyway); any other program again
 * without it when the optimiser fails (it has internal errors on some). */
static inline HRESULT shader_cache_compile(const char *hlsl, const char *name, int stage, ID3DBlob **code, ID3DBlob **errors) {
    const char *target = stage == SHADER_VERTEX ? "vs_5_0" : stage == SHADER_PIXEL ? "ps_5_0" : "cs_5_0";
    UINT flags = strstr(hlsl, "npc_") ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1;
    *code = *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), name, NULL, NULL, "main", target, flags, 0, code, errors);
    if (FAILED(hr) && flags != D3DCOMPILE_SKIP_OPTIMIZATION) {
        if (*errors) (*errors)->lpVtbl->Release(*errors);
        *errors = NULL;
        hr = D3DCompile(hlsl, strlen(hlsl), name, NULL, NULL, "main", target, D3DCOMPILE_SKIP_OPTIMIZATION, 0, code, errors);
    }
    return hr;
}
#endif
