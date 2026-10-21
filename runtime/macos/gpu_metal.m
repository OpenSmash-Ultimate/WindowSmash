/* The rendering back end: Metal (macOS on Apple silicon).
 *
 * gpu_d3d11.c's renderer, told again for Metal: the game's render targets live on the host GPU,
 * each a texture keyed by the guest address the console would have rendered into, and the same
 * shader, texture and buffer caches, write-watch bookkeeping, statistics and debug keys stand
 * around them. When the display shows a buffer, the matching texture is drawn into the window's
 * CAMetalLayer (macos/window.m makes it).
 *
 * What Direct3D 11 orders by itself is ordered here explicitly. Work is recorded into a command
 * buffer, committed at a present, before the CPU reads anything the GPU made, and at a render
 * pass boundary once it holds many draws. A draw's targets are a render pass: a new pass begins
 * when they change, and a clear is the load action of the next pass that has its target. Every
 * byte the CPU gives the GPU goes into memory no earlier work still reads - uploads one after
 * another into chunks recycled once the GPU is done with them, a kept constant buffer changed
 * after a draw used it into a copy of its own - so each draw sees memory as it was when it was
 * issued; data the GPU writes itself (compute results) is changed on the GPU's timeline, by blits
 * between passes. Buffers are bound by GPU address (the shaders' bind tables, gpu_shader.h), so
 * they are Shared memory, made resident for each encoder that reads them. Shaders are the
 * translator's Metal Shading Language, compiled at run time by our compiler threads.
 *
 * Every entry point takes the back end's lock: GPU work arrives from the GPU thread (gpu_queue.c;
 * with SSBU_GPU_THREAD=0 the game's submitting threads), notices of unmapped memory from the
 * game's threads (hle_nv.c), frames from the display's vsync thread. Nothing here is tied to the
 * thread that calls (but the command stream's pending writes, below: kept_flush). */
#define _CRT_SECURE_NO_WARNINGS
#include "gpu_metal.h"
#include "gpu_menu.h"
#include <dirent.h>
#include <math.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/stat.h>
#include <unistd.h>

void *rt_window(void);         /* (macos/window.m) the game's window (an SDL_Window) */
void *rt_metal_layer(void);    /* and its CAMetalLayer, which frames are drawn into */
/* (what @autoreleasepool is made of: here entered and left by different functions, enter and leave) */
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);

static HWND window;
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
/* The game's picture at its own size (1920x1080 times the render scale), before it is fitted
 * into the window (screenshots read it): its bytes as the game's framebuffer has them (sRGB), read
 * plainly (final_tex) or as sRGB (final_srgb: shrinking it averages in linear light). */
static id<MTLTexture> final_tex, final_srgb;
static u32 final_w, final_h;
static id<MTLLibrary> builtin;            /* the renderer's own shaders (BUILTIN_MSL) */
static id<MTLSamplerState> linear_sampler, point_sampler;
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static int ready;

u64 gfx_stat_clears, gfx_stat_presented;

/* ---- the command stream -------------------------------------------------------------------
 * Work is recorded into one command buffer at a time (cmd, the serial cmd_serial), with one
 * encoder of it open at most: a render pass, compute or blits. Textures made meanwhile are
 * uploaded by a command buffer of their own, committed just before it (nothing earlier reads a
 * new texture), so making one ends no render pass. The GPU's completion of each is noted
 * (completed_serial): memory it read is free again then. An object given up while recorded work
 * may still use it is kept until that work is done (bury). */
typedef struct Surface Surface;
static id<MTLCommandBuffer> cmd, last_cmd;
static id<MTLCommandBuffer> upload_cmd;
static id<MTLBlitCommandEncoder> upload_blit;
static id<MTLRenderCommandEncoder> renc;
static id<MTLComputeCommandEncoder> cenc;
static id<MTLBlitCommandEncoder> benc;
static NSMutableArray *graves;            /* (cmd's: released once it is done) */
static u64 cmd_serial = 1, last_serial, enc_serial = 1;
static u64 op_seq;                        /* gfx_* calls so far (enter): memory one is handed is not recycled before it returns */
static volatile u64 completed_serial;
static u32 cmd_draws;
static u64 cmd_bytes;
u64 gfx_stat_commits;
/* The open render pass: its targets, and their size (the smallest's, on the host: render scale included). */
static struct { Surface *color[8]; id<MTLTexture> view[8]; Surface *depth; id<MTLTexture> scratch; u32 w, h; } pass;
static void forget_draw_state(void);
static NSMutableArray *gpu_times_cmds;    /* (SSBU_GPU_TIMES: the command buffers of the frame being timed) */

static u64 completed(void) { return __atomic_load_n(&completed_serial, __ATOMIC_ACQUIRE); }

static void note_completed(u64 serial) {
    u64 was = __atomic_load_n(&completed_serial, __ATOMIC_RELAXED);
    while (was < serial && !__atomic_compare_exchange_n(&completed_serial, &was, serial, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}

static id<MTLCommandBuffer> command(void) {
    if (!cmd) {
        cmd = [queue commandBuffer];
        graves = [NSMutableArray new];
        cmd_draws = 0;
        cmd_bytes = 0;
    }
    return cmd;
}

static void bury(id object) {
    if (!object) return;
    command();
    [graves addObject:object];
}

static void end_encoding(void) {
    if (renc) {
        [renc endEncoding];
        renc = nil;
        for (u32 i = 0; i < 8; i++) { pass.color[i] = NULL; pass.view[i] = nil; }
        pass.depth = NULL;
        pass.scratch = nil;
        forget_draw_state();
    }
    if (cenc) { [cenc endEncoding]; cenc = nil; }
    if (benc) { [benc endEncoding]; benc = nil; }
}

u64 gfx_stat_gpu_errors;
static void gpu_failed(id<MTLCommandBuffer> b) {   /* (on Metal's thread) a command buffer the GPU could not finish */
    if (__atomic_add_fetch(&gfx_stat_gpu_errors, 1, __ATOMIC_RELAXED) <= 8)
        fprintf(stderr, "[gfx] the GPU failed a command buffer: %s\n", b.error ? b.error.localizedDescription.UTF8String : "?");
}

/* Commit what is recorded; wait: and wait until the GPU has done all of it. */
static void submit(int wait) {
    end_encoding();
    if (upload_blit) { [upload_blit endEncoding]; upload_blit = nil; }
    if (upload_cmd) { [upload_cmd commit]; upload_cmd = nil; }
    if (cmd) {
        u64 serial = cmd_serial++;
        NSMutableArray *kept = graves;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)kept;   /* (released with the block: what the commands used) */
            note_completed(serial);
            if (b.status == MTLCommandBufferStatusError) gpu_failed(b);
        }];
        if (gpu_times_cmds) [gpu_times_cmds addObject:cmd];
        [cmd commit];
        last_cmd = cmd;
        last_serial = serial;
        cmd = nil;
        graves = nil;
        gfx_stat_commits++;
    }
    if (wait && last_cmd) {
        [last_cmd waitUntilCompleted];
        note_completed(last_serial);
    }
}

static id<MTLBlitCommandEncoder> blits(void) {
    if (!benc) {
        end_encoding();
        benc = [command() blitCommandEncoder];
        enc_serial++;
    }
    return benc;
}

static id<MTLComputeCommandEncoder> computes(void) {
    if (!cenc) {
        end_encoding();
        cenc = [command() computeCommandEncoder];
        enc_serial++;
    }
    return cenc;
}

/* (texture uploads: before the recorded work, which is committed with them) */
static id<MTLBlitCommandEncoder> uploads(void) {
    command();
    if (!upload_cmd) upload_cmd = [queue commandBuffer];
    if (!upload_blit) upload_blit = [upload_cmd blitCommandEncoder];
    return upload_blit;
}

/* ---- buffers --------------------------------------------------------------------------- */
typedef struct GBuf {
    id<MTLBuffer> buf;
    u8 *cpu;              /* its contents, as the CPU sees them */
    u64 gpu;              /* its GPU address */
    u32 size;
    u64 last_use;         /* the last command buffer (serial) that may use it */
    u64 pin;              /* the gfx_* call (op_seq) that last handed it out: not again before that is over */
    u64 gen;              /* (upload chunks: times it was started again) */
    u64 resident;         /* the encoder it was last made resident for (enc_serial), */
    u32 usage;            /* and how: 1 read, 3 read and write */
} GBuf;

/* A new buffer, in g (a struct used before, whose generation goes on) or in a struct of its own. */
static GBuf *gbuf_new_in(GBuf *g, u32 size, int tracked, int write_combined) {
    MTLResourceOptions o = MTLResourceStorageModeShared | (write_combined ? MTLResourceCPUCacheModeWriteCombined : 0) |
                           (tracked ? 0 : MTLResourceHazardTrackingModeUntracked);
    id<MTLBuffer> b = [dev newBufferWithLength:size ? size : 16 options:o];
    if (!b) return NULL;
    if (!g) g = calloc(1, sizeof *g);
    g->buf = b;
    g->cpu = b.contents;
    g->gpu = b.gpuAddress;
    g->size = size;
    g->last_use = g->pin = g->resident = 0;
    g->usage = 0;
    return g;
}

static GBuf *gbuf_new(u32 size, int tracked, int write_combined) { return gbuf_new_in(NULL, size, tracked, write_combined); }

/* Buffers given up during a gfx_* call stay whole until it returns (gbuf_release, at the
 * outermost leave): the call may have bound one already - its address in a bind table, the
 * struct in the list of buffers its encoder is to make resident (need). The MTLBuffer is kept
 * further, until the recorded work is done (bury). */
static GBuf **given_up;
static u32 ngiven_up, given_up_cap;

static void gbuf_free(GBuf *g) {
    if (!g) return;
    bury(g->buf);
    if (ngiven_up == given_up_cap) {
        given_up_cap = given_up_cap ? 2 * given_up_cap : 64;
        given_up = realloc(given_up, given_up_cap * sizeof *given_up);
    }
    given_up[ngiven_up++] = g;
}

static void gbuf_release(void) {
    for (u32 i = 0; i < ngiven_up; i++) {
        given_up[i]->buf = nil;
        free(given_up[i]);
    }
    ngiven_up = 0;
}

/* Buffers a draw or dispatch reads (and writes) by address: made resident for its encoder, just
 * before it (resident). */
#define USES_MAX 256
static GBuf *draw_bufs[2][USES_MAX];
static u32 ndraw_bufs[2];
static void need(GBuf *b, int write) {
    if (!b) return;
    u32 *n = &ndraw_bufs[write != 0];
    if (*n && draw_bufs[write != 0][*n - 1] == b) return;
    if (*n < USES_MAX) draw_bufs[write != 0][(*n)++] = b;
}

static void resident(void) {
    __unsafe_unretained id<MTLResource> list[USES_MAX];
    for (int w = 0; w < 2; w++) {
        u32 n = 0, usage = w ? 3 : 1;
        for (u32 i = 0; i < ndraw_bufs[w]; i++) {
            GBuf *b = draw_bufs[w][i];
            b->last_use = cmd_serial;
            if (b->resident == enc_serial && (b->usage & usage) == usage) continue;
            if (b->resident != enc_serial) b->usage = 0;
            b->resident = enc_serial;
            b->usage |= usage;
            list[n++] = b->buf;
        }
        ndraw_bufs[w] = 0;
        if (!n) continue;
        MTLResourceUsage u = w ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead;
        if (renc) [renc useResources:list count:n usage:u stages:MTLRenderStageVertex | MTLRenderStageFragment];
        else if (cenc) [cenc useResources:list count:n usage:u];
    }
}

/* ---- memory the CPU fills for the GPU -----------------------------------------------------
 * Uploads (constants, vertices, indices, textures' pixels) go one after another into chunks of
 * memory, a chunk started again once the command buffers that read it are done: what the
 * Direct3D renderer's rings and their DISCARD and NO_OVERWRITE maps gave it. An allocation stays
 * valid until its chunk starts again (its generation changes), so the same bytes uploaded again
 * from the same place are used where they are (and keep the chunk for this command buffer too). */
#define CHUNK (16u << 20)
#define CHUNKS_MAX 256
#define ARENA_MAX (1536ull << 20)   /* past this much, the GPU is waited for rather than more made */
typedef struct Alloc { GBuf *b; u32 off, size; u64 gen; u8 *cpu; } Alloc;
static GBuf *chunks[CHUNKS_MAX];
static u32 nchunks;
/* Chunks given up to make room (over the budget): their memory goes, their structs stay, as
 * allocations kept for reuse (upload_gpu's, index_data's) may name them still - which alive()
 * finds dead by the generation, moved on - and are the next chunks made. */
static GBuf *retired[CHUNKS_MAX];
static u32 nretired;
static GBuf *chunk;
static u32 chunk_at;
static u64 arena_total;
u64 gfx_stat_upload_bytes, gfx_stat_gpu_waits;

static Alloc arena(u32 size) {
    u32 need_ = (size + 15) & ~15u;
    if (!need_) need_ = 16;
    if (!chunk || chunk_at + need_ > chunk->size) {
        chunk = NULL;
        for (int round = 0; !chunk; round++) {
            u64 done = completed();
            for (u32 i = 0; i < nchunks && !chunk; i++)
                if (chunks[i]->size >= need_ && chunks[i]->last_use <= done && chunks[i]->pin != op_seq) chunk = chunks[i];
            if (chunk) break;
            u32 size_ = need_ > CHUNK ? (need_ + 0xFFFFF) & ~0xFFFFFu : CHUNK;
            if (nchunks < CHUNKS_MAX && arena_total + size_ <= ARENA_MAX &&
                (chunk = gbuf_new_in(nretired ? retired[nretired - 1] : NULL, size_, 0, 1))) {
                if (nretired && chunk == retired[nretired - 1]) nretired--;
                chunks[nchunks++] = chunk;
                arena_total += size_;
                break;
            }
            /* over the budget: idle chunks too small for this one make room for it; else the GPU is
             * waited for (it is far behind), which leaves all but this call's own chunks idle */
            u64 freed = 0;
            for (u32 i = 0; i < nchunks;)
                if (chunks[i]->size < need_ && chunks[i]->last_use <= done && chunks[i]->pin != op_seq) {
                    GBuf *c = chunks[i];
                    freed += c->size;
                    arena_total -= c->size;
                    bury(c->buf);
                    c->buf = nil;
                    c->cpu = NULL;
                    c->size = 0;
                    c->gen++;   /* (what was allocated in it is dead) */
                    retired[nretired++] = c;
                    chunks[i] = chunks[--nchunks];
                } else i++;
            if (freed) continue;
            if (round > 3) rt_fatal("[gfx] cannot allocate %u bytes of upload memory", size);
            gfx_stat_gpu_waits++;
            submit(1);
        }
        chunk->gen++;
        chunk_at = 0;
    }
    Alloc a = {chunk, chunk_at, need_, chunk->gen, chunk->cpu + chunk_at};
    chunk_at += need_;
    chunk->last_use = cmd_serial;
    chunk->pin = op_seq;
    cmd_bytes += need_;
    return a;
}

static int alive(const Alloc *a) {   /* still holds what was put there (then kept for this command buffer too) */
    if (!a->b || a->b->gen != a->gen) return 0;
    a->b->last_use = cmd_serial;
    a->b->pin = op_seq;
    return 1;
}

/* Buffers of one owner at a time (kept constant buffers' copies, compute's read and written
 * buffers, composed ones), by size class: one given back is handed out again once the command
 * buffers that may use it are done. [0] read-only for the GPU (untracked), [1] written by it too. */
#define POOL_CLASSES 17   /* 4 KB .. 256 MB */
typedef struct PoolClass { GBuf **q; u32 head, n, cap; } PoolClass;
static PoolClass pools[2][POOL_CLASSES];

static GBuf *pool_get(u32 size, int tracked) {
    u32 c = 0;
    while (c < POOL_CLASSES - 1 && (4096u << c) < size) c++;
    PoolClass *p = &pools[tracked][c];
    if (p->n && p->q[p->head]->last_use <= completed() && p->q[p->head]->pin != op_seq) {
        GBuf *b = p->q[p->head];
        p->head = (p->head + 1) % p->cap;
        p->n--;
        return b;
    }
    return gbuf_new(size > (4096u << c) ? size : 4096u << c, tracked, 0);
}

static void pool_put(GBuf *b, int tracked) {
    if (!b) return;
    u32 c = 0;
    while (c < POOL_CLASSES - 1 && (4096u << c) < b->size) c++;
    if ((4096u << c) != b->size) { gbuf_free(b); return; }   /* (an odd size: not kept) */
    PoolClass *p = &pools[tracked][c];
    if (p->n == p->cap) {   /* (grown in place: entries from head to the end move up) */
        u32 cap = p->cap ? p->cap * 2 : 64;
        GBuf **q = malloc(cap * sizeof *q);
        for (u32 i = 0; i < p->n; i++) q[i] = p->q[(p->head + i) % p->cap];
        free(p->q);
        p->q = q;
        p->head = 0;
        p->cap = cap;
    }
    b->last_use = cmd_serial;
    b->pin = op_seq;
    p->q[(p->head + p->n++) % p->cap] = b;
}

/* Part of a buffer, as a shader reads it: bytes [off, off + size). */
typedef struct View { GBuf *b; u64 off; u32 size; } View;
static ShaderBind bind_of(View v) { return v.b ? (ShaderBind){v.b->gpu + v.off, v.size, 0} : (ShaderBind){0, 0, 0}; }

static void copy_bytes(GBuf *src, u64 so, GBuf *dst, u64 doff, u32 n);

/* n bytes from src to dst on the GPU's timeline (after what is recorded before), clipped to both. */
static void buffer_copy(GBuf *src, u64 so, GBuf *dst, u64 doff, u32 n) {
    if (so >= src->size || doff >= dst->size) return;
    if (n > src->size - so) n = (u32)(src->size - so);
    if (n > dst->size - doff) n = (u32)(dst->size - doff);
    if (!n) return;
    src->last_use = dst->last_use = cmd_serial;
    if ((so | doff | n) & 3) { copy_bytes(src, so, dst, doff, n); return; }   /* (Metal's blits: whole words on macOS) */
    [blits() copyFromBuffer:src->buf sourceOffset:so toBuffer:dst->buf destinationOffset:doff size:n];
}

/* Bytes into a buffer the GPU may be using - writing, even: after what is recorded before, by a
 * copy (which ends a render pass), unless no recorded or running work uses the buffer, when they
 * are simply written. */
static void gpu_write(GBuf *dst, u64 off, const void *data, u32 n) {
    if (!n || off >= dst->size) return;
    if (n > dst->size - off) n = (u32)(dst->size - off);
    if (dst->last_use < cmd_serial && dst->last_use <= completed()) { memcpy(dst->cpu + off, data, n); return; }
    Alloc a = arena(n);
    memcpy(a.cpu, data, n);
    buffer_copy(a.b, a.off, dst, off, n);
}

/* ---- surfaces: render targets on the host ------------------------------------------------- */
/* Render scale (display.ini render_scale): every render target is made this many times the
 * game's size in each direction, and the draw state given in the game's pixels (viewports,
 * scissors, blits, the fragment position) is scaled to match. 1 = the game's own size. Fixed at
 * the device's start (see init; gfxtest changes it for the targets it makes next). */
static float render_scale = 1.0f;

struct Surface {
    u64 guest;
    u32 width, height, format, depth;      /* the game's size: what the surface is known and matched by */
    u32 hw, hh;                            /* the host texture's size (the game's, scaled) */
    float sx, sy;                          /* hw / width, hh / height */
    MTLPixelFormat storage, view;          /* the texture's format, and how it is rendered to and sampled */
    id<MTLTexture> tex;
    id<MTLTexture> rtv;                    /* rendered to: tex, or a view of it as `view` */
    id<MTLTexture> srv[2][2];              /* sampled: [the other reading of 8-bit colour's bytes (sRGB / plain)][as an array]; depth: [compared or not][as an array] */
    int kind, stencil;                     /* the format's kind (0 float, 1 uint, 2 sint); a stencil part too */
    u32 clear;                             /* clears still to do (the next pass's load actions): 1 colour, 2 depth, 4 stencil */
    float clear_color[4], clear_depth;
    u32 clear_stencil;
    u64 used;
};

static Surface surfaces[2048];
static u32 nsurfaces;
static u64 tick;

/* Maxwell render target formats (colour) to Metal's; *kind: 0 float, 1 uint, 2 sint. */
static MTLPixelFormat color_format(u32 f, int *kind) {
    int k = 0;
    MTLPixelFormat m = MTLPixelFormatInvalid;
    switch (f) {
    case 0xC0: case 0xC3: m = MTLPixelFormatRGBA32Float; break;
    case 0xC1: case 0xC4: m = MTLPixelFormatRGBA32Sint; k = 2; break;
    case 0xC2: case 0xC5: m = MTLPixelFormatRGBA32Uint; k = 1; break;
    case 0xC6: m = MTLPixelFormatRGBA16Unorm; break;
    case 0xC7: m = MTLPixelFormatRGBA16Snorm; break;
    case 0xC8: m = MTLPixelFormatRGBA16Sint; k = 2; break;
    case 0xC9: m = MTLPixelFormatRGBA16Uint; k = 1; break;
    case 0xCA: case 0xCE: m = MTLPixelFormatRGBA16Float; break;
    case 0xCB: m = MTLPixelFormatRG32Float; break;
    case 0xCC: m = MTLPixelFormatRG32Sint; k = 2; break;
    case 0xCD: m = MTLPixelFormatRG32Uint; k = 1; break;
    case 0xCF: case 0xE6: m = MTLPixelFormatBGRA8Unorm; break;
    case 0xD0: case 0xE7: m = MTLPixelFormatBGRA8Unorm_sRGB; break;
    case 0xD1: m = MTLPixelFormatRGB10A2Unorm; break;
    case 0xD2: m = MTLPixelFormatRGB10A2Uint; k = 1; break;
    case 0xD5: case 0xF9: m = MTLPixelFormatRGBA8Unorm; break;
    case 0xD6: case 0xFA: m = MTLPixelFormatRGBA8Unorm_sRGB; break;
    case 0xD7: m = MTLPixelFormatRGBA8Snorm; break;
    case 0xD8: m = MTLPixelFormatRGBA8Sint; k = 2; break;
    case 0xD9: m = MTLPixelFormatRGBA8Uint; k = 1; break;
    case 0xDA: m = MTLPixelFormatRG16Unorm; break;
    case 0xDB: m = MTLPixelFormatRG16Snorm; break;
    case 0xDC: m = MTLPixelFormatRG16Sint; k = 2; break;
    case 0xDD: m = MTLPixelFormatRG16Uint; k = 1; break;
    case 0xDE: m = MTLPixelFormatRG16Float; break;
    case 0xE0: m = MTLPixelFormatRG11B10Float; break;
    case 0xE3: m = MTLPixelFormatR32Sint; k = 2; break;
    case 0xE4: m = MTLPixelFormatR32Uint; k = 1; break;
    case 0xE5: m = MTLPixelFormatR32Float; break;
    case 0xE8: m = MTLPixelFormatB5G6R5Unorm; break;   /* (Metal's packed formats have DXGI's bit order: measured) */
    case 0xE9: case 0xF8: m = MTLPixelFormatBGR5A1Unorm; break;
    case 0xEA: m = MTLPixelFormatRG8Unorm; break;
    case 0xEB: m = MTLPixelFormatRG8Snorm; break;
    case 0xEC: m = MTLPixelFormatRG8Sint; k = 2; break;
    case 0xED: m = MTLPixelFormatRG8Uint; k = 1; break;
    case 0xEE: m = MTLPixelFormatR16Unorm; break;
    case 0xEF: m = MTLPixelFormatR16Snorm; break;
    case 0xF0: m = MTLPixelFormatR16Sint; k = 2; break;
    case 0xF1: m = MTLPixelFormatR16Uint; k = 1; break;
    case 0xF2: m = MTLPixelFormatR16Float; break;
    case 0xF3: m = MTLPixelFormatR8Unorm; break;
    case 0xF4: m = MTLPixelFormatR8Snorm; break;
    case 0xF5: m = MTLPixelFormatR8Sint; k = 2; break;
    case 0xF6: m = MTLPixelFormatR8Uint; k = 1; break;
    }
    if (kind) *kind = k;
    return m;
}

/* Maxwell depth formats. Apple's GPUs have no 24-bit depth: Z24S8 is 32-bit float depth with
 * stencil (no depth bias is used, so only precision differs). */
static MTLPixelFormat depth_format(u32 f, int *stencil) {
    *stencil = 0;
    switch (f) {
    case 0x0A: return MTLPixelFormatDepth32Float;                                       /* Z32_FLOAT */
    case 0x13: return MTLPixelFormatDepth16Unorm;                                       /* Z16_UNORM */
    case 0x14: case 0x15: case 0x16: *stencil = 1; return MTLPixelFormatDepth32Float_Stencil8;   /* Z24 + S8 variants */
    case 0x19: *stencil = 1; return MTLPixelFormatDepth32Float_Stencil8;                /* Z32 + S8 */
    }
    return MTLPixelFormatInvalid;
}

/* 8-bit colour is stored as plain values with views as sRGB or plain: the hardware writes and
 * reads the same bytes as sRGB or as plain values according to each use's format (render target,
 * texture header), and a surface keeps its contents across such changes. */
static MTLPixelFormat storage_of(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    default: return f;
    }
}
static int is_srgb(MTLPixelFormat f) { return f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm_sRGB; }
static MTLPixelFormat other_gamma(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm: return MTLPixelFormatRGBA8Unorm_sRGB;
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm: return MTLPixelFormatBGRA8Unorm_sRGB;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    default: return MTLPixelFormatInvalid;
    }
}

/* How shaders sample a colour surface: as floats (the translator declares every texture float).
 * 32-bit integers through a float view of the same bits, exactly what the guest reads; smaller
 * integers as normalised values (no float view has their bits: approximately). */
static MTLPixelFormat sampled_format(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: return MTLPixelFormatR32Float;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: return MTLPixelFormatRG32Float;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return MTLPixelFormatRGBA32Float;
    case MTLPixelFormatRGBA16Uint: return MTLPixelFormatRGBA16Unorm;
    case MTLPixelFormatRGBA16Sint: return MTLPixelFormatRGBA16Snorm;
    case MTLPixelFormatRG16Uint: return MTLPixelFormatRG16Unorm;
    case MTLPixelFormatRG16Sint: return MTLPixelFormatRG16Snorm;
    case MTLPixelFormatR16Uint: return MTLPixelFormatR16Unorm;
    case MTLPixelFormatR16Sint: return MTLPixelFormatR16Snorm;
    case MTLPixelFormatRGBA8Uint: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatRGBA8Sint: return MTLPixelFormatRGBA8Snorm;
    case MTLPixelFormatRG8Uint: return MTLPixelFormatRG8Unorm;
    case MTLPixelFormatRG8Sint: return MTLPixelFormatRG8Snorm;
    case MTLPixelFormatR8Uint: return MTLPixelFormatR8Unorm;
    case MTLPixelFormatR8Sint: return MTLPixelFormatR8Snorm;
    case MTLPixelFormatRGB10A2Uint: return MTLPixelFormatRGB10A2Unorm;
    default: return f;
    }
}

/* A surface's bytes read as sRGB (srgb) or as plain values, for an 8-bit colour surface (its own
 * view otherwise), as a 2D texture or a one-layer array of them. A depth surface is read as its
 * depth in red, as Direct3D's view of it (R32_FLOAT) reads - Metal's view of depth repeats it in
 * green and blue - unless compared (srgb then: 1). */
static id<MTLTexture> surface_srv(Surface *s, int srgb, int array) {
    if (!s->tex) return nil;
    MTLPixelFormat f = s->depth ? s->storage : sampled_format(s->view), other = other_gamma(s->view);
    int o = s->depth ? srgb != 0 : other != MTLPixelFormatInvalid && srgb != is_srgb(s->view);
    if (o && !s->depth) f = other;
    if (!s->srv[o][array]) {
        MTLTextureType type = array ? MTLTextureType2DArray : MTLTextureType2D;
        if (s->depth && !o) {
            MTLTextureSwizzleChannels red = {MTLTextureSwizzleRed, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne};
            s->srv[o][array] = [s->tex newTextureViewWithPixelFormat:f textureType:type levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:red];
        } else if (f == s->storage && !array) s->srv[o][array] = s->tex;
        else s->srv[o][array] = [s->tex newTextureViewWithPixelFormat:f textureType:type levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
    }
    return s->srv[o][array] ? s->srv[o][array] : s->tex;
}

static void warn_format(const char *what, u32 f) {
    static u32 seen[64], n;
    for (u32 i = 0; i < n; i++) if (seen[i] == (f | (u32)(what[0] << 24))) return;
    if (n < 64) seen[n++] = f | (u32)(what[0] << 24);
    fprintf(stderr, "[gfx] unsupported %s format %#x\n", what, f);
}

static int attached(const Surface *s) {   /* a target of the open render pass */
    if (!renc) return 0;
    if (pass.depth == s) return 1;
    for (u32 i = 0; i < 8; i++) if (pass.color[i] == s) return 1;
    return 0;
}

static void release_surface(Surface *s) {
    if (attached(s)) end_encoding();
    *s = (Surface){0};
}

static void settle(Surface *s);   /* (below: its clears done now) */

/* The views of a surface rendered as `view` (a colour format, or a depth one). */
static void make_views(Surface *s, MTLPixelFormat view) {
    s->view = view;
    s->rtv = view == s->storage ? s->tex : [s->tex newTextureViewWithPixelFormat:view];
    for (u32 o = 0; o < 2; o++) s->srv[o][0] = s->srv[o][1] = nil;
}

/* SSBU_SCALE_SKIP (testing): render targets kept at the game's size whatever the render scale,
 * to find which one a scaling artifact comes from. A list of WxH sizes, and/or "square" for the
 * power-of-two squares (shadow maps), e.g. "square,960x540". */
static int scale_skipped(u32 w, u32 h) {
    static int loaded;
    static u32 sizes[32][2], nsizes, squares;
    if (!loaded) {
        const char *e = getenv("SSBU_SCALE_SKIP");
        for (const char *p = e; p && *p;) {
            u32 a, b;
            if (!_strnicmp(p, "square", 6)) squares = 1;
            else if (sscanf(p, "%ux%u", &a, &b) == 2 && nsizes < 32) { sizes[nsizes][0] = a; sizes[nsizes][1] = b; nsizes++; }
            p = strchr(p, ',');
            if (p) p++;
        }
        loaded = 1;
    }
    if (squares && w == h && !(w & (w - 1))) return 1;
    for (u32 i = 0; i < nsizes; i++) if (sizes[i][0] == w && sizes[i][1] == h) return 1;
    return 0;
}

/* The host size of a w x h render target. It depends on the game's size only (never the format
 * or depth), so a colour target and the depth target drawn with it always match. A target too
 * big to scale stays at the game's size. */
static void scaled_size(u32 w, u32 h, u32 *hw, u32 *hh) {
    *hw = w; *hh = h;
    if (render_scale == 1.0f || scale_skipped(w, h)) return;
    float x = (float)w * render_scale + 0.5f, y = (float)h * render_scale + 0.5f;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > 16384 || y > 16384) return;
    *hw = (u32)x; *hh = (u32)y;
}

/* The host texture standing for the render target at `guest`, created on first use (cleared to
 * zeros, as Direct3D's new textures are). */
