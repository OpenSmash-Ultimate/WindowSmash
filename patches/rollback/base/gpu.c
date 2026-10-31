/* The GPU: Tegra X1 (Maxwell, GM20B) command processing.
 *
 * Channels receive GPFIFO entries, each pointing at a command list in GPU virtual memory.
 * A command list is a stream of method calls: a header word naming a register (method), a
 * subchannel and a count, followed by argument words. Method 0 binds an engine class to a
 * subchannel; methods below 0x40 belong to the channel's host FIFO itself.
 *
 * Every write lands in the engine's register file. Methods with side effects are executed:
 *   - semaphore releases (host FIFO, 3D reports, compute, DMA): the driver's fences are
 *     values the GPU writes to memory, and it waits for them
 *   - sync point increments
 *   - memory writes: inline uploads (I2M), constant buffer data, linear DMA copies
 *   - macros (the 3D engine's MME), which expand into further method calls
 * Rendering itself (draws, clears, 2D blits, compute dispatch) is counted, not executed -
 * that is the job of a rendering back end built on this state. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "gpu_backend.h"

enum { CLASS_FIFO = 0xB06F, CLASS_3D = 0xB197, CLASS_COMPUTE = 0xB1C0, CLASS_DMA = 0xB0B5, CLASS_I2M = 0xA140,
       CLASS_2D = 0x902D };

/* The 3D engine's macro unit (MME): small programs uploaded by the driver and called through
 * methods 0xE00+ - NVN issues its draws and much of its state through them. */
typedef struct Mme { u32 code[0x2000]; u32 start[0x80]; u32 *params; u32 nparams, cap; u32 calling; } Mme;
typedef struct Engine { u32 cls; u32 regs[0x1000]; u32 i2m_pos; Mme *mme; CbBinding cb[STAGES][18]; int indexed; } Engine;
typedef struct Channel { Engine *sub[8]; u32 fifo[0x40]; } Channel;

static Channel channels[256];
u64 gpu_stat_lists, gpu_stat_batches, gpu_stat_draws, gpu_stat_methods, gpu_stat_dispatches;

static u64 guest(u64 va) {
    u64 g = gpu_va_to_guest(va);
    if (!g) {
        static int warned;
        if (warned++ < 8) fprintf(stderr, "[gpu] access to unmapped GPU address %#llx\n", va);
    }
    return g;
}

static void write32(u64 va, u32 value) { u64 g = guest(va); if (g) st32(g, value); }
static void write64(u64 va, u64 value) { u64 g = guest(va); if (g) st64(g, value); }

static u64 timestamp(void) { return rt_now_ns() * 384 / 625; }   /* GPU timer ticks at 19.2 MHz * 32 / 1.6 */

/* A semaphore release: one word, or four words (payload, padding, 64-bit timestamp). The
 * payload goes last, after a release fence: a guest thread that sees it reads what the work
 * before it wrote (copies, the timestamp), which a weakly ordered host (arm64) would otherwise
 * let it see later. */