static Surface *surface(u64 guest, u32 width, u32 height, u32 format, int depth) {
    if (!guest || !width || !height || width > 16384 || height > 16384) return NULL;
    Surface *free_slot = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex) { if (!free_slot) free_slot = s; continue; }
        if (s->guest == guest && s->depth == (u32)depth) {
            if (s->width == width && s->height == height && s->format == format) { s->used = ++tick; return s; }
            int kind;
            MTLPixelFormat now = depth ? MTLPixelFormatInvalid : color_format(format, &kind);
            if (s->width == width && s->height == height && now != MTLPixelFormatInvalid && storage_of(now) == s->storage) {
                if (s->clear) settle(s);   /* (a clear still to do is of the old reading of the bytes) */
                if (attached(s)) end_encoding();
                make_views(s, now);       /* the same bytes, now sRGB or plain (the old views stay with the work using them) */
                s->format = format;
                s->used = ++tick;
                return s;
            }
            release_surface(s);   /* the memory now holds a different surface */
            if (!free_slot) free_slot = s;
        }
    }
    int kind = 0, stencil = 0;
    MTLPixelFormat view = depth ? depth_format(format, &stencil) : color_format(format, &kind);
    if (view == MTLPixelFormatInvalid) { warn_format(depth ? "depth" : "color", format); return NULL; }
    MTLPixelFormat storage = depth ? view : storage_of(view);

    Surface *s = free_slot;
    if (!s) {
        if (nsurfaces < sizeof surfaces / sizeof *surfaces) s = &surfaces[nsurfaces++];
        else {   /* evict the least recently used */
            s = &surfaces[0];
            for (u32 i = 1; i < nsurfaces; i++) if (surfaces[i].used < s->used) s = &surfaces[i];
            release_surface(s);
        }
    }
    u32 hw, hh;
    scaled_size(width, height, &hw, &hh);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:storage width:hw height:hh mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    if (!tex) return NULL;
    *s = (Surface){0};
    s->tex = tex;
    s->depth = (u32)depth;
    s->storage = storage;
    s->kind = kind;
    s->stencil = stencil;
    make_views(s, view);
    s->guest = guest; s->width = width; s->height = height; s->format = format;
    s->hw = hw; s->hh = hh;
    s->sx = (float)hw / (float)width; s->sy = (float)hh / (float)height;
    s->clear = depth ? 2 | (stencil ? 4 : 0) : 1;   /* (zeros, all of it) */
    s->used = ++tick;
    return s;
}

/* ---- render passes ------------------------------------------------------------------------
 * A draw's targets are the open render pass's, or a new pass is begun for them (the old one
 * ended: its targets kept, store). A target's clears still to do are the new pass's load
 * actions; its contents are loaded otherwise. */
typedef struct Targets { Surface *color[8]; Surface *depth; int scratch_depth; } Targets;
static id<MTLTexture> scratch_depth(u32 w, u32 h);
static void begin_pass_log(const Targets *t);

static id<MTLRenderCommandEncoder> render_pass(const Targets *t) {
    if (renc) {
        int same = pass.depth == t->depth && (pass.scratch != nil) == (t->scratch_depth != 0);
        for (u32 i = 0; i < 8 && same; i++) same = pass.color[i] == t->color[i] && (!t->color[i] || pass.view[i] == t->color[i]->rtv);
        if (same) return renc;
    }
    end_encoding();
    if (cmd_draws >= 1000) submit(0);   /* (a long run of work: the GPU starts on it now) */
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    u32 w = 16384, h = 16384;
    for (u32 i = 0; i < 8; i++) {
        Surface *s = t->color[i];
        if (!s) continue;
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[i];
        a.texture = s->rtv;
        a.loadAction = s->clear & 1 ? MTLLoadActionClear : MTLLoadActionLoad;
        a.clearColor = MTLClearColorMake(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
        a.storeAction = MTLStoreActionStore;
        s->clear = 0;
        if (s->hw < w) w = s->hw;
        if (s->hh < h) h = s->hh;
        pass.color[i] = s;
        pass.view[i] = s->rtv;
    }
    if (t->depth) {
        Surface *s = t->depth;
        rp.depthAttachment.texture = s->tex;
        rp.depthAttachment.loadAction = s->clear & 2 ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = s->clear_depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (s->stencil) {
            rp.stencilAttachment.texture = s->tex;
            rp.stencilAttachment.loadAction = s->clear & 4 ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = s->clear_stencil & 0xFF;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        s->clear = 0;
        if (s->hw < w) w = s->hw;
        if (s->hh < h) h = s->hh;
        pass.depth = s;
    }
    if (t->scratch_depth && !t->depth) {   /* (a pixel shader that writes depth needs a depth target: one that is never kept) */
        pass.scratch = scratch_depth(w, h);
        rp.depthAttachment.texture = pass.scratch;
        rp.depthAttachment.loadAction = MTLLoadActionDontCare;
        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    }
    pass.w = w;
    pass.h = h;
    renc = [command() renderCommandEncoderWithDescriptor:rp];
    enc_serial++;
    begin_pass_log(t);
    return renc;
}

/* A surface's clears still to do, done now (it is about to be read: sampled, copied, shown). */
static void settle(Surface *s) {
    if (!s || !s->clear || !s->tex) return;
    Targets t = {{0}, NULL, 0};
    if (s->depth) t.depth = s;
    else t.color[0] = s;
    render_pass(&t);
    end_encoding();
}

/* ---- window and device ------------------------------------------------------------------- */
/* Debug keys, taken at the next present: F1 the overlay, F2 its details, F3 a screenshot,
 * F4 a log of the next frame's draws and its render targets, F5 depth sampling on/off, F6 a
 * 10-second sampling profile. macos/window.m passes them on (F1-F6, Cmd+1 - Cmd+6). */
static volatile LONG hotkeys;
void gfx_hotkey(int n) { if (n >= 0 && n < 6) InterlockedOr(&hotkeys, 1 << n); }

/* Time spent in the renderer (the outermost enter to its leave), for the overlay - from
 * rt_clock_ns, as this is twice a draw (clock.h: the system timer can be slow). */
static u32 enter_depth;
static u64 enter_t0;
static void *enter_pool;
u64 gfx_stat_busy_ns, gfx_stat_composed, gfx_stat_upload_kind[4];
static struct { u32 size, bank, extent; char shader[24]; } gfx_stat_biggest_cb;

static volatile DWORD last_enter_thread;
DWORD gfx_lock_owner(void) { return ready ? lock.OwningThread : 0; }
DWORD gfx_last_enter_thread(void) { return last_enter_thread; }

static WINDOWS_BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *c);

static int enter(void) {
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!ready) return 0;
    EnterCriticalSection(&lock);
    last_enter_thread = GetCurrentThreadId();
    if (!enter_depth++) {
        op_seq++;
        enter_t0 = rt_clock_ns();
        enter_pool = objc_autoreleasePoolPush();   /* (Metal's autoreleased objects, released at the leave) */
    }
    return 1;
}

/* GPU addresses and guest memory taken away, whose cached data is not dropped yet ([lo, hi);
 * hi 0: none). Dropped as the outermost entry point leaves (below: forget_unmapped). */
typedef struct Span { u64 lo, hi; } Span;
static Span unmapped_va, unmapped_guest;
static void forget_unmapped(void);

static void leave(void) {
    if (enter_depth == 1 && (unmapped_va.hi || unmapped_guest.hi)) forget_unmapped();
    if (!--enter_depth) {
        gbuf_release();   /* (buffers given up meanwhile, forget_unmapped's too: no longer bound by anything of the call's) */
        objc_autoreleasePoolPop(enter_pool);
        gfx_stat_busy_ns += rt_clock_ns() - enter_t0;
    }
    LeaveCriticalSection(&lock);
}

static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* The options every shader is compiled with: Metal's fast math breaks the translator's NaN-aware
 * compares, and positions are invariant (the same position from different programs is the same,
 * for passes that test depth for equality). */
static MTLCompileOptions *msl_options(void) {
    static MTLCompileOptions *o;
    if (!o) {
        MTLCompileOptions *n = [MTLCompileOptions new];
        n.preserveInvariance = YES;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
        if (@available(macOS 15.0, *)) n.mathMode = MTLMathModeSafe;
        else
#endif
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            n.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        o = n;
    }
    return o;
}

/* The first error in a compiler's message (they come after its warnings). */
static const char *first_error(NSError *err) {
    const char *all = err ? err.localizedDescription.UTF8String : NULL;
    if (!all) return "?";
    const char *e = strstr(all, "error: ");
    return e ? e : all;
}

/* ---- shaders --------------------------------------------------------------------------- */
/* Translated at their first draw (on the submitting thread, well under a millisecond), then made
 * ready by compiler threads of our own: Metal compiles the MSL (its system cache remembers what
 * it compiled before, so a second run's compiles take a fraction of a millisecond) - cache loads
 * first, then new programs. A pixel program's functions are specialised, and a draw's pipeline
 * built - where the GPU code is made - by the compiler threads too, for each combination of
 * programs, render target formats and blending. A draw whose shaders or pipeline are not ready
 * is skipped. The disk cache keeps each program's MSL (gpu_shader_cache.h's names), for the
 * prewarm. Ready (state 1) is published with a release store and read with acquire loads: what
 * the compiler made is all there when the renderer sees it. */
enum { JOB_SHADER, JOB_PIPELINE };
typedef struct Job { struct Job *next_job; int kind; } Job;   /* (the start of a Shader or a Pipeline) */

typedef struct Shader {
    Job job;
    u64 hash, table_hash;
    u32 vs_outputs;
    int stage;
    volatile LONG state;   /* 0 compiling, 1 ready, 2 failed */
    id<MTLLibrary> lib;
    id<MTLFunction> fn;    /* vs_main or cs_main (ps_main is specialised for each pipeline) */
    id<MTLComputePipelineState> cs;
    char *msl;             /* while compiling */
    char name[64];
    ShaderInfo info;
    u32 written_mask, learn_runs;   /* compute: storage buffers it was seen to write, over its first runs */
    u32 local[3];          /* compute: its workgroup's size */
    int cached;            /* an earlier run compiled it (its job is a load) */
    struct Prewarm *prewarm;   /* a prewarm job (not a shader of the game yet): the entry it fills */
} Shader;

static LONG state_of(volatile LONG *s) { return __atomic_load_n(s, __ATOMIC_ACQUIRE); }
static void publish(volatile LONG *s, LONG v) { __atomic_store_n(s, v, __ATOMIC_RELEASE); }

static Shader **shader_cache;
static u32 shader_count, shader_cap;
u64 gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_pipelines;
static volatile LONG compiles_pending;

/* The compiler threads: a queue of cache loads, one of compiles. Below the game's threads in
 * priority (Metal's compiler itself runs in a service process of its own). */
#define COMPILERS_MAX 16
#define COMPILER_STACK (16ull << 20)
enum { STEP_IDLE, STEP_CACHE, STEP_COMPILE, STEP_PIPELINE };
static SRWLOCK jobs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE jobs_cv = CONDITION_VARIABLE_INIT;
/* [0] shaders compiled before and pipelines of them (fast), [1] compiles, [2] prewarm (the disk
 * cache at start; never taken by the first thread, which stays free for the game's own shaders) */
static struct { Job *head, *tail; } job_queue[3];
static struct Compiler {
    volatile DWORD thread;
    Job *volatile job;
    volatile LONG step;
    volatile u64 since_ms;
} compilers[COMPILERS_MAX];
static u32 ncompilers;

static const char *cache_dir(void) {
    static char dir[1024];
    if (!dir[0]) {   /* SSBU_SHADER_CACHE: another directory (an empty one: a first run's compiles) */
        if (getenv("SSBU_SHADER_CACHE")) snprintf(dir, sizeof dir, "%s", getenv("SSBU_SHADER_CACHE"));
        else snprintf(dir, sizeof dir, "%s/../shadercache", rt_data_dir);
        mkdir(dir, 0755);
    }
    return dir;
}

static void cache_path(const Shader *s, char *path, size_t n) {
    shader_cache_file_msl(path, n, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash);
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) text[n] = 0;
    else { free(text); text = NULL; }
    fclose(f);
    return text;
}

/* (on a compiler thread) a program's library and its entry point (a pixel program's checked by
 * a specialisation for float targets); a compute program's pipeline too. 1 ready, 2 failed. */
static LONG compile_msl(struct Compiler *me, const char *msl, int stage, const char *name, const u32 *local, id<MTLLibrary> *lib_out,
                        id<MTLFunction> *fn_out, id<MTLComputePipelineState> *cs_out) {
    me->step = STEP_COMPILE;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(msl) options:msl_options() error:&err];
    if (!lib) {
        fprintf(stderr, "[gfx] shader %s failed to compile:\n%.2000s\n", name, first_error(err));
        return 2;
    }
    id<MTLFunction> fn;
    if (stage == SHADER_PIXEL) {
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        u32 types = 0;
        [cv setConstantValue:&types type:MTLDataTypeUInt atIndex:SHADER_MSL_RT_TYPES];
        fn = [lib newFunctionWithName:@"ps_main" constantValues:cv error:&err];
    } else fn = [lib newFunctionWithName:stage == SHADER_VERTEX ? @"vs_main" : @"cs_main"];
    if (!fn) {
        fprintf(stderr, "[gfx] shader %s has no entry point: %s\n", name, first_error(err));
        return 2;
    }
    if (stage == SHADER_COMPUTE) {
        me->step = STEP_PIPELINE;
        id<MTLComputePipelineState> cs = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!cs) {   /* (more than 32 KB of shared memory, for one) */
            fprintf(stderr, "[gfx] compute shader %s: no pipeline: %s\n", name, first_error(err));
            return 2;
        }
        if (local && cs.maxTotalThreadsPerThreadgroup < (NSUInteger)local[0] * local[1] * local[2]) {
            fprintf(stderr, "[gfx] compute shader %s: %ux%ux%u threads a workgroup, the GPU takes %lu\n", name, local[0], local[1], local[2],
                    (unsigned long)cs.maxTotalThreadsPerThreadgroup);
            return 2;
        }
        *cs_out = cs;
    }
    *lib_out = lib;
    *fn_out = fn;
    return 1;
}

/* ---- prewarm: the disk cache made ready while the game starts --------------------------
 * Every MSL file of this SHADER_CACHE_MSL_VERSION the game has used before (gpu_shader_cache.h),
 * newest first, is compiled by the compiler threads (queue 2, below the game's own shaders) into
 * a table by the cache's key - which also leaves Metal's system cache warm for the pipelines.
 * When the game first uses a program, get_shader_ex still translates it (that gives its
 * interface), then takes the library from here instead of compiling it, so the draw is not
 * skipped for that. SSBU_SHADER_PREWARM=0: no prewarm. */
typedef struct Prewarm {
    u64 hash, table_hash;
    u32 vs_outputs;
    int stage;
    LONG state;
    id<MTLLibrary> lib;
    id<MTLFunction> fn;
    id<MTLComputePipelineState> cs;
} Prewarm;
enum { PW_EMPTY, PW_QUEUED, PW_LOADING, PW_READY, PW_TAKEN, PW_FAILED };
#define PREWARM_SLOTS 32768
static Prewarm *volatile prewarm_table;
static SRWLOCK prewarm_lock = SRWLOCK_INIT;
static volatile LONG prewarm_total, prewarm_done, prewarm_ready;
static u64 prewarm_t0, prewarm_bytes0;

static u64 private_bytes(void) {   /* (the process's own memory: what Activity Monitor shows) */
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? info.phys_footprint : 0;
}

/* (under prewarm_lock) the entry for a key; a new one when insert */
static Prewarm *prewarm_slot(int stage, u64 hash, u32 vs_outputs, u64 table_hash, int insert) {
    u64 k = (hash ^ table_hash * 31 ^ (u64)vs_outputs << 7 ^ (u64)stage) * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < PREWARM_SLOTS; i++) {
        Prewarm *p = &prewarm_table[((k >> 40) + i) & (PREWARM_SLOTS - 1)];
        if (p->state == PW_EMPTY) {
            if (!insert) return NULL;
            p->hash = hash; p->stage = stage; p->vs_outputs = vs_outputs; p->table_hash = table_hash;
            return p;
        }
        if (p->hash == hash && p->stage == stage && p->vs_outputs == vs_outputs && p->table_hash == table_hash) return p;
    }
    return NULL;
}

/* The game asks for a program: its library, if the prewarm made it (1); else 0, and the prewarm
 * leaves it alone from now on (the game's own compile takes over). */
static int prewarm_take(Shader *s) {
    if (!prewarm_table) return 0;
    int got = 0;
    AcquireSRWLockExclusive(&prewarm_lock);
    Prewarm *p = prewarm_slot(s->stage, s->hash, s->vs_outputs, s->table_hash, 0);
    if (p && p->state == PW_READY) {
        s->lib = p->lib; s->fn = p->fn; s->cs = p->cs;
        p->lib = nil; p->fn = nil; p->cs = nil;
        got = 1;
    }
    if (p && p->state != PW_FAILED) p->state = PW_TAKEN;
    ReleaseSRWLockExclusive(&prewarm_lock);
    return got;
}

/* (on a compiler thread) one prewarm job: the file, compiled */
static void prewarm_job(struct Compiler *me, Shader *s) {
    Prewarm *p = s->prewarm;
    AcquireSRWLockExclusive(&prewarm_lock);
    int go = p->state == PW_QUEUED;
    if (go) p->state = PW_LOADING;
    ReleaseSRWLockExclusive(&prewarm_lock);
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    id<MTLComputePipelineState> cs = nil;
    int ok = 0;
    if (go) {
        char path[1200];
        cache_path(s, path, sizeof path);
        me->step = STEP_CACHE;
        char *msl = read_text(path);
        if (msl) {
            s->msl = msl;
            ok = compile_msl(me, msl, s->stage, s->name, NULL, &lib, &fn, &cs) == 1;
            s->msl = NULL;
            free(msl);
        }
    }
    AcquireSRWLockExclusive(&prewarm_lock);
    if (go && p->state == PW_LOADING) {
        p->lib = lib; p->fn = fn; p->cs = cs;
        p->state = ok ? PW_READY : PW_FAILED;
        if (ok) prewarm_ready++;
    }
    ReleaseSRWLockExclusive(&prewarm_lock);
    if (InterlockedIncrement(&prewarm_done) == prewarm_total)
        fprintf(stderr, "[gfx] prewarm: %ld of %ld shaders ready in %.1f s; memory %.0f -> %.0f MB\n", (long)prewarm_ready, (long)prewarm_total,
                (GetTickCount64() - prewarm_t0) / 1000.0, prewarm_bytes0 / 1048576.0, private_bytes() / 1048576.0);
}

/* A file written whole or not at all (another process, or the prewarm, may read it meanwhile). */
static void write_file(const char *path, const char *text) {
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.%u.tmp", path, (unsigned)GetCurrentThreadId());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    if (fclose(f) || !ok || rename(tmp, path)) unlink(tmp);
}

/* (on a compiler thread) the shader's library: 1 ready, 2 failed. Its MSL goes to the disk cache
 * (for the next start's prewarm), or the file there is dated now (the AOT translator's files are
 * dated 2001 until the game uses them). */
static LONG make_ready(struct Compiler *me, Shader *s) {
    char path[1200];
    cache_path(s, path, sizeof path);
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    id<MTLComputePipelineState> cs = nil;
    LONG state = compile_msl(me, s->msl, s->stage, s->name, s->stage == SHADER_COMPUTE ? s->local : NULL, &lib, &fn, &cs);
    if (state != 1) return state;
    me->step = STEP_CACHE;
    if (s->cached) shader_cache_touch(path);
    else write_file(path, s->msl);
    s->lib = lib; s->fn = fn; s->cs = cs;
    return 1;
}

/* Pipelines: a draw's programs, its targets' formats and its blending, compiled together (the
 * pixel program specialised for its targets' kinds). */
typedef struct PipeKey {
    Shader *vs, *ps;
    u32 rt_types;                    /* (SHADER_MSL_RT_TYPES: 2 bits a target, and the dual-source bit) */
    u16 color[8], depth, stencil;    /* MTLPixelFormat */
    u8 mask[8];                      /* MTLColorWriteMask */
    u8 blend[8][7];                  /* enabled; colour op, source, destination; alpha op, source, destination */
} PipeKey;
typedef struct Pipeline {
    Job job;
    PipeKey key;
    volatile LONG state;             /* 0 compiling, 1 ready, 2 failed */
    id<MTLRenderPipelineState> pso;
} Pipeline;

static int source1(u8 f);

static LONG make_pipeline(struct Compiler *me, Pipeline *p) {
    me->step = STEP_PIPELINE;
    const PipeKey *k = &p->key;
    /* (what Metal's validation would abort the process for, refused here: dual_source keeps draws from it) */
    for (u32 i = 1; i < 8; i++)
        if ((k->rt_types & SHADER_MSL_DUAL_SOURCE && k->color[i]) ||
            (k->blend[i][0] && (source1(k->blend[i][2]) || source1(k->blend[i][3]) || source1(k->blend[i][5]) || source1(k->blend[i][6])))) {
            fprintf(stderr, "[gfx] pipeline %s + %s (targets %#x): dual-source blending with target %u, which Metal does not allow\n", k->vs->name,
                    k->ps ? k->ps->name : "-", k->rt_types, i);
            return 2;
        }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = k->vs->fn;   /* (no vertex descriptor: the program fetches its vertices) */
    NSError *err = nil;
    if (k->ps) {
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        u32 types = k->rt_types;
        [cv setConstantValue:&types type:MTLDataTypeUInt atIndex:SHADER_MSL_RT_TYPES];
        id<MTLFunction> ps = [k->ps->lib newFunctionWithName:@"ps_main" constantValues:cv error:&err];
        if (!ps) {
            fprintf(stderr, "[gfx] %s cannot be specialised for targets %#x: %s\n", k->ps->name, k->rt_types, first_error(err));
            return 2;
        }
        d.fragmentFunction = ps;
    }
    for (u32 i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        a.pixelFormat = (MTLPixelFormat)k->color[i];
        if (!k->color[i]) continue;
        a.writeMask = k->mask[i];
        a.blendingEnabled = k->blend[i][0] != 0;
        if (!k->blend[i][0]) continue;
        a.rgbBlendOperation = k->blend[i][1]; a.sourceRGBBlendFactor = k->blend[i][2]; a.destinationRGBBlendFactor = k->blend[i][3];
        a.alphaBlendOperation = k->blend[i][4]; a.sourceAlphaBlendFactor = k->blend[i][5]; a.destinationAlphaBlendFactor = k->blend[i][6];
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) {
        static volatile LONG reported;
        if (InterlockedIncrement(&reported) <= 64)
            fprintf(stderr, "[gfx] pipeline %s + %s (targets %#x) failed: %s\n", k->vs->name, k->ps ? k->ps->name : "-", k->rt_types, first_error(err));
        return 2;
    }
    p->pso = pso;
    return 1;
}

static DWORD WINAPI compiler_main(void *arg) {
    struct Compiler *me = arg;
    me->thread = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"shader compiler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int prewarms = me != &compilers[0];
    for (;;) @autoreleasepool {
        AcquireSRWLockExclusive(&jobs_lock);
        while (!job_queue[0].head && !job_queue[1].head && !(prewarms && job_queue[2].head))
            SleepConditionVariableSRW(&jobs_cv, &jobs_lock, INFINITE, 0);
        int q = job_queue[0].head ? 0 : job_queue[1].head ? 1 : 2;
        Job *j = job_queue[q].head;
        if (!(job_queue[q].head = j->next_job)) job_queue[q].tail = NULL;
        ReleaseSRWLockExclusive(&jobs_lock);
        me->since_ms = GetTickCount64();
        me->job = j;
        if (j->kind == JOB_PIPELINE) {
            Pipeline *p = (Pipeline *)j;
            LONG state = make_pipeline(me, p);
            me->step = STEP_IDLE;
            me->job = NULL;
            if (state == 1) InterlockedIncrement64((volatile LONG64 *)&gfx_stat_pipelines);
            publish(&p->state, state);
            InterlockedDecrement(&compiles_pending);
            continue;
        }
        Shader *s = (Shader *)j;
        if (s->prewarm) {
            prewarm_job(me, s);
            me->step = STEP_IDLE;
            me->job = NULL;
            free(s);
            continue;
        }
        LONG state = make_ready(me, s);
        me->step = STEP_IDLE;
        me->job = NULL;
        free(s->msl);
        s->msl = NULL;
        if (state == 1) InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        else InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shader_failures);
        publish(&s->state, state);
        InterlockedDecrement(&compiles_pending);
    }
}

/* The compiler threads, started with the renderer (init). */
static void start_compilers(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char *env = getenv("SSBU_COMPILE_THREADS");
    u32 n = env ? (u32)atoi(env) : si.dwNumberOfProcessors / 3;
    n = n < 2 ? 2 : n > COMPILERS_MAX ? COMPILERS_MAX : n;
    for (u32 i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, COMPILER_STACK, compiler_main, &compilers[i], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t) break;
        CloseHandle(t);
        ncompilers++;
    }
    if (!ncompilers) rt_fatal("cannot start the shader compiler threads (error %lu)", (unsigned long)GetLastError());
    fprintf(stderr, "[gfx] %u shader compiler threads\n", ncompilers);
}

typedef struct { char name[96]; long long written; } Found;
static int newest_first(const void *a, const void *b) {
    long long x = ((const Found *)a)->written, y = ((const Found *)b)->written;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* (a thread of its own, at start) every file of the disk cache into the prewarm queue */
static DWORD WINAPI prewarm_main(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"shader prewarm");
    Found *found = NULL;
    u32 n = 0, cap = 0;
    char prefix[32];
    snprintf(prefix, sizeof prefix, "m%u_", SHADER_CACHE_MSL_VERSION);
    DIR *d = opendir(cache_dir());
    for (struct dirent *e; d && (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, strlen(prefix)) || len < 7 || strcmp(e->d_name + len - 6, ".metal") || len >= sizeof found->name) continue;
        char path[1200];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", cache_dir(), e->d_name);
        /* only what the game has used: not the ahead-of-time translator's files it never asked for */
        if (stat(path, &st) || (long long)st.st_mtime < SHADER_CACHE_PLAYED_UNIX) continue;
        if (n == cap) found = realloc(found, sizeof *found * (cap = cap * 2 + 1024));
        snprintf(found[n].name, sizeof found[n].name, "%s", e->d_name);
        found[n++].written = (long long)st.st_mtime;
    }
    if (d) closedir(d);
    qsort(found, n, sizeof *found, newest_first);   /* the shaders of what was played last, first */
    prewarm_t0 = GetTickCount64();
    prewarm_bytes0 = private_bytes();
    Prewarm *table = calloc(PREWARM_SLOTS, sizeof *table);
    prewarm_table = table;
    Job *head = NULL, *tail = NULL;
    u32 queued = 0;
    for (u32 i = 0; i < n && table; i++) {
        unsigned version, vs_outputs;
        char stage_name[4];
        unsigned long long hash, tables;
        if (sscanf(found[i].name, "m%u_%2[a-z]_%16llx_%8x_%16llx.metal", &version, stage_name, &hash, &vs_outputs, &tables) != 5) continue;
        int stage = !strcmp(stage_name, "vs") ? SHADER_VERTEX : !strcmp(stage_name, "ps") ? SHADER_PIXEL : !strcmp(stage_name, "cs") ? SHADER_COMPUTE : -1;
        if (stage < 0 || queued >= PREWARM_SLOTS / 2) continue;
        Shader *s = calloc(1, sizeof *s);
        s->job.kind = JOB_SHADER;
        s->hash = hash; s->stage = stage; s->vs_outputs = vs_outputs; s->table_hash = tables;
        snprintf(s->name, sizeof s->name, "%s_%016llx", stage_name, hash);
        AcquireSRWLockExclusive(&prewarm_lock);
        s->prewarm = prewarm_slot(stage, hash, vs_outputs, tables, 1);
        if (s->prewarm && s->prewarm->state == PW_EMPTY) s->prewarm->state = PW_QUEUED;
        else { free(s); s = NULL; }
        ReleaseSRWLockExclusive(&prewarm_lock);
        if (!s) continue;
        if (tail) tail->next_job = &s->job; else head = &s->job;
        tail = &s->job;
        queued++;
    }
    free(found);
    if (!queued) return 0;
    prewarm_total = (LONG)queued;
    AcquireSRWLockExclusive(&jobs_lock);
    if (job_queue[2].tail) job_queue[2].tail->next_job = head; else job_queue[2].head = head;
    job_queue[2].tail = tail;
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeAllConditionVariable(&jobs_cv);
    fprintf(stderr, "[gfx] prewarm: %u shaders from the disk cache, on %u threads\n", queued, ncompilers - 1);
    return 0;
}

/* Hand a job to the compiler threads: to the queue's end, or (front) its start. */
static void enqueue(Job *j, int q, int front) {
    InterlockedIncrement(&compiles_pending);
    j->next_job = NULL;
    AcquireSRWLockExclusive(&jobs_lock);
    if (front) {
        if (!(j->next_job = job_queue[q].head)) job_queue[q].tail = j;
        job_queue[q].head = j;
    } else {
        if (job_queue[q].tail) job_queue[q].tail->next_job = j;
        else job_queue[q].head = j;
        job_queue[q].tail = j;
    }
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeConditionVariable(&jobs_cv);
}

/* A shader to the compiler threads. A compute program's cache load goes to the front: its
 * dispatch waits for it. */
static void enqueue_shader(Shader *s) { enqueue(&s->job, s->cached ? 0 : 1, s->cached && s->stage == SHADER_COMPUTE); }

void gfx_status_lines(void) {}   /* (no lines of its own yet) */

/* For a crash or freeze report: what each compiler thread is doing (to line, a line at a time),
 * its program kept next to the disk cache (shadercrash/) so it can be compiled again outside the
 * game. */
void gfx_report_compiles(void (*line)(const char *text)) {
    char text[600];
    static const char *const what[] = {"idle", "reading its MSL from the disk cache", "Metal compiling its MSL", "Metal making its pipeline"};
    DWORD self = GetCurrentThreadId();
    u64 now = GetTickCount64();
    int busy = 0;
    for (u32 i = 0; i < ncompilers; i++) {
        Job *j = compilers[i].job;
        LONG step = compilers[i].step;
        if (!j || step == STEP_IDLE) continue;
        if (!busy++) line("shader compiler threads at the time:\n");
        char name[160];
        const char *saved = "";
        if (j->kind == JOB_PIPELINE) {
            const Pipeline *p = (const Pipeline *)j;
            snprintf(name, sizeof name, "the pipeline of %s and %s", p->key.vs->name, p->key.ps ? p->key.ps->name : "no pixel shader");
        } else {
            const Shader *s = (const Shader *)j;
            snprintf(name, sizeof name, "%s", s->name);
            char dir[1100], path[1200];
            snprintf(dir, sizeof dir, "%s/../shadercrash", rt_data_dir);
            mkdir(dir, 0755);
            if (s->msl) {
                snprintf(path, sizeof path, "%s/%s_%08x_%016llx.metal", dir, s->name, s->vs_outputs, s->table_hash);
                FILE *f = fopen(path, "w");
                if (f) { fputs(s->msl, f); fclose(f); saved = "; MSL saved"; }
            }
        }
        snprintf(text, sizeof text, "  thread %lu%s: %s - %s, for %.1f s%s\n", (unsigned long)compilers[i].thread, compilers[i].thread == self ? " (this thread)" : "",
                 name, what[step], (now - compilers[i].since_ms) / 1000.0, saved);
        line(text);
    }
    if (busy) { snprintf(text, sizeof text, "  (programs in %s/../shadercrash)\n", rt_data_dir); line(text); }
}

typedef struct CbufSource { const CbBinding *cb; } CbufSource;   /* one stage's bindings */
static void read_gpu(u64 va, u32 size, void *dst);
static u32 read_cbuf_word(void *user, u32 bank, u32 offset) {
    const CbufSource *src = user;
    u32 v = 0;
    if (bank < 18 && src->cb[bank].address && offset + 4 <= src->cb[bank].size) read_gpu(src->cb[bank].address + offset, 4, &v);
    return v;
}

static u64 table_hash(const ShaderInfo *info, CbufSource *src) {
    u64 h = 0;
    for (u32 t = 0; t < info->ntables; t++)
        for (u32 e = 0; e < info->table[t].count; e++)
            h = (h ^ read_cbuf_word(src, info->table[t].bank, info->table[t].offset + 4 * e)) * 1099511628211ull;
    return h;
}

/* Programs by where they are: finding a shader by its contents means decoding the program for
 * its length and hashing all of it - per draw, per stage, far too slow - so each address
 * remembers the shader it last held, recognised again by a few of its words. */
typedef struct ShaderSlot { u64 g, key, print; u32 size; Shader *s; u64 tables_at; } ShaderSlot;

/* Where a program's jump tables were read from, if they are all its compiler constants (bank 1,
 * which lie after its code and do not change): found there before, they are the same tables -
 * not read and hashed again for every draw. 0: anywhere else. */
static u64 tables_place(const ShaderInfo *info, const CbBinding *cb) {
    if (!info->ntables) return 0;
    for (u32 t = 0; t < info->ntables; t++) if (info->table[t].bank != 1) return 0;
    return cb[1].address ? cb[1].address ^ (u64)cb[1].size << 48 : 0;
}
#define SHADER_SLOTS 16384
static ShaderSlot shader_slots[SHADER_SLOTS];

static u64 fingerprint(int stage, const u8 *p, u32 size) {   /* 32 words spread over the program (a compute one's code only) */
    if (stage == SHADER_COMPUTE) { p += 0x50; size -= 0x50; }
    u64 h = size;
    for (u32 i = 0; i < 32; i++) {
        u64 v;
        memcpy(&v, p + (((u64)size - 8) * i / 31 & ~7ull), 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

/* debug (SSBU_GFX_FIND): other copies of a program in guest memory, and what follows them */
static void find_copies(const Shader *s, u64 g, const u8 *p, u32 size) {
    fprintf(stderr, "[find] %s at %#llx, %#x bytes; around it:", s->name, g, size);
    for (u32 i = 0; i < 0x1000 / 4; i++) {
        u32 v = ld32((g & ~0xFFFull) + i * 4);
        if (i % 8 == 0) fprintf(stderr, "\n[find]   %#llx:", (g & ~0xFFFull) + i * 4);
        fprintf(stderr, " %08x", v);
    }
    fprintf(stderr, "\n");
    MEMORY_BASIC_INFORMATION mbi;
    for (u64 a = ALIAS_BASE; a < HEAP_BASE + HEAP_LIMIT && VirtualQuery(G2H(a), &mbi, sizeof mbi); a = H2G(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        const u8 *r = mbi.BaseAddress;
        for (u64 o = 0; o + 64 <= mbi.RegionSize; o += 16)
            if (!memcmp(r + o, p + 0x50, 64) && H2G(r + o) != g + 0x50) {
                fprintf(stderr, "[find]   copy at %#llx; bytes after its end:", H2G(r + o) - 0x50);
                for (u32 i = 0; i < 0x180 / 4 && o - 0x50 + size + i * 4 + 4 <= mbi.RegionSize; i++) {
                    u32 v;
                    memcpy(&v, r + o - 0x50 + size + i * 4, 4);
                    fprintf(stderr, "%s%08x", i % 8 ? " " : "\n[find]     ", v);
                }
                fprintf(stderr, "\n");
            }
    }
}

/* The shader for the program at guest address g, ready (state 1) or not. alpha_tested: a pixel
 * shader's variant that does the fixed-function alpha test (its own hash, so its own cache file,
 * and its own pipelines: a Shader of its own). */
static Shader *find_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared, int alpha_tested) {
    /* a compute program has no header: it is translated as if it had one (never read) */
    const u8 *p = stage == SHADER_COMPUTE ? P(u8, g) - 0x50 : P(u8, g);
    u64 key = (u64)stage | (u64)vs_outputs << 4 | (stage == SHADER_COMPUTE ? ((u64)local[0] ^ (u64)local[1] << 11 ^ (u64)local[2] << 22 ^ (u64)shared << 33) << 36 : 0)
            | (u64)(alpha_tested != 0) << 63;
    ShaderSlot *slot = &shader_slots[((g >> 4) * 0x9E3779B97F4A7C15ull ^ key) >> 50 & (SHADER_SLOTS - 1)];
    if (slot->s && slot->g == g && slot->key == key && slot->print == fingerprint(stage, p, slot->size)) {
        Shader *s = slot->s;
        if (!s->info.ntables) return s;
        u64 place = tables_place(&s->info, stage_cb);
        if (place && slot->tables_at == place) return s;
        CbufSource src = {stage_cb};
        if (s->table_hash == table_hash(&s->info, &src)) { slot->tables_at = place; return s; }
    }
    u32 size = shader_size(p, 0x40000), skip = stage == SHADER_COMPUTE ? 0x50 : 0;
    u64 h = shader_program_hash(p + skip, size - skip);
    if (stage == SHADER_COMPUTE) h = (h ^ ((u64)local[0] | (u64)local[1] << 16 | (u64)local[2] << 32 | (u64)shared << 40)) * 1099511628211ull;
    u64 program_hash = h;   /* (the shader's name) */
    h = shader_cache_hash(h, p, size, stage);   /* (the few programs the render scale changes: files of their own) */
    /* (gpu_d3d11.c's salt: it changes with the alpha test's code, so cache files of an older form are not used) */
    if (alpha_tested) h = (h ^ 0x616C706861743032ull) * 1099511628211ull;
    CbufSource src = {stage_cb};
    for (u32 i = 0; i < shader_count; i++) {
        Shader *s = shader_cache[i];
        if (s->hash != h || s->stage != stage || (stage == SHADER_PIXEL && s->vs_outputs != vs_outputs)) continue;
        if (s->info.ntables && s->table_hash != table_hash(&s->info, &src)) continue;
        *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s, 0};
        return s;
    }
    if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
    Shader *s = calloc(1, sizeof *s);
    shader_cache[shader_count++] = s;
    s->job.kind = JOB_SHADER;
    s->hash = h; s->stage = stage; s->vs_outputs = vs_outputs;
    if (stage == SHADER_COMPUTE) memcpy(s->local, local, sizeof s->local);
    *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s, 0};
    snprintf(s->name, sizeof s->name, "%s_%016llx%s", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", program_hash,
             alpha_tested ? "_alphatest" : "");
    s->msl = alpha_tested ? shader_translate_alpha_tested(SHADER_MSL, p, size, vs_outputs, &s->info, read_cbuf_word, &src)
                          : shader_translate(SHADER_MSL, p, size, stage, vs_outputs, stage == SHADER_COMPUTE ? local : NULL, shared, &s->info, read_cbuf_word, &src);
    const char *cdump = getenv("SSBU_DUMP_SHADERS");
    if (cdump && stage == SHADER_COMPUTE) {   /* C_<hash>_<local x>x<y>x<z>_<shared>.bin, behind 0x50 zero bytes */
        char path[1024];
        snprintf(path, sizeof path, "%s/C_%016llx_%ux%ux%u_%u.bin", cdump, h, local[0], local[1], local[2], shared);
        FILE *f = fopen(path, "wb");
        if (f) { static const u8 zero[0x50]; fwrite(zero, 1, 0x50, f); fwrite(p + 0x50, 1, size - 0x50, f); fclose(f); }
    }
    s->table_hash = table_hash(&s->info, &src);
    if (s->info.unsupported[0]) fprintf(stderr, "[gfx] %s: %s\n", s->name, s->info.unsupported);
    const char *find = getenv("SSBU_GFX_FIND");   /* debug: other copies of this program in guest memory */
    if (find && strstr(s->name, find) && size > 0x90) find_copies(s, g, p, size);
    static const char *dump;
    static int checked;
    if (!checked++) dump = getenv("SSBU_DUMP_MSL") ? getenv("SSBU_DUMP_MSL") : getenv("SSBU_DUMP_HLSL");   /* (the program as compiled) */
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.metal", dump, s->name);
        FILE *f = fopen(path, "w");
        if (f) { fputs(s->msl, f); fclose(f); }
    }
    if (prewarm_take(s)) {   /* the prewarm made it: ready now */
        free(s->msl);
        s->msl = NULL;
        InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        publish(&s->state, 1);
        return s;
    }
    char path[1200];
    struct stat st;
    cache_path(s, path, sizeof path);
    s->cached = stat(path, &st) == 0;
    enqueue_shader(s);
    /* a compute program that was compiled before is waited for (a few ms): a skipped launch
     * leaves its outputs (skinned vertices) unwritten for a frame */
    if (stage == SHADER_COMPUTE && s->cached)
        for (u64 until = GetTickCount64() + 250; state_of(&s->state) == 0 && GetTickCount64() < until;) SwitchToThread();
    return s;
}

/* The ready shader for the program at guest address g, or NULL while it compiles. */
static Shader *get_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared) {
    Shader *s = find_shader_ex(g, stage, vs_outputs, stage_cb, local, shared, 0);
    return s && state_of(&s->state) == 1 ? s : NULL;
}

/* The pipeline for a key: ready, or NULL while the compiler threads make it (or if they could not). */
static Pipeline **pipelines;
static u32 npipelines, pipelines_cap;
static u32 key_hash(const void *d, size_t size);
static Pipeline *get_pipeline(const PipeKey *k) {
    if (npipelines * 4 >= pipelines_cap * 3) {   /* (grown: every entry placed again) */
        u32 cap = pipelines_cap ? pipelines_cap * 2 : 4096;
        Pipeline **t = calloc(cap, sizeof *t);
        for (u32 i = 0; i < pipelines_cap; i++)
            if (pipelines[i]) {
                u32 j = key_hash(&pipelines[i]->key, sizeof *k) & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = pipelines[i];
            }
        free(pipelines);
        pipelines = t;
        pipelines_cap = cap;
    }
    u32 j = key_hash(k, sizeof *k) & (pipelines_cap - 1);
    for (; pipelines[j]; j = (j + 1) & (pipelines_cap - 1))
        if (!memcmp(&pipelines[j]->key, k, sizeof *k)) return state_of(&pipelines[j]->state) == 1 ? pipelines[j] : NULL;
    Pipeline *p = calloc(1, sizeof *p);
    p->job.kind = JOB_PIPELINE;
    p->key = *k;
    pipelines[j] = p;
    npipelines++;
    /* programs compiled before: in Metal's cache, their pipelines most likely too (quick) */
    enqueue(&p->job, k->vs->cached && (!k->ps || k->ps->cached) ? 0 : 1, 0);
    return NULL;
}

/* ---- buffers: guest memory uploaded per draw ----------------------------------------------- */
static void read_gpu(u64 va, u32 size, void *dst);

/* What each stage's slot was given last, with a copy of the bytes: a draw that uploads the same
 * bytes from the same place again - most of a menu's constants, draw after draw - gets the same
 * memory, whose data is still there (its chunk not started again since): no copy. */
#define REUSE_MAX 65536
/* A slot whose bytes keep changing (most of a match's per-draw constants) is not compared or
 * copied for a while: after a frame of at least REUSE_TRIES uploads with none reused, its next
 * REUSE_PAUSE frames go straight from guest memory, then it is tried again (in a player's
 * 8-fighter match the comparing alone was 3-7.5% of the main thread, and the extra copy came on
 * top, for next to nothing). A slot reused at all in a frame - a menu's - keeps being compared. */
#define REUSE_TRIES 8
#define REUSE_PAUSE 8
typedef struct LastUpload {
    u64 va; u32 size, cap; Alloc where; int valid; u8 *copy;
    u64 batch, seq;         /* compared in submission batch, write log at seq (see untouched_since) */
    u64 frame, paused_to;   /* the frame counted (gfx_stat_presented); not compared before frame paused_to */
    u32 tries, hits;        /* in that frame */
} LastUpload;
static LastUpload last_upload[3][128];
u64 gfx_stat_reused, gfx_stat_reuse_compared;

/* What the GPU's own commands wrote to guest memory (constant buffer loads, inline writes,
 * copies, semaphores, compute results brought back), by guest address. Within one submission
 * nothing else changes what its draws read - the game may not touch it, as the console's GPU
 * reads it later - so bytes compared once in a submission need no comparing again unless one of
 * these writes touched them since: a results screen draws ~6,000 times a frame, with ~14 uploads
 * a draw that are nearly all the same as before. (The lock held: writes come through kept_flush,
 * gfx_resident_written/flush and gfx_guest_wrote.) */
#define WROTE_LOG 1024
extern u64 gpu_stat_batches;   /* (gpu.c: submissions so far) */
typedef struct Wrote { u64 lo, hi; } Wrote;
static Wrote wrote_log[WROTE_LOG];
static u64 wrote_seq;   /* writes logged so far (an entry is never changed once there: a check
                         * that has seen it must not miss what is added) */

static void guest_wrote(u64 va, u64 bytes) {   /* bytes == 0: anywhere */
    u64 lo = 0, hi = ~0ull, g = bytes ? gpu_va_to_guest(va) : 0;
    if (g && gpu_va_to_guest(va + bytes - 1) == g + bytes - 1) { lo = g; hi = g + bytes; }
    wrote_log[wrote_seq % WROTE_LOG] = (Wrote){lo, hi};
    wrote_seq++;
}
void gfx_guest_wrote(u64 va, u64 bytes) {
    if (!enter()) return;
    guest_wrote(va, bytes);
    leave();
}
/* Guest memory [g, g + size) as it was when write `since` was the next to come, in this submission? */
static int untouched_since(u64 batch, u64 since, u64 g, u64 size) {
    if (batch != gpu_stat_batches || wrote_seq - since > WROTE_LOG) return 0;
    for (u64 i = since; i < wrote_seq; i++)
        if (wrote_log[i % WROTE_LOG].lo < g + size && g < wrote_log[i % WROTE_LOG].hi) return 0;
    return 1;
}

/* Bytes for a stage's slot (data NULL: zeros), padded to 16 with zeros. */
static View upload_raw(int stage, u32 slot, const void *data, u32 size) {
    last_upload[stage][slot].valid = 0;
    u32 padded = (size + 15) & ~15u;
    if (!padded) padded = 16;
    gfx_stat_upload_bytes += padded;
    gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += padded;   /* constant buffers, vertex streams, storage buffers */
    Alloc a = arena(padded);
    if (data) memcpy(a.cpu, data, size);
    else memset(a.cpu, 0, size);
    if (padded > size) memset(a.cpu + size, 0, padded - size);
    return (View){a.b, a.off, padded};
}

/* Guest GPU memory [va, va + size) uploaded for a stage's slot: straight from guest memory when it
 * is one mapping, else page by page. */
static View upload_gpu(int stage, u32 slot, u64 va, u32 size) {
    u32 padded = (size + 15) & ~15u;
    if (!padded) padded = 16;
    LastUpload *lu = &last_upload[stage][slot];
    u64 g = size && size <= REUSE_MAX ? gpu_va_to_guest(va) : 0;
    const u8 *src = g && gpu_va_to_guest(va + size - 1) == g + size - 1 ? P(const u8, g) : NULL;   /* (one mapping) */
    int keep = src != NULL;   /* compared now, and a copy kept to compare with next time */
    if (lu->frame != gfx_stat_presented) {   /* a new frame: was the last one counted all misses? */
        if (lu->tries >= REUSE_TRIES && !lu->hits) lu->paused_to = gfx_stat_presented + REUSE_PAUSE;
        lu->frame = gfx_stat_presented;
        lu->tries = lu->hits = 0;
    }
    if (gfx_stat_presented < lu->paused_to) keep = 0;
    if (keep) lu->tries++;
    if (keep && lu->valid && lu->va == va && lu->size == size && alive(&lu->where)) {
        View v = {lu->where.b, lu->where.off, lu->where.size};
        if (untouched_since(lu->batch, lu->seq, g, size)) {   /* compared already in this submission, and not written since */
            lu->hits++;
            gfx_stat_reused++;
            return v;
        }
        gfx_stat_reuse_compared += size;
        if (!memcmp(src, lu->copy, size)) {
            lu->batch = gpu_stat_batches;
            lu->seq = wrote_seq;
            lu->hits++;
            gfx_stat_reused++;
            return v;
        }
    }
    if (keep) {   /* (the copy, taken now: what is compared next time) */
        if (lu->cap < size) {
            free(lu->copy);
            lu->cap = size;
            if (!(lu->copy = malloc(size))) lu->cap = 0;
        }
        if (lu->copy) memcpy(lu->copy, src, size);
        else keep = 0;
    }
    Alloc a = arena(padded);
    if (keep) memcpy(a.cpu, lu->copy, size);
    else if (src) memcpy(a.cpu, src, size);
    else read_gpu(va, size, a.cpu);
    if (padded > size) memset(a.cpu + size, 0, padded - size);
    gfx_stat_upload_bytes += padded;
    gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += padded;
    lu->va = va; lu->size = size; lu->where = a; lu->valid = keep;
    lu->batch = gpu_stat_batches; lu->seq = wrote_seq;
    return (View){a.b, a.off, padded};
}

/* Copy guest GPU memory [va, va + size) into dst (zeros where unmapped). */
static void read_gpu(u64 va, u32 size, void *dst) {
    u64 g = gpu_va_to_guest(va);
    if (!size) return;
    if (g && gpu_va_to_guest(va + size - 1) == g + size - 1) { memcpy(dst, P(void, g), size); return; }
    /* not one contiguous mapping: page by page, zeros where unmapped */
    for (u32 done = 0; done < size;) {
        u32 n = 0x1000 - (u32)((va + done) & 0xFFF);
        if (n > size - done) n = size - done;
        u64 p = gpu_va_to_guest(va + done);
        if (p) memcpy((u8 *)dst + done, P(void, p), n);
        else memset((u8 *)dst + done, 0, n);
        done += n;
    }
}

#define DIV_UP_(a, b) (((a) + (b) - 1) / (b))

static int draw_logging;   /* (defined below) */
int gfx_draw_logging(void) { return draw_logging; }

/* ---- GPU-resident buffers ---------------------------------------------------------------- */
/* What compute programs write stays on the GPU: a buffer per guest range they write (and per
 * copy-engine destination of such data), used directly by later dispatches and by draws whose
 * vertex streams lie inside it; reads that only partly overlap one get the guest's data around
 * it. Guest memory under a region is stale; pages the CPU writes inside one (seen through the
 * write watch) are taken from guest memory - on the GPU's timeline (gpu_write), as the GPU
 * writes these buffers itself. */
typedef struct Resident {
    u64 va, guest;
    u32 size;
    GBuf *buf;
    /* Guest memory under the region's first and last partial host pages (16 KiB: head and tail
     * bytes), as it was when the GPU copy last took it: whether the bytes inside the region were
     * written (rather than someone's data next to it) is told by comparing with these. */
    u8 *edge;
    u32 head, tail;
    u64 checked;   /* the submission it was last checked against the write watch in */
    u64 seen;      /* (the write watch's clock then: watch_written) */
    u64 used;      /* the gfx_* call (op_seq) that last used it: with the table full, the region used longest ago goes */
} Resident;
static Resident residents[512];
static u32 nresidents;
u64 gfx_stat_residents, gfx_stat_resident_pages;
static int watched(u64 g, u64 size);

/* The write watch keeps one bit a 4 KiB page of the alias area (macos/memory.c), and a query that
 * resets it takes the news from everyone else watching that page. Textures, kept constant buffers,
 * compute inputs and resident regions share pages - memory a scene gives up is handed out again
 * for something else - and whoever asked first kept the news for good (gpu_d3d11.c: the No
 * Contest screen skinned the fighters' arms from inputs never taken again). A query and a reset
 * made apart lose a write landing between them, too (the game writes on other threads as the GPU
 * thread draws). So every question goes through here: pages found written get the next stamp,
 * kept per page, taken and reset in one call; each user keeps the clock as it was when it last
 * asked ("seen"): its data is current unless one of its pages has a newer stamp. (A write
 * anywhere in a 16 KiB host page reports all of its 4 KiB pages: more news than happened, never
 * less - which the resident regions' edges see to, below.) */
#define WATCH_CHUNK 512   /* pages a chunk: 2 MB */
typedef struct WatchChunk { u64 newest, page[WATCH_CHUNK]; } WatchChunk;
static WatchChunk *watch_chunks[ALIAS_SIZE / (WATCH_CHUNK * 0x1000ull)];
static u64 watch_clock;   /* stamps given so far */

/* Take the write watch's news for guest pages [lo, hi) (4 KiB aligned, watched): resetting the
 * bits as they are read (under the shim's lock: a write is either reported now or news next time). */
static void watch_take(u64 lo, u64 hi) {
    static void *pages[65536];   /* (256 MB: the largest range asked about, in one call) */
    void *first;
    ULONG_PTR count = 1;
    ULONG granularity;
    /* (asking is cheap, resetting is not - it protects the host pages again, even where nothing
     * was written: so ask first) */
    if (GetWriteWatch(0, G2H(lo), hi - lo, &first, &count, &granularity) != 0 || !count) return;
    for (u64 at = H2G(first); at < hi;) {
        count = 65536;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, G2H(at), hi - at, pages, &count, &granularity) != 0 || !count) return;
        u64 stamp = ++watch_clock;
        for (ULONG_PTR k = 0; k < count; k++) {
            u64 p = (H2G(pages[k]) - ALIAS_BASE) >> 12;
            WatchChunk **c = &watch_chunks[p / WATCH_CHUNK];
            if (!*c && !(*c = calloc(1, sizeof **c))) continue;
            (*c)->page[p % WATCH_CHUNK] = (*c)->newest = stamp;
        }
        if (count < 65536) return;
        at = H2G(pages[count - 1]) + 0x1000;
    }
}

/* The first page in [p, hi) stamped after `seen` (hi if none). */
static u64 watch_next(u64 p, u64 hi, u64 seen) {
    while (p < hi) {
        u64 i = (p - ALIAS_BASE) >> 12, end = (i / WATCH_CHUNK + 1) * WATCH_CHUNK;
        const WatchChunk *c = watch_chunks[i / WATCH_CHUNK];
        if (c && c->newest > seen)
            for (; i < end && ALIAS_BASE + (i << 12) < hi; i++)
                if (c->page[i % WATCH_CHUNK] > seen) return ALIAS_BASE + (i << 12);
        p = ALIAS_BASE + (end << 12);
    }
    return hi;
}

/* Has guest memory [g, g + size) (watched) been written since *seen? (*seen moves on to now.) Ask
 * before reading the memory, so that a write meanwhile is news next time. */
static int watch_written(u64 g, u64 size, u64 *seen) {
    u64 lo = g & ~0xFFFull, hi = (g + size + 0xFFF) & ~0xFFFull;
    watch_take(lo, hi);
    int written = watch_next(lo, hi, *seen) < hi;
    *seen = watch_clock;
    return written;
}

static void resident_drop(Resident *r) {
    gbuf_free(r->buf);
    free(r->edge);
    *r = residents[--nresidents];
    memset(&residents[nresidents], 0, sizeof *residents);
}

static int read_guarded(void *dst, u64 g, u32 n);

/* The edges are the host's pages (16 KiB), not the guest's: a write anywhere in a host page
 * reports every 4 KiB page of it (macos/memory.c), so the write watch tells nothing of a guest
 * page that shares its host page with memory outside the region. */
static void resident_snapshot(Resident *r) {
    u32 head = (u32)(0x4000 - (r->guest & 0x3FFF)) & 0x3FFF;   /* up to the first host page boundary */
    if (head > r->size) head = r->size;
    u32 tail = (u32)((r->guest + r->size) & 0x3FFF);           /* from the last one */
    if (tail > r->size - head) tail = r->size - head;
    r->head = head;
    r->tail = tail;
    r->edge = realloc(r->edge, head + tail + 1);
    /* (zeros where the memory is gone - a region made over older ones may reach memory the
     * guest has unmapped since: resident_sync finds it gone too, and leaves it alone) */
    if (head && !read_guarded(r->edge, r->guest, head)) memset(r->edge, 0, head);
    if (tail && !read_guarded(r->edge + head, r->guest + r->size - tail, tail)) memset(r->edge + head, 0, tail);
}

/* n bytes of guest memory at g, or 0 if they are not there any more: the guest can unmap the
 * memory under a resident region (svc UnmapPhysicalMemory). The renderer hears of that and drops
 * the region (gfx_guest_unmapped), but only if the memory held an nvmap allocation when it went.
 * (The kernel reads it for us: an unmapped or inaccessible page fails the call, not the process.) */
static int read_guarded(void *dst, u64 g, u32 n) {
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)G2H(g), n, (mach_vm_address_t)(uintptr_t)dst, &got);
    return kr == KERN_SUCCESS && got == n;
}

/* A partial host page's bytes inside the region (n at g; was: as the GPU copy last took them):
 * each 4 KiB page of them taken from guest memory if it changed (one changed word does not send
 * the whole edge). 0 if that memory is gone. */
static int resident_take_edge(Resident *r, u64 g, u8 *was, u32 n) {
    u8 now[0x4000];   /* (an edge lies within one host page) */
    if (!n) return 1;
    if (!read_guarded(now, g, n)) return 0;
    for (u32 at = 0, piece; at < n; at += piece) {
        piece = 0x1000 - (u32)((g + at) & 0xFFF);
        if (piece > n - at) piece = n - at;
        if (!memcmp(was + at, now + at, piece)) continue;
        memcpy(was + at, now + at, piece);
        gpu_write(r->buf, g + at - r->guest, was + at, piece);
        gfx_stat_resident_pages++;
    }
    return 1;
}

/* The write watch's news over the region's whole host pages is taken (after its GPU copy took
 * guest memory). Not over its partial ones: they are compared by content instead. */
static void resident_watch_reset(Resident *r) {
    u64 lo = (r->guest + 0x3FFF) & ~0x3FFFull, hi = (r->guest + r->size) & ~0x3FFFull;
    if (lo < hi) watch_take(lo, hi);
    r->seen = watch_clock;
}

/* Has the CPU written guest memory inside the region since its GPU copy last took it? Host pages
 * (16 KiB) wholly inside that it wrote (the write watch says: watch_written's stamps) are taken
 * from guest memory. The partial host pages at either end are compared byte for byte, every time,
 * and taken where the bytes inside the region changed - the CPU may have written just its own
 * data next to the region, or (memory reused) right over a region no program writes any more.
 * Not by the write watch: a write anywhere in a host page reports all of it, so a report says
 * nothing of the bytes inside the region. Small regions pack several to a page in the effect ring
 * (GPU particle output among the CPU's per-draw attributes); when the first one checked took the
 * page's bit, its neighbours kept stale data for good, which drew effect meshes at a stale size
 * over the whole screen (the white-outs). Checked when a draw or dispatch uses the region (once a
 * submission) and once a frame for all. Whole pages go to the GPU copy through a copy of their
 * own too, never straight from guest memory: a region outlives the memory under it when the guest
 * unmaps that without the renderer hearing of it (gfx_guest_unmapped hears of nvmap memory only),
 * and a region with no partial pages has no edges to find that out by - the copy would fault
 * (gpu_d3d11.c: players' games crashed in the graphics driver after 20-40 minutes). */
static void resident_sync(Resident *r, int every_frame) {
    static int no_pages = -1;   /* debug: leave resident regions alone */
    if (no_pages < 0) no_pages = getenv("SSBU_NO_RESIDENT_PAGES") != NULL;
    if (no_pages || (!every_frame && r->checked == gpu_stat_batches) || !r->edge || !watched(r->guest, r->size)) return;
    r->checked = gpu_stat_batches;
    if (!resident_take_edge(r, r->guest, r->edge, r->head) || !resident_take_edge(r, r->guest + r->size - r->tail, r->edge + r->head, r->tail))
        return;   /* (its memory is gone: left alone; once mapped again its bytes differ, and are taken) */
    u64 lo = (r->guest + 0x3FFF) & ~0x3FFFull, hi = (r->guest + r->size) & ~0x3FFFull;   /* the whole host pages */
    if (lo >= hi) return;
    u64 seen = r->seen;
    watch_take(lo, hi);   /* (before taking them: what the CPU writes meanwhile is news next time) */
    r->seen = watch_clock;
    for (u64 pg = watch_next(lo, hi, seen); pg < hi;) {
        u8 now[0x4000];   /* (a run of written pages, a host page's worth at most, in one copy) */
        u32 n = 0x1000;
        while (n < sizeof now && pg + n < hi && watch_next(pg + n, pg + n + 0x1000, seen) == pg + n) n += 0x1000;
        if (read_guarded(now, pg, n)) {   /* (gone: left as it is) */
            gpu_write(r->buf, pg - r->guest, now, n);
            gfx_stat_resident_pages += n >> 12;
        }
        pg = watch_next(pg + n, hi, seen);
    }
}

static Resident *resident_covering(u64 lo, u64 hi) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va <= lo && hi <= residents[i].va + residents[i].size) {
            residents[i].used = op_seq;
            resident_sync(&residents[i], 0);
            return &residents[i];
        }
    return NULL;
}

/* Every region overlapping [va, va + size) is used by this call: none of them is given up for a
 * region made in it. */
static void resident_mark(u64 va, u32 size) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va < va + size && va < residents[i].va + residents[i].size) residents[i].used = op_seq;
}

/* [va, va + bytes) inside r, as shaders read or compute writes it (whole words). */
static View resident_view(Resident *r, u64 va, u32 bytes) {
    u64 first = (va - r->va) & ~3ull;
    bytes = (u32)((va + bytes - r->va - first + 3) & ~3ull);
    if (first + bytes > ((r->size + 15) & ~15u)) bytes = (u32)(((r->size + 15) & ~15u) - first);
    return (View){r->buf, first, bytes};
}

u64 gfx_stat_residents_given_up;
/* (Direct3D's renderer also gives up regions idle for a second that hold only what the CPU has
 * written since, counted here; this one keeps them until the table is full: none yet) */
u64 gfx_stat_residents_retired;

static Resident *resident_create(u64 va, u32 size) {
    if (nresidents == 512) {
        /* The table is full: the region used longest ago goes - never one this call uses (bound
         * already, or about to be merged). What the GPU wrote there is lost to guest memory, as
         * in Direct3D's renderer: told now and then. */
        Resident *old = NULL;
        for (u32 i = 0; i < nresidents; i++)
            if (residents[i].used != op_seq && (!old || residents[i].used < old->used)) old = &residents[i];
        if (!old) return NULL;
        u64 n = ++gfx_stat_residents_given_up;
        if (!(n & (n - 1)))
            fprintf(stderr, "[gfx] resident regions: the table is full; %llu given up so far (the last %#llx + %#x: the GPU's data there is lost)\n", n,
                    old->va, old->size);
        resident_drop(old);
    }
    Resident *r = &residents[nresidents];
    memset(r, 0, sizeof *r);
    u32 cap = (size + 15) & ~15u;
    if (!(r->buf = gbuf_new(cap, 1, 0))) return NULL;
    memset(r->buf->cpu, 0, cap);
    read_gpu(va, size, r->buf->cpu);   /* what the CPU put there (a new buffer: no work uses it yet) */
    r->va = va;
    r->size = size;
    r->guest = gpu_va_to_guest(va);
    r->used = op_seq;
    if (watched(r->guest, r->size)) resident_snapshot(r);
    nresidents++;
    gfx_stat_residents++;
    return r;
}

/* The resident region holding [va, va + size), made (or widened) as needed. Regions never
 * overlap: a range that overlaps some becomes one region over all of them, with the CPU's
 * data underneath and each old region's GPU data copied over it. */
static Resident *resident_acquire(u64 va, u32 size) {
    u64 lo = va, hi = va + size;
    int overlaps = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va <= va && va + size <= r->va + r->size) { r->used = op_seq; return r; }
        if (r->va < hi && va < r->va + r->size) {
            overlaps = 1;
            r->used = op_seq;   /* (to be merged: not the one a full table gives up) */
            if (r->va < lo) lo = r->va;
            if (r->va + r->size > hi) hi = r->va + r->size;
        }
    }
    if (hi - lo > (256u << 20)) return NULL;
    Resident *n = resident_create(lo, (u32)(hi - lo));
    if (!n) return NULL;
    if (overlaps) {
        Resident merged = *n;   /* resident_drop moves entries around */
        n->buf = NULL; n->edge = NULL;
        resident_drop(n);
        for (u32 i = 0; i < nresidents;) {
            Resident *r = &residents[i];
            if (r->va >= merged.va && r->va + r->size <= merged.va + merged.size && r->buf != merged.buf) {
                buffer_copy(r->buf, 0, merged.buf, r->va - merged.va, (r->size + 3) & ~3u);
                resident_drop(r);
            } else i++;
        }
        residents[nresidents++] = merged;
        n = &residents[nresidents - 1];
    }
    if (n->guest >= ALIAS_BASE && n->guest + n->size <= ALIAS_BASE + ALIAS_SIZE) resident_watch_reset(n);
    return n;
}

static Resident *resident_overlapping(u64 va, u32 size) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va < va + size && va < residents[i].va + residents[i].size) {
            residents[i].used = op_seq;
            resident_sync(&residents[i], 0);
            return &residents[i];
        }
    return NULL;
}

/* dst holds guest memory's [va, va + size): copy over it what resident regions hold of that. */
static void resident_patch(View dst, u64 va, u64 size) {
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + size < r->va + r->size ? va + size : r->va + r->size;
        if (lo >= hi) continue;
        r->used = op_seq;
        resident_sync(r, 0);
        buffer_copy(r->buf, lo - r->va, dst.b, dst.off + (lo - va), (u32)(hi - lo));
    }
}