static void release(u64 va, u32 payload, int four_words) {
    if (four_words) {
        write64(va + 8, timestamp());
        write32(va + 4, 0);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    write32(va, payload);
    gfx_guest_wrote(va, four_words ? 16 : 4);
}

static void syncpoint_increment(u32 id) {
    if (id < SYNCPT_MAX) {
        if ((i32)(gpu_syncpt_max[id] - gpu_syncpt_min[id]) <= 0) gpu_syncpt_max[id]++;
        gpu_syncpt_min[id]++;
    }
}

/* ---- per-engine side effects ----------------------------------------------------- */
static void fifo_method(Channel *ch, u32 method, u32 arg) {
    ch->fifo[method] = arg;
    switch (method) {
    case 0x07: {   /* SEMAPHORED: execute; A/B hold the address, C the payload */
        u64 va = (u64)(ch->fifo[0x04] & 0xFF) << 32 | ch->fifo[0x05];
        u32 op = arg & 0x1F;
        if (op == 2) release(va, ch->fifo[0x06], !(arg & (1u << 24)));
        break;   /* acquires: the GPU waits for a value; everything before has already run */
    }
    case 0x1D:     /* SYNCPOINTB: bit 0 increment, bits 8-15 the sync point */
        if (arg & 1) syncpoint_increment((arg >> 8) & 0xFF);
        break;
    }
}

/* debug: SSBU_GPU_WATCH=<GPU address> logs the uploads (inline or constant buffer) that cover it */
static u64 gpu_watch(void) {
    static int checked;
    static u64 v;
    if (!checked++ && getenv("SSBU_GPU_WATCH")) v = strtoull(getenv("SSBU_GPU_WATCH"), NULL, 0);
    return v;
}

static void i2m_load(Engine *e, u32 base, u32 arg) {   /* the I2M register block, at `base` */
    u64 dst = (u64)e->regs[base + 2] << 32 | e->regs[base + 3];
    u32 line = e->regs[base + 0];
    u64 w = gpu_watch();
    if (w && e->i2m_pos == 0 && w >= dst && w < dst + (u64)line * (e->regs[base + 1] ? e->regs[base + 1] : 1))
        fprintf(stderr, "[gpu] I2M to %#llx: %u bytes x %u lines, launch %#x (class %#x)\n", dst, line, e->regs[base + 1],
                e->regs[base + 0xC], e->cls);
    if (e->i2m_pos < line * (e->regs[base + 1] ? e->regs[base + 1] : 1)) {
        u32 pitch = e->regs[base + 4] ? e->regs[base + 4] : line;
        u32 y = e->i2m_pos / (line ? line : 1), x = e->i2m_pos % (line ? line : 1);
        u64 g = guest(dst + (u64)y * pitch + x);
        if (g) {
            u32 n = line - x < 4 ? line - x : 4;
            memcpy(P(void, g), &arg, n);
            gfx_stream_wrote(dst + (u64)y * pitch + x, n);
        }
    }
    e->i2m_pos += 4;
}

/* The copy engine (class B0B5). A copy moves LINE_COUNT lines of LINE_LENGTH_IN elements
 * between two surfaces, each either pitch linear (OFFSET + y * PITCH) or block linear (GOBs
 * of 64 bytes x 8 rows stacked in blocks; the surface described at 0x1C3/0x1CA: block size,
 * width, height, depth, layer, origin). With remapping, an element is several components
 * which may be rearranged or replaced by constants (that is how the driver fills memory);
 * without, an element is a byte. */
typedef struct DmaSurface { u64 base; int pitch_linear; u32 pitch, width, height, depth, layer, bh, bd, ox, oy; } DmaSurface;

static u64 dma_address(const DmaSurface *s, u32 x, u32 y) {   /* x in bytes */
    if (s->pitch_linear) return s->base + (u64)y * s->pitch + x;
    u32 gobs_x = (s->width + 63) / 64, block_rows = 8u << s->bh;
    u64 block = 512ull << (s->bh + s->bd);
    u64 slice = (u64)gobs_x * ((s->height + block_rows - 1) / block_rows) * block;
    x += s->ox;
    y += s->oy;
    u32 yin = y % block_rows;
    return s->base + slice * s->layer + ((u64)(y / block_rows) * gobs_x + x / 64) * block + (yin / 8) * 512ull +
           ((x % 64) / 32) * 256 + ((yin % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (yin % 2) * 16 + (x % 16);
}

static void dma_surface(Engine *e, DmaSurface *s, u64 base, int pitch_linear, u32 pitch, u32 r) {
    u32 block = e->regs[r];
    *s = (DmaSurface){base, pitch_linear, pitch, e->regs[r + 1], e->regs[r + 2], e->regs[r + 3], e->regs[r + 4],
                      (block >> 4) & 15, (block >> 8) & 15, e->regs[r + 5] & 0xFFFF, e->regs[r + 5] >> 16};
    if (s->bh > 5) s->bh = 5;
    if (s->bd > 5) s->bd = 5;
}

static void dma_launch(Engine *e, u32 arg) {
    u64 src_va = (u64)e->regs[0x100] << 32 | e->regs[0x101], dst_va = (u64)e->regs[0x102] << 32 | e->regs[0x103];
    u32 line = e->regs[0x106], lines = (arg >> 9) & 1 ? e->regs[0x107] : 1;
    int remap = (arg >> 10) & 1;
    u64 w = gpu_watch();
    if (w && w >= dst_va && w < dst_va + (u64)lines * (e->regs[0x105] ? e->regs[0x105] : line) + line)
        fprintf(stderr, "[gpu] copy to %#llx from %#llx: %u x %u, launch %#x\n", dst_va, src_va, line, lines, arg);
    if (gfx_draw_logging())
        fprintf(stderr, "[draw] copy engine: %#llx -> %#llx, %u x %u, launch %#x, pitch %u -> %u\n", src_va, dst_va, line, lines, arg,
                e->regs[0x104], e->regs[0x105]);
    if (arg & 3) {
        DmaSurface src, dst;
        dma_surface(e, &src, src_va, (arg >> 7) & 1, e->regs[0x104], 0x1CA);
        dma_surface(e, &dst, dst_va, (arg >> 8) & 1, e->regs[0x105], 0x1C3);
        u32 comps = e->regs[0x1C2], csize = ((comps >> 16) & 3) + 1, nsrc = ((comps >> 20) & 3) + 1, ndst = ((comps >> 24) & 3) + 1;
        u32 src_bpe = remap ? csize * nsrc : 1, dst_bpe = remap ? csize * ndst : 1;
        int identity = !remap || (nsrc == ndst && (comps & 0x7777) == (0x3210 & ((1u << (4 * ndst)) - 1)));
        if (remap) {   /* surface widths and origins count elements */
            if (!src.pitch_linear) { src.width *= src_bpe; src.ox *= src_bpe; }
            if (!dst.pitch_linear) { dst.width *= dst_bpe; dst.ox *= dst_bpe; }
        }
        /* data the GPU wrote and keeps (compute results): copied there, or brought back first */
        u64 src_span = src.pitch_linear ? (u64)(lines ? lines - 1 : 0) * src.pitch + (u64)line * src_bpe : 0;
        u64 dst_span = dst.pitch_linear ? (u64)(lines ? lines - 1 : 0) * dst.pitch + (u64)line * dst_bpe : 0;
        if (src.pitch_linear && dst.pitch_linear && identity) {
            for (u32 y = 0; y < lines; y++) {
                u64 sv = src.base + (u64)y * src.pitch, dv = dst.base + (u64)y * dst.pitch;
                if (gfx_copy_resident(dv, sv, (u64)line * src_bpe)) continue;
                u64 s = guest(sv), d = guest(dv);
                if (s && d) memmove(P(void, d), P(void, s), (size_t)line * src_bpe);
                gfx_resident_written(dv, (u64)line * src_bpe);
            }
        } else if (identity) {
            if (src_span) gfx_resident_flush(src.base, src_span);   /* to or from block linear: in runs that stay within 16 bytes */
            u32 bytes = line * src_bpe;
            for (u32 y = 0; y < lines; y++)
                for (u32 x = 0; x < bytes;) {
                    u32 n = 16 - (x % 16);
                    if (!src.pitch_linear) n = 16 - ((x + src.ox) % 16);
                    if (!dst.pitch_linear && 16 - ((x + dst.ox) % 16) < n) n = 16 - ((x + dst.ox) % 16);
                    if (n > bytes - x) n = bytes - x;
                    u64 s = guest(dma_address(&src, x, y)), d = guest(dma_address(&dst, x, y));
                    if (s && d) memcpy(P(void, d), P(void, s), n);
                    x += n;
                }
        } else {   /* remapped components, constants included: element by element */
            if (src_span) gfx_resident_flush(src.base, src_span);
            for (u32 y = 0; y < lines; y++)
                for (u32 x = 0; x < line; x++) {
                    u8 in[16] = {0}, out[16];
                    u64 s = guest(dma_address(&src, x * src_bpe, y)), d = guest(dma_address(&dst, x * dst_bpe, y));
                    if (!d) continue;
                    int needs_src = 0;
                    for (u32 k = 0; k < ndst; k++) needs_src |= ((comps >> (4 * k)) & 7) < 4;
                    if (needs_src && s) memcpy(in, P(void, s), src_bpe);
                    memcpy(out, P(void, d), dst_bpe);
                    for (u32 k = 0; k < ndst; k++) {
                        u32 sel = (comps >> (4 * k)) & 7;
                        if (sel < 4) memcpy(out + k * csize, in + sel * csize, csize);
                        else if (sel == 4) memcpy(out + k * csize, &e->regs[0x1C0], csize);
                        else if (sel == 5) memcpy(out + k * csize, &e->regs[0x1C1], csize);
                    }
                    memcpy(P(void, d), out, dst_bpe);
                }
        }
        if (dst_span && !(src.pitch_linear && identity)) gfx_resident_written(dst.base, dst_span);   /* (the plain case did it per line) */
        if (!dst.pitch_linear) gfx_guest_wrote(0, 0);   /* (into a block-linear surface: where, exactly, is not worked out) */
    }
    u32 semaphore = (arg >> 3) & 3;
    if (semaphore) release((u64)e->regs[0x90] << 32 | e->regs[0x91], e->regs[0x92], semaphore == 2);
}

static void engine_method(Channel *ch, Engine *e, u32 method, u32 arg);

/* ---- the macro unit -------------------------------------------------------------- */
typedef struct MmeRun { Channel *ch; Engine *e; u32 r[8], method, next, carry; const u32 *params; u32 nparams; } MmeRun;

static void mme_send(MmeRun *m, u32 value) {
    u32 method = m->method & 0xFFF;
    if (method < 0xE00) engine_method(m->ch, m->e, method, value);
    m->method = (m->method & ~0xFFFu) | ((method + ((m->method >> 12) & 0x3F)) & 0xFFF);
}

static u32 mme_fetch(MmeRun *m) { return m->next < m->nparams ? m->params[m->next++] : 0; }

static void mme_result(MmeRun *m, u32 op, u32 dst, u32 result) {
    u32 value = result;
    switch (op) {
    case 0: value = mme_fetch(m); break;                                          /* fetch, ignore result */
    case 1: break;                                                                /* move */
    case 2: m->method = result; break;                                            /* move, set method */
    case 3: value = mme_fetch(m); mme_send(m, result); break;                     /* fetch, send result */
    case 4: mme_send(m, result); break;                                           /* move, send */
    case 5: value = mme_fetch(m); m->method = result; break;                      /* fetch, set method from result */
    case 6: m->method = result; mme_send(m, mme_fetch(m)); break;                 /* move, set method, send fetched */
    case 7: m->method = result; mme_send(m, (result >> 12) & 0x3F); break;        /* move, set method, send bits 12-17 */
    }
    if (dst) m->r[dst] = value;
}

static u32 mme_alu(MmeRun *m, u32 op, u32 a, u32 b) {
    u64 r;
    switch (op) {
    case 0: r = (u64)a + b; m->carry = (u32)(r >> 32); return (u32)r;
    case 1: r = (u64)a + b + m->carry; m->carry = (u32)(r >> 32); return (u32)r;
    case 2: r = (u64)a - b; m->carry = r < 0x100000000ull; return (u32)r;
    case 3: r = (u64)a - b - (m->carry ? 0 : 1); m->carry = r < 0x100000000ull; return (u32)r;
    case 8: return a ^ b;
    case 9: return a | b;
    case 10: return a & b;
    case 11: return a & ~b;
    case 12: return ~(a & b);
    }
    return 0;
}

/* One instruction at pc (in words); returns the next pc, or ~0u when the program ends.
 * Branches and exits have a delay slot (which a branch can annul). */
static u32 mme_step(MmeRun *m, const u32 *code, u32 pc, int delay_slot) {
    u32 op = code[pc & 0x1FFF];
    u32 kind = op & 7, result = (op >> 4) & 7, dst = (op >> 8) & 7;
    u32 a = m->r[(op >> 11) & 7], b = m->r[(op >> 14) & 7];
    i32 imm = (i32)op >> 14;
    u32 src_bit = (op >> 17) & 31, size = (op >> 22) & 31, dst_bit = (op >> 27) & 31, mask = (1u << size) - 1;
    switch (kind) {
    case 0: mme_result(m, result, dst, mme_alu(m, (op >> 17) & 31, a, b)); break;
    case 1: mme_result(m, result, dst, a + (u32)imm); break;
    case 2: mme_result(m, result, dst, (a & ~(mask << dst_bit)) | (((b >> src_bit) & mask) << dst_bit)); break;
    case 3: mme_result(m, result, dst, ((b >> (a & 31)) & mask) << dst_bit); break;
    case 4: mme_result(m, result, dst, ((b >> src_bit) & mask) << (a & 31)); break;
    case 5: mme_result(m, result, dst, m->e->regs[(a + (u32)imm) & 0xFFF]); break;
    case 7: {
        int taken = ((op >> 4) & 1) ? a != 0 : a == 0;
        if (taken && !delay_slot) {
            u32 target = pc + (u32)imm;
            if ((op >> 5) & 1) return target;                       /* annulled: no delay slot */
            return mme_step(m, code, pc + 1, 1) == ~0u ? ~0u : target;
        }
        break;
    }
    }
    if ((op >> 7) & 1 && !delay_slot) {                             /* exit, after its delay slot */
        mme_step(m, code, pc + 1, 1);
        return ~0u;
    }
    return pc + 1;
}

static void mme_call(Channel *ch, Engine *e, u32 index, const u32 *params, u32 n) {
    MmeRun m = {ch, e, {0}, 0, 1, 0, params, n};
    m.r[1] = n ? params[0] : 0;
    u32 pc = e->mme->start[index & 0x7F];
    for (u32 steps = 0; pc != ~0u && steps < 1000000; steps++) pc = mme_step(&m, e->mme->code, pc, 0);
}

/* Arguments to a macro accumulate until the command that carries them ends. */
static void macro_method(Channel *ch, Engine *e, u32 method, u32 arg, int last) {
    Mme *x = e->mme;
    if (!x->calling) x->calling = method | 0x10000;
    if (x->nparams == x->cap) x->params = realloc(x->params, sizeof *x->params * (x->cap = x->cap * 2 + 64));
    x->params[x->nparams++] = arg;
    if (last) {
        mme_call(ch, e, ((x->calling & 0xFFFF) - 0xE00) >> 1, x->params, x->nparams);
        x->nparams = 0;
        x->calling = 0;
    }
}

static void engine_method(Channel *ch, Engine *e, u32 method, u32 arg) {
    (void)ch;
    e->regs[method] = arg;
    switch (e->cls) {
    case CLASS_3D:
        if ((method >= 0x4E0 && method <= 0x4E7) || (method >= 0x3D5 && method <= 0x3D7) || (method >= 0x565 && method <= 0x569)) {
            static int left = -1;   /* debug: SSBU_LOG_STENCIL=n logs the first n writes to the stencil registers */
            if (left < 0) left = getenv("SSBU_LOG_STENCIL") ? atoi(getenv("SSBU_LOG_STENCIL")) : 0;
            if (left > 0 && left--) fprintf(stderr, "[gpu] stencil register %#x = %#x\n", method, arg);
        }
        switch (method) {
        case 0x046: e->mme->code[e->regs[0x45]++ & 0x1FFF] = arg; break;              /* macro upload */
        case 0x048: e->mme->start[e->regs[0x47]++ & 0x7F] = arg; break;              /* macro start address */
        case 0x6C3: {   /* SET_REPORT_SEMAPHORE_D: 0 release, 2 report (counter or timestamp) */
            u64 va = (u64)e->regs[0x6C0] << 32 | e->regs[0x6C1];
            u32 op = arg & 3;
            int one_word = (arg >> 28) & 1;
            if (op == 0) release(va, e->regs[0x6C2], !one_word);
            else if (op == 2) release(va, 0, !one_word);   /* counters read as zero until rendering exists */
            break;
        }
        case 0x0B2: if (arg & (1u << 16)) syncpoint_increment(arg & 0xFFFF); break;   /* SYNCPOINT_ACTION */
        case 0x06C: e->i2m_pos = 0; break;                                            /* I2M launch */
        case 0x06D: i2m_load(e, 0x60, arg); break;                                    /* I2M data */
        case M_CB_BIND(0): case M_CB_BIND(1): case M_CB_BIND(2): case M_CB_BIND(3): case M_CB_BIND(4): {
            CbBinding *b = &e->cb[(method - M_CB_BIND(0)) / 8][((arg >> 4) & 31) % 18];   /* bind a constant buffer */
            b->address = arg & 1 ? (u64)e->regs[M_CB_ADDRESS] << 32 | e->regs[M_CB_ADDRESS + 1] : 0;
            b->size = arg & 1 ? e->regs[M_CB_SIZE] : 0;
            break;
        }
        case 0x8E4: case 0x8E5: case 0x8E6: case 0x8E7: case 0x8E8: case 0x8E9: case 0x8EA: case 0x8EB:
        case 0x8EC: case 0x8ED: case 0x8EE: case 0x8EF: case 0x8F0: case 0x8F1: case 0x8F2: case 0x8F3: {
            u64 cb = (u64)e->regs[0x8E1] << 32 | e->regs[0x8E2];   /* constant buffer data */
            if (gpu_watch() && (gpu_watch() & ~3ull) == cb + e->regs[0x8E3])
                fprintf(stderr, "[gpu] CB_LOAD to %#llx + %#x: %#x\n", cb, e->regs[0x8E3], arg);
            write32(cb + e->regs[0x8E3], arg);
            gfx_stream_wrote(cb + e->regs[0x8E3], 4);
            e->regs[0x8E3] += 4;
            break;
        }
        case 0x585: gpu_stat_draws++; gfx_draw(e->regs, e->cb, e->indexed); break;
        case M_DRAW_INDEX_COUNT: e->indexed = 1; break;                                /* indexed draw follows */
        case M_VERTEX_ARRAY_COUNT: e->indexed = 0; break;
        case 0x674: gfx_clear(e->regs, arg); break;                                  /* clear surface */
        }
        break;
    case CLASS_COMPUTE:
        if (method == 0x06C) e->i2m_pos = 0;
        else if (method == 0x06D) i2m_load(e, 0x60, arg);
        else if (method == 0x0AF) {   /* LAUNCH: the descriptor (QMD) can release two semaphores */
            u64 qmd = guest((u64)e->regs[0x0AD] << 8);
            if (qmd) {
                u32 *q = P(u32, qmd);
                gpu_stat_dispatches++;
                gfx_dispatch(e->regs, q);
                if (q[11] & (1u << 4)) release((u64)(q[0x31] & 0xFF) << 32 | q[0x30], q[0x32], 1);
                if (q[11] & (1u << 5)) release((u64)(q[0x34] & 0xFF) << 32 | q[0x33], q[0x35], 1);
            }
        }
        break;
    case CLASS_DMA:
        if (method == 0x0C0) dma_launch(e, arg);
        break;
    case CLASS_2D:
        if (method == 0x237) gfx_blit2d(e->regs);   /* PIXELS_FROM_MEMORY_SRC_Y0_INT: blit */
        break;
    case CLASS_I2M:
        if (method == 0x06C) e->i2m_pos = 0;
        else if (method == 0x06D) i2m_load(e, 0x60, arg);
        break;
    }
}

static void method(Channel *ch, u32 sub, u32 m, u32 arg, int last) {
    gpu_stat_methods++;
    if (m < 0x40) {
        if (m == 0) {   /* bind an engine class to the subchannel */
            if (!ch->sub[sub]) ch->sub[sub] = calloc(1, sizeof(Engine));
            ch->sub[sub]->cls = arg & 0xFFFF;
            if (ch->sub[sub]->cls == CLASS_3D && !ch->sub[sub]->mme) ch->sub[sub]->mme = calloc(1, sizeof(Mme));
        }
        fifo_method(ch, m, arg);
        return;
    }
    Engine *e = ch->sub[sub];
    if (!e || m >= 0x1000) return;
    if (m >= 0xE00 && e->mme) macro_method(ch, e, m, arg, last);
    else engine_method(ch, e, m, arg);
}

/* ---- command lists --------------------------------------------------------------- */
static void run_list(Channel *ch, const u32 *w, u64 n) {
    for (u64 i = 0; i < n;) {
        u32 h = w[i++];
        u32 m = h & 0x1FFF, sub = (h >> 13) & 7, count = (h >> 16) & 0x1FFF, mode = h >> 29;
        switch (mode) {
        case 1: case 0:   /* increasing */
            for (u32 k = 0; k < count && i < n; k++) method(ch, sub, m + k, w[i++], k + 1 == count || i == n);
            break;
        case 3: case 2:   /* non-increasing */
            for (u32 k = 0; k < count && i < n; k++) method(ch, sub, m, w[i++], k + 1 == count || i == n);
            break;
        case 4:           /* inline: the argument is in the header */
            method(ch, sub, m, count, 1);
            break;
        case 5:           /* increase once */
            for (u32 k = 0; k < count && i < n; k++) method(ch, sub, k ? m + 1 : m, w[i++], k + 1 == count || i == n);
            break;
        default:
            return;       /* not a command: stop this list */
        }
    }
}

void gpu_submit(u32 channel, const u64 *entries, u32 count) {
    Channel *ch = &channels[channel & 0xFF];
    gpu_stat_batches++;
    for (u32 i = 0; i < count; i++) {
        u64 e = entries[i];
        u64 va = e & 0xFFFFFFFFFCull, words = (e >> 42) & 0x1FFFFF;
        gpu_stat_lists++;
        if (!words) continue;
        u64 g = guest(va);
        if (g) run_list(ch, P(u32, g), words);
    }
    gfx_stream_end();
}