/* A range only partly held by resident regions, as the GPU sees it: the guest's data (in data)
 * with the regions' parts copied over it, in a buffer of its own (handed back at once: it is not
 * given out again before the work recorded now is done). Regions are never widened for a read:
 * what lies around them is the CPU's, and it keeps changing in guest memory. */
static View upload_composed(int stage, u32 slot, u64 va, const void *data, u32 size) {
    u32 need_ = (size + 15) & ~15u;
    gfx_stat_upload_bytes += size;
    gfx_stat_composed++;
    if (!need_) return upload_raw(stage, slot, data, size);
    GBuf *b = pool_get(need_, 1);
    if (!b) return (View){0};
    memcpy(b->cpu, data, size);   /* (a buffer no work uses: written as it is) */
    memset(b->cpu + size, 0, need_ - size);
    View v = {b, 0, need_};
    resident_patch(v, va, size);
    pool_put(b, 1);
    return v;
}

/* The copy engine and resident data. */
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) {
    if (!bytes || bytes > (256u << 20) || !enter()) return 0;
    Resident *src = resident_overlapping(src_va, (u32)bytes);
    if (!src || src_va < src->va || src_va + bytes > src->va + src->size) {
        leave();
        if (src) gfx_resident_flush(src_va, bytes);   /* partly resident: through guest memory */
        return 0;
    }
    Resident *dst = resident_acquire(dst_va, (u32)bytes);   /* may merge regions: look the source up again */
    src = resident_covering(src_va, src_va + bytes);
    if (!dst || !src) { leave(); return 0; }
    if (dst != src) buffer_copy(src->buf, src_va - src->va, dst->buf, dst_va - dst->va, (u32)bytes);
    else {   /* within one buffer: through a scratch one */
        GBuf *tmp = pool_get((u32)bytes, 1);
        if (!tmp) { leave(); return 0; }
        buffer_copy(src->buf, src_va - src->va, tmp, 0, (u32)bytes);
        buffer_copy(tmp, 0, dst->buf, dst_va - dst->va, (u32)bytes);
        pool_put(tmp, 1);
    }
    leave();
    return 1;
}

/* Bring the GPU's data for [va, va + bytes) back to guest memory (waits for the GPU). */
void gfx_resident_flush(u64 va, u64 bytes) {
    if (!enter()) return;
    guest_wrote(va, bytes);
    int any = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        any = 1;
    }
    if (any) {
        gfx_stat_gpu_waits++;
        submit(1);   /* (the buffers are the CPU's to read once the GPU is done) */
    }
    for (u32 i = 0; i < nresidents && any; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        if (hi > r->va + r->buf->size) hi = r->va + r->buf->size;
        for (u64 a = lo; a < hi; a += 4) {
            u64 g = gpu_va_to_guest(a);
            u32 w;
            memcpy(&w, r->buf->cpu + (a - r->va), 4);
            if (g) st32(g, w);
        }
        if (r->guest >= ALIAS_BASE && r->guest + r->size <= ALIAS_BASE + ALIAS_SIZE) { resident_watch_reset(r); resident_snapshot(r); }
    }
    leave();
}

static void kept_apply(u64 lo, u64 hi);
static void kept_flush(void);

/* Guest memory in [va, va + bytes) changed under a resident region: update the GPU's copy. */
void gfx_resident_written(u64 va, u64 bytes) {
    if (!bytes || !enter()) return;
    guest_wrote(va, bytes > (256u << 20) ? 0 : bytes);
    if (bytes > (256u << 20)) { leave(); return; }
    kept_flush();
    kept_apply(va, va + bytes);   /* (constant buffers kept on the GPU too) */
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        if (hi > r->va + ((r->size + 15) & ~15u)) hi = r->va + ((r->size + 15) & ~15u);
        u8 *data = malloc((size_t)(hi - lo));
        read_gpu(lo, (u32)(hi - lo), data);
        gpu_write(r->buf, lo - r->va, data, (u32)(hi - lo));
        free(data);
        if (r->guest >= ALIAS_BASE && r->guest + r->size <= ALIAS_BASE + ALIAS_SIZE) { resident_watch_reset(r); resident_snapshot(r); }
    }
    leave();
}

/* ---- kept constant buffers ------------------------------------------------------------------
 * Constant buffers of 4 KB or more stay on the GPU between draws: NVN binds 64 KB for each
 * program's own constants, and sending that again for every draw took most of the frame. They
 * are kept as the GPU would see memory: writes in the command stream (constant loads, inline
 * and copy-engine writes - the HUD's positions change between draws that way) are applied to
 * them in order, and what the CPU wrote is found by the write watch, asked at a buffer's first
 * use in each submission; memory outside the watched area is never kept. A buffer a recorded
 * draw has used is not written again: a change after that goes into a copy of it (what the
 * draws before it read stays as it was), which later draws use. */
typedef struct KeptCb { u64 va, guest, used, checked, seen; u32 size, mapped; int stale, bound; GBuf *buf; u8 *shadow; } KeptCb;
#define KEPT_SETS 256
#define KEPT_WAYS 4
static KeptCb kept[KEPT_SETS * KEPT_WAYS];
static u64 kept_clock;
static u16 kept_blocks[1 << 16];   /* how many kept buffers touch each (hashed) 64 KB block of GPU VA */
#define KEPT_BLOCK(block) ((u32)((u64)(block) * 0x9E3779B97F4A7C15ull >> 48))
/* Command stream writes not yet applied to kept buffers: per thread (a command list runs on one
 * thread, its draws too - the GPU thread, or with SSBU_GPU_THREAD=0 the one that submits it), so
 * extending the run needs no lock. */
static __thread u64 pending_lo, pending_hi;
static int kept_verify = -1;   /* SSBU_KEPT_VERIFY (debugging): keep a copy of what each holds, and report draws that find memory different */
u64 gfx_stat_kept_mismatches, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_reloaded, gfx_stat_kept_copies;

static void kept_blocks_add(const KeptCb *e, int d) {
    for (u64 b = e->va >> 16; b <= (e->va + e->size - 1) >> 16; b++) kept_blocks[KEPT_BLOCK(b)] += (u16)d;
}

/* n bytes into a kept buffer at off: into a copy of it if a draw has used it. */
static void kept_write(KeptCb *e, u32 off, const void *src, u32 n) {
    if (e->bound) {
        GBuf *b = pool_get(e->buf->size, 0);
        if (!b) return;
        memcpy(b->cpu, e->buf->cpu, e->buf->size < b->size ? e->buf->size : b->size);
        pool_put(e->buf, 0);
        e->buf = b;
        e->bound = 0;
        gfx_stat_kept_copies++;
    }
    memcpy(e->buf->cpu + off, src, n);
}

/* A kept buffer given up: its memory is handed out again once the work recorded now is done. */
static void kept_drop(KeptCb *e) {
    if (e->buf) kept_blocks_add(e, -1);
    pool_put(e->buf, 0);   /* (not handed out again before what is recorded is done) */
    free(e->shadow);
    memset(e, 0, sizeof *e);
}

/* The command stream wrote [lo, hi) of GPU VA (guest memory has it already). */
static void kept_apply(u64 lo, u64 hi) {
    int any = 0;
    for (u64 b = lo >> 16; b <= (hi - 1) >> 16 && !any; b++) any = kept_blocks[KEPT_BLOCK(b)] != 0;
    if (!any) return;   /* (almost always: no kept buffer is near) */
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) {
        KeptCb *e = &kept[i];
        if (!e->buf || e->stale || hi <= e->va || lo >= e->va + e->mapped) continue;
        u64 a = (lo > e->va ? lo : e->va) & ~3ull, z = ((hi < e->va + e->mapped ? hi : e->va + e->mapped) + 3) & ~3ull;
        if (z > e->va + e->mapped) z = e->va + e->mapped;
        /* Only from the memory it was made from, still mapped there: the guest may have mapped
         * other memory at these addresses since (and unmapped the old: the copy would fault
         * reading it). Otherwise it is taken again, or made again, at its next use. */
        u64 g = gpu_va_to_guest(a);
        if (g != e->guest + (a - e->va) || gpu_va_to_guest(z - 1) != g + (z - 1 - a)) { e->stale = 1; continue; }
        kept_write(e, (u32)(a - e->va), P(void, e->guest + (a - e->va)), (u32)(z - a));
        if (e->shadow) memcpy(e->shadow + (a - e->va), P(void, e->guest + (a - e->va)), (size_t)(z - a));
        gfx_stat_kept_applied++;
        gfx_stat_upload_bytes += z - a;
        gfx_stat_upload_kind[0] += z - a;
    }
}

static void kept_flush(void) {
    if (!pending_hi) return;
    u64 lo = pending_lo, hi = pending_hi;
    pending_hi = 0;
    guest_wrote(lo, hi - lo);
    kept_apply(lo, hi);
}

/* The command stream wrote guest memory at [va, va + bytes): runs of it are applied to the
 * kept buffers together, before anything is drawn. */
void gfx_stream_wrote(u64 va, u64 bytes) {
    if (pending_hi && pending_hi == va) { pending_hi += bytes; return; }
    if (pending_hi && enter()) { kept_flush(); leave(); }
    pending_lo = va;
    pending_hi = va + bytes;
}

/* A command list has been run: what it wrote reaches the kept buffers before another
 * thread's draws can use them. */
void gfx_stream_end(void) {
    if (pending_hi && enter()) { kept_flush(); leave(); }
}

/* How much of [va, va + size) maps to guest memory in one piece from g (at va). */
static u32 kept_mapped(u64 va, u64 g, u32 size) {
    u32 mapped = 0x1000 - (u32)(va & 0xFFF);   /* (the 64 KB bound may run past what is mapped: that part reads as zeros) */
    while (mapped < size && gpu_va_to_guest(va + mapped) == g + mapped) mapped += 0x1000;
    return mapped > size ? size : mapped;
}

static View kept_cb(u64 va, u32 size) {
    KeptCb *set = &kept[((va >> 4) * 0x9E3779B97F4A7C15ull >> 56) * KEPT_WAYS], *e = NULL, *lru = &set[0];
    for (u32 w = 0; w < KEPT_WAYS && !e; w++) {
        if (set[w].buf && set[w].va == va && set[w].size == size) e = &set[w];
        else if (set[w].used < lru->used) lru = &set[w];
    }
    u64 g = gpu_va_to_guest(va);
    /* (still mapped as when it was made, asked at its first use in a submission and before it
     * is taken again: a 64 KB binding reaches past the program's constants, into memory the
     * guest may have unmapped since - the copy would fault reading it - or mapped other memory
     * at: then it is made again, below) */
    if (e && e->guest == g && ((e->checked == gpu_stat_batches && !e->stale) || kept_mapped(va, g, size) == e->mapped)) {
        if (e->checked != gpu_stat_batches) {   /* first use in this submission: has the CPU written it since? */
            if (watch_written(g, e->mapped, &e->seen)) e->stale = 1;
            e->checked = gpu_stat_batches;
        }
        if (e->stale) {   /* take it all again */
            kept_write(e, 0, P(void, g), e->mapped);
            if (e->shadow) memcpy(e->shadow, P(void, g), e->mapped);
            gfx_stat_kept_reloaded++;
            gfx_stat_upload_bytes += e->mapped;
            gfx_stat_upload_kind[0] += e->mapped;
            e->stale = 0;
        }
        e->used = ++kept_clock;
        if (e->shadow && memcmp(e->shadow, P(void, g), e->mapped)) {
            u32 o = 0;
            while (o + 4 <= e->mapped && !memcmp(e->shadow + o, P(u8, g) + o, 4)) o += 4;
            u32 was = 0;
            if (o + 4 <= e->mapped) memcpy(&was, e->shadow + o, 4);
            if (gfx_stat_kept_mismatches++ < 200)
                fprintf(stderr, "[kept] va %#llx (%#x bytes) +%#x: kept %08x, memory %08x (submission %llu)\n", va, e->mapped, o, was,
                        o + 4 <= e->mapped ? ld32(g + o) : 0, gpu_stat_batches);
        }
        e->bound = 1;
        return (View){e->buf, 0, (size + 15) & ~15u};
    }
    if (!g || size < 0x1000) return (View){0};
    u32 mapped = kept_mapped(va, g, size);
    if (!watched(g, mapped)) return (View){0};
    if (!e) e = lru;
    kept_drop(e);
    if (kept_verify < 0) kept_verify = getenv("SSBU_KEPT_VERIFY") != NULL;
    u32 cap = (size + 15) & ~15u;
    if (!(e->buf = pool_get(cap, 0))) return (View){0};
    memset(e->buf->cpu, 0, e->buf->size);
    watch_written(g, mapped, &e->seen);   /* (before reading: a write meanwhile is news next submission) */
    memcpy(e->buf->cpu, P(void, g), mapped);
    e->va = va; e->guest = g; e->size = size; e->mapped = mapped; e->used = ++kept_clock; e->checked = gpu_stat_batches;
    if (kept_verify) { e->shadow = malloc(mapped); memcpy(e->shadow, P(void, g), mapped); }
    gfx_stat_kept_made++;
    kept_blocks_add(e, 1);
    gfx_stat_upload_bytes += size;
    gfx_stat_upload_kind[0] += size;
    e->bound = 1;
    return (View){e->buf, 0, cap};
}

/* ---- textures and samplers ------------------------------------------------------------------ */
typedef struct Texture {
    u64 guest, size;
    u32 tic[8];
    u64 sample_hash;
    int watched, stale;    /* in the write-watched alias area; written since it was read */
    u64 seen;              /* (the write watch's clock when it was read: watch_written) */
    int integer;           /* integer texels, as floats of the same bits (sampled nearest) */
    id<MTLTexture> tex;
    id<MTLTexture> view[8];   /* as each type a shader declares (TEX_*, shader_msl_texture) where that differs */
    u64 used, asked;       /* (asked: the frame the write watch was last asked about it, gfx_stat_presented) */
} Texture;
static Texture textures[4096];
static u32 ntextures;
u64 gfx_stat_textures, gfx_stat_watch_us;
/* Block-compressed textures copied to their exact size (the status line's "unpacked"): Direct3D
 * 11 makes such a texture whole 4x4 blocks, so gpu_d3d11.c copies one whose size is not a
 * multiple of 4 into an uncompressed one of its size. Metal makes it at its size (upload_texture:
 * the last blocks' copy is clipped to the texture's edge), so none here. */
u64 gfx_stat_unpacked;

static void watch_time(u64 ns) {   /* (to gfx_stat_watch_us, in microseconds) */
    static u64 rest;
    rest += ns;
    gfx_stat_watch_us += rest / 1000;
    rest %= 1000;
}

/* Which textures has the CPU (or a copy) written since they were read? The alias area is
 * reserved with MEM_WRITE_WATCH, so the shim tracks the pages written (macos/memory.c: by page
 * protection, 4 KB pages reported; watch_written). A texture's range is asked about the first
 * time a frame draws with it - not every texture every frame: the cost goes by the pages asked
 * about, and a match keeps thousands of textures from earlier screens it never draws. A write is
 * news to every texture on its pages (and everything else there), each taken again at its next
 * use. */
static int watched(u64 g, u64 size) { return g >= ALIAS_BASE && g + size <= ALIAS_BASE + ALIAS_SIZE; }

static void texture_ask(Texture *t) {
    if (!t->watched || t->stale || t->asked == gfx_stat_presented) return;
    t->asked = gfx_stat_presented;
    u64 t0 = rt_clock_ns();
    if (watch_written(t->guest, t->size, &t->seen)) t->stale = 1;
    watch_time(rt_clock_ns() - t0);
}

/* Once a frame: GPU-resident buffers the CPU has written to (those no draw has checked yet). */
static void note_writes(void) {
    u64 t0 = rt_clock_ns();
    for (u32 i = 0; i < nresidents; i++) resident_sync(&residents[i], 1);
    watch_time(rt_clock_ns() - t0);
}

/* A sparse checksum of guest memory, to notice when a texture's pixels change. */
static u64 sample_hash(u64 g, u64 size) {
    u64 h = 1469598103934665603ull;
    const u8 *p = P(u8, g);
    u64 step = size / 64 ? size / 64 : 1;
    for (u64 i = 0; i < size; i += step) { u64 v; memcpy(&v, p + (i & ~7ull), 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}

static void warn_texture(const TextureDesc *d) {
    static u32 seen[64], n;
    u32 key = d->format | d->type << 8 | d->header_version << 12;
    for (u32 i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 64) seen[n++] = key;
    fprintf(stderr, "[gfx] unsupported texture: format %#x, type %u, layout %u (%ux%u)\n", d->format, d->type, d->header_version,
            d->width, d->height);
}

/* The texture cache, indexed by guest address and texture header (draws look up thousands a
 * frame among thousands kept). Entries are checked against the texture they name, so a slot
 * given to another texture just stops matching; the index is rebuilt now and then. */
#define TEXTURE_INDEX 16384
static u16 texture_index[TEXTURE_INDEX];   /* textures[] index + 1 */
static u32 texture_index_adds;

static u32 texture_key(u64 g, const u32 *tic) {
    u64 h = g * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < 8; i++) h = (h ^ tic[i]) * 0x100000001B3ull;
    return (u32)(h >> 40) & (TEXTURE_INDEX - 1);
}

static Texture *texture_find(u64 g, const u32 *tic) {
    for (u32 k = texture_key(g, tic), n = 0; texture_index[k] && n < TEXTURE_INDEX; k = (k + 1) & (TEXTURE_INDEX - 1), n++) {
        Texture *t = &textures[texture_index[k] - 1];
        if (t->tex && t->guest == g && !memcmp(t->tic, tic, 32)) return t;
    }
    return NULL;
}

static void texture_index_put(Texture *t) {
    u32 k = texture_key(t->guest, t->tic);
    while (texture_index[k]) k = (k + 1) & (TEXTURE_INDEX - 1);
    texture_index[k] = (u16)(t - textures + 1);
}

static void texture_index_add(Texture *t) {
    if (++texture_index_adds >= TEXTURE_INDEX / 2) {   /* entries for textures since replaced pile up: start again */
        memset(texture_index, 0, sizeof texture_index);
        texture_index_adds = 0;
        for (u32 i = 0; i < ntextures; i++)
            if (textures[i].tex && &textures[i] != t) { texture_index_put(&textures[i]); texture_index_adds++; }
    }
    texture_index_put(t);
}

/* How a texture format from the header (gpu_texture.c's DXGI names) is kept for Metal: its pixel
 * format, and what is done to the texels on the way (conv). Integer formats are kept as floats of
 * the same bits - the translator reads every texture as floats and takes the integer's bits from
 * them, as Direct3D gives them - widened to 32 bits a channel where narrower (no float format has
 * their bits). Depth from guest memory is 32-bit float: Metal has no 24-bit depth to upload, and a
 * float format is one a comparing (shadow) sampler takes. */
enum { CONV_NONE, CONV_R24, CONV_R16_UNORM, CONV_RGBA4, CONV_U8, CONV_S8, CONV_U16, CONV_S16 };
typedef struct Kept { MTLPixelFormat format; u8 conv, channels, integer; } Kept;
static Kept texture_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return (Kept){MTLPixelFormatRGBA32Float, 0, 0, 0};
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return (Kept){MTLPixelFormatRGBA32Float, 0, 0, 1};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return (Kept){MTLPixelFormatRGBA16Unorm, 0, 0, 0};
    case DXGI_FORMAT_R16G16B16A16_SNORM: return (Kept){MTLPixelFormatRGBA16Snorm, 0, 0, 0};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return (Kept){MTLPixelFormatRGBA16Float, 0, 0, 0};
    case DXGI_FORMAT_R32G32_FLOAT: return (Kept){MTLPixelFormatRG32Float, 0, 0, 0};
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return (Kept){MTLPixelFormatRG32Float, 0, 0, 1};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return (Kept){MTLPixelFormatRGBA8Unorm, 0, 0, 0};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return (Kept){MTLPixelFormatRGBA8Unorm_sRGB, 0, 0, 0};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return (Kept){MTLPixelFormatRGBA8Snorm, 0, 0, 0};
    case DXGI_FORMAT_R8G8B8A8_UINT: return (Kept){MTLPixelFormatRGBA32Float, CONV_U8, 4, 1};
    case DXGI_FORMAT_R8G8B8A8_SINT: return (Kept){MTLPixelFormatRGBA32Float, CONV_S8, 4, 1};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return (Kept){MTLPixelFormatRGB10A2Unorm, 0, 0, 0};
    case DXGI_FORMAT_R16G16_UNORM: return (Kept){MTLPixelFormatRG16Unorm, 0, 0, 0};
    case DXGI_FORMAT_R16G16_SNORM: return (Kept){MTLPixelFormatRG16Snorm, 0, 0, 0};
    case DXGI_FORMAT_R16G16_FLOAT: return (Kept){MTLPixelFormatRG16Float, 0, 0, 0};
    case DXGI_FORMAT_R32_FLOAT: return (Kept){MTLPixelFormatR32Float, 0, 0, 0};
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return (Kept){MTLPixelFormatR32Float, 0, 0, 1};
    case DXGI_FORMAT_BC6H_SF16: return (Kept){MTLPixelFormatBC6H_RGBFloat, 0, 0, 0};
    case DXGI_FORMAT_BC6H_UF16: return (Kept){MTLPixelFormatBC6H_RGBUfloat, 0, 0, 0};
    case DXGI_FORMAT_B4G4R4A4_UNORM: return (Kept){MTLPixelFormatABGR4Unorm, CONV_RGBA4, 0, 0};   /* (red in Metal's top bits: measured) */
    case DXGI_FORMAT_B5G5R5A1_UNORM: return (Kept){MTLPixelFormatBGR5A1Unorm, 0, 0, 0};
    case DXGI_FORMAT_B5G6R5_UNORM: return (Kept){MTLPixelFormatB5G6R5Unorm, 0, 0, 0};
    case DXGI_FORMAT_BC7_UNORM: return (Kept){MTLPixelFormatBC7_RGBAUnorm, 0, 0, 0};
    case DXGI_FORMAT_BC7_UNORM_SRGB: return (Kept){MTLPixelFormatBC7_RGBAUnorm_sRGB, 0, 0, 0};
    case DXGI_FORMAT_R8G8_UNORM: return (Kept){MTLPixelFormatRG8Unorm, 0, 0, 0};
    case DXGI_FORMAT_R8G8_SNORM: return (Kept){MTLPixelFormatRG8Snorm, 0, 0, 0};
    case DXGI_FORMAT_R16_UNORM: return (Kept){MTLPixelFormatR16Unorm, 0, 0, 0};
    case DXGI_FORMAT_R16_SNORM: return (Kept){MTLPixelFormatR16Snorm, 0, 0, 0};
    case DXGI_FORMAT_R16_FLOAT: return (Kept){MTLPixelFormatR16Float, 0, 0, 0};
    case DXGI_FORMAT_R16_UINT: return (Kept){MTLPixelFormatR32Float, CONV_U16, 1, 1};
    case DXGI_FORMAT_R16_SINT: return (Kept){MTLPixelFormatR32Float, CONV_S16, 1, 1};
    case DXGI_FORMAT_R8_UNORM: return (Kept){MTLPixelFormatR8Unorm, 0, 0, 0};
    case DXGI_FORMAT_R8_SNORM: return (Kept){MTLPixelFormatR8Snorm, 0, 0, 0};
    case DXGI_FORMAT_R8_UINT: return (Kept){MTLPixelFormatR32Float, CONV_U8, 1, 1};
    case DXGI_FORMAT_R8_SINT: return (Kept){MTLPixelFormatR32Float, CONV_S8, 1, 1};
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return (Kept){MTLPixelFormatRGB9E5Float, 0, 0, 0};
    case DXGI_FORMAT_R11G11B10_FLOAT: return (Kept){MTLPixelFormatRG11B10Float, 0, 0, 0};
    case DXGI_FORMAT_BC1_UNORM: return (Kept){MTLPixelFormatBC1_RGBA, 0, 0, 0};
    case DXGI_FORMAT_BC1_UNORM_SRGB: return (Kept){MTLPixelFormatBC1_RGBA_sRGB, 0, 0, 0};
    case DXGI_FORMAT_BC2_UNORM: return (Kept){MTLPixelFormatBC2_RGBA, 0, 0, 0};
    case DXGI_FORMAT_BC2_UNORM_SRGB: return (Kept){MTLPixelFormatBC2_RGBA_sRGB, 0, 0, 0};
    case DXGI_FORMAT_BC3_UNORM: return (Kept){MTLPixelFormatBC3_RGBA, 0, 0, 0};
    case DXGI_FORMAT_BC3_UNORM_SRGB: return (Kept){MTLPixelFormatBC3_RGBA_sRGB, 0, 0, 0};
    case DXGI_FORMAT_BC4_UNORM: return (Kept){MTLPixelFormatBC4_RUnorm, 0, 0, 0};
    case DXGI_FORMAT_BC4_SNORM: return (Kept){MTLPixelFormatBC4_RSnorm, 0, 0, 0};
    case DXGI_FORMAT_BC5_UNORM: return (Kept){MTLPixelFormatBC5_RGUnorm, 0, 0, 0};
    case DXGI_FORMAT_BC5_SNORM: return (Kept){MTLPixelFormatBC5_RGSnorm, 0, 0, 0};
    case DXGI_FORMAT_R24G8_TYPELESS: return (Kept){MTLPixelFormatR32Float, CONV_R24, 0, 0};   /* (its depth, as Direct3D's R24_UNORM_X8 view reads it) */
    default: return (Kept){MTLPixelFormatInvalid, 0, 0, 0};
    }
}

/* One level's texels (rows of bw blocks of `bytes`) as Metal keeps them, from src to dst. */
static void convert_texels(const Kept *k, const u8 *src, u8 *dst, size_t blocks, u32 bytes) {
    switch (k->conv) {
    case CONV_R24:
        for (size_t i = 0; i < blocks; i++) { u32 v; memcpy(&v, src + 4 * i, 4); float f = (float)(v & 0xFFFFFF) / 16777215.0f; memcpy(dst + 4 * i, &f, 4); }
        return;
    case CONV_R16_UNORM:
        for (size_t i = 0; i < blocks; i++) { u16 v; memcpy(&v, src + 2 * i, 2); float f = v / 65535.0f; memcpy(dst + 4 * i, &f, 4); }
        return;
    case CONV_RGBA4:   /* DXGI's B4G4R4A4 has blue in the low bits, Metal's ABGR4 alpha */
        for (size_t i = 0; i < blocks; i++) { u16 v; memcpy(&v, src + 2 * i, 2); v = (u16)(v << 4 | v >> 12); memcpy(dst + 2 * i, &v, 2); }
        return;
    case CONV_U8: case CONV_S8: case CONV_U16: case CONV_S16: {
        size_t n = blocks * k->channels;
        for (size_t i = 0; i < n; i++) {
            u32 v = k->conv == CONV_U8 ? src[i] : k->conv == CONV_S8 ? (u32)(i32)(int8_t)src[i]
                  : k->conv == CONV_U16 ? (u32)((const u16 *)(const void *)src)[i] : (u32)(i32)((const int16_t *)(const void *)src)[i];
            memcpy(dst + 4 * i, &v, 4);
        }
        return;
    }
    }
    memcpy(dst, src, blocks * bytes);
}

static u32 kept_bytes(const Kept *k, u32 bytes) {   /* bytes a block takes as kept */
    return k->conv == CONV_R16_UNORM ? 4 : k->channels ? 4u * k->channels : bytes;
}

/* Textures standing in for none (a header the renderer cannot read, a type no view gives): zeros
 * of each type a shader declares, as Direct3D reads an empty slot (a single float channel for
 * shadow slots, which compare with it). */
static id<MTLTexture> dummies[8][2];
static id<MTLTexture> dummy_texture(int type, int shadow) {
    if (type < 0 || type > 7) type = TEX_2D;
    if (dummies[type][shadow]) return dummies[type][shadow];
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = type == TEX_ARRAY_2D ? MTLTextureType2DArray : type == TEX_3D ? MTLTextureType3D : type == TEX_CUBE ? MTLTextureTypeCube
                   : type == TEX_ARRAY_CUBE ? MTLTextureTypeCubeArray : MTLTextureType2D;
    td.pixelFormat = shadow ? MTLPixelFormatR32Float : MTLPixelFormatRGBA32Float;
    td.width = td.height = td.depth = 1;
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    static const float zero[4];
    u32 slices = type == TEX_CUBE || type == TEX_ARRAY_CUBE ? 6 : 1;
    for (u32 s = 0; s < slices; s++)
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:s withBytes:zero bytesPerRow:shadow ? 4 : 16 bytesPerImage:0];
    return dummies[type][shadow] = t;
}

static int depth_like(MTLPixelFormat f) {   /* what a shadow slot (depth2d...) takes */
    return f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatR32Float;
}

/* A texture bound for a draw: the texture, the surface it is (if a render target), and whether its
 * texels are integers (sampled nearest). */
typedef struct Bound { id<MTLTexture> tex; Surface *surface; int integer; } Bound;

/* tex as the type a shader declares (TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_CUBE or TEX_ARRAY_CUBE):
 * itself, or a view of it, or nil where no view gives that. */
static id<MTLTexture> as_type(id<MTLTexture> tex, int want, __strong id<MTLTexture> *cache) {
    MTLTextureType have = tex.textureType, type = want == TEX_ARRAY_2D ? MTLTextureType2DArray : want == TEX_3D ? MTLTextureType3D
                                                : want == TEX_CUBE ? MTLTextureTypeCube : want == TEX_ARRAY_CUBE ? MTLTextureTypeCubeArray
                                                                                            : MTLTextureType2D;
    if (have == type) return tex;
    if (*cache) return *cache;
    NSUInteger layers = have == MTLTextureTypeCube ? 6 : have == MTLTextureTypeCubeArray ? 6 * tex.arrayLength : tex.arrayLength;
    NSUInteger slices = layers;
    if (have == MTLTextureType3D || type == MTLTextureType3D) return nil;
    if (type == MTLTextureType2D) slices = 1;
    if (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) {
        if (tex.width != tex.height || layers < 6 || (have == MTLTextureType2D)) return nil;
        slices = type == MTLTextureTypeCube ? 6 : layers / 6 * 6;
    }
    return *cache = [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:type levels:NSMakeRange(0, tex.mipmapLevelCount)
                                                slices:NSMakeRange(0, slices)];
}

static int depth_sampling = -1;   /* depth buffers as textures (shadow maps, ...); SSBU_NO_DEPTH_SAMPLING or F5: off */
static void note_sampled(Surface *s);   /* (F4, below) */

/* Upload one texture's every level and layer (3D: level 0, every slice) from guest memory, by the
 * upload command buffer (before the work that will read it). */
static id<MTLTexture> upload_texture(const TextureDesc *d, u64 g, const Kept *k, u32 layers, u32 levels) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = k->format;
    td.width = d->width;
    td.height = d->type == TIC_1D || d->type == TIC_1D_ARRAY ? 1 : d->height;   /* (1D: a row, as shaders sample it at y 0.5) */
    td.mipmapLevelCount = levels;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (d->type == TIC_3D) { td.textureType = MTLTextureType3D; td.depth = d->depth; }
    else if (d->type == TIC_CUBE && d->width == d->height) td.textureType = MTLTextureTypeCube;
    else if (d->type == TIC_CUBE_ARRAY && d->width == d->height) { td.textureType = MTLTextureTypeCubeArray; td.arrayLength = layers / 6; }
    else if (layers > 1 || d->type == TIC_2D_ARRAY || d->type == TIC_1D_ARRAY) { td.textureType = MTLTextureType2DArray; td.arrayLength = layers; }
    else td.textureType = MTLTextureType2D;
    if (td.height != d->height) {   /* (a 1D texture's rows past the first are never read: only its first is kept) */
        td.mipmapLevelCount = levels = 1;
    }
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    if (!tex) return nil;
    u32 kb = kept_bytes(k, d->bytes);
    if (d->type == TIC_3D) {   /* level 0 only, which is all a lookup table has */
        u32 bw = DIV_UP_(d->width, d->bw), rows = DIV_UP_(d->height, d->bh);
        size_t n = (size_t)bw * rows * d->depth;
        u8 *buf = malloc(n * d->bytes + 64);
        texture_read_3d(d, g, buf);
        Alloc a = arena((u32)(n * kb));
        convert_texels(k, buf, a.cpu, n, d->bytes);
        free(buf);
        [uploads() copyFromBuffer:a.b->buf sourceOffset:a.off sourceBytesPerRow:bw * kb sourceBytesPerImage:(NSUInteger)bw * kb * rows
                       sourceSize:MTLSizeMake(d->width, d->height, d->depth) toTexture:tex destinationSlice:0 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        gfx_stat_upload_bytes += n * kb;
        gfx_stat_upload_kind[3] += n * kb;
        return tex;
    }
    u8 *buf = NULL;
    size_t cap = 0;
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 w = (d->width >> l) ? d->width >> l : 1, h = (d->height >> l) ? d->height >> l : 1;
            u32 bw = (w + d->bw - 1) / d->bw, bh = (h + d->bh - 1) / d->bh;
            size_t n = (size_t)bw * bh, bytes = n * d->bytes + 64;
            if (bytes > cap) { free(buf); buf = malloc(cap = bytes); }
            texture_read(d, g, layer, l, buf);
            if (td.height == 1 && h > 1) { bh = 1; n = bw; h = 1; }
            Alloc a = arena((u32)(n * kb));
            convert_texels(k, buf, a.cpu, n, d->bytes);
            [uploads() copyFromBuffer:a.b->buf sourceOffset:a.off sourceBytesPerRow:(NSUInteger)bw * kb sourceBytesPerImage:(NSUInteger)bw * kb * bh
                           sourceSize:MTLSizeMake(w, h, 1) toTexture:tex destinationSlice:layer destinationLevel:l
                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            gfx_stat_upload_bytes += n * kb;
            gfx_stat_upload_kind[3] += n * kb;
        }
    free(buf);
    return tex;
}

/* The texture a header names, as the shader's texture k declares it (type: shader_msl_texture's). */
static Bound get_texture(const u32 *tic, int type, int shadow) {
    TextureDesc d;
    int ok = tic_decode(tic, &d);
    u64 g = gpu_va_to_guest(d.address);
    Bound none = {dummy_texture(type, shadow), NULL, 0};
    if (!g) return none;
    /* A render target at this address: use it directly, read as the texture header says (sRGB
     * or not) - the latest one, as memory is reused. A depth buffer only for a depth format of
     * its size (shadow maps): the same memory may hold a colour image by now. */
    if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
    int depth_tic = d.format == 0x29 || d.format == 0x2A || d.format == 0x2B || d.format == 0x2F || d.format == 0x30 || d.format == 0x3A;
    Surface *rt = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->guest != g) continue;
        if (s->depth && (!depth_sampling || !depth_tic || !ok || s->width != d.width || s->height != d.height)) continue;
        if (!rt || s->used > rt->used) rt = s;
    }
    if (rt && draw_logging) note_sampled(rt);
    if (rt) {
        if (type != TEX_2D && type != TEX_ARRAY_2D) return none;   /* (a render target is one 2D image) */
        settle(rt);
        id<MTLTexture> v = surface_srv(rt, rt->depth ? shadow : (int)d.srgb, type == TEX_ARRAY_2D);
        if (shadow && !depth_like(v.pixelFormat)) return none;
        return (Bound){v, rt, rt->kind != 0};
    }
    if (!ok || d.type == TIC_1D_BUFFER || d.bytes == 0) { warn_texture(&d); return none; }
    Kept k = texture_format(d.dxgi);
    if (d.format == 0x3A) k = (Kept){MTLPixelFormatR32Float, CONV_R16_UNORM, 0, 0};   /* (16-bit depth: as a float, which comparing samplers take) */
    if (k.format == MTLPixelFormatInvalid) { warn_texture(&d); return none; }
    u32 layers = d.type == TIC_CUBE ? 6 : d.type == TIC_CUBE_ARRAY ? 6 * d.depth : d.type == TIC_2D_ARRAY || d.type == TIC_1D_ARRAY ? d.depth : 1;
    if (d.type == TIC_3D) layers = 1;
    u64 size = d.type == TIC_3D ? texture_3d_size(&d) : texture_layer_size(&d) * layers;
    int watch = watched(g, size);
    u64 sh = watch ? 0 : sample_hash(g, size);
    Texture *free_slot = NULL, *lru = NULL, *hit = texture_find(g, tic), *t;
    if (hit) {
        if (watch) texture_ask(hit);
        if (watch ? !hit->stale : hit->sample_hash == sh) { hit->used = ++tick; t = hit; goto bind; }
        free_slot = hit;   /* contents changed: rebuild in place */
    } else
        for (u32 i = 0; i < ntextures; i++) {   /* a new one: a free slot, else the least recently used */
            Texture *o = &textures[i];
            if (!o->tex) { if (!free_slot) free_slot = o; continue; }
            if (!lru || o->used < lru->used) lru = o;
        }
    t = free_slot;
    if (!t) t = ntextures < 4096 ? &textures[ntextures++] : lru;
    t->tex = nil;   /* (draws recorded with the old one keep it) */
    for (u32 i = 0; i < 8; i++) t->view[i] = nil;
    if (t != hit) { t->guest = g; memcpy(t->tic, tic, 32); texture_index_add(t); }
    u32 levels = d.type == TIC_3D ? 1 : d.levels, max_levels = 1;
    while ((d.width >> max_levels) || (d.height >> max_levels)) max_levels++;
    if (levels > max_levels) levels = max_levels;
    if (watch) watch_written(g, size, &t->seen);   /* (before reading: a write meanwhile is news next frame) */
    if (!(t->tex = upload_texture(&d, g, &k, layers, levels))) { warn_texture(&d); t->guest = 0; return none; }
    t->guest = g; t->size = size; t->watched = watch; t->stale = 0; t->asked = gfx_stat_presented; t->integer = k.integer;
    memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
    gfx_stat_textures++;
bind:;
    id<MTLTexture> v = as_type(t->tex, type, &t->view[type & 7]);
    if (!v || (shadow && !depth_like(v.pixelFormat))) return none;
    return (Bound){v, NULL, t->integer};
}

/* Samplers by their 32-byte descriptors (TSC), and state objects by their descriptions (below):
 * in hash tables, as draws look them up thousands of times a frame among hundreds kept. */
#define STATE_SLOTS 4096
static u32 key_hash(const void *d, size_t size) {
    const u8 *p = d;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i + 4 <= size; i += 4) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 0x100000001B3ull; }
    return (u32)(h >> 32 ^ h);
}

/* A sampler is the TSC's, for a shadow slot (comparing) or not, for integer texels (nearest) or not. */
typedef struct Sampler { u32 tsc[8], how; id<MTLSamplerState> state; } Sampler;
static Sampler samplers[STATE_SLOTS];
static u32 nsamplers;

static MTLSamplerAddressMode wrap_mode(u32 m) {
    switch (m) {
    case 0: return MTLSamplerAddressModeRepeat;
    case 1: return MTLSamplerAddressModeMirrorRepeat;
    case 3: case 6: return MTLSamplerAddressModeClampToBorderColor;
    case 5: case 7: return MTLSamplerAddressModeMirrorClampToEdge;
    }
    return MTLSamplerAddressModeClampToEdge;
}

/* Metal has three border colours: the nearest of them to the TSC's. */
static MTLSamplerBorderColor border_color(const u32 *rgba) {
    float c[4];
    memcpy(c, rgba, 16);
    static const float preset[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < 3; i++) {
        float dist = 0;
        for (int j = 0; j < 4; j++) { float e = (isnan(c[j]) ? 0 : c[j]) - preset[i][j]; dist += e * e; }
        if (dist < best_d) { best_d = dist; best = i; }
    }
    return best == 0 ? MTLSamplerBorderColorTransparentBlack : best == 1 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
}

static int sampler_lod_bias(void) {   /* (MTLSamplerDescriptor.lodBias: macOS 26, and its SDK) */
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) return 1;
#endif
    return 0;
}

static id<MTLSamplerState> get_sampler(const u32 *tsc, int shadow, int integer) {
    u32 how = (u32)shadow | (u32)integer << 1;
    u32 k = (key_hash(tsc, 32) ^ how * 0x9E3779B9u) & (STATE_SLOTS - 1);
    for (u32 n = 0; n < STATE_SLOTS && samplers[k].state; n++, k = (k + 1) & (STATE_SLOTS - 1))
        if (samplers[k].how == how && !memcmp(samplers[k].tsc, tsc, 32)) return samplers[k].state;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3, aniso = (tsc[0] >> 20) & 7;
    int lin_mag = mag == 2, lin_min = min == 2, lin_mip = mip == 3;
    sd.magFilter = lin_mag ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = lin_min ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = lin_mip ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    if (aniso) {   /* (Direct3D's anisotropic filter is linear throughout; both stop at 16) */
        sd.magFilter = sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.maxAnisotropy = aniso >= 4 ? 16 : 1u << aniso;
    }
    if (integer) {   /* (filtering the bits of integers is meaningless) */
        sd.magFilter = sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        sd.maxAnisotropy = 1;
    }
    sd.sAddressMode = wrap_mode(tsc[0] & 7); sd.tAddressMode = wrap_mode((tsc[0] >> 3) & 7); sd.rAddressMode = wrap_mode((tsc[0] >> 6) & 7);
    sd.borderColor = border_color(&tsc[4]);
    /* Direct3D's comparison function is the TSC's plus one; Metal's is in the same order from 0 */
    if (shadow) sd.compareFunction = (MTLCompareFunction)((tsc[0] >> 10) & 7);
    i32 bias = (i32)(tsc[1] << 7) >> 19;   /* 13-bit signed, 8 fractional bits */
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) sd.lodBias = bias / 256.0f;   /* (before macOS 26 or its SDK: the shader's, see bind_resources) */
#else
    (void)bias;
#endif
    float lo = (tsc[2] & 0xFFF) / 256.0f, hi = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    sd.lodMinClamp = lo < hi ? lo : hi;
    sd.lodMaxClamp = hi;
    id<MTLSamplerState> state = [dev newSamplerStateWithDescriptor:sd];
    if (state && nsamplers < STATE_SLOTS * 3 / 4) {   /* (k: the free slot the search ended at) */
        memcpy(samplers[k].tsc, tsc, 32);
        samplers[k].how = how;
        samplers[k].state = state;
        nsamplers++;
    }
    return state;
}

/* ---- fixed-function state ------------------------------------------------------------------ */
static MTLCompareFunction compare_func(u32 f) {
    if (f >= 0x200 && f <= 0x207) return (MTLCompareFunction)(f - 0x200);   /* GL-style values */
    if (f >= 1 && f <= 8) return (MTLCompareFunction)(f - 1);              /* D3D-style */
    return MTLCompareFunctionAlways;
}

static MTLStencilOperation stencil_op(u32 op) {
    switch (op) {
    case 1: case 0x1E00: return MTLStencilOperationKeep;
    case 2: case 0: return MTLStencilOperationZero;
    case 3: case 0x1E01: return MTLStencilOperationReplace;
    case 4: case 0x1E02: return MTLStencilOperationIncrementClamp;
    case 5: case 0x1E03: return MTLStencilOperationDecrementClamp;
    case 6: case 0x150A: return MTLStencilOperationInvert;
    case 7: case 0x8507: return MTLStencilOperationIncrementWrap;
    case 8: case 0x8508: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

static MTLBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 0x1: case 0x4000: return MTLBlendFactorZero;
    case 0x2: case 0x4001: return MTLBlendFactorOne;
    case 0x3: case 0x4300: return MTLBlendFactorSourceColor;
    case 0x4: case 0x4301: return MTLBlendFactorOneMinusSourceColor;
    case 0x5: case 0x4302: return MTLBlendFactorSourceAlpha;
    case 0x6: case 0x4303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x7: case 0x4304: return MTLBlendFactorDestinationAlpha;
    case 0x8: case 0x4305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x9: case 0x4306: return MTLBlendFactorDestinationColor;
    case 0xA: case 0x4307: return MTLBlendFactorOneMinusDestinationColor;
    case 0xB: case 0x4308: return MTLBlendFactorSourceAlphaSaturated;
    case 0xE: case 0xC001: case 0xC003: return MTLBlendFactorBlendColor;
    case 0xF: case 0xC002: case 0xC004: return MTLBlendFactorOneMinusBlendColor;
    case 0x10: case 0xC900: return MTLBlendFactorSource1Color;
    case 0x11: case 0xC901: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: case 0xC902: return MTLBlendFactorSource1Alpha;
    case 0x13: case 0xC903: return MTLBlendFactorOneMinusSource1Alpha;
    }
    return MTLBlendFactorOne;
}

static MTLBlendFactor alpha_factor(MTLBlendFactor b) {   /* colour factors as Direct3D takes them for alpha */
    switch (b) {
    case MTLBlendFactorSourceColor: return MTLBlendFactorSourceAlpha;
    case MTLBlendFactorOneMinusSourceColor: return MTLBlendFactorOneMinusSourceAlpha;
    case MTLBlendFactorDestinationColor: return MTLBlendFactorDestinationAlpha;
    case MTLBlendFactorOneMinusDestinationColor: return MTLBlendFactorOneMinusDestinationAlpha;
    case MTLBlendFactorSource1Color: return MTLBlendFactorSource1Alpha;
    case MTLBlendFactorOneMinusSource1Color: return MTLBlendFactorOneMinusSource1Alpha;
    case MTLBlendFactorBlendColor: return MTLBlendFactorBlendAlpha;
    case MTLBlendFactorOneMinusBlendColor: return MTLBlendFactorOneMinusBlendAlpha;
    default: return b;
    }
}

static int source1(u8 f) {
    return f == MTLBlendFactorSource1Color || f == MTLBlendFactorOneMinusSource1Color || f == MTLBlendFactorSource1Alpha ||
           f == MTLBlendFactorOneMinusSource1Alpha;
}

/* Blending with the pixel program's second source (dual-source) as Metal allows it - its pipeline
 * validation aborts the process otherwise, debug layer or not (measured): source 1 is target 0's
 * alone, from a program that writes target 1 (then that source, not a target), and a pipeline
 * that takes it has no other colour attachment. NVN, as OpenGL (one dual-source draw buffer),
 * defines no more; Direct3D leaves the rest undefined. So a draw that blends target 0 with source
 * 1 renders into target 0 (and depth) alone, in a render pass of its own; a source-1 factor
 * anywhere else (another target's, or a program's with no second output) takes source 1 as 0. */
static void dual_source(PipeKey *pk, Targets *tg, const Shader *ps) {
    static const u8 factors[4] = {2, 3, 5, 6};   /* (PipeKey.blend's: colour and alpha, source and destination) */
    int dual = 0;
    for (u32 i = 0; i < 8; i++) {
        u8 *bl = pk->blend[i];
        int any = 0;
        for (u32 f = 0; f < 4; f++) any |= source1(bl[factors[f]]);
        if (!bl[0] || !any) continue;
        if (i == 0 && ps && (ps->info.color_mask >> 1 & 1)) { dual = 1; continue; }
        for (u32 f = 0; f < 4; f++) {
            u8 *b = &bl[factors[f]];
            if (source1(*b)) *b = (u8)(*b == MTLBlendFactorSource1Color || *b == MTLBlendFactorSource1Alpha ? MTLBlendFactorZero : MTLBlendFactorOne);
        }
    }
    if (!dual) return;
    pk->rt_types |= SHADER_MSL_DUAL_SOURCE;
    u32 others = 0;
    for (u32 i = 1; i < 8; i++) {
        if (!tg->color[i]) continue;
        tg->color[i] = NULL;
        pk->color[i] = 0;
        pk->mask[i] = 0;
        memset(pk->blend[i], 0, sizeof pk->blend[i]);
        pk->rt_types &= ~(3u << (2 * i));
        others++;
    }
    static u32 told;
    if (others && told++ < 4)
        fprintf(stderr, "[gfx] dual-source blending with %u other render target%s bound: drawn into target 0 alone\n", others, others == 1 ? "" : "s");
}

static MTLBlendOperation blend_op(u32 op) {
    switch (op) {
    case 2: case 0x800A: return MTLBlendOperationSubtract;
    case 3: case 0x800B: return MTLBlendOperationReverseSubtract;
    case 4: case 0x8007: return MTLBlendOperationMin;
    case 5: case 0x8008: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

/* Depth and stencil state, cached by its description (as the samplers above). */
typedef struct DepthKey {
    u8 depth_on, depth_write, depth_func, stencil_on;
    u8 ops[2][4];        /* per face (front, back): fail, depth fail, pass, function */
    u8 read[2], write[2];
} DepthKey;
static struct { DepthKey key; id<MTLDepthStencilState> obj; } depth_cache[STATE_SLOTS];
static u32 depth_state_count;

static id<MTLDepthStencilState> depth_state(const DepthKey *d) {
    u32 k = key_hash(d, sizeof *d) & (STATE_SLOTS - 1);
    for (u32 n = 0; n < STATE_SLOTS && depth_cache[k].obj; n++, k = (k + 1) & (STATE_SLOTS - 1))
        if (!memcmp(&depth_cache[k].key, d, sizeof *d)) return depth_cache[k].obj;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = d->depth_on ? (MTLCompareFunction)d->depth_func : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = d->depth_on && d->depth_write;   /* (Direct3D writes no depth with its test off) */
    if (d->stencil_on)
        for (int f = 0; f < 2; f++) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilFailureOperation = d->ops[f][0];
            s.depthFailureOperation = d->ops[f][1];
            s.depthStencilPassOperation = d->ops[f][2];
            s.stencilCompareFunction = d->ops[f][3];
            s.readMask = d->read[f];
            s.writeMask = d->write[f];
            if (f) dd.backFaceStencil = s;
            else dd.frontFaceStencil = s;
        }
    id<MTLDepthStencilState> o = [dev newDepthStencilStateWithDescriptor:dd];
    if (o && depth_state_count < STATE_SLOTS * 3 / 4) {   /* (k: the free slot the search ended at) */
        depth_cache[k].key = *d;
        depth_cache[k].obj = o;
        depth_state_count++;
    }
    return o;
}

/* ---- draws ------------------------------------------------------------------------------ */
u64 gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target;
/* SSBU_GFX_DEBUG bits: 1 all colour channels written, 2 no depth/stencil test, 4 no culling,
 * 8 no blending, 16 log every draw of one frame (after SSBU_GFX_LOG_AT seconds), 32 magenta,
 * 64 no alpha test (Direct3D's 128 has nothing to turn off here: see gfx_stat_unpacked) */
static u32 gfx_debug(void) {
    static int checked;
    static u32 v;
    if (!checked++ && getenv("SSBU_GFX_DEBUG")) v = (u32)strtoul(getenv("SSBU_GFX_DEBUG"), NULL, 0);
    return v;
}
static u32 instance_counter;

/* Maxwell topologies; 0 for those Metal has none of (adjacency: no geometry stage reads it). */
static int topology(u32 t, MTLPrimitiveType *type) {
    switch (t) {
    case 0: *type = MTLPrimitiveTypePoint; return 1;
    case 1: *type = MTLPrimitiveTypeLine; return 1;
    case 3: *type = MTLPrimitiveTypeLineStrip; return 1;
    case 4: *type = MTLPrimitiveTypeTriangle; return 1;
    case 5: *type = MTLPrimitiveTypeTriangleStrip; return 1;
    }
    if (t >= 10 && t <= 13) {   /* line list/strip and triangle list/strip with adjacency */
        static u32 logged;
        if (!(logged >> t & 1)) { logged |= 1u << t; fprintf(stderr, "[gfx] draws with adjacency (topology %u) skipped: Metal has no such topology\n", t); }
    }
    return 0;
}

/* ---- draw state, set only when it changes ---------------------------------------------
 * A draw sets the whole pipeline; most of it is what the draw before set. What the open render
 * encoder has is mirrored here; a new encoder knows nothing, so its first draw sets everything. */
#define UNKNOWN_PTR ((__bridge id)(void *)1)
static struct {
    __unsafe_unretained id tex[2][32], smp[2][16];
    __unsafe_unretained id pso, ds;
    MTLViewport vp;
    MTLScissorRect scissor;
    int cull, winding, fill, known;
    u32 stencil_ref[2];
    float blend[4];
    int bytes_known[2];      /* params and binds below are what each stage (vertex, pixel) has */
    DrawParams params[2];
    ShaderBinds binds[2];
} cur;

static void forget_draw_state(void) { cur.known = 0; }

/* A stage's per-draw parameters and bind table (set*Bytes): only when they differ from what it has
 * (most draws of a run share them - Metal copies what it is given, 4 KB a draw). */
static void set_bytes(int stage, const DrawParams *params, const ShaderBinds *binds) {
    u32 n = shader_binds_size(stage ? SHADER_PIXEL : SHADER_VERTEX);
    if (cur.bytes_known[stage] && !memcmp(&cur.params[stage], params, sizeof *params) && !memcmp(&cur.binds[stage], binds, n)) {
        gfx_stat_reused++;
        return;
    }
    if (stage) {
        [renc setFragmentBytes:params length:sizeof *params atIndex:SHADER_MSL_PARAMS];
        [renc setFragmentBytes:binds length:n atIndex:SHADER_MSL_BINDS];
    } else {
        [renc setVertexBytes:params length:sizeof *params atIndex:SHADER_MSL_PARAMS];
        [renc setVertexBytes:binds length:n atIndex:SHADER_MSL_BINDS];
    }
    cur.params[stage] = *params;
    memcpy(&cur.binds[stage], binds, n);
    cur.bytes_known[stage] = 1;
}

static void set_texture(int stage, u32 k, id<MTLTexture> t) {
    if (cur.known && cur.tex[stage][k] == t) return;
    cur.tex[stage][k] = t;
    if (stage) [renc setFragmentTexture:t atIndex:k];
    else [renc setVertexTexture:t atIndex:k];
}

static void set_sampler(int stage, u32 k, id<MTLSamplerState> s) {
    if (cur.known && cur.smp[stage][k] == s) return;
    cur.smp[stage][k] = s;
    if (stage) [renc setFragmentSamplerState:s atIndex:k];
    else [renc setVertexSamplerState:s atIndex:k];
}

/* A stage's resources for a draw (or dispatch, as the pixel stage's textures: never). */
typedef struct StageBinds {
    ShaderBinds bt;
    Bound tex[32];
    id<MTLSamplerState> smp[16];
} StageBinds;

/* Bind a stage's constant buffers and textures. */
static void bind_resources(const u32 *regs, const CbBinding cb[STAGES][18], int stage, const Shader *s, DrawParams *params, StageBinds *out) {
    int mstage = stage == SHADER_VERTEX ? STAGE_VERTEX : STAGE_FRAGMENT;
    static u8 data[0x10000];
    for (u32 b = 0; b < 18; b++) {
        if (!(s->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[mstage][b].size;
        if (size > 0x10000) size = 0x10000;
        if (size > s->info.cbuf_extent[b]) size = s->info.cbuf_extent[b];   /* only what the program can read */
        View v = {0};
        if (size >= 0x1000 && cb[mstage][b].address && !draw_logging && (v = kept_cb(cb[mstage][b].address, size)).b) {   /* big ones stay on the GPU */
            out->bt.cb[b] = bind_of(v);
            need(v.b, 0);
            continue;
        }
        if (size > gfx_stat_biggest_cb.size) {   /* (for the statistics: the largest one lately) */
            gfx_stat_biggest_cb.size = size;
            gfx_stat_biggest_cb.bank = b;
            gfx_stat_biggest_cb.extent = s->info.cbuf_extent[b];
            snprintf(gfx_stat_biggest_cb.shader, sizeof gfx_stat_biggest_cb.shader, "%s", s->name);
        }
        if (cb[mstage][b].address && size && !draw_logging) {
            v = upload_gpu(stage, b, cb[mstage][b].address, size);
            out->bt.cb[b] = bind_of(v);
            need(v.b, 0);
            continue;
        }
        if (cb[mstage][b].address && size) read_gpu(cb[mstage][b].address, size, data);
        else { size = 16; memset(data, 0, 16); }
        if (size < 32) memset(data + size, 0, 32 - size);   /* (not the previous buffer's words: the log shows 8) */
        if (draw_logging) {
            const float *f = (const float *)data;
            fprintf(stderr, "[draw]   %s cb%u: va %#llx size %#x guest %#llx: %g %g %g %g | %g %g %g %g\n",
                    stage == SHADER_VERTEX ? "vs" : "ps", b, cb[mstage][b].address, cb[mstage][b].size,
                    gpu_va_to_guest(cb[mstage][b].address), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            const char *more = getenv("SSBU_GFX_LOG_CB");   /* this bank in full (first 64 words) */
            if (more && strtoul(more, NULL, 0) == b) {
                u32 before[64];
                read_gpu(cb[mstage][b].address - 0x100, 0x100, before);
                for (u32 i = 0; i < 64; i += 8)
                    fprintf(stderr, "[draw]     -%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", 0x100 - i * 4, before[i], before[i + 1],
                            before[i + 2], before[i + 3], before[i + 4], before[i + 5], before[i + 6], before[i + 7]);
                const u32 *u = (const u32 *)data;
                for (u32 i = 0; i < 64 && i * 4 < size; i += 8)
                    fprintf(stderr, "[draw]     +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, u[i], u[i + 1], u[i + 2], u[i + 3],
                            u[i + 4], u[i + 5], u[i + 6], u[i + 7]);
            }
        }
        v = upload_raw(stage, b, data, size);
        out->bt.cb[b] = bind_of(v);
        need(v.b, 0);
    }

    /* storage buffers, for global memory loads: their descriptors are in constant buffer 0 */
    if (s->info.sb_count) {
        static u8 *gdata;
        static u32 gcap;
        for (u32 k = 0; k < s->info.sb_count; k++) {
            u32 desc[4] = {0};
            if (cb[mstage][0].address) read_gpu(cb[mstage][0].address + s->info.sb_offset[k], 16, desc);
            u64 va = (u64)desc[1] << 32 | desc[0];
            /* the recorded size is not always the extent read: take the rest of the last page too */
            u64 end = (va + desc[2] + 0xFFF) & ~0xFFFull;
            u32 size = desc[2] && end - va <= (16u << 20) ? (u32)(end - va) : desc[2] ? 16u << 20 : 0;
            params->sb[stage][k][0] = desc[0];
            params->sb[stage][k][1] = desc[1];
            params->sb[stage][k][2] = size;
            if (draw_logging)
                fprintf(stderr, "[draw]   %s storage buffer %u (cb0+%#x): va %#llx size %#x\n", stage == SHADER_VERTEX ? "vs" : "ps", k,
                        s->info.sb_offset[k], va, desc[2]);
            if (!va || !size) continue;
            View v = {0};
            Resident *res = (va & 3) == 0 ? resident_covering(va, va + size) : NULL;   /* written by compute: the GPU's copy */
            if (res && (v = resident_view(res, va, size)).b) { out->bt.gm[k] = bind_of(v); need(v.b, 0); continue; }
            int partly = resident_overlapping(va, size) != NULL;
            if (!partly && !draw_logging) {
                v = upload_gpu(stage, 96 + k, va, size);
                out->bt.gm[k] = bind_of(v);
                need(v.b, 0);
                continue;
            }
            if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
            read_gpu(va, size, gdata);
            if (partly && (v = upload_composed(stage, 96 + k, va, gdata, size)).b) {   /* partly: the guest's data around the GPU's */
                out->bt.gm[k] = bind_of(v);
                need(v.b, 0);
                continue;
            }
            if (draw_logging) {
                const u32 *u = (const u32 *)gdata;
                for (u32 i = 0; i < 32 && i * 4 < size; i += 8)
                    fprintf(stderr, "[draw]     +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, u[i], u[i + 1], u[i + 2], u[i + 3],
                            u[i + 4], u[i + 5], u[i + 6], u[i + 7]);
            }
            v = upload_raw(stage, 96 + k, gdata, size);
            out->bt.gm[k] = bind_of(v);
            need(v.b, 0);
        }
    }

    u32 tex_cb = regs[M_BINDLESS_TEXTURE] & 0x1F;
    u64 tic_pool = (u64)regs[M_TEX_HEADER_POOL] << 32 | regs[M_TEX_HEADER_POOL + 1];
    u64 tsc_pool = (u64)regs[M_TEX_SAMPLER_POOL] << 32 | regs[M_TEX_SAMPLER_POOL + 1];
    u32 nsmp = shader_msl_samplers(&s->info);
    for (u32 k = 0; k < s->info.ntex; k++) {
        u32 handle = 0, tic[8], tsc[8];
        if (tex_cb < 18 && cb[mstage][tex_cb].address) read_gpu(cb[mstage][tex_cb].address + s->info.tex[k].handle_offset, 4, &handle);
        u32 tic_index = handle & 0xFFFFF, tsc_index = regs[M_SAMPLER_BINDING] & 1 ? tic_index : (handle >> 20) & 0xFFF;
        read_gpu(tic_pool + tic_index * 32ull, 32, tic);
        read_gpu(tsc_pool + tsc_index * 32ull, 32, tsc);
        int shadow = s->info.tex[k].shadow != 0;
        out->tex[k] = get_texture(tic, shader_msl_texture(&s->info.tex[k]), shadow);
        if (k < nsmp) out->smp[k] = get_sampler(tsc, shadow, out->tex[k].integer);
        params->swz[stage][k] = (tic[0] >> 19) & 0xFFF;
        /* Before macOS 26 a sampler has no LOD bias: the TSC's (13 bits, signed, 1/256ths) goes to
         * the shader above the swizzle's 12 bits, for the translator to apply (its swizzle reads
         * only those 12) */
        if (!sampler_lod_bias()) params->swz[stage][k] |= ((tsc[1] >> 12) & 0x1FFF) << 12;
        /* a render target's render scale, 4.12 fixed point (0: 1), for the few programs that step
         * in texels (gpu_shader.c: tsc_) - in a storage buffer descriptor's spare fourth word, two
         * textures' a word, the swizzle word's bits being the LOD bias's */
        Surface *rt = out->tex[k].surface;
        if (rt && rt->sx != 1.0f) params->sb[stage][k >> 1][3] |= (u32)(rt->sx * 4096.0f + 0.5f) << ((k & 1) * 16);
        if (draw_logging) {
            TextureDesc d;
            int ok = tic_decode(tic, &d);
            int rt = 0;
            for (u32 i = 0; i < nsurfaces && !rt; i++) rt = surfaces[i].tex && surfaces[i].guest == gpu_va_to_guest(d.address);
            fprintf(stderr, "[draw]   %s tex%u: cb%u+%#x handle %#x -> tic %u tsc %u: fmt %#x %ux%u type %u layout %u va %#llx swz %#x srgb %u "
                            "wrap %u/%u filter %#x %s%s%s\n",
                    stage == SHADER_VERTEX ? "vs" : "ps", k, tex_cb, s->info.tex[k].handle_offset, handle, tic_index, tsc_index,
                    d.format, d.width, d.height, d.type, d.header_version, d.address, (tic[0] >> 19) & 0xFFF, d.srgb, tsc[0] & 7,
                    (tsc[0] >> 3) & 7, tsc[1] & 0xFF, ok ? "" : "(format unknown) ", out->tex[k].tex != dummy_texture(shader_msl_texture(&s->info.tex[k]), shadow) ? "bound" : "NOT BOUND",
                    rt ? " (render target)" : "");
        }
    }
}

/* A stage's textures and samplers to the open render encoder. A render target the draw renders
 * to reads as nothing, as Direct3D unbinds the view of a bound target (and Metal may not read
 * an attachment while it writes it). */
static void set_resources(int stage, const Shader *s, const StageBinds *b) {
    u32 nsmp = shader_msl_samplers(&s->info);
    for (u32 k = 0; k < s->info.ntex; k++) {
        id<MTLTexture> t = b->tex[k].tex;
        if (b->tex[k].surface && attached(b->tex[k].surface)) t = dummy_texture(shader_msl_texture(&s->info.tex[k]), s->info.tex[k].shadow != 0);
        set_texture(stage, k, t);
        if (k < nsmp) set_sampler(stage, k, b->smp[k]);
    }
}

/* SSBU_GPU_TIMES=<second>: the frame from then on timed on the GPU - the time between its first
 * command buffer's start and its present's end, and each command buffer's own - written to the
 * log. (Direct3D's renderer times every draw; Apple's GPUs take timestamps only between encoders.) */
static int gpu_times_state;   /* 0: not asked for, 1: waiting for the second, 2: timing a frame, 3: done */
static u32 gpu_times_draws;

static void gpu_times_frame(void) {   /* (at each present, before its command buffer is committed) */
    if (gpu_times_state == 0) gpu_times_state = getenv("SSBU_GPU_TIMES") ? 1 : 3;
    if (gpu_times_state == 1 && rt_now_ns() > (u64)(atof(getenv("SSBU_GPU_TIMES")) * 1e9)) {
        gpu_times_cmds = [NSMutableArray new];
        gpu_times_draws = 0;
        gpu_times_state = 2;
        return;
    }
    if (gpu_times_state != 2) return;
    gpu_times_state = 3;
    NSArray *list = gpu_times_cmds;
    u32 draws = gpu_times_draws;
    gpu_times_cmds = nil;
    [command() addCompletedHandler:^(id<MTLCommandBuffer> last) {
        for (id<MTLCommandBuffer> b in list) [b waitUntilCompleted];
        double first = 0, busy = 0;
        u32 n = 0;
        for (id<MTLCommandBuffer> b in [list arrayByAddingObject:last]) {
            if (b.GPUEndTime <= 0) continue;
            if (!n++ || b.GPUStartTime < first) first = b.GPUStartTime;
            busy += b.GPUEndTime - b.GPUStartTime;
        }
        fprintf(stderr, "[gpu times] one frame: %.0f us on the GPU from start to present, %u command buffers busy for %.0f us, %u draws\n",
                (last.GPUEndTime - first) * 1e6, n, busy * 1e6, draws);
    }];
}


/* The depth target a pixel shader writing depth needs where the draw has none (never kept: no memory). */
static id<MTLTexture> scratch_depth(u32 w, u32 h) {
    static id<MTLTexture> made[8];
    for (u32 i = 0; i < 8; i++)
        if (made[i] && made[i].width == w && made[i].height == h) return made[i];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeMemoryless;
    td.usage = MTLTextureUsageRenderTarget;
    static u32 next;
    return made[next++ % 8] = [dev newTextureWithDescriptor:td];
}

static void begin_pass_log(const Targets *t) {
    if (!draw_logging) return;
    fprintf(stderr, "[draw]   (a render pass:");
    for (u32 i = 0; i < 8; i++) if (t->color[i]) fprintf(stderr, " rt%u %#llx", i, t->color[i]->guest);
    if (t->depth) fprintf(stderr, " zt %#llx", t->depth->guest);
    fprintf(stderr, ")\n");
}

/* debug: SSBU_GFX_PS_FILE (MSL, with a fragment function ps_main taking the pixel program's PSIn)
 * replaces the pixel shader named by SSBU_GFX_PS_FOR; SSBU_GFX_DEBUG 32 makes every pixel magenta
 * (only for pixel shaders named by SSBU_GFX_MAGENTA, if that is set). Shaders of their own, which
 * pipelines are made with like any other. */
static Shader *debug_shader(const char *src, const char *name) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:msl_options() error:&err];
    if (!lib) { fprintf(stderr, "[gfx] %s does not compile: %s\n", name, first_error(err)); return NULL; }
    Shader *s = calloc(1, sizeof *s);
    s->job.kind = JOB_SHADER;
    s->stage = SHADER_PIXEL;
    s->lib = lib;
    s->cached = 1;
    snprintf(s->name, sizeof s->name, "%s", name);
    publish(&s->state, 1);
    return s;
}

static Shader *debug_pixel_shader(Shader *ps) {
    static const char *ps_for = (const char *)1;
    static Shader *ps_override;
    if (ps_for == (const char *)1) {
        ps_for = getenv("SSBU_GFX_PS_FOR");
        char *src = ps_for && getenv("SSBU_GFX_PS_FILE") ? read_text(getenv("SSBU_GFX_PS_FILE")) : NULL;
        if (src) { ps_override = debug_shader(src, "ps_override"); free(src); }
    }
    if (ps_override && ps && strstr(ps->name, ps_for)) {
        ps_override->info = ps->info;
        return ps_override;
    }
    static const char *magenta_only = (const char *)1;
    if (magenta_only == (const char *)1) magenta_only = getenv("SSBU_GFX_MAGENTA");   /* only draws with this pixel shader */
    if ((gfx_debug() & 32) && ps && (!magenta_only || strstr(ps->name, magenta_only))) {   /* debug: every pixel magenta, to see what geometry lands where */
        static Shader *magenta;
        if (!magenta)
            magenta = debug_shader("#include <metal_stdlib>\nusing namespace metal;\n"
                                   "fragment float4 ps_main() { return float4(1, 0, 1, 1); }\n", "ps_magenta");
        if (magenta) { magenta->info.color_mask = 1; return magenta; }
    }
    return ps;
}

static MTLColorWriteMask write_mask(u32 rgba) {   /* R 1, G 2, B 4, A 8 -> Metal's (red the highest bit) */
    return (rgba & 1 ? MTLColorWriteMaskRed : 0) | (rgba & 2 ? MTLColorWriteMaskGreen : 0) | (rgba & 4 ? MTLColorWriteMaskBlue : 0) |
           (rgba & 8 ? MTLColorWriteMaskAlpha : 0);
}

static void dump_shaders(const u32 *regs);

/* Index data for an indexed draw, into the upload chunks: where (buffer, offset), the type. */
static int index_data(const u32 *regs, GBuf **buf, u32 *offset, MTLIndexType *type) {
    u32 fmt = regs[M_INDEX_BUFFER + 4], isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4, first = regs[M_INDEX_BUFFER + 5];
    u32 n = regs[M_DRAW_INDEX_COUNT];
    u64 va = ((u64)regs[M_INDEX_BUFFER] << 32 | regs[M_INDEX_BUFFER + 1]) + (u64)first * isz;
    if (!n || n > (16u << 20)) return 0;
    u32 bytes = n * (isz == 1 ? 2 : isz);   /* (8-bit indices widen to 16: Metal has none) */
    /* the same indices from the same place as the last indexed draw's, still where they were put */
    static struct { u64 va; u32 bytes, isz; Alloc where; int ok; u64 batch, seq; u8 copy[65536]; } last_ix;
    u64 ig = isz != 1 && bytes <= sizeof last_ix.copy ? gpu_va_to_guest(va) : 0;
    const u8 *isrc = ig && gpu_va_to_guest(va + bytes - 1) == ig + bytes - 1 ? P(const u8, ig) : NULL;
    int again = isrc && last_ix.ok && last_ix.va == va && last_ix.bytes == bytes && last_ix.isz == isz && alive(&last_ix.where) &&
                (untouched_since(last_ix.batch, last_ix.seq, ig, bytes) || !memcmp(isrc, last_ix.copy, bytes));
    *type = isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    if (again) {
        last_ix.batch = gpu_stat_batches;
        last_ix.seq = wrote_seq;
        gfx_stat_reused++;
        *buf = last_ix.where.b;
        *offset = last_ix.where.off;
        return 1;
    }
    Alloc a = arena(bytes);
    gfx_stat_upload_bytes += bytes;
    last_ix.ok = 0;
    if (isrc) {   /* (kept, to compare the next draw's with) */
        memcpy(last_ix.copy, isrc, bytes);
        memcpy(a.cpu, last_ix.copy, bytes);
        last_ix.va = va; last_ix.bytes = bytes; last_ix.isz = isz; last_ix.where = a; last_ix.ok = 1;
        last_ix.batch = gpu_stat_batches; last_ix.seq = wrote_seq;
    } else if (isz == 1) {   /* 8-bit indices widen to 16 */
        static u8 tmp8[1 << 16];
        u32 left = n, done = 0;
        while (left) {
            u32 k = left > sizeof tmp8 ? (u32)sizeof tmp8 : left;
            read_gpu(va + done, k, tmp8);
            for (u32 i = 0; i < k; i++) ((u16 *)(void *)a.cpu)[done + i] = tmp8[i];
            done += k; left -= k;
        }
    } else read_gpu(va, n * isz, a.cpu);
    if (draw_logging) {
        u32 ix[6] = {0};
        for (u32 i = 0; i < 6 && i < n; i++) ix[i] = isz == 4 ? ((u32 *)(void *)a.cpu)[i] : ((u16 *)(void *)a.cpu)[i];
        fprintf(stderr, "[draw]   indices (%u-byte): %u %u %u %u %u %u\n", isz, ix[0], ix[1], ix[2], ix[3], ix[4], ix[5]);
    }
    *buf = a.b;
    *offset = a.off;
    return 1;
}

static void save_sampled(void);

void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    if (!enter()) return;
    kept_flush();
    if (draw_logging) save_sampled();
    dump_shaders(regs);
    u32 begin = regs[M_BEGIN];
    if (begin & (1u << 26)) instance_counter++;
    else if (!(begin & (1u << 27))) instance_counter = 0;

    MTLPrimitiveType prim = MTLPrimitiveTypeTriangle;
    int topo = topology(begin & 0xFFFF, &prim);
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 vs_g = gpu_va_to_guest(region + regs[M_PIPELINE(1) + 1]);
    u64 ps_g = regs[M_PIPELINE(5)] & 1 ? gpu_va_to_guest(region + regs[M_PIPELINE(5) + 1]) : 0;
    /* the pixel shader is asked for once the vertex shader is translated (its outputs are the
     * pixel shader's inputs), not once it is compiled: both compile at the same time */
    Shader *vs_e = vs_g ? find_shader_ex(vs_g, SHADER_VERTEX, 0, cb[STAGE_VERTEX], NULL, 0, 0) : NULL;
    Shader *ps_e = vs_e && state_of(&vs_e->state) != 2 && ps_g ? find_shader_ex(ps_g, SHADER_PIXEL, vs_e->info.outputs, cb[STAGE_FRAGMENT], NULL, 0, 0) : NULL;
    /* The fixed-function alpha test (ui2d's alpha compare: the main menu's stencil masks are cut
     * from a texture's alpha this way) needs the pixel shader's variant that discards; while that
     * compiles, the plain one draws as before. */
    MTLCompareFunction alpha_func = regs[M_ALPHA_TEST] & 1 && !(gfx_debug() & 64) ? compare_func(regs[M_ALPHA_FUNC]) : MTLCompareFunctionAlways;
    Shader *pa_e = alpha_func != MTLCompareFunctionAlways && ps_e && state_of(&ps_e->state) != 2 && (ps_e->info.color_mask & 1)
                 ? find_shader_ex(ps_g, SHADER_PIXEL, vs_e->info.outputs, cb[STAGE_FRAGMENT], NULL, 0, 1) : NULL;
    Shader *vs = vs_e && state_of(&vs_e->state) == 1 ? vs_e : NULL;
    Shader *ps = pa_e && state_of(&pa_e->state) == 1 ? pa_e : ps_e && state_of(&ps_e->state) == 1 ? ps_e : NULL;
    /* no fragment stage (depth only: shadow maps, depth pre-passes) draws with no pixel shader */
    if (!vs || (ps_g && !ps) || !topo) {
        gfx_stat_skipped++;
        if (!topo) gfx_skip_topology++; else gfx_skip_shader++;
        leave();
        return;
    }
    Shader *ps_used = debug_pixel_shader(ps);

    /* render targets */
    Targets tg = {{0}, NULL, 0};
    u32 sel = regs[M_CT_SELECT], count = sel & 15, rt_w = 0, rt_h = 0;
    float rt_sx = 1.0f, rt_sy = 1.0f;   /* the targets' render scale: host pixels per game pixel */
    for (u32 i = 0; i < count && i < 8; i++) {
        u32 ct = (sel >> (4 + 3 * i)) & 7;
        u64 va = (u64)regs[M_CT(ct)] << 32 | regs[M_CT(ct) + 1];
        if (!va || !regs[M_CT(ct) + 4]) continue;
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_CT(ct) + 2], regs[M_CT(ct) + 3], regs[M_CT(ct) + 4], 0);
        if (sf) { tg.color[i] = sf; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = sf->sx; rt_sy = sf->sy; } }
    }
    if (regs[M_ZT_SELECT] & 1) {
        u64 va = (u64)regs[M_ZT_A] << 32 | regs[M_ZT_A + 1];
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_ZT_SIZE], regs[M_ZT_SIZE + 1], regs[M_ZT_A + 2], 1);
        if (sf) { tg.depth = sf; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = sf->sx; rt_sy = sf->sy; } }
    }
    if (!rt_w) { gfx_stat_skipped++; gfx_skip_target++; leave(); return; }
    tg.scratch_depth = !tg.depth && ps_used && ps_used->info.writes_depth;

    /* viewport and the clip-space fix-up */
    DrawParams params;
    memset(&params, 0, sizeof params);
    float sx = f32_of(regs[M_VIEWPORT_SCALE(0)]), sy = f32_of(regs[M_VIEWPORT_SCALE(0) + 1]), sz = f32_of(regs[M_VIEWPORT_SCALE(0) + 2]);
    float ox = f32_of(regs[M_VIEWPORT_SCALE(0) + 3]), oy = f32_of(regs[M_VIEWPORT_SCALE(0) + 4]), oz = f32_of(regs[M_VIEWPORT_SCALE(0) + 5]);
    int lower_left = regs[M_WINDOW_ORIGIN] & 1;
    if (lower_left) { oy = (float)rt_h - oy; sy = -sy; }
    /* Depth: the hardware clips z to [-w, w] or [0, w] (VIEWPORT_Z_CLIP), then maps it with
     * the viewport's z scale and offset. Metal clips to [0, w], as Direct3D does, so a [-w, w]
     * range is halved into it, and the mapping becomes the viewport's depth range. */
    int minus_one = !(regs[M_VIEWPORT_Z_CLIP] & 1);
    float zmin = minus_one ? oz - sz : oz, zmax = oz + sz;
    if (zmin < 0) zmin = 0;
    if (zmin > 1) zmin = 1;
    if (zmax < 0) zmax = 0;
    if (zmax > 1) zmax = 1;
    MTLViewport vp = {ox - fabsf(sx), oy - fabsf(sy), 2 * fabsf(sx), 2 * fabsf(sy), zmin, zmax};
    if (vp.width < 1) { vp.originX = 0; vp.width = rt_w; }
    if (vp.height < 1) { vp.originY = 0; vp.height = rt_h; }
    /* (worked out in the game's pixels, then in the targets' own) */
    vp.originX *= rt_sx; vp.width *= rt_sx;
    vp.originY *= rt_sy; vp.height *= rt_sy;
    params.fix[0] = sx < 0 ? -1.0f : 1.0f;
    params.fix[1] = sy < 0 ? 1.0f : -1.0f;
    params.fix[2] = minus_one ? 0.5f : 1.0f;
    params.fix[3] = minus_one ? 0.5f : 0.0f;
    params.frag[0] = lower_left ? 1.0f : 0.0f;
    params.frag[1] = (float)rt_h;
    /* (the variant's passing outcomes, alpha_test_mask's of a D3D11_COMPARISON_FUNC: Metal's
     * MTLCompareFunction is that less one) */
    if (pa_e && ps == pa_e) { params.frag[2] = f32_of(alpha_test_mask((u32)alpha_func + 1)); params.frag[3] = f32_of(regs[M_ALPHA_REF]); }
    /* Facing: the hardware takes the signed area in window coordinates (x, y_w = scale * ndc + offset)
     * as GL does, counter-clockwise positive, and WINDOW_ORIGIN.FLIP_Y (bit 4) swaps the front face.
     * Metal judges the winding as seen on the target, as Direct3D does: after the fix-up above that
     * is the window area itself for a lower-left origin and its mirror image for an upper-left one. */
    int flipped = (!lower_left) != ((regs[M_WINDOW_ORIGIN] >> 4) & 1);
    i32 sc[4] = {0, 0, 16384, 16384};   /* left, top, right, bottom ("no scissor": all of it) */
    if (regs[M_SCISSOR(0)] & 1) {
        u32 h = regs[M_SCISSOR(0) + 1], v = regs[M_SCISSOR(0) + 2];
        sc[0] = (i32)(h & 0xFFFF); sc[1] = (i32)(v & 0xFFFF); sc[2] = (i32)(h >> 16); sc[3] = (i32)(v >> 16);
        if (lower_left) { i32 t = (i32)rt_h - sc[3]; sc[3] = (i32)rt_h - sc[1]; sc[1] = t; }
        /* scaled outwards: at a fractional scale neighbouring rectangles overlap by a pixel
         * rather than leaving a gap between them */
        sc[0] = (i32)floorf((float)sc[0] * rt_sx); sc[1] = (i32)floorf((float)sc[1] * rt_sy);
        sc[2] = (i32)ceilf((float)sc[2] * rt_sx); sc[3] = (i32)ceilf((float)sc[3] * rt_sy);
    }

    /* rasterizer, depth/stencil, blending */
    MTLCullMode cull = MTLCullModeNone;
    if (regs[M_CULL_ENABLE] & 1) {
        u32 face = regs[M_CULL_FACE];
        cull = face == 0x404 ? MTLCullModeFront : face == 0x405 ? MTLCullModeBack : MTLCullModeNone;
    }
    if (gfx_debug() & 4) cull = MTLCullModeNone;
    int front_ccw = (regs[M_FRONT_FACE] == 0x901) != flipped;
    MTLTriangleFillMode fill = regs[M_FILL_MODE] == 0x1B01 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;

    /* Two-sided stencil: Metal has a reference and masks for each face. Which face is which
     * differs from culling: WINDOW_ORIGIN.FLIP_Y swaps the front face for culling (see the facing
     * above) but not for the stencil state's reference and masks - with it set, the hardware's
     * "back" ones are what the front faces get (seen on the main menu's tiles, whose fills need it). */
    DepthKey dk;
    memset(&dk, 0, sizeof dk);
    Surface *zt = tg.depth;
    dk.depth_on = (regs[M_DEPTH_TEST] & 1) && zt;
    dk.depth_write = regs[M_DEPTH_WRITE] & 1;
    dk.depth_func = (u8)compare_func(regs[M_DEPTH_FUNC]);
    dk.stencil_on = (regs[M_STENCIL_TEST] & 1) && zt && zt->stencil;
    u8 front_ops[4] = {(u8)stencil_op(regs[M_STENCIL_OP_FAIL]), (u8)stencil_op(regs[M_STENCIL_OP_ZFAIL]), (u8)stencil_op(regs[M_STENCIL_OP_ZPASS]),
                       (u8)compare_func(regs[M_STENCIL_FUNC])};
    memcpy(dk.ops[0], front_ops, 4);
    memcpy(dk.ops[1], front_ops, 4);
    int two_sided = regs[M_TWO_SIDED_STENCIL] & 1;
    if (two_sided) {
        u8 back_ops[4] = {(u8)stencil_op(regs[M_BACK_STENCIL_OP_FAIL]), (u8)stencil_op(regs[M_BACK_STENCIL_OP_ZFAIL]),
                          (u8)stencil_op(regs[M_BACK_STENCIL_OP_ZPASS]), (u8)compare_func(regs[M_BACK_STENCIL_FUNC])};
        memcpy(dk.ops[1], back_ops, 4);
    }
    u32 ref[2] = {regs[M_STENCIL_REF] & 0xFF, regs[M_STENCIL_REF] & 0xFF};
    dk.read[0] = dk.read[1] = (u8)regs[M_STENCIL_FUNC_MASK];
    dk.write[0] = dk.write[1] = (u8)regs[M_STENCIL_MASK];
    if (two_sided) {
        static int force_swap = -2;   /* debug: SSBU_STENCIL_SWAP=0/1 overrides */
        if (force_swap == -2) force_swap = getenv("SSBU_STENCIL_SWAP") ? atoi(getenv("SSBU_STENCIL_SWAP")) != 0 : -1;
        int swap_faces = force_swap >= 0 ? force_swap : (int)((regs[M_WINDOW_ORIGIN] >> 4) & 1);
        int b = !swap_faces;   /* (the face that gets the hardware's back reference and masks) */
        ref[b] = regs[M_BACK_STENCIL_REF] & 0xFF;
        dk.read[b] = (u8)regs[M_BACK_STENCIL_FUNC_MASK];
        dk.write[b] = (u8)regs[M_BACK_STENCIL_MASK];
        ref[!b] = regs[M_STENCIL_REF] & 0xFF;
        dk.read[!b] = (u8)regs[M_STENCIL_FUNC_MASK];
        dk.write[!b] = (u8)regs[M_STENCIL_MASK];
    }
    if (!dk.stencil_on) { memset(dk.ops, 0, sizeof dk.ops); dk.read[0] = dk.read[1] = dk.write[0] = dk.write[1] = 0; }
    if (gfx_debug() & 2) { dk.depth_on = 0; dk.stencil_on = 0; memset(dk.ops, 0, sizeof dk.ops); }

    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.vs = vs;
    pk.ps = ps_used;
    for (u32 i = 0; i < 8; i++) {
        Surface *s = tg.color[i];
        if (!s) continue;
        pk.color[i] = (u16)s->view;
        pk.rt_types |= (u32)s->kind << (2 * i);
        u32 mask = regs[M_CT_WRITE(i)];
        u32 rgba = (mask & 1 ? 1 : 0) | (mask & 0x10 ? 2 : 0) | (mask & 0x100 ? 4 : 0) | (mask & 0x1000 ? 8 : 0);
        if (gfx_debug() & 1) rgba = 15;
        if (!ps_used || !(ps_used->info.color_mask >> i & 1)) rgba = 0;   /* (a target the program does not write is left as it is) */
        pk.mask[i] = (u8)write_mask(rgba);
        if (!(regs[M_BLEND_ENABLE(i)] & 1) || (gfx_debug() & 8) || s->kind) continue;   /* (integer targets do not blend) */
        u32 base = regs[M_BLEND_STATE_PER_TARGET] & 1 ? M_BLEND_PER_TARGET(i) + 1 : 0;
        u32 cop, csrc, cdst, aop, asrc, adst;
        if (base) {
            cop = regs[base]; csrc = regs[base + 1]; cdst = regs[base + 2]; aop = regs[base + 3]; asrc = regs[base + 4]; adst = regs[base + 5];
            if (!(regs[base - 1] & 1)) { aop = cop; asrc = csrc; adst = cdst; }   /* the target's own "separate alpha" */
        }
        else {
            cop = regs[M_BLEND_COLOR_OP]; csrc = regs[M_BLEND_COLOR_SRC]; cdst = regs[M_BLEND_COLOR_DST];
            aop = regs[M_BLEND_ALPHA_OP]; asrc = regs[M_BLEND_ALPHA_SRC]; adst = regs[M_BLEND_ALPHA_DST];
            if (!(regs[M_BLEND_SEPARATE_ALPHA] & 1)) { aop = cop; asrc = csrc; adst = cdst; }
        }
        u8 *bl = pk.blend[i];
        bl[0] = 1;
        bl[1] = (u8)blend_op(cop); bl[2] = (u8)blend_factor(csrc); bl[3] = (u8)blend_factor(cdst);
        bl[4] = (u8)blend_op(aop); bl[5] = (u8)alpha_factor(blend_factor(asrc)); bl[6] = (u8)alpha_factor(blend_factor(adst));
    }
    dual_source(&pk, &tg, ps_used);
    if (tg.depth) {
        pk.depth = (u16)tg.depth->storage;
        if (tg.depth->stencil) pk.stencil = (u16)tg.depth->storage;
    } else if (tg.scratch_depth) pk.depth = (u16)MTLPixelFormatDepth32Float;
    float blend_const[4] = {f32_of(regs[M_BLEND_CONST(0)]), f32_of(regs[M_BLEND_CONST(1)]), f32_of(regs[M_BLEND_CONST(2)]), f32_of(regs[M_BLEND_CONST(3)])};
    Pipeline *pipe = get_pipeline(&pk);
    if (!pipe) {   /* (made by the compiler threads: skipped until then) */
        gfx_stat_skipped++;
        gfx_skip_shader++;
        leave();
        return;
    }
    if (draw_logging)
        fprintf(stderr, "[draw] programs: region %#llx vs +%#x ps +%#x\n", region, regs[M_PIPELINE(1) + 1], regs[M_PIPELINE(5) + 1]);
    if (draw_logging)
        fprintf(stderr, "[draw] %s %s rt0 %#llx %ux%u fmt %#x (ct sel %#x) zt %d | vp %.0f,%.0f %.0fx%.0f fix %.2f %.2f %.2f %.2f | "
                "%s %u (first %u) topo %u | mask %#x blend %d | depth %d/%d func %#x | cull %d (en %#x face %#x front %#x origin %#x scale %g %g)\n",
                vs->name, ps ? ps->name : "(no ps)", gpu_va_to_guest((u64)regs[M_CT(sel >> 4 & 7)] << 32 | regs[M_CT(sel >> 4 & 7) + 1]), rt_w, rt_h,
                regs[M_CT(sel >> 4 & 7) + 4], sel, zt != NULL, vp.originX, vp.originY, vp.width, vp.height, params.fix[0], params.fix[1],
                params.fix[2], params.fix[3], indexed ? "indexed" : "arrays", indexed ? regs[M_DRAW_INDEX_COUNT] : regs[M_VERTEX_ARRAY_COUNT],
                indexed ? regs[M_INDEX_BUFFER + 5] : regs[M_VERTEX_ARRAY_START], begin & 0xFFFF, regs[M_CT_WRITE(0)], pk.blend[0][0],
                dk.depth_on, regs[M_DEPTH_WRITE] & 1, regs[M_DEPTH_FUNC], (int)cull, regs[M_CULL_ENABLE], regs[M_CULL_FACE], regs[M_FRONT_FACE],
                regs[M_WINDOW_ORIGIN], f32_of(regs[M_VIEWPORT_SCALE(0)]), f32_of(regs[M_VIEWPORT_SCALE(0) + 1]));
    if (draw_logging) {
        const u8 *bl = pk.blend[0];
        u32 sh = regs[M_SCISSOR(0) + 1], sv = regs[M_SCISSOR(0) + 2];
        fprintf(stderr, "[draw]   scissor %s x %u-%u y %u-%u | stencil %d func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "two-sided %u: back func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "blend src %d dst %d op %d, alpha src %d dst %d op %d | alpha test %u func %#x ref %g\n",
                regs[M_SCISSOR(0)] & 1 ? "on" : "off", sh & 0xFFFF, sh >> 16, sv & 0xFFFF, sv >> 16, dk.stencil_on, regs[M_STENCIL_FUNC],
                regs[M_STENCIL_REF] & 0xFF, regs[M_STENCIL_FUNC_MASK] & 0xFF, regs[M_STENCIL_MASK] & 0xFF, regs[M_STENCIL_OP_FAIL],
                regs[M_STENCIL_OP_ZFAIL], regs[M_STENCIL_OP_ZPASS], regs[M_TWO_SIDED_STENCIL] & 1, regs[M_BACK_STENCIL_FUNC],
                regs[M_BACK_STENCIL_REF] & 0xFF, regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF, regs[M_BACK_STENCIL_MASK] & 0xFF,
                regs[M_BACK_STENCIL_OP_FAIL], regs[M_BACK_STENCIL_OP_ZFAIL], regs[M_BACK_STENCIL_OP_ZPASS], bl[2], bl[3], bl[1], bl[5], bl[6], bl[4],
                regs[M_ALPHA_TEST] & 1, regs[M_ALPHA_FUNC], f32_of(regs[M_ALPHA_REF]));
    }

    /* vertex streams: each attribute the shader reads gets its stream's data (bind table vb[n]) */
    StageBinds vb_, pb_;
    StageBinds *vbind = &vb_, *pbind = &pb_;
    memset(&vbind->bt, 0, sizeof vbind->bt);
    memset(&pbind->bt, 0, sizeof pbind->bt);
    ShaderBind stream_bind[32] = {{0}};
    GBuf *stream_buf[32] = {0};
    static u8 *stream_data;
    static u32 stream_cap;
    for (u32 n = 0; n < 32; n++) {
        if (!(vs->info.inputs >> n & 1)) continue;
        u32 fmt = regs[M_VERTEX_ATTRIBUTE(n)], j = fmt & 0x1F;
        u32 st = regs[M_VERTEX_STREAM(j)];
        u64 start = (u64)regs[M_VERTEX_STREAM(j) + 1] << 32 | regs[M_VERTEX_STREAM(j) + 2];
        u64 limit = (u64)regs[M_VERTEX_STREAM_LIMIT(j)] << 32 | regs[M_VERTEX_STREAM_LIMIT(j) + 1];
        params.attr[n][0] = fmt;
        params.attr[n][1] = st & 0xFFF;
        params.attr[n][2] = regs[M_VERTEX_STREAM_INSTANCED(j)] & 1 ? (regs[M_VERTEX_STREAM(j) + 3] ? regs[M_VERTEX_STREAM(j) + 3] : 1) : 0;
        params.attr[n][3] = (st >> 12) & 1 && start;
        if (!params.attr[n][3]) continue;
        u64 size = limit >= start ? limit - start + 1 : 0;
        if (size > (64u << 20)) size = 64u << 20;
        if (stream_buf[j]) { vbind->bt.vb[n] = stream_bind[j]; continue; }   /* another attribute of the same stream */
        /* A stream inside data the GPU keeps (compute results) is read from there; one only
         * partly inside gets the guest's data around the GPU's. */
        View v = {0};
        Resident *res = (start & 3) == 0 ? resident_covering(start, start + size) : NULL;
        if (res) {
            v = resident_view(res, start, (u32)size);
            if (draw_logging)
                fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) from resident %#llx + %#x\n", n, j, start, size, res->va, res->size);
        }
        if (!v.b) {
            Resident *part = resident_overlapping(start, (u32)size);
            if (!part && !draw_logging) v = upload_gpu(SHADER_VERTEX, 18 + n, start, (u32)size);
            else {
                if (size > stream_cap) { stream_data = realloc(stream_data, size + 16); stream_cap = (u32)size; }
                read_gpu(start, (u32)size, stream_data);
                if (part) v = upload_composed(SHADER_VERTEX, 18 + n, start, stream_data, (u32)size);
                if (part && draw_logging)
                    fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) partly in resident %#llx + %#x\n", n, j, start, size, part->va, part->size);
                if (!v.b) v = upload_raw(SHADER_VERTEX, 18 + n, stream_data, (u32)size);
                if (draw_logging) {
                    u32 off = (fmt >> 7) & 0x3FFF, stride = st & 0xFFF;
                    float f[6] = {0};
                    for (u32 k = 0; k < 3; k++)
                        if (off + stride * k + 8 <= size) memcpy(&f[2 * k], stream_data + off + stride * k, 8);
                    fprintf(stderr, "[draw]   attr%u: fmt %#x (stream %u, offset %u, size %#x, type %u) stride %u, %llu bytes at %#llx: v0 %g %g v1 %g %g v2 %g %g\n",
                            n, fmt, j, off, (fmt >> 21) & 0x3F, (fmt >> 27) & 7, stride, size, start, f[0], f[1], f[2], f[3], f[4], f[5]);
                }
            }
        }
        if (!v.b) continue;
        vbind->bt.vb[n] = stream_bind[j] = bind_of(v);
        stream_buf[j] = v.b;
        need(v.b, 0);
    }
    params.base[0] = regs[M_GLOBAL_BASE_VERTEX];
    params.base[1] = regs[M_GLOBAL_BASE_INSTANCE] + instance_counter;
    /* the fragment position in the game's pixels: [[position]] times these (gpu_shader.c ps_input) */
    params.base[2] = f32_bits(1.0f / rt_sx);
    params.base[3] = f32_bits(1.0f / rt_sy);

    bind_resources(regs, cb, SHADER_VERTEX, vs, &params, vbind);
    if (ps) bind_resources(regs, cb, SHADER_PIXEL, ps, &params, pbind);
    GBuf *ibuf = NULL;
    u32 ioff = 0;
    MTLIndexType itype = MTLIndexTypeUInt16;
    if (indexed && !index_data(regs, &ibuf, &ioff, &itype)) {
        ndraw_bufs[0] = ndraw_bufs[1] = 0;
        gfx_stat_skipped++;
        leave();
        return;
    }

    /* everything at hand (any copies done): the draw itself, in its targets' render pass */
    id<MTLRenderCommandEncoder> e = render_pass(&tg);
    if (ibuf) need(ibuf, 0);
    resident();
    if (!cur.known) {
        cur.pso = cur.ds = UNKNOWN_PTR;
        for (u32 s = 0; s < 2; s++) {
            for (u32 k = 0; k < 32; k++) cur.tex[s][k] = UNKNOWN_PTR;
            for (u32 k = 0; k < 16; k++) cur.smp[s][k] = UNKNOWN_PTR;
        }
        cur.vp.width = -1;
        cur.scissor.width = (NSUInteger)-1;
        cur.cull = cur.winding = cur.fill = -1;
        cur.stencil_ref[0] = cur.stencil_ref[1] = ~0u;
        cur.blend[0] = NAN;
        cur.bytes_known[0] = cur.bytes_known[1] = 0;
        cur.known = 1;
    }
    if (cur.pso != pipe->pso) { [e setRenderPipelineState:pipe->pso]; cur.pso = pipe->pso; }
    id<MTLDepthStencilState> ds = depth_state(&dk);
    if (cur.ds != ds) { [e setDepthStencilState:ds]; cur.ds = ds; }
    if (cur.stencil_ref[0] != ref[0] || cur.stencil_ref[1] != ref[1]) {
        [e setStencilFrontReferenceValue:ref[0] backReferenceValue:ref[1]];
        cur.stencil_ref[0] = ref[0]; cur.stencil_ref[1] = ref[1];
    }
    if (cur.cull != (int)cull) { [e setCullMode:cull]; cur.cull = (int)cull; }
    if (cur.winding != front_ccw) { [e setFrontFacingWinding:front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise]; cur.winding = front_ccw; }
    if (cur.fill != (int)fill) { [e setTriangleFillMode:fill]; cur.fill = (int)fill; }
    if (memcmp(&cur.vp, &vp, sizeof vp)) { [e setViewport:vp]; cur.vp = vp; }
    /* (Metal wants the scissor inside the targets) */
    i32 l = sc[0] < 0 ? 0 : sc[0], t = sc[1] < 0 ? 0 : sc[1], r = sc[2] > (i32)pass.w ? (i32)pass.w : sc[2], b = sc[3] > (i32)pass.h ? (i32)pass.h : sc[3];
    if (r <= l || b <= t) {   /* nothing to draw */
        gfx_stat_drawn++;
        leave();
        return;
    }
    MTLScissorRect scissor = {(NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t)};
    if (memcmp(&cur.scissor, &scissor, sizeof scissor)) { [e setScissorRect:scissor]; cur.scissor = scissor; }
    if (memcmp(cur.blend, blend_const, sizeof blend_const)) {
        [e setBlendColorRed:blend_const[0] green:blend_const[1] blue:blend_const[2] alpha:blend_const[3]];
        memcpy(cur.blend, blend_const, sizeof blend_const);
    }
    set_bytes(0, &params, &vbind->bt);
    set_resources(0, vs, vbind);
    if (ps_used) {
        set_bytes(1, &params, &pbind->bt);
        if (ps) set_resources(1, ps, pbind);
    }
    if (indexed)
        [e drawIndexedPrimitives:prim indexCount:regs[M_DRAW_INDEX_COUNT] indexType:itype indexBuffer:ibuf->buf indexBufferOffset:ioff
                   instanceCount:1 baseVertex:(NSInteger)(i32)regs[M_GLOBAL_BASE_VERTEX] baseInstance:0];
    else
        [e drawPrimitives:prim vertexStart:regs[M_VERTEX_ARRAY_START] vertexCount:regs[M_VERTEX_ARRAY_COUNT]];
    cmd_draws++;
    if (gpu_times_state == 2) gpu_times_draws++;
    gfx_stat_drawn++;
    leave();
}

/* ---- the renderer's own shaders ------------------------------------------------------------ */
static const char BUILTIN_MSL[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint id [[vertex_id]]) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "fragment float4 ps(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return float4(t.sample(s, i.uv).rgb, 1); }\n"
    "fragment float4 psa(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"   /* debug: alpha as grey */
    "  float a = t.sample(s, i.uv).a; return float4(a, a, a, 1); }\n"
    /* a clear of some channels: drawn, with a write mask (into an integer target: the colour's bits, as Direct3D's draw writes them) */
    "fragment float4 psclear(constant float4 &k [[buffer(0)]]) { return k; }\n"
    "fragment uint4 psclear_u(constant float4 &k [[buffer(0)]]) { return as_type<uint4>(k); }\n"
    "fragment int4 psclear_i(constant float4 &k [[buffer(0)]]) { return as_type<int4>(k); }\n"
    /* the 2D engine's blit: destination pixel p reads source point src0 + (p - dst0) * step */
    "struct R { float4 src0_step; float4 dst0_inv; };\n"
    "static float4 blit2d(V i, texture2d<float> t, sampler s, constant R &r) {\n"
    "  return t.sample(s, (r.src0_step.xy + (i.pos.xy - r.dst0_inv.xy) * r.src0_step.zw) * r.dst0_inv.zw); }\n"
    "fragment float4 ps2d(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant R &r [[buffer(0)]]) {\n"
    "  return blit2d(i, t, s, r); }\n"
    "fragment uint4 ps2d_u(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant R &r [[buffer(0)]]) {\n"
    "  return as_type<uint4>(blit2d(i, t, s, r)); }\n"
    "fragment int4 ps2d_i(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant R &r [[buffer(0)]]) {\n"
    "  return as_type<int4>(blit2d(i, t, s, r)); }\n"
    /* the picture shrunk into the window (render scale): each window pixel the average of the
     * picture's pixels under it, n x n bilinear taps spread over its footprint - read through an
     * sRGB view (averaged in linear light), written as sRGB values (the window's are) */
    "struct D { float4 size; float4 taps; };\n"   /* picture w, h, 1/w, 1/h; taps x, y; footprint x, y */
    "fragment float4 psdown(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant D &d [[buffer(0)]]) {\n"
    "  float2 c = i.uv * d.size.xy - d.taps.zw * 0.5f; float3 sum = 0.0f;\n"
    "  for (int y = 0; y < int(d.taps.y); y++) for (int x = 0; x < int(d.taps.x); x++)\n"
    "    sum += t.sample(s, (c + d.taps.zw * (float2(x, y) + 0.5f) / d.taps.xy) * d.size.zw, level(0)).rgb;\n"
    "  float3 l = sum / (d.taps.x * d.taps.y);\n"
    "  return float4(select(1.055f * powr(l, 1.0f / 2.4f) - 0.055f, l * 12.92f, l <= 0.0031308f), 1.0f); }\n"
    /* bytes from one buffer to another at any offsets (blits want whole words) */
    "struct C { uint from, to, n, pad; };\n"
    "kernel void copy_bytes(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant C &c [[buffer(2)]],\n"
    "                       uint i [[thread_position_in_grid]]) { if (i < c.n) dst[c.to + i] = src[c.from + i]; }\n";

/* A pipeline of the vertex shader vs and a fragment function of BUILTIN_MSL, for one target. */
static id<MTLRenderPipelineState> builtin_pipeline(const char *fn, MTLPixelFormat format, MTLColorWriteMask mask) {
    static struct { const char *fn; MTLPixelFormat format; MTLColorWriteMask mask; id<MTLRenderPipelineState> pso; } made[256];
    static u32 n;
    for (u32 i = 0; i < n; i++)
        if (!strcmp(made[i].fn, fn) && made[i].format == format && made[i].mask == mask) return made[i].pso;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [builtin newFunctionWithName:@"vs"];
    d.fragmentFunction = [builtin newFunctionWithName:@(fn)];
    d.colorAttachments[0].pixelFormat = format;
    d.colorAttachments[0].writeMask = mask;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fprintf(stderr, "[gfx] the built-in pipeline %s for format %lu failed: %s\n", fn, (unsigned long)format, first_error(err));
    if (n < 256) { made[n].fn = fn; made[n].format = format; made[n].mask = mask; made[n++].pso = pso; }
    return pso;
}

static void copy_bytes(GBuf *src, u64 so, GBuf *dst, u64 doff, u32 n) {
    static id<MTLComputePipelineState> pso;
    if (!pso) {
        NSError *err = nil;
        pso = [dev newComputePipelineStateWithFunction:[builtin newFunctionWithName:@"copy_bytes"] error:&err];
        if (!pso) { fprintf(stderr, "[gfx] cannot copy bytes: %s\n", first_error(err)); return; }
    }
    struct { u32 from, to, n, pad; } c = {(u32)so, (u32)doff, n, 0};
    id<MTLComputeCommandEncoder> e = computes();
    [e setComputePipelineState:pso];
    [e setBuffer:src->buf offset:0 atIndex:0];
    [e setBuffer:dst->buf offset:0 atIndex:1];
    [e setBytes:&c length:sizeof c atIndex:2];
    [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(n < 256 ? n : 256, 1, 1)];
}

/* A full-target triangle of the vertex shader vs into the open render pass, with the fragment
 * function fn: the encoder's state is the draw's own after it. */
static void draw_builtin(id<MTLRenderCommandEncoder> e, const char *fn, MTLPixelFormat format, MTLColorWriteMask mask, MTLViewport vp) {
    id<MTLRenderPipelineState> pso = builtin_pipeline(fn, format, mask);
    if (!pso) return;
    static id<MTLDepthStencilState> none;
    if (!none) none = [dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    [e setRenderPipelineState:pso];
    [e setDepthStencilState:none];
    [e setCullMode:MTLCullModeNone];
    [e setTriangleFillMode:MTLTriangleFillModeFill];
    [e setViewport:vp];
    [e setScissorRect:(MTLScissorRect){0, 0, pass.w, pass.h}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    forget_draw_state();
}

/* ---- the 3D engine's work -------------------------------------------------------------------- */
#define RT(i, field) regs[0x200 + (i) * 0x10 + (field)]

/* A clear of only some colour channels (mask: R 1, G 2, B 4, A 8): a full-target triangle of the
 * clear colour, written through that channel mask. */
static void clear_channels(Surface *s, const float color[4], u32 mask) {
    Targets t = {{s}, NULL, 0};
    id<MTLRenderCommandEncoder> e = render_pass(&t);
    [e setFragmentBytes:color length:16 atIndex:0];
    draw_builtin(e, s->kind == 1 ? "psclear_u" : s->kind == 2 ? "psclear_i" : "psclear", s->view, write_mask(mask),
                 (MTLViewport){0, 0, s->hw, s->hh, 0, 1});
}

void gfx_clear(const u32 *regs, u32 arg) {
    if (!enter()) return;
    if (draw_logging) save_sampled();
    gfx_stat_clears++;
    u32 rt = (arg >> 6) & 0xF;
    if (draw_logging)
        fprintf(stderr, "[draw] clear rt %u (%#llx %ux%u fmt %#x) channels %#x%s, zt %#llx: color %g %g %g %g depth %g stencil %#x\n", rt,
                gpu_va_to_guest((u64)RT(rt & 7, 0) << 32 | RT(rt & 7, 1)), RT(rt & 7, 2), RT(rt & 7, 3), RT(rt & 7, 4), (arg >> 2) & 0xF,
                arg & 3 ? (arg & 1 ? " +depth" : " +stencil") : "", gpu_va_to_guest((u64)regs[0x3F8] << 32 | regs[0x3F9]),
                f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363]), f32_of(regs[0x364]), regs[0x368] & 0xFF);
    if (arg & 0x3C && rt < 8) {   /* any of R, G, B, A */
        u64 va = (u64)RT(rt, 0) << 32 | RT(rt, 1);
        Surface *s = surface(gpu_va_to_guest(va), RT(rt, 2), RT(rt, 3), RT(rt, 4), 0);
        if (s) {
            float color[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            if (((arg >> 2) & 0xF) == 0xF) {   /* all of it: the next pass's load action */
                if (attached(s)) end_encoding();
                s->clear |= 1;
                memcpy(s->clear_color, color, sizeof color);
            } else clear_channels(s, color, (arg >> 2) & 0xF);   /* only some channels: a masked draw */
        }
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) {
            if (attached(s)) end_encoding();
            float z = f32_of(regs[0x364]);
            if (arg & 1) { s->clear |= 2; s->clear_depth = z > 0 ? (z < 1 ? z : 1) : 0; }   /* (as Direct3D's: within [0, 1]) */
            if ((arg & 2) && s->stencil) { s->clear |= 4; s->clear_stencil = regs[0x368] & 0xFF; }
        }
    }
    leave();
}

/* SSBU_DUMP_SHADERS=<dir>: save each distinct shader program a draw uses, as
 * <pipeline stage>_<hash>.bin (the 0x50-byte program header, then the code). */
static void dump_shaders(const u32 *regs) {
    static const char *dir;
    static int checked;
    static u64 seen[8192];
    static u32 nseen;
    if (!checked++) dir = getenv("SSBU_DUMP_SHADERS");
    if (!dir) return;
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    for (u32 j = 0; j < 6; j++) {
        if (j != 1 && !(regs[M_PIPELINE(j)] & 1)) continue;   /* vertex stage B is always on */
        u64 g = gpu_va_to_guest(region + regs[M_PIPELINE(j) + 1]);
        if (!g) continue;
        u32 size = 0x50;   /* up to the first EXIT that no branch jumps past, then the padding loop */
        while (size < 0x40000 && ld64(g + size) != 0xE2400FFFFF87000Full) size += 8;
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < size; i++) h = (h ^ ld8(g + i)) * 1099511628211ull;
        h ^= j;
        int known = 0;
        for (u32 i = 0; i < nseen; i++) known |= seen[i] == h;
        if (known || nseen == 8192) continue;
        seen[nseen++] = h;
        char path[1024];
        snprintf(path, sizeof path, "%s/%u_%016llx.bin", dir, j, h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(P(void, g), 1, size, f); fclose(f); }
    }
}

/* ---- captures ---------------------------------------------------------------------------- */
/* A 2D texture's first level read back as RGBA8 rows (malloc'd), drawn into an RGBA8 image first
 * with the blit fragment function fn (so any format can be saved); NULL fn: it is RGBA8 already. */
static u8 *read_rgba8(id<MTLTexture> src, const char *fn, u32 *w, u32 *h) {
    u32 W = (u32)src.width, H = (u32)src.height;
    id<MTLTexture> img = src;
    if (fn) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        img = [dev newTextureWithDescriptor:td];
        if (!img) return NULL;
        end_encoding();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = img;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        renc = [command() renderCommandEncoderWithDescriptor:rp];
        enc_serial++;
        pass.w = W; pass.h = H;
        [renc setFragmentTexture:src atIndex:0];
        [renc setFragmentSamplerState:linear_sampler atIndex:0];
        draw_builtin(renc, fn, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, (MTLViewport){0, 0, W, H, 0, 1});
        end_encoding();
    }
    id<MTLBuffer> out = [dev newBufferWithLength:(NSUInteger)W * H * 4 options:MTLResourceStorageModeShared];
    if (!out) return NULL;
    [blits() copyFromTexture:img sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                    toBuffer:out destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:(NSUInteger)W * H * 4];
    gfx_stat_gpu_waits++;
    submit(1);
    u8 *px = malloc((size_t)W * H * 4);
    if (px) memcpy(px, out.contents, (size_t)W * H * 4);
    *w = W;
    *h = H;
    return px;
}

/* RGBA8 rows into a 24-bit BMP file. */
static void write_bmp(const char *path, const u8 *px, u32 w, u32 h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    u32 row = w * 3, pad = (4 - row % 4) % 4, data = (row + pad) * h;
    u8 hdr[54] = {'B', 'M'};
    u32 v32;
    i32 s32;
    u16 v16;
    v32 = 54 + data; memcpy(hdr + 2, &v32, 4);
    v32 = 54; memcpy(hdr + 10, &v32, 4);
    v32 = 40; memcpy(hdr + 14, &v32, 4);
    s32 = (i32)w; memcpy(hdr + 18, &s32, 4);
    s32 = -(i32)h; memcpy(hdr + 22, &s32, 4);   /* (top row first) */
    v16 = 1; memcpy(hdr + 26, &v16, 2);
    v16 = 24; memcpy(hdr + 28, &v16, 2);
    v32 = data; memcpy(hdr + 34, &v32, 4);
    fwrite(hdr, 1, 54, f);
    u8 *line = malloc(row + pad + 4);
    for (u32 y = 0; y < h; y++) {
        const u8 *p = px + (size_t)y * w * 4;
        for (u32 x = 0; x < w; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
        memset(line + row, 0, pad);
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

/* A texture's first level saved as a BMP (fn: as read_rgba8 takes it). */
static void save_bmp(id<MTLTexture> tex, const char *fn, const char *path) {
    u32 w, h;
    u8 *px = read_rgba8(tex, fn, &w, &h);
    if (px) { write_bmp(path, px, w, h); free(px); }
}

/* A surface drawn into an RGBA8 image (so any format can be saved) with the given blit fragment
 * function, saved as a BMP. */
static void save_surface_bmp(Surface *s, const char *fn, const char *path) {
    settle(s);
    id<MTLTexture> raw = surface_srv(s, 0, 0);   /* the bytes as stored */
    if (raw) save_bmp(raw, fn, path);
}

/* With SSBU_CAPTURE_SURFACES as well: every colour render target, drawn into an RGBA8
 * image first (so any format can be saved), as surf_<frame>_<guest address>.bmp. */
static void capture_textures(const char *dir, int frame);
static void capture_surfaces(const char *dir, int frame) {
    capture_textures(dir, frame);
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->depth) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/surf_%03d_%llx_fmt%x.bmp", dir, frame, s->guest, s->format);
        save_surface_bmp(s, "ps", path);
    }
}

/* The plain 2D textures in the cache, drawn into RGBA8 images like the surfaces
 * (tex_<frame>_<guest address>_fmt<format>.bmp). */
static void capture_textures(const char *dir, int frame) {
    u32 saved = 0;
    const char *only = getenv("SSBU_CAPTURE_TEX_FMT");   /* just this texture format */
    for (u32 i = 0; i < ntextures && saved < 40; i++) {
        Texture *t = &textures[i];
        if (!t->tex || t->tex.textureType != MTLTextureType2D) continue;
        if (only && (t->tic[0] & 0x7F) != strtoul(only, NULL, 0)) continue;
        /* a plain 2D view of level 0, whatever view the shaders use */
        id<MTLTexture> view = [t->tex newTextureViewWithPixelFormat:t->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                             slices:NSMakeRange(0, 1)];
        if (!view) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, (u32)view.width, (u32)view.height);
        save_bmp(view, "ps", path);
        snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u_alpha.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, (u32)view.width, (u32)view.height);
        save_bmp(view, "psa", path);   /* and its alpha */
        saved++;
    }
}

/* F4: render targets that draws sample are saved as they are at that moment (a frame's
 * intermediate images: effects buffers, bloom levels), colour and alpha, as
 * in_<n>_<guest>_fmt<f>_<w>x<h>.bmp; the draw log says which draw sampled each. They are
 * saved just before the next piece of GPU work, so the sampling draw is undisturbed. */
static char sampled_dir[160];
static Surface *sampled_pending[16];
static int sampled_seq_of[16];
static u32 nsampled_pending;
static struct { u64 guest, used; } sampled_saved[256];
static u32 nsampled_saved;
static int sampled_seq;

static void note_sampled(Surface *s) {
    if (!sampled_dir[0] || nsampled_saved >= 256) return;
    for (u32 i = 0; i < nsampled_saved; i++)
        if (sampled_saved[i].guest == s->guest && sampled_saved[i].used == s->used) return;   /* (unchanged since saved) */
    for (u32 i = 0; i < nsampled_pending; i++) if (sampled_pending[i] == s) return;
    if (nsampled_pending == 16) return;
    sampled_saved[nsampled_saved].guest = s->guest;
    sampled_saved[nsampled_saved++].used = s->used;
    sampled_seq_of[nsampled_pending] = sampled_seq;
    sampled_pending[nsampled_pending++] = s;
    fprintf(stderr, "[draw]   (samples render target %#llx %ux%u fmt %#x: saved as in_%03d, %ux%u on the host)\n", s->guest, s->width,
            s->height, s->format, sampled_seq++, s->hw, s->hh);
}

static void save_sampled(void) {
    for (u32 i = 0; i < nsampled_pending; i++) {
        Surface *s = sampled_pending[i];
        if (!s->tex) continue;
        char path[300];
        snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
        save_surface_bmp(s, "ps", path);
        if (!s->depth) {
            snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u_alpha.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
            save_surface_bmp(s, "psa", path);
        }
    }
    nsampled_pending = 0;
}

/* SSBU_CAPTURE=<dir>: save the game's picture there every few seconds (frame_N.bmp); with
 * SSBU_CAPTURE_WINDOW too, the window's picture as well, as fitted into it (window_N.bmp, saved
 * by gfx_present once it is drawn: capture_window names it). */
static char capture_window[1100];
static void capture(void) {
    static const char *dir;
    static int checked, count;
    static u64 last;
    static int most = 40;   /* SSBU_CAPTURE_MAX: how many (6 MB each at 1920x1080, 4 times that at render scale 2) */
    static u64 every = 3000000000ull;   /* SSBU_CAPTURE_EVERY: seconds apart */
    if (!checked++) {
        dir = getenv("SSBU_CAPTURE");
        if (getenv("SSBU_CAPTURE_MAX")) most = atoi(getenv("SSBU_CAPTURE_MAX"));
        if (getenv("SSBU_CAPTURE_EVERY")) every = (u64)(atof(getenv("SSBU_CAPTURE_EVERY")) * 1e9);
    }
    if (!dir || count >= most || rt_now_ns() - last < every) return;
    last = rt_now_ns();
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%03d.bmp", dir, count);
    save_bmp(final_tex, NULL, path);
    if (getenv("SSBU_CAPTURE_WINDOW")) snprintf(capture_window, sizeof capture_window, "%s/window_%03d.bmp", dir, count);
    if (getenv("SSBU_CAPTURE_SURFACES")) capture_surfaces(dir, count);
    count++;
}

static u32 pixel_bytes(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float:
    case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatDepth16Unorm: return 2;
    case MTLPixelFormatRG32Float: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float: return 8;
    case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return 16;
    default: return 4;   /* (8-bit RGBA and BGRA, 10-bit, 11-bit float, 16-bit pairs, 32-bit singles, depth: its 32-bit depth) */
    }
}

int gfx_metal_read_surface(u64 guest, int depth, void *out, u32 size, u32 *w, u32 *h, u32 *format) {
    if (!enter()) return 0;
    Surface *s = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && surfaces[i].guest == guest && surfaces[i].depth == (u32)!!depth && (!s || surfaces[i].used > s->used)) s = &surfaces[i];
    if (!s) { leave(); return 0; }
    settle(s);
    u32 bpp = pixel_bytes(s->storage), row = s->hw * bpp;
    id<MTLBuffer> buf = [dev newBufferWithLength:(NSUInteger)row * s->hh options:MTLResourceStorageModeShared];
    MTLBlitOption opt = s->storage == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
    [blits() copyFromTexture:s->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(s->hw, s->hh, 1)
                    toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:(NSUInteger)row * s->hh options:opt];
    submit(1);
    size_t n = (size_t)row * s->hh;
    memcpy(out, buf.contents, n < size ? n : size);
    *w = s->hw; *h = s->hh; *format = (u32)s->storage;
    leave();
    return 1;
}

/* ---- the 2D engine ------------------------------------------------------------------------- */
/* Source and destination surfaces (format, layout, block size, depth, layer, pitch, width,
 * height, address) at methods 0x80 and 0x8C; the blit rectangle and the 32.32 fixed-point source
 * position and step at 0x22C-0x237. */
u64 gfx_stat_blits;

void gfx_blit2d(const u32 *regs) {
    if (!enter()) return;
    if (draw_logging) save_sampled();
    gfx_stat_blits++;
    u64 src_va = (u64)regs[0x94] << 32 | regs[0x95], dst_va = (u64)regs[0x88] << 32 | regs[0x89];
    u64 src_g = gpu_va_to_guest(src_va), dst_g = gpu_va_to_guest(dst_va);
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && !surfaces[i].depth && surfaces[i].guest == src_g) src = &surfaces[i];
    Surface *dst = surface(dst_g, regs[0x86], regs[0x87], regs[0x80], 0);
    if (draw_logging)
        fprintf(stderr, "[draw] 2D blit %#llx (%ux%u fmt %#x, %s) -> %#llx (%ux%u fmt %#x): dst rect %u,%u %ux%u\n", src_g, regs[0x92],
                regs[0x93], regs[0x8C], src ? "on the host" : "in memory", dst_g, regs[0x86], regs[0x87], regs[0x80], regs[0x22C],
                regs[0x22D], regs[0x22E], regs[0x22F]);
    if (!src || !dst) { leave(); return; }   /* sources only in guest memory: not yet */
    if (!regs[0x22E] || !regs[0x22F]) { leave(); return; }   /* (nothing to draw) */
    double du = regs[0x231] + regs[0x230] / 4294967296.0, dv = regs[0x233] + regs[0x232] / 4294967296.0;
    double sx = regs[0x235] + regs[0x234] / 4294967296.0, sy = (i32)regs[0x237] + regs[0x236] / 4294967296.0;
    u32 dx = regs[0x22C], dy = regs[0x22D], dw = regs[0x22E], dh = regs[0x22F];
    settle(src);
    /* draw the source rectangle into the destination rectangle (a source that is the destination
     * reads as nothing, as Direct3D unbinds a bound target's view) */
    id<MTLTexture> in = src == dst ? dummy_texture(TEX_2D, 0) : surface_srv(src, is_srgb(dst->view), 0);   /* read as the destination writes: bytes unchanged */
    Targets t = {{dst}, NULL, 0};
    id<MTLRenderCommandEncoder> e = render_pass(&t);
    /* each destination pixel samples the source at src0 + (pixel - dst0) * step, in source pixels:
     * the game's, whatever either surface's render scale (the shader works in the destination's
     * host pixels, so dst0 and step are given in those, and the source is read normalized) */
    float r[8] = {(float)sx, (float)sy, (float)(du / dst->sx), (float)(dv / dst->sy), (float)dx * dst->sx, (float)dy * dst->sy,
                  1.0f / (float)src->width, 1.0f / (float)src->height};
    [e setFragmentBytes:r length:sizeof r atIndex:0];
    [e setFragmentTexture:in atIndex:0];
    [e setFragmentSamplerState:(regs[0x223] >> 4) & 1 ? linear_sampler : point_sampler atIndex:0];
    /* (the rectangles are in the game's pixels; the destination's own are dst->sx, sy times those) */
    draw_builtin(e, dst->kind == 1 ? "ps2d_u" : dst->kind == 2 ? "ps2d_i" : "ps2d", dst->view, MTLColorWriteMaskAll,
                 (MTLViewport){dx * dst->sx, dy * dst->sy, dw * dst->sx, dh * dst->sy, 0, 1});
    leave();
}

/* ---- compute --------------------------------------------------------------------------- */
/* A launch runs its program over the grid. Global memory is the storage buffers described in
 * constant buffer 0, given to it as writable buffers; while it is learnt (its first runs),
 * whatever the program changed is written back to guest memory afterwards - word by word against
 * what was uploaded, so that nothing the CPU wrote meanwhile elsewhere in those buffers is
 * overwritten - and the buffers it writes are noted: from then on what it writes stays on the GPU
 * (resident regions). (The game skins its fighters this way: the draws that follow read the
 * results as vertex data.) */
u64 gfx_stat_dispatched;
static u64 first_dispatch_ns;

/* Storage buffers a compute program only reads (the fighters' bind pose and skin weights,
 * the whole model's for every part skinned): kept on the GPU for as long as guest memory under
 * them goes unwritten (the write watch says), instead of uploaded in full for every dispatch. A
 * buffer a dispatch recorded uses is not written: new data goes into a new copy. */
typedef struct InputBuf { u64 va, guest; u32 size; GBuf *buf; int bound; u64 used, seen; u8 *shadow; } InputBuf;
static InputBuf inputs[128];
static u64 input_clock;

static void input_drop(InputBuf *b) {
    pool_put(b->buf, 1);   /* (not handed out again before what is recorded is done) */
    free(b->shadow);
    memset(b, 0, sizeof *b);
}

static View input_view(u64 va, u32 size) {
    static int verify = -1;   /* SSBU_INPUT_VERIFY (debugging): keep a copy of what each holds, and report uses that find memory different */
    if (verify < 0) verify = getenv("SSBU_INPUT_VERIFY") != NULL;
    u64 g = gpu_va_to_guest(va);
    if (!g || !size || !watched(g, size) || gpu_va_to_guest(va + size - 1) != g + size - 1) return (View){0};   /* one watched piece only */
    InputBuf *b = NULL, *lru = &inputs[0];
    for (u32 i = 0; i < 128 && !b; i++) {
        if (inputs[i].buf && inputs[i].va == va && inputs[i].size == size && inputs[i].guest == g) b = &inputs[i];
        else if (inputs[i].used < lru->used) lru = &inputs[i];
    }
    u32 cap = (size + 15) & ~15u;
    if (b) {
        if (watch_written(g, size, &b->seen)) {   /* written since: take it again */
            if (b->bound) {
                GBuf *n = pool_get(cap, 1);
                if (!n) return (View){0};
                pool_put(b->buf, 1);
                b->buf = n;
                b->bound = 0;
            }
            memcpy(b->buf->cpu, P(void, g), size);
            gfx_stat_upload_bytes += size;
            if (b->shadow) memcpy(b->shadow, b->buf->cpu, size);
        } else if (b->shadow && memcmp(b->shadow, P(void, g), size)) {
            static int reported;
            u32 o = 0;
            while (o < size && b->shadow[o] == P(u8, g)[o]) o++;
            if (reported++ < 200)
                fprintf(stderr, "[input] va %#llx (%#x bytes) +%#x: kept %02x, memory %02x (submission %llu)\n", va, size, o, b->shadow[o], P(u8, g)[o],
                        gpu_stat_batches);
            memcpy(b->shadow, P(void, g), size);
        }
        b->used = ++input_clock;
        b->bound = 1;
        return (View){b->buf, 0, cap};
    }
    b = lru;
    input_drop(b);
    if (!(b->buf = pool_get(cap, 1))) return (View){0};
    memset(b->buf->cpu + size, 0, b->buf->size - size);
    watch_written(g, size, &b->seen);   /* (before reading: a write meanwhile is news next time) */
    read_gpu(va, size, b->buf->cpu);
    if (verify && (b->shadow = malloc(size))) memcpy(b->shadow, b->buf->cpu, size);
    gfx_stat_upload_bytes += size;
    b->va = va; b->guest = g; b->size = size; b->used = ++input_clock; b->bound = 1;
    return (View){b->buf, 0, cap};
}

/* ---- memory taken away --------------------------------------------------------------------
 * Resident regions, kept constant buffers and compute inputs are made from the guest memory at
 * a GPU address, and outlive the draws that made them. When the game unmaps that address, or
 * the memory under it, the device layer says so (hle_nv.c, once the GPU thread's queued work
 * has run) and they are dropped: their next use makes them again from what is there then.
 * Before, nothing told them: a region went on checking memory that was gone (gpu_d3d11.c: the
 * graphics driver crashed reading it on the vsync thread, in players' games after 20-40
 * minutes), and a buffer the game mapped at the same address later read the old region's GPU
 * data instead of its own. Dropped under the lock, as the outermost entry point leaves (for
 * these calls, their own): no entry point keeps a pointer to a region or buffer across leave(),
 * and resident_drop moves regions around. What is dropped may still be bound by recorded work:
 * gbuf_free and pool_put keep its memory until that is done. */
u64 gfx_stat_unmapped, gfx_stat_unmap_dropped;   /* (notices the renderer had; what they dropped) */

static int spans(Span s, u64 a, u64 n) { return a && n && a < s.hi && s.lo < a + n; }

static void forget_unmapped(void) {
    Span va = unmapped_va, g = unmapped_guest;
    unmapped_va = unmapped_guest = (Span){0, 0};
    for (u32 i = 0; i < nresidents;) {
        Resident *r = &residents[i];
        if (spans(va, r->va, r->size) || spans(g, r->guest, r->size)) { resident_drop(r); gfx_stat_unmap_dropped++; }
        else i++;   /* (resident_drop moved the last region here) */
    }
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) {   /* (a kept buffer holds the part mapped in one piece) */
        KeptCb *e = &kept[i];
        if (e->buf && (spans(va, e->va, e->mapped) || spans(g, e->guest, e->mapped))) { kept_drop(e); gfx_stat_unmap_dropped++; }
    }
    for (u32 i = 0; i < 128; i++) {
        InputBuf *b = &inputs[i];
        if (b->buf && (spans(va, b->va, b->size) || spans(g, b->guest, b->size))) { input_drop(b); gfx_stat_unmap_dropped++; }
    }
}

static void note_unmapped(Span *s, u64 lo, u64 size) {   /* (one waiting already: widened over both) */
    if (!size) return;
    if (!s->hi || lo < s->lo) s->lo = lo;
    if (lo + size > s->hi) s->hi = lo + size;
    gfx_stat_unmapped++;
}

void gfx_gpu_unmapped(u64 va, u64 size) {
    if (!ready || !enter()) return;   /* (nothing is kept before the renderer is up) */
    note_unmapped(&unmapped_va, va, size);
    leave();
}

void gfx_guest_unmapped(u64 addr, u64 size) {
    if (!ready || !enter()) return;
    note_unmapped(&unmapped_guest, addr, size);
    leave();
}

void gfx_dispatch(const u32 *regs, const u32 *q) {
    if (!enter()) return;
    kept_flush();
    if (draw_logging) save_sampled();
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 g = gpu_va_to_guest(region + q[8]);
    u32 local[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    u32 grid[3] = {q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16};
    u32 shared = q[17] & 0x3FFFF;
    CbBinding cb[18];
    memset(cb, 0, sizeof cb);
    for (u32 i = 0; i < 8; i++)
        if (q[20] >> i & 1) { cb[i].address = (u64)(q[30 + 2 * i] & 0xFF) << 32 | q[29 + 2 * i]; cb[i].size = q[30 + 2 * i] >> 15; }
    if (!g || !grid[0] || !grid[1] || !grid[2] || !local[0] || !local[1] || !local[2]) { leave(); return; }
    Shader *cs = get_shader_ex(g, SHADER_COMPUTE, 0, cb, local, shared);
    if (!cs) { gfx_skip_shader++; leave(); return; }
    if (cs->cs.maxTotalThreadsPerThreadgroup < (NSUInteger)local[0] * local[1] * local[2]) {   /* (a workgroup the GPU cannot run) */
        gfx_skip_shader++;
        leave();
        return;
    }

    /* constant buffers */
    ShaderBinds bt;
    memset(&bt, 0, sizeof bt);
    static u8 data[0x10000];
    for (u32 b = 0; b < 18; b++) {
        if (!(cs->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[b].size > 0x10000 ? 0x10000 : cb[b].size;
        if (size > cs->info.cbuf_extent[b]) size = cs->info.cbuf_extent[b];
        if (cb[b].address && size) read_gpu(cb[b].address, size, data);
        else { size = 16; memset(data, 0, 16); }
        View v = upload_raw(SHADER_COMPUTE, b, data, size);
        bt.cb[b] = bind_of(v);
        need(v.b, 0);
    }

    /* storage buffers */
    DrawParams params;
    memset(&params, 0, sizeof params);
    GBuf *bound[16] = {0};
    static u8 *before[16];
    static u32 before_cap[16];
    u64 va_of[16] = {0};
    u32 size_of[16] = {0};
    static u8 *gdata;
    static u32 gcap;
    static int no_resident = -1;   /* debug: always read results back to guest memory */
    if (no_resident < 0) no_resident = getenv("SSBU_NO_RESIDENT") != NULL;
    int learned = cs->learn_runs >= 8 && !no_resident;   /* until then, what it writes is read back and noted */
    static int log_left = -1;
    if (log_left < 0) log_left = getenv("SSBU_LOG_DISPATCH") ? atoi(getenv("SSBU_LOG_DISPATCH")) : 0;
    int logging = learned && log_left > 0 && log_left--;
    u64 sb_va[16] = {0};
    u32 sb_size[16] = {0};
    for (u32 k = 0; k < cs->info.sb_count; k++) {
        u32 desc[4] = {0};
        if (cb[0].address) read_gpu(cb[0].address + cs->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u32 size = desc[2] > (64u << 20) ? 64u << 20 : desc[2];
        if (logging)
            fprintf(stderr, "[cs] %s grid %ux%ux%u: buffer %u (cb0+%#x) %#llx + %#x%s\n", cs->name, grid[0], grid[1], grid[2], k,
                    cs->info.sb_offset[k], va, desc[2], cs->written_mask >> k & 1 ? " written" : "");
        params.sb[0][k][0] = desc[0];
        params.sb[0][k][1] = desc[1];
        params.sb[0][k][2] = size;
        sb_va[k] = va;
        sb_size[k] = size;
        if (learned && va && size) resident_mark(va, size);
    }
    /* What it writes stays on the GPU: the regions for that first, as making one may merge
     * regions or (the table full) give one up - not one this dispatch uses (marked above) -
     * and only then is anything bound, each buffer's region looked up again. */
    for (u32 k = 0; k < cs->info.sb_count && learned; k++)
        if (sb_va[k] && sb_size[k] && (cs->written_mask >> k & 1)) resident_acquire(sb_va[k], sb_size[k]);
    for (u32 k = 0; k < cs->info.sb_count; k++) {
        u64 va = sb_va[k];
        u32 size = sb_size[k];
        if (!va || !size) continue;
        Resident *over = resident_overlapping(va, size);
        if (learned) {
            /* what it writes is its region's (made above); what it only reads is the GPU's data
             * where there is some, the guest's elsewhere */
            Resident *res = over ? resident_covering(va, va + size) : NULL;
            View v = res ? resident_view(res, va, size) : (View){0};
            if (v.b) { bt.gm[k] = bind_of(v); need(v.b, 1); continue; }
            if (!(cs->written_mask >> k & 1) && !over && (v = input_view(va, size)).b) { bt.gm[k] = bind_of(v); need(v.b, 1); continue; }   /* only read: cached */
        } else if (over)
            gfx_resident_flush(va, size);   /* learning diffs it all against guest memory, which needs the GPU's data first */
        if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
        read_gpu(va, size, gdata);
        u32 need_ = (size + 15) & ~15u;
        GBuf *b = pool_get(need_, 1);
        if (!b) continue;
        gfx_stat_upload_bytes += size;
        if (need_ > before_cap[k]) { before[k] = realloc(before[k], need_); before_cap[k] = need_; }
        memset(before[k], 0, need_);
        memcpy(before[k], gdata, size);
        memcpy(b->cpu, before[k], need_);   /* (a buffer no work uses: written as it is) */
        if (learned && over) resident_patch((View){b, 0, need_}, va, size);
        bound[k] = b;
        va_of[k] = va;
        size_of[k] = size;
        bt.gm[k] = (ShaderBind){b->gpu, need_, 0};
        need(b, 1);
    }
    id<MTLComputeCommandEncoder> e = computes();
    resident();
    [e setComputePipelineState:cs->cs];
    [e setBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
    [e setBytes:&bt length:shader_binds_size(SHADER_COMPUTE) atIndex:SHADER_MSL_BINDS];
    /* textures: none bound (Direct3D's renderer binds none for compute either: they read 0) */
    u32 nsmp = shader_msl_samplers(&cs->info);
    for (u32 k = 0; k < cs->info.ntex; k++) {
        [e setTexture:dummy_texture(shader_msl_texture(&cs->info.tex[k]), cs->info.tex[k].shadow != 0) atIndex:k];
        if (k < nsmp) [e setSamplerState:linear_sampler atIndex:k];
    }
    [e dispatchThreadgroups:MTLSizeMake(grid[0], grid[1], grid[2]) threadsPerThreadgroup:MTLSizeMake(local[0], local[1], local[2])];
    gfx_stat_dispatched++;
    if (!first_dispatch_ns) first_dispatch_ns = rt_now_ns();
    if (draw_logging)
        fprintf(stderr, "[draw] dispatch %s grid %ux%ux%u, buffers %#llx+%#x %#llx+%#x %#llx+%#x (written %#x)\n", cs->name, grid[0], grid[1],
                grid[2], (u64)params.sb[0][0][1] << 32 | params.sb[0][0][0], params.sb[0][0][2], (u64)params.sb[0][1][1] << 32 | params.sb[0][1][0],
                params.sb[0][1][2], (u64)params.sb[0][2][1] << 32 | params.sb[0][2][0], params.sb[0][2][2], cs->written_mask);

    /* while learning, what it wrote goes back to guest memory; the buffers written are noted */
    int learning = cs->info.stores && !learned;
    if (learning) {
        cs->learn_runs++;
        gfx_stat_gpu_waits++;
        submit(1);
    }
    for (u32 k = 0; k < 16; k++) {
        GBuf *b = bound[k];
        if (!b) continue;
        if (learning) {
            const u32 *now = (const u32 *)(const void *)b->cpu, *was = (const u32 *)(const void *)before[k];
            int changed = 0;
            for (u32 i = 0; i < size_of[k] / 4; i++) {
                if (now[i] == was[i]) continue;
                changed = 1;
                u64 gg = gpu_va_to_guest(va_of[k] + 4ull * i);
                if (gg) st32(gg, now[i]);
            }
            if (changed) {
                cs->written_mask |= 1u << k;
                gfx_resident_written(va_of[k], size_of[k]);   /* a region over it takes the new data too */
            }
        }
        pool_put(b, 1);
    }
    leave();
}

/* ---- frames ------------------------------------------------------------------------------ */
/* How far behind the GPU is: at each present, how many earlier frames it has yet to finish
 * (counted by the frames' completion). Around 2 or more means the GPU is the limit; near 0 means
 * the CPU side is. At most display_latency() frames are with the GPU at once (frames_free): a
 * present waits for the oldest beyond that, as Direct3D's maximum frame latency makes it wait. */
static volatile u64 frames_committed, frames_done;
static dispatch_semaphore_t frames_free;
static int frames_allowed;   /* (frames_free's count: display_latency(), or the settings menu's since) */
static u64 gfx_stat_inflight, gfx_stat_inflight_frames, gfx_stat_present_ticks;

static void gpu_timer_frame(void) {
    u64 behind = __atomic_load_n(&frames_committed, __ATOMIC_ACQUIRE) - __atomic_load_n(&frames_done, __ATOMIC_ACQUIRE);
    gfx_stat_inflight += behind;
    gfx_stat_inflight_frames++;
}

void watchdog_present(void);   /* debug_watchdog.c */

/* The picture's texture at w x h (the presented surface's host size), made again when that
 * changes: written and saved as the plain bytes, shrunk through its sRGB view. */
static int make_final(u32 w, u32 h) {
    if (final_tex && final_w == w && final_h == h) return 1;
    MTLTextureDescriptor *fd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    fd.storageMode = MTLStorageModePrivate;
    fd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:fd];
    id<MTLTexture> srgb = tex ? [tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB] : nil;
    if (!srgb) return 0;
    if (final_tex) { bury(final_tex); bury(final_srgb); }   /* (work recorded with them keeps them) */
    final_tex = tex; final_srgb = srgb;
    final_w = w; final_h = h;
    return 1;
}

void gfx_present(u64 guest, u32 width, u32 height) {
    (void)width; (void)height;
    if (!enter()) return;
    watchdog_present();
    hitch_present();
    if (draw_logging) save_sampled();
    gfx_stat_presented++;
    note_writes();
    static int logged, frames_logged, frames_to_log;
    static const char *after_compute = (const char *)1;
    if (after_compute == (const char *)1) after_compute = getenv("SSBU_GFX_LOG_AFTER_COMPUTE");
    if (draw_logging) {
        fprintf(stderr, "[draw] -- frame presented from %#llx\n", guest);
        const char *n = getenv("SSBU_GFX_LOG_FRAMES");   /* how many frames to log (1; F4: 2) */
        if (++frames_logged >= (n ? atoi(n) : frames_to_log ? frames_to_log : 1)) { draw_logging = 0; logged = 1; frames_to_log = 0; }
    }
    else if (gfx_debug() & 16) {   /* SSBU_GFX_LOG_AT: the seconds to log a frame at (comma-separated; 10) */
        static double at[16];
        static int nat = -1, next;
        if (nat < 0) {
            const char *s = getenv("SSBU_GFX_LOG_AT");
            nat = 0;
            for (char *e; s && *s && nat < 16; s = *e ? e + 1 : e) { at[nat++] = strtod(s, &e); if (e == s) break; }
            if (!nat) at[nat++] = 10;
        }
        if (next < nat && rt_now_ns() > (u64)(at[next] * 1e9)) {
            next++;
            draw_logging = 1;
            frames_logged = 0;
            fprintf(stderr, "[draw] -- frame start (at %.1f s)\n", at[next - 1]);
        }
    } else if (!logged && first_dispatch_ns && after_compute &&
               rt_now_ns() > first_dispatch_ns + strtoull(after_compute, NULL, 0) * 1000000000ull) {
        draw_logging = 1;
        fprintf(stderr, "[draw] -- frame start (battle); %u resident regions:\n", nresidents);
        for (u32 i = 0; i < nresidents; i++) fprintf(stderr, "[draw]    resident %#llx + %#x\n", residents[i].va, residents[i].size);
    }
    /* the window's new size (gpu_display.c): the layer's drawables follow it */
    unsigned bw, bh;
    if (display_take_size(&bw, &bh) && layer && (layer.drawableSize.width != bw || layer.drawableSize.height != bh)) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.drawableSize = CGSizeMake(bw, bh);
        [CATransaction commit];
    }
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    /* the game's picture at its own size (render scale included): pixel for pixel, the bytes as
     * they are (the display shows them as sRGB, which an sRGB framebuffer's already are) */
    settle(src);
    end_encoding();
    if (src && (src->hw != final_w || src->hh != final_h)) make_final(src->hw, src->hh);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = final_tex;
    rp.colorAttachments[0].loadAction = src ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.12, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    renc = [command() renderCommandEncoderWithDescriptor:rp];
    enc_serial++;
    pass.w = final_w; pass.h = final_h;
    if (src) {
        [renc setFragmentTexture:surface_srv(src, 0, 0) atIndex:0];
        [renc setFragmentSamplerState:linear_sampler atIndex:0];
        draw_builtin(renc, "ps", MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, (MTLViewport){0, 0, final_w, final_h, 0, 1});
    }
    end_encoding();
    capture();

    /* debug keys */
    LONG keys = InterlockedExchange(&hotkeys, 0);
    static double f4_at = -1;   /* SSBU_F4_AT: the second to press F4 at (debug runs) */
    if (f4_at == -1) f4_at = getenv("SSBU_F4_AT") ? atof(getenv("SSBU_F4_AT")) : -2;
    if (f4_at >= 0 && rt_now_ns() > (u64)(f4_at * 1e9)) { f4_at = -2; keys |= 8; }
    static int overlay = -1, details;
    if (overlay < 0) overlay = getenv("SSBU_OVERLAY") ? atoi(getenv("SSBU_OVERLAY")) != 0 : display_overlay();
    if (keys & 1) overlay = !overlay;
    if (keys & 2) { details = !details; overlay = 1; }
    int menu_shot = menu_frame(&overlay, (keys & 4) != 0) & MENU_SHOT;   /* the settings menu (gpu_menu.c) */
    if (menu_shot) keys &= ~4;
    if (keys & 4) {   /* F3: the frame as the game drew it (without the overlay) */
        mkdir("screenshots", 0755);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[128];
        snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        save_bmp(final_tex, NULL, path);
        fprintf(stderr, "[gfx] screenshot: %s\n", path);
    }
    static char f4_dir[160];   /* F4's render targets go here, once the logged frame is drawn */
    if (f4_dir[0] && !draw_logging) {
        capture_surfaces(f4_dir, 0);
        fprintf(stderr, "[gfx] render targets of the logged frame: %s\n", f4_dir);
        f4_dir[0] = 0;
        sampled_dir[0] = 0;
    }
    if (keys & 8) {   /* F4: every draw of the next frame to the log, and its render targets to a folder */
        draw_logging = 1;
        frames_logged = 0;
        /* two presents' worth: the game presents in the middle of drawing a frame (after its
         * scene, before its effects, post-processing and HUD), so one would miss half */
        frames_to_log = 2;
        SYSTEMTIME st;
        GetLocalTime(&st);
        mkdir("screenshots", 0755);
        snprintf(f4_dir, sizeof f4_dir, "screenshots/frame_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        mkdir(f4_dir, 0755);
        snprintf(sampled_dir, sizeof sampled_dir, "%s", f4_dir);
        nsampled_saved = nsampled_pending = 0;
        sampled_seq = 0;
        char screen[200];
        snprintf(screen, sizeof screen, "%s/screen.bmp", f4_dir);
        save_bmp(final_tex, NULL, screen);
        fprintf(stderr, "[draw] -- frame start (F4)\n");
    }
    if (keys & 16) {   /* F5: depth buffers sampled as textures, or not */
        if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
        depth_sampling = !depth_sampling;
        fprintf(stderr, "[gfx] depth sampling %s (F5)\n", depth_sampling ? "on" : "off");
    }
    static double profile_at = -1;   /* SSBU_PROFILE_AT: the second to start one at (debug runs) */
    if (profile_at == -1) profile_at = getenv("SSBU_PROFILE_AT") ? atof(getenv("SSBU_PROFILE_AT")) : -2;
    if (profile_at >= 0 && rt_now_ns() > (u64)(profile_at * 1e9)) { profile_at = -2; keys |= 32; }
    if (keys & 32) profile_start(10.0);   /* F6 */
    static char detail[1800], hint[640];
    static u64 last_ns, last_presented, last_drawn, last_dispatched, last_skipped, last_watch, last_busy, last_upload, last_waits, last_composed, last_reused, last_kind[4];
    u64 now = rt_now_ns();
    if (now - last_ns >= 500000000ull) {   /* the details, twice a second */
        double secs = (now - last_ns) / 1e9;
        u64 frames = gfx_stat_presented - last_presented;
        if (!frames) frames = 1;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        static u64 last_inflight, last_inflight_frames, last_present_ticks;
        u64 gpu_frames = gfx_stat_inflight_frames - last_inflight_frames;
        snprintf(detail, sizeof detail,
                 "GPU behind by %.1f frames  Present blocked %.1f ms/frame\n"
                 "renderer %.1f ms/frame  uploads %.1f MB/frame (%llu composed, %llu reused)\n"
                 "  constants %.1f  vertices %.1f  storage %.1f  textures %.1f MB/frame\n"
                 "draws %llu/frame  compute %llu/frame  GPU waits %llu  skipped %.0f/s\n"
                 "shaders %llu (%ld compiling, prewarm %ld/%ld)  textures %u  surfaces %u\n"
                 "GPU-resident buffers %u  write watch %.1f ms/s  depth sampling %s\n"
                 "Esc settings  F1 overlay  F2 details  F3 screenshot  F4 log a frame\n"
                 "F5 depth sampling on/off  F6 profile 10 s%s",
                 gpu_frames ? (double)(gfx_stat_inflight - last_inflight) / gpu_frames : 0.0,
                 (gfx_stat_present_ticks - last_present_ticks) * 1000.0 / (double)freq.QuadPart / frames,
                 (gfx_stat_busy_ns - last_busy) / 1e6 / frames,
                 (gfx_stat_upload_bytes - last_upload) / 1048576.0 / frames, (gfx_stat_composed - last_composed) / frames,
                 (gfx_stat_reused - last_reused) / frames,
                 (gfx_stat_upload_kind[0] - last_kind[0]) / 1048576.0 / frames, (gfx_stat_upload_kind[1] - last_kind[1]) / 1048576.0 / frames,
                 (gfx_stat_upload_kind[2] - last_kind[2]) / 1048576.0 / frames, (gfx_stat_upload_kind[3] - last_kind[3]) / 1048576.0 / frames,
                 (gfx_stat_drawn - last_drawn) / frames, (gfx_stat_dispatched - last_dispatched) / frames, gfx_stat_gpu_waits - last_waits,
                 (gfx_stat_skipped - last_skipped) / secs, gfx_stat_shaders, (long)compiles_pending, (long)prewarm_ready, (long)prewarm_total, ntextures,
                 nsurfaces, nresidents, (gfx_stat_watch_us - last_watch) / 1000.0 / secs, depth_sampling != 0 ? "on" : "off",
                 profile_running() ? "  (profiling...)" : "");
        char net[512];
        net_status(net, sizeof net);
        snprintf(hint, sizeof hint, "F2 details  Esc settings%s%s%s%s", depth_sampling == 0 ? "  depth sampling OFF" : "",
                 profile_running() ? "  profiling..." : "", net[0] ? "\n" : "", net);
        if (net[0]) { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\n%s", net); }
        if (render_scale != 1.0f) {
            size_t l = strlen(detail);
            snprintf(detail + l, sizeof detail - l, "\nrender scale %.3g: picture %ux%u", render_scale, final_w, final_h);
        }
        char pads[256];
        pad_describe(pads, sizeof pads);
        { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\npads: %s", pads[0] ? pads : "none (keyboard only)"); }
        static int log_stats = -1, stat_tick;   /* SSBU_LOG_STATS: the details to the log too, every 5 s */
        if (log_stats < 0) log_stats = getenv("SSBU_LOG_STATS") != NULL;
        if (log_stats && ++stat_tick % 10 == 0) {
            char line[1024];
            snprintf(line, sizeof line, "%s", detail);
            for (char *nl = strchr(line, '\n'); nl; nl = strchr(nl, '\n')) *nl = '|';
            fprintf(stderr, "[stats] %.1f fps | %s | largest constants: %s cb%u, %#x bytes (extent %#x)\n", frames / secs, line,
                    gfx_stat_biggest_cb.shader, gfx_stat_biggest_cb.bank, gfx_stat_biggest_cb.size, gfx_stat_biggest_cb.extent);
            memset(&gfx_stat_biggest_cb, 0, sizeof gfx_stat_biggest_cb);
            u32 nk = 0;
            for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) nk += kept[i].buf != NULL;
            fprintf(stderr, "[stats] kept constant buffers: %u; so far %llu made, %llu updated by the command stream (%llu into copies), "
                            "%llu taken again after CPU writes, %llu mismatches\n",
                    nk, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_copies, gfx_stat_kept_reloaded, gfx_stat_kept_mismatches);
            fprintf(stderr, "[stats] memory taken away: %llu unmaps heard, %llu resident regions, kept buffers and compute inputs dropped\n",
                    gfx_stat_unmapped, gfx_stat_unmap_dropped);
            static u64 last_compared, compared_frames0;
            fprintf(stderr, "[stats] state objects: %u pipelines (%llu ready), %u depth/stencil, %u samplers; %llu command buffers (%llu failed); "
                            "upload memory %.0f MB; reuse checks compared %.2f MB/frame\n",
                    npipelines, gfx_stat_pipelines, depth_state_count, nsamplers, gfx_stat_commits, gfx_stat_gpu_errors, arena_total / 1048576.0,
                    (gfx_stat_reuse_compared - last_compared) / 1048576.0 / (double)(gfx_stat_presented - compared_frames0 ? gfx_stat_presented - compared_frames0 : 1));
            last_compared = gfx_stat_reuse_compared;
            compared_frames0 = gfx_stat_presented;
        }
        last_ns = now; last_presented = gfx_stat_presented; last_drawn = gfx_stat_drawn;
        last_dispatched = gfx_stat_dispatched; last_skipped = gfx_stat_skipped; last_watch = gfx_stat_watch_us;
        memcpy(last_kind, gfx_stat_upload_kind, sizeof last_kind);
        last_busy = gfx_stat_busy_ns; last_upload = gfx_stat_upload_bytes; last_waits = gfx_stat_gpu_waits; last_composed = gfx_stat_composed; last_reused = gfx_stat_reused;
        last_inflight = gfx_stat_inflight; last_inflight_frames = gfx_stat_inflight_frames; last_present_ticks = gfx_stat_present_ticks;
    }

    /* then into the window: 16:9, centred, black around it, the overlay over it. No more than
     * display_latency() frames are with the GPU: the wait for the oldest, and for a drawable, is
     * what "Present blocked" counts. */
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    int latency = display_take_latency();   /* (changed in the settings menu) */
    if (latency) {
        for (; frames_allowed < latency; frames_allowed++) dispatch_semaphore_signal(frames_free);
        for (; frames_allowed > latency; frames_allowed--) dispatch_semaphore_wait(frames_free, DISPATCH_TIME_FOREVER);   /* (a frame done) */
        if (layer) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer.maximumDrawableCount = latency >= 2 ? 3 : 2;
            [CATransaction commit];
        }
        fprintf(stderr, "[gfx] %d-frame latency\n", latency);
    }
    dispatch_semaphore_wait(frames_free, DISPATCH_TIME_FOREVER);
    if (layer) {
        int sync = !(display_present_flags() & 0x200);   /* (VRR pacing: shown at once, as Direct3D's ALLOW_TEARING) */
        if (layer.displaySyncEnabled != sync) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer.displaySyncEnabled = sync;
            [CATransaction commit];
        }
    }
    id<CAMetalDrawable> drawable = layer ? [layer nextDrawable] : nil;
    QueryPerformanceCounter(&p1);
    gfx_stat_present_ticks += (u64)(p1.QuadPart - p0.QuadPart);
    if (drawable) {
        id<MTLTexture> back = drawable.texture;
        unsigned fit_x, fit_y, fit_w, fit_h;
        display_fit((unsigned)back.width, (unsigned)back.height, &fit_x, &fit_y, &fit_w, &fit_h);
        MTLRenderPassDescriptor *wp = [MTLRenderPassDescriptor renderPassDescriptor];
        wp.colorAttachments[0].texture = back;
        wp.colorAttachments[0].loadAction = MTLLoadActionClear;
        wp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        wp.colorAttachments[0].storeAction = MTLStoreActionStore;
        end_encoding();
        renc = [command() renderCommandEncoderWithDescriptor:wp];
        enc_serial++;
        pass.w = (u32)back.width; pass.h = (u32)back.height;
        MTLViewport fit = {fit_x, fit_y, fit_w, fit_h, 0, 1};
        if (final_w > fit_w || final_h > fit_h) {
            /* a picture bigger than its place (render scale): shrunk, each window pixel the average
             * of the picture's under it, in linear light */
            float rx = (float)final_w / (float)fit_w, ry = (float)final_h / (float)fit_h;
            float k[8] = {(float)final_w, (float)final_h, 1.0f / (float)final_w, 1.0f / (float)final_h,
                          ceilf(rx), ceilf(ry), rx, ry};   /* taps per axis; the footprint in picture pixels */
            [renc setFragmentBytes:k length:sizeof k atIndex:0];
            [renc setFragmentTexture:final_srgb atIndex:0];
            [renc setFragmentSamplerState:linear_sampler atIndex:0];
            draw_builtin(renc, "psdown", back.pixelFormat, MTLColorWriteMaskAll, fit);
        } else {   /* as big as its place or smaller: drawn bilinear */
            [renc setFragmentTexture:final_tex atIndex:0];
            [renc setFragmentSamplerState:linear_sampler atIndex:0];
            draw_builtin(renc, "ps", back.pixelFormat, MTLColorWriteMaskAll, fit);
        }
        if (capture_window[0]) {   /* (SSBU_CAPTURE_WINDOW: the window as the picture is fitted into it, before the overlay) */
            end_encoding();
            save_bmp(back, "ps", capture_window);
            wp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            renc = [command() renderCommandEncoderWithDescriptor:wp];
            enc_serial++;
            pass.w = (u32)back.width; pass.h = (u32)back.height;
        }
        overlay_present_metal(dev, renc, back.pixelFormat, fit_x, fit_y, fit_w, fit_h, overlay, details ? detail : hint);
        end_encoding();
        if (menu_shot) {   /* F3 with the settings menu open: the window as it is, menu and all (waits for the GPU) */
            mkdir("screenshots", 0755);
            SYSTEMTIME st;
            GetLocalTime(&st);
            char path[128];
            snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u_menu.bmp", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond, st.wMilliseconds);
            save_bmp(back, "ps", path);   /* (through a draw: the drawable is BGRA) */
            fprintf(stderr, "[gfx] screenshot: %s\n", path);
        }
        [command() presentDrawable:drawable];
    }
    capture_window[0] = 0;
    dispatch_semaphore_t free_ = frames_free;
    [command() addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        __atomic_add_fetch(&frames_done, 1, __ATOMIC_RELEASE);
        dispatch_semaphore_signal(free_);
    }];
    __atomic_add_fetch(&frames_committed, 1, __ATOMIC_RELEASE);
    gpu_timer_frame();
    gpu_times_frame();
    submit(0);
    leave();
}

void gfx_metal_set_render_scale(float scale) {
    if (!enter()) return;
    render_scale = scale;
    leave();
}

int gfx_window_focused(void) { return window && GetForegroundWindow() == window; }
void *gfx_window_handle(void) { return window; }

/* The output (monitor) the swap chain is on, for display pacing: Direct3D's alone (an
 * IDXGIOutput). On macOS display pacing asks the window (macos/window.m): none here. */
void *gfx_display_output(void) { return NULL; }

/* ---- the device ------------------------------------------------------------------------------ */
static WINDOWS_BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&lock);
    @autoreleasepool {
        window = (HWND)rt_window();
        layer = (__bridge CAMetalLayer *)rt_metal_layer();
        dev = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "[gfx] no Metal device\n"); return TRUE; }
        if (![dev supportsFamily:MTLGPUFamilyMetal3]) {   /* (GPU addresses for the shaders' buffers) */
            fprintf(stderr, "[gfx] %s is not a Metal 3 GPU: the renderer needs one (Apple silicon)\n", dev.name.UTF8String);
            return TRUE;
        }
        if (!layer) fprintf(stderr, "[gfx] no window to draw into: frames are drawn but not shown\n");
        queue = [dev newCommandQueueWithMaxCommandBufferCount:64];
        if (@available(macOS 13.3, *)) dev.shouldMaximizeConcurrentCompilation = YES;   /* (the compiler threads' compiles, side by side) */
        NSError *err = nil;
        builtin = [dev newLibraryWithSource:@(BUILTIN_MSL) options:msl_options() error:&err];
        if (!builtin) {
            fprintf(stderr, "[gfx] shader compile failed: %s\n", first_error(err));
            rt_fatal("cannot compile a built-in shader");
        }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        linear_sampler = [dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        point_sampler = [dev newSamplerStateWithDescriptor:sd];
        /* The render scale (display.ini render_scale; auto: the picture's height in the window as it
         * is now - in full screen the screen's, which the window is on its way to - over the
         * game's 1080), fixed from here on: render targets are made at it. */
        float scale = display_render_scale();
        if (scale == 0) {
            scale = 1;
            CGSize size = layer ? layer.drawableSize : CGSizeZero;
            if (layer && display_window_mode() != DISPLAY_WINDOWED) {
                CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
                if (mode) {
                    size = CGSizeMake(CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode));
                    CGDisplayModeRelease(mode);
                }
            }
            if (size.width >= 1 && size.height >= 1) {
                unsigned fx, fy, fw, fh;
                display_fit((unsigned)size.width, (unsigned)size.height, &fx, &fy, &fw, &fh);
                scale = (float)fh / 1080.0f;
                if (scale < 1) scale = 1;
                if (scale > 4) scale = 4;
            }
        }
        if (scale < 0.5f || scale > 4) scale = 1;
        render_scale = scale;
        u32 pw, ph;
        scaled_size(1920, 1080, &pw, &ph);
        if (!make_final(pw, ph)) {
            fprintf(stderr, "[gfx] cannot create the picture's texture\n");
            return TRUE;
        }
        int latency = display_latency();
        frames_free = dispatch_semaphore_create(latency);
        frames_allowed = latency;
        if (layer) {   /* (macos/window.m made it: BGRA8 in sRGB, at the window's size in pixels) */
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            layer.device = dev;
            layer.maximumDrawableCount = latency >= 2 ? 3 : 2;
            layer.displaySyncEnabled = YES;
            layer.allowsNextDrawableTimeout = YES;
            [CATransaction commit];
        }
        display_set_tearing(1);   /* (displaySyncEnabled off: frames shown as soon as they are done) */
        start_compilers();
        /* The prewarm compiles the disk cache's MSL while the game starts: Metal keeps what it
         * compiled in its own cache, so a cache that was compiled before takes a fraction of a
         * millisecond a shader. SSBU_SHADER_PREWARM=0: off. */
        const char *prewarm = getenv("SSBU_SHADER_PREWARM");
        if (!prewarm || atoi(prewarm) != 0) CloseHandle(CreateThread(NULL, 0, prewarm_main, NULL, 0, NULL));
        ready = 1;
        fprintf(stderr, "[gfx] Metal ready on %s (%u-frame latency)\n", dev.name.UTF8String, latency);
        if (render_scale != 1.0f)
            fprintf(stderr, "[gfx] render scale %.3g%s: the game's 1920x1080 drawn at %ux%u\n", render_scale,
                    display_render_scale() == 0 ? " (auto)" : "", pw, ph);
    }
    return TRUE;
}
