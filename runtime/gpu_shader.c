/* Maxwell (SM 5.x) shader programs to HLSL, Shader Model 5.
 *
 * Instruction set: opcodes from the top 16 bits (most specific pattern wins); operand fields
 * as decoded by the yuzu project's translator. A program is a 0x50-byte header (SPH), then
 * 64-bit instructions in groups of four whose first is scheduling control.
 *
 * Model: every general register is a uint variable, every predicate a bool. Floating point
 * values are reinterpreted with asfloat/asuint, so registers keep exact bits. Constant
 * buffers are raw buffers (any offset, out of range reads 0). Vertex shaders pull their
 * attributes from raw vertex-stream buffers themselves, decoding every Maxwell format.
 *
 * Control flow: a program without branches is emitted straight. Otherwise its basic blocks
 * become cases of a switch inside a loop driven by a `pc` variable, with the SSY/PBK
 * reconvergence targets on small per-thread stacks - per-thread execution makes SYNC and
 * BRK plain jumps to the saved target. Pixel shaders in that form sample with explicit LOD
 * zero (gradients are unavailable inside a loop).
 *
 * The same text is also Metal Shading Language, for the macOS renderer: a prelude gives MSL
 * HLSL's names (asfloat, f16tof32, raw buffers' Load, textures' Sample and Gather...), and
 * as MSL has no global resources, the program and its helpers become member functions of a
 * struct holding the resources, which the entry point (vs_main/ps_main/cs_main) fills from
 * its arguments. Only what neither language can say the other's way is written per language
 * (see msl_out and the c->msl tests); gpu_shader.h has the layout the Metal renderer binds. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

#include "gpu_shader.h"

/* The Linux build compiles HLSL with vkd3d-shader, which lacks a few things fxc has
 * (CalculateLevelOfDetail, RWByteAddressBuffer.Load, gathers with four offsets): for it the
 * translator writes those the long way. SSBU_HLSL_FOR_VKD3D does the same on Windows, to test. */
#if !defined(_WIN32) || defined(SSBU_HLSL_FOR_VKD3D)
#define HLSL_DEFAULT SHADER_HLSL_VKD3D
#else
#define HLSL_DEFAULT SHADER_HLSL_FXC
#endif

#define BITS(w, pos, n) ((u32)(((w) >> (pos)) & ((1ull << (n)) - 1)))
#define RZ 255
#define PT 7

/* ---- opcodes ----------------------------------------------------------------------- */
#define OPCODES(X) \
    X(ALD, "1110111111011---") X(AST, "1110111111110---") X(BAR, "1111000010101---") \
    X(BFE_reg, "0101110000000---") X(BFE_cbuf, "0100110000000---") X(BFE_imm, "0011100-00000---") \
    X(BFI_reg, "0101101111110---") X(BFI_rc, "0101001111110---") X(BFI_cr, "0100101111110---") X(BFI_imm, "0011011-11110---") \
    X(BRA, "111000100100----") X(BRK, "111000110100----") X(BRX, "111000100101----") \
    X(DEPBAR, "1111000011110---") X(EXIT, "111000110000----") \
    X(F2F_reg, "0101110010101---") X(F2F_cbuf, "0100110010101---") X(F2F_imm, "0011100-10101---") \
    X(F2I_reg, "0101110010110---") X(F2I_cbuf, "0100110010110---") X(F2I_imm, "0011100-10110---") \
    X(FADD_reg, "0101110001011---") X(FADD_cbuf, "0100110001011---") X(FADD_imm, "0011100-01011---") X(FADD32I, "000010----------") \
    X(FFMA_reg, "010110011-------") X(FFMA_rc, "010100011-------") X(FFMA_cr, "010010011-------") X(FFMA_imm, "0011001-1-------") \
    X(FFMA32I, "000011----------") \
    X(FMNMX_reg, "0101110001100---") X(FMNMX_cbuf, "0100110001100---") X(FMNMX_imm, "0011100-01100---") \
    X(FMUL_reg, "0101110001101---") X(FMUL_cbuf, "0100110001101---") X(FMUL_imm, "0011100-01101---") X(FMUL32I, "00011110--------") \
    X(CSET, "0101000010011---") X(CSETP, "0101000010100---")     X(FSET_reg, "01011000--------") X(FSET_cbuf, "01001000--------") X(FSET_imm, "0011000---------") \
    X(FSETP_reg, "010110111011----") X(FSETP_cbuf, "010010111011----") X(FSETP_imm, "0011011-1011----") \
    X(HADD2_reg, "0101110100010---") X(HADD2_cbuf, "0111101-1-------") X(HADD2_imm, "0111101-0-------") X(HADD2_32I, "0010110---------") \
    X(HFMA2_reg, "0101110100000---") X(HFMA2_rc, "01100---1-------") X(HFMA2_cr, "01110---1-------") X(HFMA2_imm, "01110---0-------") \
    X(HFMA2_32I, "0010100---------") \
    X(HSET2_reg, "0101110100011---") X(HSET2_cbuf, "0111110-1-------") X(HSET2_imm, "0111110-0-------")     X(HSETP2_reg, "0101110100100---") X(HSETP2_cbuf, "0111111-1-------") X(HSETP2_imm, "0111111-0-------")     X(HMUL2_reg, "0101110100001---") X(HMUL2_cbuf, "0111100-1-------") X(HMUL2_imm, "0111100-0-------") X(HMUL2_32I, "0010101---------") \
    X(I2F_reg, "0101110010111---") X(I2F_cbuf, "0100110010111---") X(I2F_imm, "0011100-10111---") \
    X(I2I_reg, "0101110011100---") X(I2I_cbuf, "0100110011100---") X(I2I_imm, "0011100-11100---") \
    X(IADD_reg, "0101110000010---") X(IADD_cbuf, "0100110000010---") X(IADD_imm, "0011100-00010---") X(IADD32I, "0001110---------") \
    X(IMNMX_reg, "0101110000100---") X(IMNMX_cbuf, "0100110000100---") X(IMNMX_imm, "0011100-00100---") \
    X(IPA, "11100000--------") \
    X(ISCADD_reg, "0101110000011---") X(ISCADD_cbuf, "0100110000011---") X(ISCADD_imm, "0011100-00011---") X(ISCADD32I, "000101----------") \
    X(ISET_reg, "010110110101----") X(ISET_cbuf, "010010110101----") X(ISET_imm, "0011011-0101----") \
    X(ISETP_reg, "010110110110----") X(ISETP_cbuf, "010010110110----") X(ISETP_imm, "0011011-0110----") \
    X(KIL, "111000110011----") X(LDC, "1110111110010---") X(LDG, "1110111011010---") X(STG, "1110111011011---") X(LDS, "1110111101001---") X(STS, "1110111101011---") X(LDL, "1110111101000---") X(STL, "1110111101010---") \
    X(LOP_reg, "0101110001000---") X(LOP_cbuf, "0100110001000---") X(LOP_imm, "0011100-01000---") X(LOP32I, "000001----------") \
    X(LOP3_reg, "0101101111100---") X(LOP3_cbuf, "0000001---------") X(LOP3_imm, "001111----------") \
    X(MEMBAR, "1110111110011---") \
    X(MOV_reg, "0101110010011---") X(MOV_cbuf, "0100110010011---") X(MOV_imm, "0011100-10011---") X(MOV32I, "000000010000----") \
    X(MUFU, "0101000010000---") X(NOP, "0101000010110---") X(PBK, "111000101010----") \
    X(PSET, "0101000010001---") X(PSETP, "0101000010010---") \
    X(R2P_reg, "0101110011110---") X(R2P_cbuf, "0100110011110---") X(R2P_imm, "0011100-11110---") \
    X(IADD3_reg, "010111001100----") X(IADD3_cbuf, "010011001100----") X(IADD3_imm, "0011100-1100----") \
    X(TMML, "1101111101011---") \
    X(RRO_reg, "0101110010010---") X(RRO_cbuf, "0100110010010---") X(RRO_imm, "0011100-10010---") \
    X(S2R, "1111000011001---") \
    X(SEL_reg, "0101110010100---") X(SEL_cbuf, "0100110010100---") X(SEL_imm, "0011100-10100---") \
    X(SHL_reg, "0101110001001---") X(SHL_cbuf, "0100110001001---") X(SHL_imm, "0011100-01001---") \
    X(SHR_reg, "0101110000101---") X(SHR_cbuf, "0100110000101---") X(SHR_imm, "0011100-00101---") \
    X(SSY, "111000101001----") X(SYNC, "1111000011111---") \
    X(TEX, "11000-----------") X(TEXS, "1101-00---------") X(TLD4, "110010----------") X(TLD4S, "11011111-0------") \
    X(TLDS, "1101-01---------") X(TXQ, "1101111101001---") \
    X(VOTE, "0101000011011---") X(VOTE_vtg, "0101000011100---") \
    X(XMAD_reg, "0101101100------") X(XMAD_rc, "010100010-------") X(XMAD_cr, "0100111---------") X(XMAD_imm, "0011011-00------")

enum {
    OP_UNKNOWN,
#define ENUM(name, pat) OP_##name,
    OPCODES(ENUM)
#undef ENUM
    OP_COUNT
};
static const struct { const char *name, *pattern; } OPS[] = {
    {"?", ""},
#define ROW(name, pat) {#name, pat},
    OPCODES(ROW)
#undef ROW
};
static struct { u16 mask, value, op, bits; } table[OP_COUNT];
static u32 table_size;

static int by_specificity(const void *a, const void *b) {
    return (int)((const __typeof__(table[0]) *)b)->bits - (int)((const __typeof__(table[0]) *)a)->bits;
}

static void build_table(void) {
    for (u32 i = 1; i < OP_COUNT; i++) {
        u16 mask = 0, value = 0;
        for (int b = 0; b < 16; b++) {
            char ch = OPS[i].pattern[b];
            mask = (u16)(mask << 1 | (ch != '-'));
            value = (u16)(value << 1 | (ch == '1'));
        }
        table[table_size++] = (__typeof__(table[0])){mask, value, (u16)i, (u16)__builtin_popcount(mask)};
    }
    qsort(table, table_size, sizeof table[0], by_specificity);
}

/* Built once, whichever thread translates first (the Metal renderer translates on several). */
#ifdef _WIN32
static INIT_ONCE table_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK table_once_fn(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    build_table();
    return TRUE;
}
static void init_table(void) { InitOnceExecuteOnce(&table_once, table_once_fn, NULL, NULL); }
#else
static pthread_once_t table_once = PTHREAD_ONCE_INIT;
static void init_table(void) { pthread_once(&table_once, build_table); }
#endif

static u32 decode(u64 w) {
    u16 top = (u16)(w >> 48);
    for (u32 i = 0; i < table_size; i++)
        if ((top & table[i].mask) == table[i].value) return table[i].op;
    return OP_UNKNOWN;
}

/* ---- output text ----------------------------------------------------------------- */
typedef struct Str { char *s; size_t n, cap; } Str;

static void put(Str *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (b->n + need + 1 > b->cap) {
        b->cap = (b->n + need + 1) * 2 + 4096;
        b->s = realloc(b->s, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, need + 1, fmt, ap);
    va_end(ap);
    b->n += need;
}

/* Short-lived strings for operand expressions. */
static char *tmp(const char *fmt, ...) {
    static __declspec(thread) char ring[64][512];
    static __declspec(thread) u32 next;
    char *s = ring[next++ & 63];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, 512, fmt, ap);
    va_end(ap);
    return s;
}

/* ---- translation context ------------------------------------------------------------- */
typedef struct Insn { u32 addr; u64 w; u32 op; } Insn;

typedef struct Ctx {
    int stage;
    const u8 *prog;
    Insn *ins;
    u32 n;
    Str body;
    u8 reg_used[256], pred_used[8];
    ShaderInfo *info;
    int dispatch;                 /* blocks in a switch loop */
    u8 *leader;                   /* per instruction: starts a block */
    u32 *stack_to;                /* per SYNC/BRK: where it goes, known at translation (resolve_stacks), or NULL */
    u32 out_attr_mask;            /* vertex: generic attributes written (from AST) */
    int uses_cc;
    int uses_gmem;                /* global memory loads (LDG) */
    int uses_smem;                /* compute: shared memory */
    int uses_lmem;                /* local (per-thread) memory */
    u32 local[3], shared;         /* compute: workgroup size, shared memory bytes */
    u32 vs_outputs;               /* pixel: the paired vertex shader's outputs */
    int alpha_test;               /* pixel: the alpha-tested variant (shader_hlsl_alpha_tested) */
    CbufReader *read_cbuf;        /* optional: constant buffer contents, for jump tables */
    void *read_user;
    int indent;
    int brx_checked, brx_ok;      /* every indirect branch's table resolved (asked once) */
    /* the output language (SHADER_HLSL_FXC, ...), and what it has */
    int lang, msl;
    int four_gathers;             /* a gather with an offset per texel as four gathers (vkd3d-shader, MSL) */
    int lod_long;                 /* TMML from the coordinates' derivatives (vkd3d-shader) */
    int sb_words;                 /* compute storage buffers as structured buffers of words (vkd3d-shader) */
    u32 tex_counter;              /* MSL: texture results' names (HLSL's count on across programs) */
} Ctx;

static void line(Ctx *c, const char *fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    put(&c->body, "%*s%s\n", c->indent * 2, "", text);
}

static void unsupported(Ctx *c, const Insn *in, const char *why) {
    if (!c->info->unsupported[0])
        snprintf(c->info->unsupported, sizeof c->info->unsupported, "%s at %#x (%016llx): %s", OPS[in->op].name, in->addr,
                 in->w, why);
    line(c, "/* unsupported: %s %016llx (%s) */", OPS[in->op].name, in->w, why);
}

/* Registers: RZ reads as zero; writes to it vanish. */
static const char *R(Ctx *c, u32 r) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    return tmp("r%u", r);
}

static const char *F(Ctx *c, u32 r) { return r == RZ ? "0.0f" : tmp("asfloat(%s)", R(c, r)); }

static const char *PRED(Ctx *c, u32 p, int neg) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    return tmp(neg ? "!p%u" : "p%u", p);
}

static void set_u(Ctx *c, u32 r, const char *expr) {
    if (r != RZ) line(c, "%s = %s;", R(c, r), expr);
}

static void set_f(Ctx *c, u32 r, const char *expr) {
    if (r != RZ) line(c, "%s = asuint(%s);", R(c, r), expr);
}

static void set_p(Ctx *c, u32 p, const char *expr) {
    if (p != PT) line(c, "%s = %s;", PRED(c, p, 0), expr);
}

static const char *cbuf(Ctx *c, u32 bank, u32 offset) {
    c->info->cbuf_mask |= 1u << bank;
    if (c->info->cbuf_extent[bank] < (offset & ~3u) + 4) c->info->cbuf_extent[bank] = (offset & ~3u) + 4;
    if (bank == 0 && offset >= SB_FIRST && offset < SB_END) {   /* a storage buffer descriptor: note it */
        u32 o = offset & ~15u, k = 0;
        while (k < c->info->sb_count && c->info->sb_offset[k] != o) k++;
        if (k == c->info->sb_count && k < 16) c->info->sb_offset[c->info->sb_count++] = o;
    }
    return tmp("cb%u.Load(%uu)", bank, offset & ~3u);
}

/* Operand forms. */
enum { FORM_REG, FORM_CBUF, FORM_IMM, FORM_RC, FORM_CR, FORM_32I };

static u32 imm20(u64 w) { u32 v = BITS(w, 20, 19); return BITS(w, 56, 1) ? v | 0xFFF80000u : v; }
static u32 fimm20(u64 w) { return BITS(w, 20, 19) << 12 | BITS(w, 56, 1) << 31; }

/* Operand B, as uint. */
static const char *b_u(Ctx *c, u64 w, int form) {
    switch (form) {
    case FORM_REG: return R(c, BITS(w, 20, 8));
    case FORM_CBUF: case FORM_CR: return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    case FORM_RC: return R(c, BITS(w, 39, 8));
    case FORM_IMM: return tmp("0x%08xu", imm20(w));
    case FORM_32I: return tmp("0x%08xu", BITS(w, 20, 32));
    }
    return "0u";
}

static const char *b_f(Ctx *c, u64 w, int form) {
    if (form == FORM_IMM) return tmp("asfloat(0x%08xu)", fimm20(w));
    return tmp("asfloat(%s)", b_u(c, w, form));
}

/* Operand C of three-operand instructions, as uint. */
static const char *c_u(Ctx *c, u64 w, int form) {
    if (form == FORM_RC) return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    return R(c, BITS(w, 39, 8));
}

static const char *absneg(const char *x, int abs, int neg) {
    if (abs) x = tmp("abs(%s)", x);
    if (neg) x = tmp("(-%s)", x);
    return x;
}

static int form_of(u32 op) {
    const char *n = OPS[op].name, *u = strrchr(n, '_');
    size_t len = strlen(n);
    if (len > 3 && !strcmp(n + len - 3, "32I")) return FORM_32I;   /* IADD32I, FMUL32I, HADD2_32I, ... */
    if (!u) return FORM_REG;
    if (!strcmp(u, "_reg")) return FORM_REG;
    if (!strcmp(u, "_cbuf")) return FORM_CBUF;
    if (!strcmp(u, "_imm")) return FORM_IMM;
    if (!strcmp(u, "_rc")) return FORM_RC;
    if (!strcmp(u, "_cr")) return FORM_CR;
    if (!strcmp(u, "_32I")) return FORM_32I;
    return FORM_REG;
}

/* ---- comparisons ------------------------------------------------------------------ */
static const char *fcompare(u32 op, const char *a, const char *b) {
    switch (op) {
    case 0: return "false";
    case 1: return tmp("(%s < %s)", a, b);
    case 2: return tmp("(%s == %s)", a, b);
    case 3: return tmp("(%s <= %s)", a, b);
    case 4: return tmp("(%s > %s)", a, b);
    case 5: return tmp("(%s != %s && !isnan(%s) && !isnan(%s))", a, b, a, b);
    case 6: return tmp("(%s >= %s)", a, b);
    case 7: return tmp("(!isnan(%s) && !isnan(%s))", a, b);
    case 8: return tmp("(isnan(%s) || isnan(%s))", a, b);
    case 9: return tmp("!(%s >= %s)", a, b);
    case 10: return tmp("!(%s != %s && !isnan(%s) && !isnan(%s))", a, b, a, b);
    case 11: return tmp("!(%s > %s)", a, b);
    case 12: return tmp("!(%s <= %s)", a, b);
    case 13: return tmp("(%s != %s)", a, b);
    case 14: return tmp("!(%s < %s)", a, b);
    }
    return "true";
}

static const char *icompare(u32 op, int is_signed, const char *a, const char *b) {
    if (is_signed) { a = tmp("asint(%s)", a); b = tmp("asint(%s)", b); }
    static const char *const ops[] = {"", "<", "==", "<=", ">", "!=", ">="};
    if (op == 0) return "false";
    if (op == 7) return "true";
    return tmp("(%s %s %s)", a, ops[op], b);
}

static const char *combine(u32 bop, const char *x, const char *pred) {
    if (!strcmp(pred, "true") && bop == 0) return x;
    static const char *const ops[] = {"&&", "||", "!="};
    return tmp("(%s %s %s)", x, ops[bop < 3 ? bop : 0], pred);
}

/* ---- textures ------------------------------------------------------------------------ */
static u32 texture_slot(Ctx *c, u32 handle_offset, u32 type, int shadow) {
    ShaderInfo *i = c->info;
    for (u32 k = 0; k < i->ntex; k++)
        if (i->tex[k].handle_offset == handle_offset && i->tex[k].type == type && i->tex[k].shadow == shadow) return k;
    if (i->ntex == 32) return 31;
    i->tex[i->ntex] = (ShaderTexture){handle_offset, (u8)type, (u8)shadow};
    return i->ntex++;
}

/* Can this stage compute gradients here? (Implicit-LOD sampling needs them.) */
static int gradients(Ctx *c) { return c->stage == SHADER_PIXEL && !c->dispatch; }

static const char *coord_of(u32 type, const char *x, const char *y, const char *z, const char *layer) {
    switch (type) {
    case TEX_1D: return x;
    case TEX_ARRAY_1D: return tmp("float2(%s, %s)", x, layer);
    case TEX_2D: return tmp("float2(%s, %s)", x, y);
    case TEX_ARRAY_2D: return tmp("float3(%s, %s, %s)", x, y, layer);
    case TEX_3D: case TEX_CUBE: return tmp("float3(%s, %s, %s)", x, y, z);
    case TEX_ARRAY_CUBE: return tmp("float4(%s, %s, %s, %s)", x, y, z, layer);
    }
    return x;
}

/* A sample expression. lod_mode: 0 implicit, 1 zero, 2 bias, 3 explicit. */
static const char *sample(Ctx *c, u32 k, u32 type, const char *coord, int lod_mode, const char *lod, const char *dref,
                          const char *offset) {
    const char *t = tmp("tex%u", k), *s = tmp("smp%u", k);
    if ((lod_mode == 0 || lod_mode == 2) && !gradients(c)) lod_mode = 1;
    const char *off = offset ? tmp(", %s", offset) : "";
    /* MSL: the sampler's LOD bias, where Metal's has none (before macOS 26): lodb_, added to
     * every LOD a sample takes - level 0's and compares' too, as Direct3D, Vulkan and Metal's
     * own lodBias add it (a compare's LOD is HLSL's: implicit, or 0) */
    const char *b = c->msl ? tmp("lodb_(%uu)", k) : NULL;
    if (dref) {   /* depth compare: one value */
        if (b) return tmp("%s.%s(%s, %s, %s, %s%s).xxxx", t, lod_mode == 0 || lod_mode == 2 ? "SampleCmpBias" : "SampleCmpLevel", s, coord, dref, b, off);
        if (lod_mode == 0 || lod_mode == 2) return tmp("%s.SampleCmp(%s, %s, %s%s).xxxx", t, s, coord, dref, off);
        return tmp("%s.SampleCmpLevelZero(%s, %s, %s%s).xxxx", t, s, coord, dref, off);
    }
    (void)type;
    if (b) {
        if (lod_mode == 0) return tmp("%s.SampleBias(%s, %s, %s%s)", t, s, coord, b, off);
        if (lod_mode == 1) return tmp("%s.SampleLevel(%s, %s, %s%s)", t, s, coord, b, off);
        return tmp("%s.%s(%s, %s, (%s) + %s%s)", t, lod_mode == 2 ? "SampleBias" : "SampleLevel", s, coord, lod, b, off);
    }
    switch (lod_mode) {
    case 0: return tmp("%s.Sample(%s, %s%s)", t, s, coord, off);
    case 2: return tmp("%s.SampleBias(%s, %s, %s%s)", t, s, coord, lod, off);
    case 3: return tmp("%s.SampleLevel(%s, %s, %s%s)", t, s, coord, lod, off);
    }
    return tmp("%s.SampleLevel(%s, %s, 0%s)", t, s, coord, off);
}

/* Write the components selected by `mask` of float4 variable `v` to the TEXS-style
 * destinations: slots 0,1 -> a, a+1; slots 2,3 -> b, b+1 (or packed halves for F16). */
static void store_split(Ctx *c, const char *v, u32 mask, u32 da, u32 db, int f16) {
    const char *comp[4] = {"x", "y", "z", "w"};
    const char *sel[4];
    u32 n = 0;
    for (u32 i = 0; i < 4; i++) if (mask >> i & 1) sel[n++] = comp[i];
    if (f16) {
        if (n >= 1) set_u(c, da, tmp("f32tof16(%s.%s) | (%s << 16)", v, sel[0], n >= 2 ? tmp("f32tof16(%s.%s)", v, sel[1]) : "0u"));
        if (n >= 3) set_u(c, db, tmp("f32tof16(%s.%s) | (%s << 16)", v, sel[2], n >= 4 ? tmp("f32tof16(%s.%s)", v, sel[3]) : "0u"));
        return;
    }
    u32 dst[4] = {da, da == RZ ? RZ : da + 1, db, db == RZ ? RZ : db + 1};
    for (u32 i = 0; i < n; i++) set_f(c, dst[i], tmp("%s.%s", v, sel[i]));
}

/* Consecutive destinations for the components selected by mask (TEX, TLD4). */
static void store_seq(Ctx *c, const char *v, u32 mask, u32 d) {
    const char *comp[4] = {"x", "y", "z", "w"};
    for (u32 i = 0; i < 4; i++)
        if (mask >> i & 1) { set_f(c, d, tmp("%s.%s", v, comp[i])); if (d != RZ) d++; }
}

static const char *layer_of(Ctx *c, u32 r) { return tmp("(float)(%s & 0xFFFFu)", R(c, r)); }

/* Names for texture results: in HLSL counted on from program to program (as they always were);
 * in MSL from 0 in each, so that a program's text - the key of Metal's own cache - is always
 * the same, and translating on several threads at once is safe. */
static u32 tex_counter;
static u32 next_tex(Ctx *c) { return c->msl ? c->tex_counter++ : tex_counter++; }

static void emit_texs(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz] : RGBA[swz];
    u32 type = TEX_2D; int lod = 0; const char *coord, *lodx = NULL, *dref = NULL;
    switch (enc) {
    case 0: type = TEX_1D; lod = 1; coord = F(c, a); break;
    case 1: coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 2: lod = 1; coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 3: lod = 3; lodx = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 4: dref = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 5: lod = 3; lodx = F(c, b); dref = F(c, b + 1); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 6: lod = 1; dref = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 7: type = TEX_ARRAY_2D; coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 8: type = TEX_ARRAY_2D; lod = 1; coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 9: type = TEX_ARRAY_2D; lod = 1; dref = F(c, b + 1); coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 10: type = TEX_3D; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 11: type = TEX_3D; lod = 1; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 12: type = TEX_CUBE; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 13: type = TEX_CUBE; lod = 3; lodx = F(c, b + 1); coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    default: unsupported(c, in, "TEXS encoding"); return;
    }
    u32 k = texture_slot(c, handle, type, dref != NULL);
    const char *v = tmp("t%u", next_tex(c));
    line(c, dref ? "{ float4 %s = %s;" : "{ float4 %s = swz(%s, %uu);", v, sample(c, k, type, coord, lod, lodx, dref, NULL), k);
    if (dref) line(c, "%s.w = 1.0f;", v);
    c->indent++;
    store_split(c, v, mask, da, db, f16);
    c->indent--;
    line(c, "}");
}

static void emit_tex(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), aoffi = BITS(w, 54, 1), blod = BITS(w, 55, 3);
    const char *x, *y = 0, *z = 0, *layer = 0;
    switch (type) {
    case TEX_1D: x = F(c, a); break;
    case TEX_ARRAY_1D: x = F(c, a + 1); layer = layer_of(c, a); break;
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_3D: case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TEX type"); return;
    }
    int lod = blod == 1 ? 1 : blod == 2 || blod == 6 ? 2 : blod == 3 || blod == 7 ? 3 : 0;
    const char *lodx = NULL, *dref = NULL, *offset = NULL;
    if (lod >= 2) lodx = F(c, m++);
    if (aoffi) {
        const char *o = R(c, m++);
        offset = tmp("int2(((int)(%s << 28)) >> 28, ((int)(%s << 24)) >> 28)", o, o);   /* 4-bit signed */
    }
    if (dc) dref = F(c, m++);
    u32 k = texture_slot(c, handle, type, dc);
    const char *coord = coord_of(type, x, y, z, layer);
    const char *v = tmp("t%u", next_tex(c));
    /* HLSL's Sample needs constant offsets: shift the coordinates instead, in the game's texels
     * (tsc_). MSL's sample takes any offset, in -8..7 as these are, except on cube maps: there
     * the coordinates move by the rest of the offset on a scaled render target (by 0 at 1) */
    if (offset && !(c->msl && type != TEX_CUBE && type != TEX_ARRAY_CUBE)) {
        line(c, "{ float tw_, th_, tl_; tex%u.GetDimensions(0, tw_, th_, tl_);", k);
        coord = coord_of(type, tmp("(%s + float(%s.x) * tsc_(%uu) / tw_)", x, offset, k),
                         y ? tmp("(%s + float(%s.y) * tsc_(%uu) / th_)", y, offset, k) : 0, z, layer);
        offset = NULL;
    } else if (offset) {
        line(c, "{");
        coord = coord_of(type, tmp("(%s + float(%s.x) * (tsc_(%uu) - 1.0f) / float(tex%u.t.get_width()))", x, offset, k, k),
                         y ? tmp("(%s + float(%s.y) * (tsc_(%uu) - 1.0f) / float(tex%u.t.get_height()))", y, offset, k, k) : 0, z, layer);
    } else line(c, "{");
    line(c, dref ? "  float4 %s = %s;" : "  float4 %s = swz(%s, %uu);", v, sample(c, k, type, coord, lod, lodx, dref, offset), k);
    if (dref) line(c, "  %s.w = 1.0f;", v);
    c->indent++;
    store_seq(c, v, mask, d);
    c->indent--;
    line(c, "}");
}

static const char *gather(Ctx *c, u32 k, u32 comp, const char *coord, const char *dref, const char *offset) {
    static const char *const ch[4] = {"Red", "Green", "Blue", "Alpha"};
    const char *off = offset ? tmp(", %s", offset) : "";
    if (dref) return tmp("tex%u.GatherCmp(smp%u, %s, %s%s)", k, k, coord, dref, off);
    return tmp("tex%u.Gather%s(smp%u, %s%s)", k, ch[comp & 3], k, coord, off);
}

static void emit_tld4(Ctx *c, const Insn *in, int scalar) {
    u64 w = in->w;
    const char *v = tmp("t%u", next_tex(c));
    if (scalar) {   /* TLD4S: 2D only */
        u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8), handle = BITS(w, 36, 13) * 4;
        u32 dc = BITS(w, 50, 1), aoffi = BITS(w, 51, 1), comp = BITS(w, 52, 2), f16 = BITS(w, 55, 1);
        const char *coord, *dref = NULL, *offset = NULL;
        if (!aoffi && !dc) coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0);
        else {
            coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0);
            if (aoffi) {
                const char *o = R(c, b);
                offset = tmp("int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26)", o, o);
                if (dc) dref = F(c, b + 1);
            } else dref = F(c, b);
        }
        u32 k = texture_slot(c, handle, TEX_2D, dc);
        if (offset) offset = tmp("toff_(%s, %uu)", offset, k);   /* (in the game's texels) */
        line(c, "{ float4 %s = %s;", v, gather(c, k, comp, coord, dref, offset));
        c->indent++;
        store_split(c, v, 15, da, db, f16);
        c->indent--;
        line(c, "}");
        return;
    }
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), offmode = BITS(w, 54, 2), comp = BITS(w, 56, 2);
    const char *x, *y = 0, *z = 0, *layer = 0, *offset = NULL, *dref = NULL;
    switch (type) {
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TLD4 type"); return;
    }
    u32 k = texture_slot(c, handle, type, dc);
    /* (offsets are in the game's texels: toff_ turns them into a render target's own) */
    if (offmode == 1) {
        const char *o = R(c, m++);
        offset = tmp("toff_(int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26), %uu)", o, o, k);
    } else if (offmode == 2) {   /* a separate offset per texel: signed 6-bit x, y pairs in two registers */
        const char *o0 = R(c, m), *o1 = R(c, m + 1);
        m += 2;
        offset = tmp("toff_(int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26), %uu), "
                     "toff_(int2(((int)(%s << 10)) >> 26, ((int)(%s << 2)) >> 26), %uu), "
                     "toff_(int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26), %uu), "
                     "toff_(int2(((int)(%s << 10)) >> 26, ((int)(%s << 2)) >> 26), %uu)",
                     o0, o0, k, o0, o0, k, o1, o1, k, o1, o1, k);
    }
    if (dc) dref = F(c, m);
    if (offmode == 2 && c->four_gathers) {   /* four gathers, each with its own offset, one component of each (what the four-offset form returns) */
        const char *o0 = R(c, m - 2), *o1 = R(c, m - 1);   /* (the two offset registers read above) */
        static const char *const part[4][2] = {{"<< 26", "<< 18"}, {"<< 10", "<< 2"}, {"<< 26", "<< 18"}, {"<< 10", "<< 2"}};
        static const char *const xyzw[4] = {"x", "y", "z", "w"};
        line(c, "{ float4 %s;", v);
        for (u32 i = 0; i < 4; i++) {
            const char *o = i < 2 ? o0 : o1;
            const char *one = tmp("toff_(int2(((int)(%s %s)) >> 26, ((int)(%s %s)) >> 26), %uu)", o, part[i][0], o, part[i][1], k);
            line(c, "  %s.%s = (%s).%s;", v, xyzw[i], gather(c, k, comp, coord_of(type, x, y, z, layer), dref, one), xyzw[i]);
        }
    } else
    line(c, "{ float4 %s = %s;", v, gather(c, k, comp, coord_of(type, x, y, z, layer), dref, offset));
    c->indent++;
    store_seq(c, v, mask, d);
    c->indent--;
    line(c, "}");
}

static void emit_tlds(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz] : RGBA[swz];
    u32 type = TEX_2D;
    const char *load;
    const char *ia = tmp("asint(%s)", R(c, a));
    switch (enc) {
    case 0: type = TEX_1D; load = tmp("int2(%s, 0)", ia); break;
    case 1: type = TEX_1D; load = tmp("int2(%s, asint(%s))", ia, R(c, b)); break;
    case 2: load = tmp("int3(%s, asint(%s), 0)", ia, R(c, b)); break;
    case 4: load = tmp("int3(%s, asint(%s), 0)", ia, R(c, a + 1)); break;   /* offset ignored */
    case 5: load = tmp("int3(%s, asint(%s), asint(%s))", ia, R(c, a + 1), R(c, b)); break;
    case 7: type = TEX_3D; load = tmp("int4(%s, asint(%s), asint(%s), 0)", ia, R(c, a + 1), R(c, b)); break;
    case 8: type = TEX_ARRAY_2D; load = tmp("int4(asint(%s), asint(%s), (int)(%s & 0xFFFFu), 0)", R(c, b), R(c, b + 1), R(c, a)); break;
    case 12: load = tmp("int3(%s, asint(%s), asint(%s))", ia, R(c, a + 1), R(c, b)); break;
    default: unsupported(c, in, "TLDS encoding"); return;
    }
    u32 k = texture_slot(c, handle, type, 0);
    const char *v = tmp("t%u", next_tex(c));
    line(c, "{ float4 %s = swz(tex%u.Load(tld_(%s, %uu)), %uu);", v, k, load, k, k);   /* (texel numbers: the game's) */
    c->indent++;
    store_split(c, v, mask, da, db, f16);
    c->indent--;
    line(c, "}");
}

/* ---- attributes -------------------------------------------------------------------- */
static const char *const XYZW[4] = {"x", "y", "z", "w"};

/* A vertex shader input attribute (ALD), as uint. */
static const char *vs_input(Ctx *c, u32 addr) {
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        return tmp("va%u.%s", n, XYZW[(addr >> 2) & 3]);
    }
    if (addr == 0x2F8) return "(iid + params.base.y)";
    if (addr == 0x2FC) return "vid";
    return "0u";
}

static void vs_output(Ctx *c, u32 addr, const char *value) {
    if (addr >= 0x70 && addr < 0x80) line(c, "o.pos.%s = asfloat(%s);", XYZW[(addr >> 2) & 3], value);
    else if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << n;
        line(c, "o.a%u.%s = asfloat(%s);", n, XYZW[(addr >> 2) & 3], value);
    }
    /* point size, layer, viewport index, clip distances: not forwarded */
}

/* A pixel shader input (IPA), as float, before the IPA modifiers. */
static const char *ps_input(Ctx *c, u32 addr, int *perspective) {
    *perspective = 0;
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        u32 comp = (addr >> 2) & 3, mode = (c->prog[0x18 + n] >> (comp * 2)) & 3;
        *perspective = mode == 2;
        if (!(c->vs_outputs >> n & 1)) return "0.0f";
        return tmp("i.a%u.%s", n, XYZW[comp]);
    }
    /* The fragment position in the game's pixels: SV_Position is in the render target's own,
     * render scale included (base.zw: 1 / that scale). See shader_uses_pixels. */
    switch (addr) {
    case 0x70: return "(i.pos.x * asfloat(params.base.z))";
    case 0x74: return "(params.frag[0] != 0 ? params.frag[1] - i.pos.y * asfloat(params.base.w) : i.pos.y * asfloat(params.base.w))";
    case 0x78: return "i.pos.z";
    case 0x7C: return "(1.0f / i.clipw)";
    case 0x3FC: return "asfloat(front ? 0xFFFFFFFFu : 0u)";
    case 0x2E0: case 0x2E4: return "0.0f";
    }
    return "0.0f";
}

static void emit_exit(Ctx *c) {
    if (c->stage == SHADER_COMPUTE) {
        line(c, "return;");
        return;
    }
    if (c->stage == SHADER_VERTEX) {
        line(c, "return finish(o);");
        return;
    }
    const u8 *h = c->prog;
    u32 target = *(const u32 *)(h + 0x48), flags = *(const u32 *)(h + 0x4C), reg = 0;
    line(c, c->msl ? "{ PSOut o = {};" : "{ PSOut o = (PSOut)0;");
    for (u32 rt = 0; rt < 8; rt++) {
        u32 m = (target >> (rt * 4)) & 15;
        if (!m) continue;
        c->info->color_mask |= 1u << rt;
        for (u32 k = 0; k < 4; k++)
            if (m >> k & 1) line(c, "  o.c%u.%s = %s;", rt, XYZW[k], F(c, reg++));
    }
    if (flags & 2) { c->info->writes_depth = 1; line(c, "  o.depth = %s;", F(c, reg + 1)); }
    if (c->alpha_test && (target & 15)) line(c, c->msl ? "  if (!alpha_pass(o.c0.w)) discard_fragment();" : "  if (!alpha_pass(o.c0.w)) discard;");
    line(c, "  return o; }");
}

/* ---- control flow ------------------------------------------------------------------- */
static i32 branch_offset(u64 w) { i32 o = (i32)BITS(w, 20, 24); return o & 0x800000 ? o - 0x1000000 : o; }

/* Every fourth word, from the code's start after the 0x50-byte header, holds scheduling
 * controls for the next three instructions; a branch to one goes to the first of them. */
static u32 align_target(u32 t) { return (t & 31) == 0x10 ? t + 8 : t; }
static u32 branch_target(const Insn *in) { return align_target(in->addr + 8 + (u32)branch_offset(in->w)); }

/* A control instruction's flow test, as an expression; 0 when it can never pass. Tests on
 * the condition codes read the flags the CC forms of instructions set; the culling tests
 * (CSM/FCSM, fed by VOTE.VTG in vertex shaders) never pass, as nothing is culled early. */
static int flow_true(Ctx *c, const Insn *in, const char **cond) {
    u32 test = BITS(in->w, 0, 5);
    switch (test) {
    case 15: *cond = "true"; return 1;
    case 0: case 16: case 18: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
        *cond = "false"; return 0;
    }
    c->uses_cc = 1;
    switch (test) {
    case 1: case 9: *cond = "cc_s"; break;                     /* LT */
    case 2: case 10: *cond = "cc_z"; break;                    /* EQ */
    case 3: case 11: *cond = "(cc_s || cc_z)"; break;          /* LE */
    case 4: case 12: *cond = "(!cc_s && !cc_z)"; break;        /* GT */
    case 5: case 13: *cond = "!cc_z"; break;                   /* NE */
    case 6: case 14: *cond = "!cc_s"; break;                   /* GE */
    case 17: *cond = "(cc_c == 0u)"; break;                    /* LO: carry clear */
    case 19: *cond = "(cc_c == 0u || cc_z)"; break;            /* LS */
    case 20: *cond = "(cc_c != 0u && !cc_z)"; break;           /* HI */
    case 22: *cond = "(cc_c != 0u)"; break;                    /* HS */
    case 30: *cond = "(cc_s || cc_z)"; break;                  /* RLE */
    case 31: *cond = "(!cc_s && !cc_z)"; break;                /* RGT */
    default: *cond = "false"; return 0;                        /* NUM, NaN */
    }
    return 1;
}

static void goto_pc(Ctx *c, u32 target) { line(c, "npc_ = 0x%xu; break;", target); }

/* ---- instructions ------------------------------------------------------------------- */
/* A constant shift count: Direct3D's shifts use its low 5 bits, MSL's (C++'s) by 32 or more
 * are undefined. */
static u32 shift(Ctx *c, u32 n) { return c->msl ? n & 31 : n; }

static const char *half(const char *x, u32 swizzle, int hi) {   /* one half of a packed pair, as float */
    switch (swizzle) {
    case 0: return tmp(hi ? "f16tof32(%s >> 16)" : "f16tof32(%s)", x);   /* H1_H0 */
    case 1: return tmp("asfloat(%s)", x);                                  /* F32 */
    case 2: return tmp("f16tof32(%s)", x);                                 /* H0_H0 */
    }
    return tmp("f16tof32(%s >> 16)", x);                                   /* H1_H1 */
}

static void merge_halves(Ctx *c, u32 d, const char *lo, const char *hi, u32 merge) {
    if (d == RZ) return;
    switch (merge) {
    case 0: set_u(c, d, tmp("f32tof16(%s) | (f32tof16(%s) << 16)", lo, hi)); break;
    case 1: set_f(c, d, lo); break;
    case 2: set_u(c, d, tmp("(%s & 0xFFFF0000u) | f32tof16(%s)", R(c, d), lo)); break;
    default: set_u(c, d, tmp("(%s & 0x0000FFFFu) | (f32tof16(%s) << 16)", R(c, d), hi)); break;
    }
}

/* Half-precision pair operations: kind 0 add, 1 mul, 2 fma. */
static void emit_half(Ctx *c, const Insn *in, int kind) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8);
    const char *a = R(c, BITS(w, 8, 8)), *b, *cc = NULL;
    u32 swa = BITS(w, 47, 2), swb = 0, swc = 0, merge = BITS(w, 49, 2), sat = 0;
    int abs_a = 0, neg_a = 0, abs_b = 0, neg_b = 0, neg_c = 0;
    if (kind == 0) {   /* HADD2 */
        neg_a = BITS(w, 43, 1); abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            sat = BITS(w, 52, 1); break;
        default: b = tmp("0x%08xu", BITS(w, 20, 32)); sat = BITS(w, 52, 1); neg_a = BITS(w, 56, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else if (kind == 1) {   /* HMUL2 */
        abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_a = BITS(w, 43, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            sat = BITS(w, 52, 1); neg_a = BITS(w, 43, 1); break;
        default: b = tmp("0x%08xu", BITS(w, 20, 32)); sat = BITS(w, 52, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else {   /* HFMA2 */
        switch (form) {
        case FORM_REG:
            b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); swb = BITS(w, 28, 2); neg_b = BITS(w, 31, 1);
            neg_c = BITS(w, 30, 1); sat = BITS(w, 32, 1); swc = BITS(w, 35, 2); break;
        case FORM_RC:
            b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swb = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swc = 1; break;
        case FORM_CR:
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1); sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); break;
        default:
            b = tmp("0x%08xu", BITS(w, 20, 32)); cc = R(c, BITS(w, 0, 8)); neg_c = BITS(w, 52, 1); swa = BITS(w, 53, 2);
            merge = 0; break;
        }
    }
    const char *lo, *hi;
    const char *a0 = absneg(half(a, swa, 0), abs_a, neg_a), *a1 = absneg(half(a, swa, 1), abs_a, neg_a);
    const char *b0 = absneg(half(b, swb, 0), abs_b, neg_b), *b1 = absneg(half(b, swb, 1), abs_b, neg_b);
    if (kind == 0) { lo = tmp("(%s + %s)", a0, b0); hi = tmp("(%s + %s)", a1, b1); }
    else if (kind == 1) { lo = tmp("(%s * %s)", a0, b0); hi = tmp("(%s * %s)", a1, b1); }
    else {
        const char *c0 = absneg(half(cc, swc, 0), 0, neg_c), *c1 = absneg(half(cc, swc, 1), 0, neg_c);
        lo = tmp("mad(%s, %s, %s)", a0, b0, c0); hi = tmp("mad(%s, %s, %s)", a1, b1, c1);
    }
    if (sat) { lo = tmp("saturate(%s)", lo); hi = tmp("saturate(%s)", hi); }
    line(c, "{ float lo_ = %s, hi_ = %s;", lo, hi);
    c->indent++;
    merge_halves(c, d, "lo_", "hi_", merge);
    c->indent--;
    line(c, "}");
}

static void emit_xmad(Ctx *c, const Insn *in) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8), sa = BITS(w, 48, 1), sb = BITS(w, 49, 1), ha = BITS(w, 53, 1);
    u32 hb = 0, psl = 0, mrg = 0, x = 0, mode = 0;
    const char *b, *cc;
    switch (form) {
    case FORM_REG: hb = BITS(w, 35, 1); psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); break;
    case FORM_RC: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1);
        b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
    case FORM_CR: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1); psl = BITS(w, 55, 1); mrg = BITS(w, 56, 1);
        b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); break;
    default: psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = tmp("0x%xu", BITS(w, 20, 16)); cc = R(c, BITS(w, 39, 8)); break;
    }
    const char *a = R(c, BITS(w, 8, 8));
    const char *oa = sa ? tmp("asuint(((int)(%s << %u)) >> 16)", a, ha ? 0 : 16) : tmp(ha ? "(%s >> 16)" : "(%s & 0xFFFFu)", a);
    const char *ob = sb ? tmp("asuint(((int)(%s << %u)) >> 16)", b, hb ? 0 : 16) : tmp(hb ? "(%s >> 16)" : "(%s & 0xFFFFu)", b);
    const char *oc;
    switch (mode) {
    case 1: oc = tmp("(%s & 0xFFFFu)", cc); break;
    case 2: oc = tmp("(%s >> 16)", cc); break;
    case 4: oc = tmp("(%s + (%s << 16))", cc, b); break;
    default: oc = cc; break;
    }
    const char *prod = tmp("(%s * %s)", oa, ob);
    if (psl) prod = tmp("(%s << 16)", prod);
    const char *sum = tmp("(%s + %s%s)", prod, oc, x ? " + cc_c" : "");
    if (x) c->uses_cc = 1;
    if (mrg) sum = tmp("((%s & 0xFFFFu) | (%s << 16))", sum, b);
    set_u(c, d, sum);
}

static void emit_insn(Ctx *c, const Insn *in) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8), ra = BITS(w, 8, 8);
    switch (in->op) {
    /* -- floating point -- */
    case OP_FADD_reg: case OP_FADD_cbuf: case OP_FADD_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *b = absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        const char *r = tmp("(%s + %s)", a, b);
        set_f(c, d, BITS(w, 50, 1) ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_FADD32I: {
        const char *a = absneg(F(c, ra), BITS(w, 54, 1), BITS(w, 56, 1)), *b = absneg(b_f(c, w, FORM_32I), BITS(w, 57, 1), BITS(w, 53, 1));
        set_f(c, d, tmp("(%s + %s)", a, b));
        break;
    }
    case OP_FMUL_reg: case OP_FMUL_cbuf: case OP_FMUL_imm: case OP_FMUL32I: {
        int is32 = in->op == OP_FMUL32I;
        const char *a = F(c, ra), *b = absneg(b_f(c, w, form), 0, is32 ? 0 : BITS(w, 48, 1));
        u32 fmz = is32 ? BITS(w, 53, 2) : BITS(w, 44, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        static const char *const scale[8] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        const char *sa = is32 ? "" : scale[BITS(w, 41, 3)];
        const char *r = tmp("((%s%s) * %s)", a, sa, b);
        if (fmz == 2 && !sat) r = tmp("((%s == 0.0f || %s == 0.0f) ? 0.0f : %s)", a, b, r);
        set_f(c, d, sat ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_FFMA_reg: case OP_FFMA_rc: case OP_FFMA_cr: case OP_FFMA_imm: case OP_FFMA32I: {
        int is32 = in->op == OP_FFMA32I;
        const char *a = absneg(F(c, ra), 0, is32 ? BITS(w, 56, 1) : 0);
        const char *b = is32 ? b_f(c, w, FORM_32I) : absneg(b_f(c, w, form), 0, BITS(w, 48, 1));
        const char *cc = absneg(is32 ? F(c, d) : tmp("asfloat(%s)", c_u(c, w, form)), 0, is32 ? BITS(w, 57, 1) : BITS(w, 49, 1));
        u32 fmz = BITS(w, 53, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        const char *r = tmp("mad(%s, %s, %s)", a, b, cc);
        if (fmz == 2 && !sat) r = tmp("((%s == 0.0f || %s == 0.0f) ? %s : %s)", a, b, cc, r);
        set_f(c, d, sat ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_MUFU: {
        const char *x = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *r;
        switch (BITS(w, 20, 4)) {
        case 0: r = tmp("cos(%s)", x); break;
        case 1: r = tmp("sin(%s)", x); break;
        case 2: r = tmp("exp2(%s)", x); break;
        case 3: r = tmp("log2(%s)", x); break;
        case 4: r = tmp("(1.0f / %s)", x); break;
        case 5: r = tmp("rsqrt(%s)", x); break;
        case 8: r = tmp("sqrt(%s)", x); break;
        default: unsupported(c, in, "MUFU operation"); return;
        }
        set_f(c, d, BITS(w, 50, 1) ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_RRO_reg: case OP_RRO_cbuf: case OP_RRO_imm:
        set_f(c, d, absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1)));
        break;
    case OP_FMNMX_reg: case OP_FMNMX_cbuf: case OP_FMNMX_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *b = absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        const char *p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        set_f(c, d, tmp("(%s ? min(%s, %s) : max(%s, %s))", p, a, b, a, b));
        break;
    }
    case OP_FSETP_reg: case OP_FSETP_cbuf: case OP_FSETP_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 7, 1), BITS(w, 43, 1)), *b = absneg(b_f(c, w, form), BITS(w, 44, 1), BITS(w, 6, 1));
        const char *cmp = fcompare(BITS(w, 48, 4), a, b), *bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 bop = BITS(w, 45, 2);
        line(c, "{ bool cmp_ = %s;", cmp);
        c->indent++;
        set_p(c, BITS(w, 3, 3), combine(bop, "cmp_", bp));
        set_p(c, BITS(w, 0, 3), combine(bop, "!cmp_", bp));
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_FSET_reg: case OP_FSET_cbuf: case OP_FSET_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 54, 1), BITS(w, 43, 1)), *b = absneg(b_f(c, w, form), BITS(w, 44, 1), BITS(w, 53, 1));
        const char *r = combine(BITS(w, 45, 2), fcompare(BITS(w, 48, 4), a, b), PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 52, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_CSET: case OP_CSETP: {   /* a condition code test, combined with a predicate */
        Insn t = *in;
        const char *cond;
        t.w = BITS(w, 8, 5);
        if (!flow_true(c, &t, &cond)) cond = "false";
        const char *pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (in->op == OP_CSET)
            set_u(c, d, tmp("(%s ? %s : 0u)", combine(BITS(w, 45, 2), cond, pred), BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        else {
            line(c, "{ bool t_ = %s;", cond);
            set_p(c, BITS(w, 3, 3), combine(BITS(w, 45, 2), "t_", pred));
            set_p(c, BITS(w, 0, 3), combine(BITS(w, 45, 2), "!t_", pred));
            line(c, "}");
        }
        break;
    }
    case OP_HSET2_reg: case OP_HSET2_cbuf: case OP_HSET2_imm:
    case OP_HSETP2_reg: case OP_HSETP2_cbuf: case OP_HSETP2_imm: {   /* compare two half pairs */
        int setp = in->op == OP_HSETP2_reg || in->op == OP_HSETP2_cbuf || in->op == OP_HSETP2_imm;
        const char *b;
        u32 swb, abs_b = 0, neg_b = 0, cmp, flag;   /* flag: HSET2 BF (1.0 for true), HSETP2 H_AND */
        if (form == FORM_REG) {
            b = R(c, BITS(w, 20, 8)); swb = BITS(w, 28, 2); abs_b = BITS(w, 30, 1); neg_b = BITS(w, 31, 1);
            cmp = BITS(w, 35, 4); flag = BITS(w, 49, 1);
        } else if (form == FORM_CBUF) {
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); swb = 1; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
            neg_b = BITS(w, 56, 1); abs_b = setp ? BITS(w, 54, 1) : 0;
        } else {
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            swb = 0; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
        }
        const char *a = R(c, ra);
        u32 swa = BITS(w, 47, 2), abs_a = BITS(w, 44, 1), neg_a = BITS(w, 43, 1);
        const char *pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        const char *lo = combine(BITS(w, 45, 2), fcompare(cmp, absneg(half(a, swa, 0), abs_a, neg_a), absneg(half(b, swb, 0), abs_b, neg_b)), pred);
        const char *hi = combine(BITS(w, 45, 2), fcompare(cmp, absneg(half(a, swa, 1), abs_a, neg_a), absneg(half(b, swb, 1), abs_b, neg_b)), pred);
        if (!setp) {
            const char *t = flag ? "0x3C00u" : "0xFFFFu";
            set_u(c, d, tmp("((%s ? %s : 0u) | ((%s ? %s : 0u) << 16))", lo, t, hi, t));
        } else {
            line(c, "{ bool lo_ = %s, hi_ = %s;", lo, hi);
            if (flag) { set_p(c, BITS(w, 3, 3), "(lo_ && hi_)"); set_p(c, BITS(w, 0, 3), "!(lo_ && hi_)"); }
            else { set_p(c, BITS(w, 3, 3), "lo_"); set_p(c, BITS(w, 0, 3), "hi_"); }
            line(c, "}");
        }
        break;
    }
    case OP_F2F_reg: case OP_F2F_cbuf: case OP_F2F_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), rop = BITS(w, 39, 4) & 0xB;
        const char *x = src == 1 ? half(b_u(c, w, form), 0, BITS(w, 41, 1)) : b_f(c, w, form);
        if (src == 1) x = tmp(BITS(w, 41, 1) ? "f16tof32(%s >> 16)" : "f16tof32(%s)", b_u(c, w, form));
        x = absneg(x, BITS(w, 49, 1), BITS(w, 45, 1));
        if (src == dst) {
            switch (rop) {
            case 8: x = tmp(c->msl ? "rint(%s)" : "round(%s)", x); break;   /* to nearest even (MSL's round: away from 0) */
            case 9: x = tmp("floor(%s)", x); break;
            case 10: x = tmp("ceil(%s)", x); break;
            case 11: x = tmp("trunc(%s)", x); break;
            }
        }
        if (BITS(w, 50, 1)) x = tmp("saturate(%s)", x);
        if (dst == 1) set_u(c, d, tmp("f32tof16(%s)", x));
        else set_f(c, d, x);
        break;
    }
    case OP_F2I_reg: case OP_F2I_cbuf: case OP_F2I_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), is_signed = BITS(w, 12, 1), rnd = BITS(w, 39, 2);
        const char *x = src == 1 ? tmp(BITS(w, 41, 1) ? "f16tof32(%s >> 16)" : "f16tof32(%s)", b_u(c, w, form)) : b_f(c, w, form);
        x = absneg(x, BITS(w, 45, 1), BITS(w, 49, 1));
        static const char *const rf[4] = {"round", "floor", "ceil", "trunc"};
        x = tmp("%s(%s)", rnd == 0 && c->msl ? "rint" : rf[rnd], x);
        if (dst == 1) x = is_signed ? tmp("clamp(%s, -32768.0f, 32767.0f)", x) : tmp("clamp(%s, 0.0f, 65535.0f)", x);
        if (c->msl) set_u(c, d, is_signed ? tmp("asuint(f2i_(%s))", x) : tmp("f2u_(%s)", x));   /* (saturating: see MSL_PRELUDE) */
        else set_u(c, d, is_signed ? tmp("asuint((int)%s)", x) : tmp("(uint)%s", x));
        break;
    }
    case OP_I2F_reg: case OP_I2F_cbuf: case OP_I2F_imm: {
        u32 ff = BITS(w, 8, 2), fmt = BITS(w, 10, 2), is_signed = BITS(w, 13, 1), sel = BITS(w, 41, 2);
        const char *x = b_u(c, w, form);
        if (fmt == 0) x = is_signed ? tmp("(((int)(%s << %u)) >> 24)", x, shift(c, 24 - sel * 8)) : tmp("((%s >> %u) & 0xFFu)", x, sel * 8);
        else if (fmt == 1) x = is_signed ? tmp("(((int)(%s << %u)) >> 16)", x, shift(c, 16 - sel * 8)) : tmp("((%s >> %u) & 0xFFFFu)", x, sel * 8);
        else if (is_signed) x = tmp("asint(%s)", x);
        if (BITS(w, 49, 1)) x = is_signed ? tmp(c->msl ? "asint(iabs_(asuint(%s)))" : "abs(%s)", x) : x;
        const char *f = tmp("(float)(%s)", x);
        if (BITS(w, 45, 1)) f = tmp("(-%s)", f);
        if (ff == 1) set_u(c, d, tmp("f32tof16(%s)", f));
        else set_f(c, d, f);
        break;
    }
    case OP_I2I_reg: case OP_I2I_cbuf: case OP_I2I_imm: {
        u32 dfmt = BITS(w, 8, 2), sfmt = BITS(w, 10, 2), dsig = BITS(w, 12, 1), ssig = BITS(w, 13, 1), sel = BITS(w, 41, 3);
        const char *x = b_u(c, w, form);
        u32 bits = sfmt == 0 ? 8 : sfmt == 1 ? 16 : 32;
        if (bits < 32)
            x = ssig ? tmp("asuint(((int)(%s << %u)) >> %u)", x, shift(c, 32 - bits - sel * 8), 32 - bits)
                     : tmp("((%s >> %u) & 0x%xu)", x, shift(c, sel * 8), (1u << bits) - 1);
        if (BITS(w, 49, 1)) x = tmp(c->msl ? "iabs_(%s)" : "asuint(abs(asint(%s)))", x);
        if (BITS(w, 45, 1)) x = tmp(c->msl ? "(0u - %s)" : "asuint(-asint(%s))", x);
        u32 dbits = dfmt == 0 ? 8 : dfmt == 1 ? 16 : 32;
        if (BITS(w, 50, 1) && dbits < 32)
            x = dsig ? tmp("asuint(clamp(asint(%s), %d, %d))", x, -(1 << (dbits - 1)), (1 << (dbits - 1)) - 1)
                     : tmp("(uint)clamp(asint(%s), 0, %d)", x, (1 << dbits) - 1);
        else if (dbits < 32) x = tmp("(%s & 0x%xu)", x, (1u << dbits) - 1);
        set_u(c, d, x);
        break;
    }
    /* -- half precision -- */
    case OP_HADD2_reg: case OP_HADD2_cbuf: case OP_HADD2_imm: case OP_HADD2_32I: emit_half(c, in, 0); break;
    case OP_HMUL2_reg: case OP_HMUL2_cbuf: case OP_HMUL2_imm: case OP_HMUL2_32I: emit_half(c, in, 1); break;
    case OP_HFMA2_reg: case OP_HFMA2_rc: case OP_HFMA2_cr: case OP_HFMA2_imm: case OP_HFMA2_32I: emit_half(c, in, 2); break;
    /* -- integer -- */
    case OP_IADD_reg: case OP_IADD_cbuf: case OP_IADD_imm: case OP_IADD32I: {
        int is32 = in->op == OP_IADD32I;
        u32 po = is32 ? BITS(w, 55, 2) == 3 : BITS(w, 48, 2) == 3;
        u32 neg_a = is32 ? (BITS(w, 56, 1) && !po) : BITS(w, 49, 1), neg_b = is32 ? 0 : (BITS(w, 48, 1) && !po);
        u32 x = is32 ? BITS(w, 53, 1) : BITS(w, 43, 1), cc = is32 ? BITS(w, 52, 1) : BITS(w, 47, 1);
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (neg_a) a = tmp("(0u - %s)", a);
        if (neg_b) b = tmp("(0u - %s)", b);
        if (x || cc) {
            c->uses_cc = 1;
            line(c, "{ uint a_ = %s, b_ = %s, s_ = a_ + b_, t_ = s_ + %s;", a, b, x ? "cc_c" : "0u");
            if (cc) line(c, "  cc_c = (s_ < a_ || t_ < s_) ? 1u : 0u; cc_z = t_ == 0u; cc_s = (int)t_ < 0;");
            line(c, "  %s }", d == RZ ? "" : tmp("%s = t_%s;", R(c, d), po ? " + 1u" : ""));
        } else set_u(c, d, tmp("(%s + %s%s)", a, b, po ? " + 1u" : ""));
        break;
    }
    case OP_ISCADD_reg: case OP_ISCADD_cbuf: case OP_ISCADD_imm: case OP_ISCADD32I: {
        int is32 = in->op == OP_ISCADD32I;
        u32 scale = is32 ? BITS(w, 53, 5) : BITS(w, 39, 5);
        u32 neg_b = is32 ? 0 : BITS(w, 48, 1), neg_a = is32 ? 0 : BITS(w, 49, 1), po = neg_a && neg_b;
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (po) b = tmp("(%s + 1u)", b);
        else {
            if (neg_a) a = tmp("(0u - %s)", a);
            if (neg_b) b = tmp("(0u - %s)", b);
        }
        set_u(c, d, tmp("((%s << %uu) + %s)", a, scale, b));
        break;
    }
    case OP_IMNMX_reg: case OP_IMNMX_cbuf: case OP_IMNMX_imm: {
        const char *a = R(c, ra), *b = b_u(c, w, form), *p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (BITS(w, 48, 1)) set_u(c, d, tmp("asuint(%s ? min(asint(%s), asint(%s)) : max(asint(%s), asint(%s)))", p, a, b, a, b));
        else set_u(c, d, tmp("(%s ? min(%s, %s) : max(%s, %s))", p, a, b, a, b));
        break;
    }
    case OP_ISETP_reg: case OP_ISETP_cbuf: case OP_ISETP_imm: {
        const char *cmp = icompare(BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)), *bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 bop = BITS(w, 45, 2);
        if (BITS(w, 43, 1)) line(c, "/* ISETP.X treated as a plain compare */");
        line(c, "{ bool cmp_ = %s;", cmp);
        c->indent++;
        set_p(c, BITS(w, 3, 3), combine(bop, "cmp_", bp));
        set_p(c, BITS(w, 0, 3), combine(bop, "!cmp_", bp));
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_ISET_reg: case OP_ISET_cbuf: case OP_ISET_imm: {
        const char *r = combine(BITS(w, 45, 2), icompare(BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)),
                                PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_LOP_reg: case OP_LOP_cbuf: case OP_LOP_imm: case OP_LOP32I: {
        int is32 = in->op == OP_LOP32I;
        u32 op = is32 ? BITS(w, 53, 2) : BITS(w, 41, 2);
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (is32 ? BITS(w, 55, 1) : BITS(w, 39, 1)) a = tmp("(~%s)", a);
        if (is32 ? BITS(w, 56, 1) : BITS(w, 40, 1)) b = tmp("(~%s)", b);
        static const char *const ops[3] = {"&", "|", "^"};
        const char *r = op == 3 ? b : tmp("(%s %s %s)", a, ops[op], b);
        u32 pop = is32 ? 0 : BITS(w, 44, 2), dp = is32 ? PT : BITS(w, 48, 3);
        if (pop || dp != PT) {
            line(c, "{ uint l_ = %s;", r);
            set_u(c, d, "l_");
            set_p(c, dp, pop == 0 ? "false" : pop == 1 ? "true" : pop == 2 ? "(l_ == 0u)" : "(l_ != 0u)");
            line(c, "}");
        } else set_u(c, d, r);
        break;
    }
    case OP_LOP3_reg: case OP_LOP3_cbuf: case OP_LOP3_imm: {
        u32 lut = in->op == OP_LOP3_reg ? BITS(w, 28, 8) : BITS(w, 48, 8);
        set_u(c, d, tmp("lop3(%s, %s, %s, %uu)", R(c, ra), b_u(c, w, in->op == OP_LOP3_imm ? FORM_IMM : form), R(c, BITS(w, 39, 8)), lut));
        break;
    }
    case OP_SHL_reg: case OP_SHL_cbuf: case OP_SHL_imm: {
        const char *a = R(c, ra), *s = b_u(c, w, form);
        if (BITS(w, 39, 1)) set_u(c, d, tmp("(%s << (%s & 31u))", a, s));
        else set_u(c, d, tmp("(%s < 32u ? (%s << %s) : 0u)", s, a, s));
        break;
    }
    case OP_SHR_reg: case OP_SHR_cbuf: case OP_SHR_imm: {
        const char *a = R(c, ra), *s = b_u(c, w, form);
        if (BITS(w, 40, 1)) a = tmp("reversebits(%s)", a);
        int sg = BITS(w, 48, 1);
        const char *v = sg ? tmp("asuint(asint(%s) >> %s)", a, BITS(w, 39, 1) ? tmp("(%s & 31u)", s) : tmp("min(%s, 31u)", s))
                           : (BITS(w, 39, 1) ? tmp("(%s >> (%s & 31u))", a, s) : tmp("(%s < 32u ? (%s >> %s) : 0u)", s, a, s));
        set_u(c, d, v);
        break;
    }
    case OP_BFE_reg: case OP_BFE_cbuf: case OP_BFE_imm: {
        const char *base = R(c, ra), *b = b_u(c, w, form);
        if (BITS(w, 40, 1)) base = tmp("reversebits(%s)", base);
        line(c, "{ uint o_ = %s & 0xFFu, n_ = (%s >> 8) & 0xFFu;", b, b);
        if (BITS(w, 48, 1)) set_u(c, d, tmp("(n_ == 0u ? 0u : o_ >= 32u ? ((int)%s < 0 ? 0xFFFFFFFFu : 0u) : "
                                            "asuint(((int)(%s << (32u - min(o_ + n_, 32u)))) >> (32u - min(n_, 32u - o_) )))", base, base));
        else set_u(c, d, tmp("(n_ == 0u || o_ >= 32u ? 0u : (%s >> o_) & (n_ >= 32u ? 0xFFFFFFFFu : ((1u << n_) - 1u)))", base));
        line(c, "}");
        break;
    }
    case OP_BFI_reg: case OP_BFI_rc: case OP_BFI_cr: case OP_BFI_imm: {
        const char *ins = R(c, ra), *src, *base;
        switch (form) {
        case FORM_REG: src = R(c, BITS(w, 20, 8)); base = R(c, BITS(w, 39, 8)); break;
        case FORM_RC: src = R(c, BITS(w, 39, 8)); base = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
        case FORM_CR: src = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); base = R(c, BITS(w, 39, 8)); break;
        default: src = tmp("0x%08xu", imm20(w)); base = R(c, BITS(w, 39, 8)); break;
        }
        line(c, "{ uint o_ = %s & 0xFFu, n_ = min((%s >> 8) & 0xFFu, 32u - min(o_, 32u));", src, src);
        set_u(c, d, tmp("(o_ >= 32u ? %s : ((%s & ~(((n_ >= 32u) ? 0xFFFFFFFFu : ((1u << n_) - 1u)) << o_)) | "
                        "((%s & ((n_ >= 32u) ? 0xFFFFFFFFu : ((1u << n_) - 1u))) << o_)))", base, base, ins));
        line(c, "}");
        break;
    }
    case OP_XMAD_reg: case OP_XMAD_rc: case OP_XMAD_cr: case OP_XMAD_imm: emit_xmad(c, in); break;
    case OP_MOV_reg: case OP_MOV_cbuf: case OP_MOV_imm: set_u(c, d, b_u(c, w, form)); break;
    case OP_MOV32I: set_u(c, d, b_u(c, w, FORM_32I)); break;
    case OP_SEL_reg: case OP_SEL_cbuf: case OP_SEL_imm:
        set_u(c, d, tmp("(%s ? %s : %s)", PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)), R(c, ra), b_u(c, w, form)));
        break;
    case OP_R2P_reg: case OP_R2P_cbuf: case OP_R2P_imm: {   /* predicates from register bits, under a mask */
        if (BITS(w, 40, 1)) { unsupported(c, in, "R2P.CC"); break; }
        u32 sh = BITS(w, 41, 2) * 8;
        const char *mask = b_u(c, w, form), *src = R(c, ra);
        for (u32 p = 0; p < 7; p++)
            line(c, "if ((%s >> %uu) & 1u) %s = ((%s >> %uu) & 1u) != 0u;", mask, p + sh, PRED(c, p, 0), src, p + sh);
        break;
    }
    case OP_IADD3_reg: case OP_IADD3_cbuf: case OP_IADD3_imm: {
        const char *a = R(c, ra), *b = b_u(c, w, form), *cc = R(c, BITS(w, 39, 8));
        if (form == FORM_REG) {   /* halves, and a 16-bit shift of the partial sum */
            static const char *const hv[3] = {"%s", "(%s & 0xFFFFu)", "(%s >> 16)"};
            u32 ha = BITS(w, 35, 2), hb = BITS(w, 33, 2), hc = BITS(w, 31, 2);
            if (ha && ha < 3) a = tmp(hv[ha], a);
            if (hb && hb < 3) b = tmp(hv[hb], b);
            if (hc && hc < 3) cc = tmp(hv[hc], cc);
        }
        if (BITS(w, 51, 1)) a = tmp("(0u - %s)", a);
        if (BITS(w, 50, 1)) b = tmp("(0u - %s)", b);
        if (BITS(w, 49, 1)) cc = tmp("(0u - %s)", cc);
        const char *ab = tmp("(%s + %s)", a, b);
        u32 shift = form == FORM_REG ? BITS(w, 37, 2) : 0;
        if (shift == 1) ab = tmp("(%s >> 16)", ab);
        else if (shift == 2) ab = tmp("(%s << 16)", ab);
        set_u(c, d, tmp("(%s + %s)", ab, cc));
        break;
    }
    case OP_TMML: {   /* level of detail, computed and unclamped, as 8.8 fixed point */
        u32 a = BITS(w, 8, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4), handle = BITS(w, 36, 13) * 4;
        const char *coord;
        switch (type) {
        case TEX_1D: coord = F(c, a); break;
        case TEX_2D: coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
        case TEX_ARRAY_2D: coord = tmp("float2(%s, %s)", F(c, a + 1), F(c, a + 2)); break;
        case TEX_3D: case TEX_CUBE: coord = tmp("float3(%s, %s, %s)", F(c, a), F(c, a + 1), F(c, a + 2)); break;
        default: unsupported(c, in, "TMML type"); return;
        }
        u32 k = texture_slot(c, handle, type, 0);
        const char *v = tmp("t%u", next_tex(c));
        if (gradients(c) && c->lod_long) {
            /* the level from the texel-space derivatives of the coordinates, as for isotropic
             * filtering (cube maps: on the face the direction points at), clamped to the mips */
            switch (type) {
            case TEX_1D:
                line(c, "{ float lw_, ll_; tex%u.GetDimensions(0, lw_, ll_); float lc_ = (%s) * lw_; float lr_ = max(ddx(lc_) * ddx(lc_), ddy(lc_) * ddy(lc_));", k, coord);
                break;
            case TEX_2D:
                line(c, "{ float lw_, lh_, ll_; tex%u.GetDimensions(0, lw_, lh_, ll_); float2 lc_ = (%s) * float2(lw_, lh_);", k, coord);
                line(c, "  float2 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            case TEX_ARRAY_2D:
                line(c, "{ float lw_, lh_, le_, ll_; tex%u.GetDimensions(0, lw_, lh_, le_, ll_); float2 lc_ = (%s) * float2(lw_, lh_);", k, coord);
                line(c, "  float2 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            case TEX_3D:
                line(c, "{ float lw_, lh_, ld_, ll_; tex%u.GetDimensions(0, lw_, lh_, ld_, ll_); float3 lc_ = (%s) * float3(lw_, lh_, ld_);", k, coord);
                line(c, "  float3 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            default:   /* TEX_CUBE */
                line(c, "{ float lw_, lh_, ll_; tex%u.GetDimensions(0, lw_, lh_, ll_); float3 ld_ = (%s);", k, coord);
                line(c, "  float3 lc_ = ld_ / max(max(abs(ld_.x), abs(ld_.y)), max(abs(ld_.z), 1e-20f)) * (0.5f * lw_);");
                line(c, "  float3 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            }
            line(c, "  float lu_ = 0.5f * log2(max(lr_, 1e-20f));");
            line(c, "  float4 %s = float4(clamp(lu_, 0.0f, max(ll_ - 1.0f, 0.0f)), lu_, 0, 0);", v);
        } else if (gradients(c))
            line(c, "{ float4 %s = float4(tex%u.CalculateLevelOfDetail(smp%u, %s), tex%u.CalculateLevelOfDetailUnclamped(smp%u, %s), 0, 0);",
                 v, k, k, coord, k, k, coord);
        else line(c, "{ float4 %s = 0;", v);
        c->indent++;
        u32 dst = d;
        for (u32 i = 0; i < 4; i++)
            if (mask >> i & 1) { set_u(c, dst, tmp("(uint)(%s.%s * 256.0f)", v, XYZW[i])); if (dst != RZ) dst++; }
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_PSET: {   /* predicates combined into a register */
        const char *pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), *pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1));
        const char *pc_ = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        const char *r = combine(BITS(w, 45, 2), tmp("(%s)", combine(BITS(w, 24, 2), pa, pb)), pc_);
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_PSETP: {
        const char *pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), *pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1));
        const char *pc_ = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        line(c, "{ bool l_ = %s;", combine(BITS(w, 24, 2), pa, pb));
        set_p(c, BITS(w, 3, 3), combine(BITS(w, 45, 2), "l_", pc_));
        set_p(c, BITS(w, 0, 3), combine(BITS(w, 45, 2), "!l_", pc_));
        line(c, "}");
        break;
    }
    /* -- memory and attributes -- */
    case OP_LDC: {
        u32 idx = BITS(w, 36, 5), size = BITS(w, 48, 3), mode = BITS(w, 44, 2);
        i32 off = (i32)(BITS(w, 20, 16) << 16) >> 16;
        if (mode != 0) { unsupported(c, in, "LDC addressing mode"); break; }
        c->info->cbuf_mask |= 1u << idx;
        if (ra != RZ) {   /* indexed: all of the buffer may be read - unless this is a jump table's load (found first) */
            u32 end = ~0u;
            for (u32 t = 0; t < c->info->ntables; t++)
                if (c->info->table[t].bank == idx && (i32)c->info->table[t].offset == off) end = (u32)off + 4 * c->info->table[t].count;
            if (end == ~0u) c->info->cbuf_extent[idx] = ~0u;
            else if (c->info->cbuf_extent[idx] < end) c->info->cbuf_extent[idx] = end;
        } else if (off >= 0 && c->info->cbuf_extent[idx] < ((u32)off & ~7u) + 8) c->info->cbuf_extent[idx] = ((u32)off & ~7u) + 8;
        line(c, "{ uint addr_ = %s + %du;", R(c, ra), off);
        if (size == 4) set_u(c, d, tmp("cb%u.Load(addr_ & ~3u)", idx));
        else if (size == 5) { set_u(c, d, tmp("cb%u.Load(addr_ & ~7u)", idx)); set_u(c, d == RZ ? RZ : d + 1, tmp("cb%u.Load((addr_ & ~7u) + 4u)", idx)); }
        else {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1;
            const char *word = tmp("(cb%u.Load(addr_ & ~3u) >> ((addr_ & 3u) * 8u))", idx);
            set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, (1u << bits) - 1));
        }
        line(c, "}");
        break;
    }
    case OP_STG: {   /* global memory, compute only: found in one of the storage buffers (gstore_) */
        if (c->stage != SHADER_COMPUTE) { unsupported(c, in, "STG outside a compute shader"); break; }
        u32 e = BITS(w, 45, 1), size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        c->uses_gmem = 1;
        c->info->stores = 1;
        line(c, "{ uint b_ = %s, lo_ = b_ + 0x%08xu, hi_ = %s;", R(c, ra), (u32)off, e && ra != RZ ? R(c, ra + 1) : "0u");
        line(c, off >= 0 ? "  hi_ += lo_ < b_ ? 1u : 0u;" : "  hi_ -= lo_ > b_ ? 1u : 0u;");
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16;
            line(c, "  gstore_part_(lo_, hi_, %s, %uu);", R(c, d), bits);
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words; i++)
                line(c, "  gstore_(lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u), %s);", 4 * i, 4 * i, d == RZ ? "0u" : R(c, d + i));
        }
        line(c, "}");
        break;
    }
    case OP_LDS: case OP_STS: case OP_LDL: case OP_STL: {   /* shared or local memory, as words */
        int local = in->op == OP_LDL || in->op == OP_STL, load = in->op == OP_LDS || in->op == OP_LDL;
        if (!local && c->stage != SHADER_COMPUTE) { unsupported(c, in, "shared memory outside a compute shader"); break; }
        u32 size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        const char *arr = local ? "lmem_" : "smem_", *words_ = local ? "LMEM_WORDS" : "SMEM_WORDS";
        if (local) c->uses_lmem = 1; else c->uses_smem = 1;
        line(c, "{ uint a_ = %s + 0x%08xu;", R(c, ra), (u32)off);
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1, mask = (1u << bits) - 1;
            if (load) {
                const char *word = tmp("(%s[(a_ >> 2) %% %s] >> ((a_ & 3u) * 8u))", arr, words_);
                set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, mask));
            } else
                line(c, "  { uint i_ = (a_ >> 2) %% %s, s_ = (a_ & 3u) * 8u; %s[i_] = (%s[i_] & ~(0x%xu << s_)) | ((%s & 0x%xu) << s_); }",
                     words_, arr, arr, mask, R(c, d), mask);
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words && d + i < RZ; i++) {
                if (load) set_u(c, d + i, tmp("%s[((a_ >> 2) + %uu) %% %s]", arr, i, words_));
                else line(c, "  %s[((a_ >> 2) + %uu) %% %s] = %s;", arr, i, words_, d == RZ ? "0u" : R(c, d + i));
            }
        }
        line(c, "}");
        break;
    }
    case OP_LDG: {   /* global memory, found in one of the storage buffers (gload_) */
        u32 e = BITS(w, 45, 1), size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        c->uses_gmem = 1;
        line(c, "{ uint b_ = %s, lo_ = b_ + 0x%08xu, hi_ = %s;", R(c, ra), (u32)off, e && ra != RZ ? R(c, ra + 1) : "0u");
        line(c, off >= 0 ? "  hi_ += lo_ < b_ ? 1u : 0u;" : "  hi_ -= lo_ > b_ ? 1u : 0u;");
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1;
            const char *word = "(gload_(lo_ & ~3u, hi_) >> ((lo_ & 3u) * 8u))";
            set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, (1u << bits) - 1));
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words && d != RZ && d + i < RZ; i++)
                set_u(c, d + i, i ? tmp("gload_(lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u))", 4 * i, 4 * i) : "gload_(lo_, hi_)");
        }
        line(c, "}");
        break;
    }
    case OP_ALD: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "ALD outside a vertex shader"); break; }
        if (BITS(w, 8, 8) != RZ) { unsupported(c, in, "indexed ALD"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) set_u(c, d == RZ ? RZ : d + e, vs_input(c, off + 4 * e));
        break;
    }
    case OP_AST: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "AST outside a vertex shader"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) vs_output(c, off + 4 * e, R(c, d == RZ ? RZ : d + e));
        break;
    }
    case OP_IPA: {
        if (c->stage != SHADER_PIXEL) { unsupported(c, in, "IPA outside a pixel shader"); break; }
        u32 attr = BITS(w, 30, 8) * 4, mode = BITS(w, 54, 2);
        int persp;
        const char *v = ps_input(c, attr, &persp);
        if (persp) v = tmp("(%s * (1.0f / i.clipw))", v);
        if (mode == 1) v = tmp("(%s * %s)", v, F(c, BITS(w, 20, 8)));
        if (BITS(w, 51, 1)) v = tmp("saturate(%s)", v);
        if (attr == 0x3FC) set_u(c, d, "(front ? 0xFFFFFFFFu : 0u)");
        else set_f(c, d, v);
        break;
    }
    case OP_S2R: {
        u32 sr = BITS(w, 20, 8);
        switch (sr) {
        case 0: set_u(c, d, c->stage == SHADER_COMPUTE ? "(tid_.x & 31u)" : "0u"); break;   /* lane */
        case 0x20: set_u(c, d, "(tid_.x | (tid_.y << 16) | (tid_.z << 26))"); break;  /* thread id, packed */
        case 0x21: case 0x22: case 0x23: set_u(c, d, tmp("tid_.%s", XYZW[sr - 0x21])); break;
        case 0x25: case 0x26: case 0x27: set_u(c, d, tmp("cta_.%s", XYZW[sr - 0x25])); break;
        case 0x28: set_u(c, d, tmp("0x%xu", c->local[0] | c->local[1] << 16 | c->local[2] << 26)); break;
        case 18: set_u(c, d, "0x3F800000u"); break;                                /* y direction: +1 */
        case 19: set_u(c, d, "0u"); break;                                         /* thread kill */
        case 17: case 29: set_u(c, d, "0u"); break;                                /* invocation id/info */
        case 30: case 31: set_u(c, d, "0x3F800000u"); break;                       /* w scale factors: 1.0 */
        default: set_u(c, d, "0u"); line(c, "/* S2R SR%u reads 0 */", sr); break;
        }
        break;
    }
    case OP_KIL: {
        const char *cond;
        if (flow_true(c, in, &cond)) {
            c->info->kills = 1;
            const char *discard = c->msl ? "discard_fragment();" : "discard;";
            if (!strcmp(cond, "true")) line(c, "%s", discard);
            else line(c, "if (%s) %s", cond, discard);
        }
        break;
    }
    case OP_TEXS: emit_texs(c, in); break;
    case OP_TEX: emit_tex(c, in); break;
    case OP_TLD4: emit_tld4(c, in, 0); break;
    case OP_TLD4S: emit_tld4(c, in, 1); break;
    case OP_TLDS: emit_tlds(c, in); break;
    case OP_DEPBAR: case OP_NOP: case OP_BAR: case OP_MEMBAR: case OP_VOTE_vtg: break;
    case OP_VOTE: set_u(c, d, "0xFFFFFFFFu"); break;
    default: unsupported(c, in, "instruction"); break;
    }
}

/* ---- whole programs ------------------------------------------------------------------- */
static int find(Ctx *c, u32 addr) {
    for (u32 i = 0; i < c->n; i++) if (c->ins[i].addr == addr) return (int)i;
    return -1;
}

static int is_branchy(u32 op) {
    return op == OP_BRA || op == OP_BRX || op == OP_SSY || op == OP_PBK || op == OP_SYNC || op == OP_BRK;
}

/* Decode up to the end of the program: the 'BRA to self' padding, or an unconditional EXIT
 * that no branch target lies beyond. */
static void decode_program(Ctx *c, const u8 *prog, u32 size) {
    u32 cap = 256, furthest = 0;
    c->ins = malloc(sizeof *c->ins * cap);
    for (u32 a = 0x50; a + 8 <= size; a += 8) {
        if (((a - 0x50) / 8) % 4 == 0) continue;   /* scheduling control word */
        u64 w = *(const u64 *)(prog + a);
        if (w == 0xE2400FFFFF87000Full) break;
        if (c->n == cap) c->ins = realloc(c->ins, sizeof *c->ins * (cap *= 2));
        Insn in = {a, w, decode(w)};
        c->ins[c->n++] = in;
        if (in.op == OP_BRA || in.op == OP_SSY || in.op == OP_PBK) {
            u32 t = branch_target(&in);
            if (t > furthest) furthest = t;
        }
        if (in.op == OP_BRX) furthest = size;
        if (in.op == OP_EXIT && BITS(w, 16, 4) == 7 && BITS(w, 0, 5) == 15 && a >= furthest) break;
    }
}

static void emit_guarded(Ctx *c, const Insn *in) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    line(c, "// %04x: %016llx %s%s%u %s", in->addr, in->w, p == PT && !neg ? "" : "@", neg ? "!P" : "P", p, OPS[in->op].name);
    if (p == PT && neg) return;   /* never executes */
    int guarded = p != PT;
    if (guarded) { line(c, "if (%s) {", PRED(c, p, neg)); c->indent++; }
    emit_insn(c, in);
    if (guarded) { c->indent--; line(c, "}"); }
}

/* Emit one control-flow instruction in dispatch mode; returns 1 if it ends the block. */
static int emit_flow(Ctx *c, const Insn *in, u32 next) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    line(c, "// %04x: %016llx %s%s%u %s (test %u)", in->addr, in->w, p == PT && !neg ? "" : "@", neg ? "!P" : "P", p,
         OPS[in->op].name, BITS(in->w, 0, 5));
    if (p == PT && neg) return 0;
    const char *cond;
    int taken = flow_true(c, in, &cond);
    const char *guard = p == PT ? NULL : PRED(c, p, neg);
    if (strcmp(cond, "true")) guard = guard ? tmp("(%s && %s)", guard, cond) : cond;   /* predicate and flow test */
    if (!next) next = 0xFFFFFFFFu;
    switch (in->op) {
    case OP_SSY: if (!c->stack_to) line(c, "ssy_[ssp_ & 15u] = 0x%xu; ssp_ = ssp_ + 1u;", branch_target(in)); return 0;
    case OP_PBK: if (!c->stack_to) line(c, "pbk_[psp_ & 15u] = 0x%xu; psp_ = psp_ + 1u;", branch_target(in)); return 0;
    case OP_BRA:
        if (!taken) return 0;
        if (!guard) { goto_pc(c, branch_target(in)); return 1; }
        line(c, "npc_ = %s ? 0x%xu : 0x%xu; break;", guard, branch_target(in), next);   /* (fxc mishandles break in an if) */
        return 1;
    case OP_SYNC: case OP_BRK: {
        if (!taken) return 0;
        if (c->stack_to) {   /* where it goes, known (unreached: 0, which the default case ends) */
            u32 to = c->stack_to[in - c->ins];
            if (!guard) goto_pc(c, to);
            else line(c, "npc_ = %s ? 0x%xu : 0x%xu; break;", guard, to, next);
            return 1;
        }
        const char *pop = in->op == OP_SYNC ? "ssp_ = ssp_ - 1u; npc_ = ssy_[ssp_ & 15u];" : "psp_ = psp_ - 1u; npc_ = pbk_[psp_ & 15u];";
        if (!guard) line(c, "%s break;", pop);
        else {   /* as selects: fxc fails internally on assignments under the condition */
            const char *sp = in->op == OP_SYNC ? "ssp_" : "psp_", *stack = in->op == OP_SYNC ? "ssy_" : "pbk_";
            line(c, "{ bool g_ = %s; uint t_ = %s - 1u; uint to_ = %s[t_ & 15u]; %s = g_ ? t_ : %s; npc_ = g_ ? to_ : 0x%xu; } break;",
                 guard, sp, stack, sp, sp, next);
        }
        return 1;
    }
    case OP_BRX:
        line(c, "{ uint t_ = %s + 0x%xu; npc_ = (t_ & 31u) == 16u ? t_ + 8u : t_; } break;", R(c, BITS(in->w, 8, 8)),
             in->addr + 8 + (u32)branch_offset(in->w));
        return 1;
    case OP_EXIT:
        if (!taken) return 0;
        if (!guard) { emit_exit(c); return 1; }
        line(c, "if (%s) {", guard);
        c->indent++;
        emit_exit(c);
        c->indent--;
        line(c, "}");
        return 0;
    }
    return 0;
}


/* Jump tables of indirect branches. The compiler emits
 *     IMNMX.U32 rI, rI, N-1 ;  SHL rI, rI, 2 ;  LDC rT, c[bank][rI + off] ;  BRX rT - k
 * with the table's entries relative to the BRX - the shift sometimes into another register
 * (SHL rJ, rI, 2 ; LDC rT, c[bank][rJ + off]: the main menu's UI vertex shader). If every BRX of
 * the program resolves, only their targets need to be blocks; otherwise every instruction must
 * be one, which a graphics card runs many times slower. */
static int brx_resolved(Ctx *c) {
    if (!c->read_cbuf) return 0;
    int all = 1;
    for (u32 i = 0; i < c->n; i++) {
        if (c->ins[i].op != OP_BRX) continue;
        u32 r = BITS(c->ins[i].w, 8, 8), bank = 0, off = 0, index_reg = RZ, count = 0;
        int j = (int)i - 1, found = 0;
        for (; j >= 0 && j > (int)i - 24; j--)
            if (c->ins[j].op == OP_LDC && BITS(c->ins[j].w, 0, 8) == r) {
                bank = BITS(c->ins[j].w, 36, 5);
                off = (u32)((i32)(BITS(c->ins[j].w, 20, 16) << 16) >> 16);
                index_reg = BITS(c->ins[j].w, 8, 8);
                found = 1;
                break;
            }
        u32 shifted_from = RZ;   /* (the register the index was shifted from into index_reg) */
        for (int k = j - 1; found && k >= 0 && k > j - 24; k--) {
            const Insn *p = &c->ins[k];
            if (p->op == OP_SHL_imm && shifted_from == RZ && BITS(p->w, 0, 8) == index_reg && imm20(p->w) == 2 && BITS(p->w, 8, 8) != index_reg) {
                shifted_from = BITS(p->w, 8, 8);
                continue;
            }
            if (p->op == OP_IMNMX_imm && (BITS(p->w, 0, 8) == index_reg || (shifted_from != RZ && BITS(p->w, 0, 8) == shifted_from))) {
                count = imm20(p->w) + 1;
                break;
            }
        }
        if (!found || !count || count > 256) { all = 0; continue; }
        if (c->info->ntables < 4) {
            c->info->table[c->info->ntables].bank = bank; c->info->table[c->info->ntables].offset = off;
            c->info->table[c->info->ntables++].count = count;
        }
        for (u32 e = 0; e < count; e++) {
            u32 t = align_target(c->read_cbuf(c->read_user, bank, off + 4 * e) + c->ins[i].addr + 8 + (u32)branch_offset(c->ins[i].w));
            int at = find(c, t);
            if (at < 0) { all = 0; break; }
            c->leader[at] = 1;
        }
    }
    return all;
}

static int brx_targets(Ctx *c, u32 i) {
    (void)i;
    if (!c->brx_checked) { c->brx_checked = 1; c->brx_ok = brx_resolved(c); }
    return c->brx_ok;
}

/* The SSY and PBK stacks, followed through the blocks while translating. Where each block is
 * entered with the same stacks however it is reached (as the game's programs have them), every
 * SYNC and BRK goes to a known place, and the program keeps no stacks while it runs. With the
 * stacks as arrays indexed at run time, some programs lost writes on NVIDIA's driver (their
 * bytecode was right, and moving code about changed where): the lava of the Great Cave
 * Offensive missed its lights (vs_346571090be3dfb2, whose light sum came out as what the
 * registers held before) and showed red, yellow-green or black in patches that moved with the
 * camera. A block entered with different stacks, a stack deeper than 16 or popped empty, or an
 * indirect branch (BRX) keeps the arrays. */
typedef struct Stacks { u32 nssy, npbk, ssy[16], pbk[16]; } Stacks;
typedef struct Walk { Stacks *at; u8 *seen; u32 *work, nwork; int ok; } Walk;

/* A flow test as flow_true decides it, without its side effects: 0 never passes, 1 always,
 * 2 on the condition codes. */
static int flow_test(u32 test) {
    if (test == 15) return 1;
    switch (test) {
    case 0: case 7: case 8: case 16: case 18: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: return 0;
    }
    return 2;
}

static void reach(Ctx *c, Walk *k, int i, const Stacks *s) {   /* block i entered with s (i < 0: the program ends) */
    if (i < 0 || (u32)i >= c->n) return;
    if (!k->seen[i]) { k->seen[i] = 1; k->at[i] = *s; k->work[k->nwork++] = (u32)i; return; }
    const Stacks *t = &k->at[i];
    if (t->nssy != s->nssy || t->npbk != s->npbk || memcmp(t->ssy, s->ssy, 4 * s->nssy) || memcmp(t->pbk, s->pbk, 4 * s->npbk))
        k->ok = 0;
}

static void resolve_stacks(Ctx *c, const u8 *ssy_target, const u8 *pbk_target) {
    Walk k = {calloc(c->n, sizeof(Stacks)), calloc(c->n, 1), malloc(sizeof(u32) * c->n), 0, 1};
    u32 *to = calloc(c->n, sizeof *to);
    Stacks s = {0};
    reach(c, &k, 0, &s);
    while (k.ok && k.nwork) {
        u32 b = k.work[--k.nwork];
        s = k.at[b];
        if (ssy_target[b] && s.nssy && s.ssy[s.nssy - 1] == c->ins[b].addr) s.nssy--;   /* (as the block's start does) */
        if (pbk_target[b] && s.npbk && s.pbk[s.npbk - 1] == c->ins[b].addr) s.npbk--;
        for (u32 i = b, end = 0; k.ok && !end && i < c->n; i++) {
            if (i > b && c->leader[i]) { reach(c, &k, (int)i, &s); break; }   /* on into the next block */
            const Insn *in = &c->ins[i];
            u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
            if ((!is_branchy(in->op) && in->op != OP_EXIT) || (p == PT && neg)) continue;
            int test = flow_test(BITS(in->w, 0, 5)), guarded = p != PT || test == 2;
            int sync = in->op == OP_SYNC || in->op == OP_SSY;
            u32 *depth = sync ? &s.nssy : &s.npbk, *stack = sync ? s.ssy : s.pbk;
            switch (in->op) {
            case OP_SSY: case OP_PBK:   /* (whatever the predicate and test, as emit_flow) */
                if (*depth == 16) k.ok = 0;
                else stack[(*depth)++] = branch_target(in);
                break;
            case OP_BRA:
                if (!test) break;
                reach(c, &k, find(c, branch_target(in)), &s);
                if (guarded) reach(c, &k, (int)i + 1, &s);
                end = 1;
                break;
            case OP_SYNC: case OP_BRK: {
                if (!test) break;
                if (!*depth) { k.ok = 0; break; }
                Stacks popped = s;
                (*(sync ? &popped.nssy : &popped.npbk))--;
                to[i] = stack[*depth - 1];
                reach(c, &k, find(c, to[i]), &popped);
                if (guarded) reach(c, &k, (int)i + 1, &s);
                end = 1;
                break;
            }
            case OP_EXIT: end = test && !guarded; break;
            default: k.ok = 0; break;   /* BRX */
            }
        }
    }
    if (k.ok) c->stack_to = to;
    else free(to);
    free(k.at);
    free(k.seen);
    free(k.work);
}

static void emit_body(Ctx *c) {
    int branchy = 0, brx = 0;
    for (u32 i = 0; i < c->n; i++) { branchy |= is_branchy(c->ins[i].op); brx |= c->ins[i].op == OP_BRX; }
    c->dispatch = branchy;
    if (!branchy) {
        for (u32 i = 0; i < c->n; i++) {
            const Insn *in = &c->ins[i];
            if (in->op == OP_EXIT) emit_flow(c, in, 0);
            else emit_guarded(c, in);
        }
        emit_exit(c);
        return;
    }
    /* blocks */
    c->leader = calloc(c->n + 1, 1);
    u8 *ssy_target = calloc(c->n + 1, 1), *pbk_target = calloc(c->n + 1, 1);
    c->leader[0] = 1;
    c->brx_checked = 0;
    for (u32 i = 0; i < c->n; i++) {
        const Insn *in = &c->ins[i];
        if (brx && !brx_targets(c, i)) c->leader[i] = 1;
        if (in->op == OP_BRA || in->op == OP_SSY || in->op == OP_PBK) {
            int t = find(c, branch_target(in));
            if (t >= 0) {
                c->leader[t] = 1;
                if (in->op == OP_SSY) ssy_target[t] = 1;
                if (in->op == OP_PBK) pbk_target[t] = 1;
            }
        }
        if ((in->op == OP_BRA || in->op == OP_BRX || in->op == OP_SYNC || in->op == OP_BRK || in->op == OP_EXIT) && i + 1 < c->n)
            c->leader[i + 1] = 1;
    }
    resolve_stacks(c, ssy_target, pbk_target);
    if (c->stack_to) line(c, "uint pc_ = 0x%xu;", c->ins[0].addr);
    else line(c, "uint pc_ = 0x%xu; uint ssy_[16], pbk_[16]; uint ssp_ = 0u, psp_ = 0u;", c->ins[0].addr);
    line(c, "%sfor (uint guard_ = 0u; guard_ < 1000000u; guard_++) {", c->msl ? "" : "[loop] ");
    c->indent++;
    line(c, "uint npc_ = pc_;");
    line(c, "switch (pc_) {");
    int open = 0, ended = 0;
    for (u32 i = 0; i < c->n; i++) {
        const Insn *in = &c->ins[i];
        if (c->leader[i]) {
            if (open) { if (!ended) goto_pc(c, in->addr); c->indent--; line(c, "}"); }
            line(c, "case 0x%xu: {", in->addr);
            c->indent++;
            open = 1; ended = 0;
            if (ssy_target[i] && !c->stack_to) line(c, "if (ssp_ > 0u && ssy_[(ssp_ - 1u) & 15u] == 0x%xu) ssp_ = ssp_ - 1u;", in->addr);
            if (pbk_target[i] && !c->stack_to) line(c, "if (psp_ > 0u && pbk_[(psp_ - 1u) & 15u] == 0x%xu) psp_ = psp_ - 1u;", in->addr);
        }
        if (ended) continue;   /* unreachable within the block */
        switch (in->op) {
        case OP_BRA: case OP_BRX: case OP_SSY: case OP_PBK: case OP_SYNC: case OP_BRK: case OP_EXIT:
            ended = emit_flow(c, in, i + 1 < c->n ? c->ins[i + 1].addr : 0);
            break;
        default: emit_guarded(c, in); break;
        }
    }
    if (open) {
        if (!ended) emit_exit(c);
        c->indent--;
        line(c, "}");
    }
    line(c, "default: {");
    c->indent++;
    emit_exit(c);
    c->indent--;
    line(c, "}");
    line(c, "}");
    line(c, "pc_ = npc_;");
    c->indent--;
    line(c, "}");
    emit_exit(c);
    free(ssy_target);
    free(pbk_target);
    free(c->stack_to);
    c->stack_to = NULL;
}

/* (a format: the buffer's type, then a loop attribute) */
static const char VERTEX_FETCH[] =
    "uint4 decode_attr(%s b, uint addr, uint fmt) {\n"
    "  uint size = (fmt >> 21) & 0x3Fu, type = (fmt >> 27) & 7u, n = 4u, bits = 32u;\n"
    "  bool packed = false;\n"
    "  switch (size) {\n"
    "  case 0x01u: n = 4u; bits = 32u; break; case 0x02u: n = 3u; bits = 32u; break;\n"
    "  case 0x03u: n = 4u; bits = 16u; break; case 0x04u: n = 2u; bits = 32u; break;\n"
    "  case 0x05u: n = 3u; bits = 16u; break; case 0x0Au: n = 4u; bits = 8u; break;\n"
    "  case 0x0Fu: n = 2u; bits = 16u; break; case 0x12u: n = 1u; bits = 32u; break;\n"
    "  case 0x13u: n = 3u; bits = 8u; break; case 0x18u: n = 2u; bits = 8u; break;\n"
    "  case 0x1Bu: n = 1u; bits = 16u; break; case 0x1Du: n = 1u; bits = 8u; break;\n"
    "  case 0x32u: n = 2u; bits = 8u; break; case 0x33u: n = 4u; bits = 8u; break;\n"
    "  case 0x34u: n = 1u; bits = 8u; break; case 0x30u: n = 4u; bits = 10u; packed = true; break;\n"
    "  default: return uint4(0u, 0u, 0u, 0x3F800000u);\n"
    "  }\n"
    "  uint base = addr & ~3u, sh = (addr & 3u) * 8u;\n"
    "  uint4 w = b.Load4(base); uint w4 = b.Load(base + 16u);\n"
    "  uint words[5] = {w.x, w.y, w.z, w.w, w4};\n"
    "  bool is_float = type == 7u || type == 1u || type == 2u || type == 5u || type == 6u;\n"
    "  uint4 o = uint4(0u, 0u, 0u, is_float ? 0x3F800000u : 1u);\n"
    "  %sfor (uint i = 0u; i < 4u; i++) {\n"
    "    if (i >= n) break;\n"
    "    uint cb = packed && i == 3u ? 2u : bits;\n"
    "    uint bo = sh + (packed ? i * 10u : i * bits), wi = bo >> 5, s = bo & 31u;\n"
    "    uint v = words[wi] >> s;\n"
    "    if (s + cb > 32u) v |= words[wi + 1u] << (32u - s);\n"
    "    if (cb < 32u) v &= (1u << cb) - 1u;\n"
    "    int sv = cb < 32u ? ((int)(v << (32u - cb))) >> (32u - cb) : asint(v);\n"
    "    float scale = cb < 32u ? (float)((1u << cb) - 1u) : 4294967295.0f;\n"
    "    uint r = v;\n"
    "    switch (type) {\n"
    "    case 1u: r = asuint(max((float)sv / (scale * 0.5f - 0.5f), -1.0f)); break;\n"
    "    case 2u: r = asuint((float)v / scale); break;\n"
    "    case 3u: r = asuint(sv); break;\n"
    "    case 5u: r = asuint((float)v); break;\n"
    "    case 6u: r = asuint((float)sv); break;\n"
    "    case 7u: r = cb == 16u ? asuint(f16tof32(v)) : v; break;\n"
    "    }\n"
    "    o[i] = r;\n"
    "  }\n"
    "  if ((fmt >> 31) != 0u) o = o.zyxw;\n"
    "  return o;\n"
    "}\n";

static const char LOP3[] =
    "uint lop3(uint a, uint b, uint c, uint lut) {\n"
    "  uint r = 0u;\n"
    "  if (lut & 0x01u) r |= ~a & ~b & ~c; if (lut & 0x02u) r |= ~a & ~b & c;\n"
    "  if (lut & 0x04u) r |= ~a & b & ~c;  if (lut & 0x08u) r |= ~a & b & c;\n"
    "  if (lut & 0x10u) r |= a & ~b & ~c;  if (lut & 0x20u) r |= a & ~b & c;\n"
    "  if (lut & 0x40u) r |= a & b & ~c;   if (lut & 0x80u) r |= a & b & c;\n"
    "  return r;\n"
    "}\n";

/* ---- the text around the body, shared by both languages ---------------------------------- */
static void put_swz(Str *out, int stage) {
    put(out, "float comp_(float4 v, uint s) { return s == 2u ? v.x : s == 3u ? v.y : s == 4u ? v.z : s == 5u ? v.w : s >= 6u ? 1.0f : 0.0f; }\n"
             "float4 swz(float4 v, uint k) { uint s = params.swz[%u + (k >> 2)][k & 3u];\n"
             "  return float4(comp_(v, s & 7u), comp_(v, (s >> 3) & 7u), comp_(v, (s >> 6) & 7u), comp_(v, (s >> 9) & 7u)); }\n",
        stage == SHADER_VERTEX ? 0 : 8);
}

/* Texels of a render target drawn at a render scale (gpu_d3d11.c, macos/gpu_metal.m): its scale
 * is 4.12 fixed point (0 = 1) in the top half of the texture's swizzle word - in MSL, whose
 * swizzle words hold the sampler's LOD bias there (lodb_), in the spare fourth word of a storage
 * buffer's descriptor, two textures' a word (texture k: sb[stage's first + k / 2].w, the half
 * k & 1). Offsets and texel numbers in the game's texels become the target's own. */
static void put_scale(Ctx *c, Str *out) {
    const char *body = c->body.s ? c->body.s : "";
    if (!strstr(body, "tsc_(") && !strstr(body, "toff_(") && !strstr(body, "tld_(")) return;
    if (c->msl)
        put(out, "float tsc_(uint k) { uint q = (params.sb[%u + (k >> 1)].w >> ((k & 1u) * 16u)) & 0xFFFFu; return q != 0u ? float(q) / 4096.0f : 1.0f; }\n",
            c->stage == SHADER_VERTEX ? 0 : 16);
    else
        put(out, "float tsc_(uint k) { uint q = params.swz[%u + (k >> 2)][k & 3u] >> 16; return q != 0u ? float(q) / 4096.0f : 1.0f; }\n",
            c->stage == SHADER_VERTEX ? 0 : 8);
    /* (rounding to the nearest even: HLSL's round, MSL's rint) */
    put(out, "int2 toff_(int2 o, uint k) { float s = tsc_(k); return s == 1.0f ? o : int2(%s(float2(o) * s)); }\n"
             "int tc_(int v, float s) { return s == 1.0f ? v : int(floor((float(v) + 0.5f) * s - 0.5f)); }\n"
             "int2 tld_(int2 v, uint k) { float s = tsc_(k); return int2(tc_(v.x, s), v.y); }\n"
             "int3 tld_(int3 v, uint k) { float s = tsc_(k); return int3(tc_(v.x, s), tc_(v.y, s), v.z); }\n"
             "int4 tld_(int4 v, uint k) { float s = tsc_(k); return int4(tc_(v.x, s), tc_(v.y, s), v.z, v.w); }\n",
        c->msl ? "rint" : "round");
}

/* Global memory: a global address is looked up in each storage buffer in turn; outside them, 0. */
static void put_gmem(Ctx *c, Str *out) {
    ShaderInfo *info = c->info;
    u32 first = c->stage == SHADER_PIXEL ? 16 : 0;
    /* (vkd3d-shader cannot read a RWByteAddressBuffer: there, the same words as a structured buffer) */
    int words = c->stage == SHADER_COMPUTE && c->sb_words;
    put(out, "uint gload_(uint lo, uint hi) {\n");
    for (u32 k = 0; k < info->sb_count; k++)
        put(out, words ? "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) return gm%u[d >> 2u]; }\n"
                       : "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) return gm%u.Load(d); }\n",
            first + k, k);
    put(out, "  return 0u;\n}\n");
    if (c->stage == SHADER_COMPUTE) {
        put(out, "void gstore_(uint lo, uint hi, uint v) {\n");
        for (u32 k = 0; k < info->sb_count; k++)
            put(out, words ? "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) { gm%u[d >> 2u] = v; return; } }\n"
                           : "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) { gm%u.Store(d, v); return; } }\n",
                first + k, k);
        put(out, "}\n");
        put(out, "void gstore_part_(uint lo, uint hi, uint v, uint bits) {\n"
                 "  uint m = (1u << bits) - 1u, sh = (lo & 3u) * 8u, w = gload_(lo & ~3u, hi);\n"
                 "  gstore_(lo & ~3u, hi, (w & ~(m << sh)) | ((v & m) << sh));\n}\n");
    }
}

static void put_fetch(Str *out, u32 n) {
    put(out, "uint4 fetch%u(uint vid, uint iid) {\n"
             "  uint4 a = params.attr[%u];\n"
             "  if ((a.x & 0x40u) != 0u || a.w == 0u) return uint4(0u, 0u, 0u, 0x3F800000u);\n"
             "  uint index = a.z != 0u ? params.base.y + iid / a.z : vid;\n"
             "  return decode_attr(vb%u, index * a.y + ((a.x >> 7) & 0x3FFFu), a.x);\n"
             "}\n", n, n, n);
}

static void put_finish(Str *out, int msl) {
    put(out, "VSOut finish(VSOut o) {\n"
             "  o.clipw = o.pos.w;\n"
             "  o.pos = float4(o.pos.x * params.fix.x, o.pos.y * params.fix.y, o.pos.z * params.fix.z + o.pos.w * params.fix.w, o.pos.w);\n"
             "%s"
             "  return o;\n}\n", msl ? "  o.psize_ = 1.0f;\n" : "");
}

static void put_registers(Ctx *c, Str *out) {
    for (u32 r = 0; r < 255; r++) if (c->reg_used[r]) put(out, "  uint r%u = 0u;\n", r);
    for (u32 p = 0; p < 7; p++) if (c->pred_used[p]) put(out, "  bool p%u = false;\n", p);
    if (c->uses_cc) put(out, "  uint cc_c = 0u; bool cc_z = false, cc_s = false;\n");
}

/* A pixel shader input's interpolation, from the program header: 1 flat, 2 perspective, 3 linear. */
static u32 interp_mode(const u8 *program, u32 n) {
    u8 m = program[0x18 + n];
    u32 mode = 0;
    for (u32 k = 0; k < 4 && !mode; k++) mode = (m >> (k * 2)) & 3;
    return mode;
}

/* What the text around the body tells: the same in every language. */
static void finish_info(Ctx *c) {
    ShaderInfo *info = c->info;
    if (!c->uses_gmem) info->sb_count = 0;
    if (c->stage == SHADER_COMPUTE) return;
    info->outputs = c->stage == SHADER_VERTEX ? c->out_attr_mask : 0;
    if (c->stage == SHADER_PIXEL)
        for (u32 n = 0; n < 32; n++)
            if (c->vs_outputs >> n & 1) info->interp[n] = (u8)interp_mode(c->prog, n);
}

/* ---- HLSL ------------------------------------------------------------------------------ */
static const char *texture_decl(u32 type, int shadow) {
    switch (type) {
    case TEX_1D: return "Texture1D<float4>";
    case TEX_ARRAY_1D: return "Texture1DArray<float4>";
    case TEX_ARRAY_2D: return "Texture2DArray<float4>";
    case TEX_3D: return "Texture3D<float4>";
    case TEX_CUBE: return "TextureCube<float4>";
    case TEX_ARRAY_CUBE: return "TextureCubeArray<float4>";
    }
    (void)shadow;
    return "Texture2D<float4>";
}

/* The alpha test (shader_hlsl_alpha_tested): its passing outcomes as bits, alpha_test_mask's.
 * Selects, not a switch on the comparison: with the switch, Intel graphics showed none of the
 * alpha-tested draws. (MSL: a method of the program's struct, which holds params.) */
static const char ALPHA_PASS[] =
    "bool alpha_pass(float a) {\n"
    "  float r = params.frag.w;\n"
    "  return (asuint(params.frag.z) & (a < r ? 1u : a == r ? 2u : a > r ? 4u : 8u)) != 0u;\n}\n";

static void hlsl_out(Ctx *c, Str *out) {
    ShaderInfo *info = c->info;
    int stage = c->stage;
    put(out, "cbuffer Params : register(b0) { struct { uint4 attr[32]; float4 fix; float4 frag; uint4 base; uint4 swz[16]; uint4 sb[32]; } params; };\n");
    put_swz(out, stage);
    put_scale(c, out);
    for (u32 b = 0; b < 18; b++)
        if (info->cbuf_mask >> b & 1) put(out, "ByteAddressBuffer cb%u : register(t%u);\n", b, b);
    for (u32 k = 0; k < info->ntex; k++) {
        put(out, "%s tex%u : register(t%u);\n", texture_decl(info->tex[k].type, info->tex[k].shadow), k, 64 + k);
        put(out, "%s smp%u : register(s%u);\n", info->tex[k].shadow ? "SamplerComparisonState" : "SamplerState", k, k);
    }
    if (strstr(c->body.s ? c->body.s : "", "lop3(")) put(out, "%s", LOP3);
    if (c->uses_gmem) {
        int words = stage == SHADER_COMPUTE && c->sb_words;
        for (u32 k = 0; k < info->sb_count; k++)
            put(out, stage != SHADER_COMPUTE ? "ByteAddressBuffer gm%u : register(t%u);\n" : words ? "RWStructuredBuffer<uint> gm%u : register(u%u);\n"
                     : "RWByteAddressBuffer gm%u : register(u%u);\n", k, stage == SHADER_COMPUTE ? k : 96 + k);
        put_gmem(c, out);
    }
    if (stage == SHADER_COMPUTE) {
        u32 words = c->shared / 4 ? c->shared / 4 : 1;
        put(out, "#define SMEM_WORDS %uu\n", words);
        if (c->uses_smem) put(out, "groupshared uint smem_[%u];\n", words);
        put(out, "#define LMEM_WORDS 256u\n");
        put(out, "[numthreads(%u, %u, %u)]\nvoid main(uint3 tid_ : SV_GroupThreadID, uint3 cta_ : SV_GroupID) {\n",
            c->local[0] ? c->local[0] : 1, c->local[1] ? c->local[1] : 1, c->local[2] ? c->local[2] : 1);
        put_registers(c, out);
        if (c->uses_lmem) put(out, "  uint lmem_[LMEM_WORDS];\n");
        put(out, "%s}\n", c->body.s ? c->body.s : "");
        return;
    }

    /* the interface between the stages: identical on both sides */
    u32 outputs = stage == SHADER_VERTEX ? c->out_attr_mask : c->vs_outputs;
    put(out, "struct VSOut {\n  float4 pos : SV_Position;\n  float clipw : CLIPW;\n");
    for (u32 n = 0; n < 32; n++) {
        if (!(outputs >> n & 1)) continue;
        const char *interp = "";
        if (stage == SHADER_PIXEL) {
            u32 mode = interp_mode(c->prog, n);
            interp = mode == 1 ? "nointerpolation " : mode == 3 ? "noperspective " : "";
        }
        put(out, "  %sfloat4 a%u : ATTR%u;\n", interp, n, n);
    }
    put(out, "};\n");

    if (stage == SHADER_VERTEX) {
        put(out, VERTEX_FETCH, "ByteAddressBuffer", "[unroll] ");
        for (u32 n = 0; n < 32; n++) {
            if (!(info->inputs >> n & 1)) continue;
            put(out, "ByteAddressBuffer vb%u : register(t%u);\n", n, 18 + n);
            put_fetch(out, n);
        }
        put_finish(out, 0);
        put(out, "VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n");
        put(out, "  VSOut o = (VSOut)0; o.pos.w = 1.0f;\n");
        for (u32 n = 0; n < 32; n++)
            if (info->inputs >> n & 1) put(out, "  uint4 va%u = fetch%u(vid, iid);\n", n, n);
    } else {
        u32 target = *(const u32 *)(c->prog + 0x48), flags = *(const u32 *)(c->prog + 0x4C);
        put(out, "struct PSOut {\n");
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) put(out, "  float4 c%u : SV_Target%u;\n", rt, rt);
        if (flags & 2) put(out, "  float depth : SV_Depth;\n");
        put(out, "};\n");
        if (c->alpha_test) put(out, "%s", ALPHA_PASS);
        put(out, "PSOut main(VSOut i, bool front : SV_IsFrontFace) {\n");
    }
    put_registers(c, out);
    put(out, "%s}\n", c->body.s ? c->body.s : "");
}

/* ---- Metal Shading Language ------------------------------------------------------------- */
static const char MSL_PRELUDE[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "inline float asfloat(uint x) { return as_type<float>(x); }\n"
    "inline float asfloat(int x) { return as_type<float>(x); }\n"
    "inline float asfloat(float x) { return x; }\n"
    "inline uint asuint(float x) { return as_type<uint>(x); }\n"
    "inline uint asuint(int x) { return as_type<uint>(x); }\n"
    "inline uint asuint(uint x) { return x; }\n"
    "inline int asint(uint x) { return as_type<int>(x); }\n"
    "inline int asint(float x) { return as_type<int>(x); }\n"
    "inline int asint(int x) { return x; }\n"
    "inline float f16tof32(uint x) { return float(as_type<half>(ushort(x & 0xFFFFu))); }\n"
    "inline uint f32tof16(float x) { return uint(as_type<ushort>(half(x))); }\n"
    "inline float mad(float a, float b, float c) { return fma(a, b, c); }\n"
    "inline uint reversebits(uint x) { return reverse_bits(x); }\n"
    "inline uint iabs_(uint x) { return as_type<int>(x) < 0 ? 0u - x : x; }\n"
    "/* float to integer saturating, as Direct3D's (NaN: 0). A plain conversion out of range is\n"
    " * undefined in MSL: Metal saturates it at run time, but folds a constant one to poison */\n"
    "inline int f2i_(float x) { return isnan(x) ? 0 : x >= 2147483648.0f ? 2147483647 : x <= -2147483648.0f ? -2147483647 - 1 : int(x); }\n"
    "inline uint f2u_(float x) { return !(x > 0.0f) ? 0u : x >= 4294967296.0f ? 0xFFFFFFFFu : uint(x); }\n"
    "/* raw buffers, as Direct3D's: words out of range read 0, and are not written */\n"
    "struct RawBuf {\n"
    "  device const uint *p; uint bytes;\n"
    "  uint word_(uint i) const { return i < (bytes >> 2) ? p[i] : 0u; }\n"
    "  uint Load(uint a) const { return word_(a >> 2); }\n"
    "  uint4 Load4(uint a) const { uint i = a >> 2; return uint4(word_(i), word_(i + 1u), word_(i + 2u), word_(i + 3u)); }\n"
    "};\n"
    "struct RWRawBuf {\n"
    "  device uint *p; uint bytes;\n"
    "  uint Load(uint a) const { return (a >> 2) < (bytes >> 2) ? p[a >> 2] : 0u; }\n"
    "  void Store(uint a, uint v) const { if ((a >> 2) < (bytes >> 2)) p[a >> 2] = v; }\n"
    "};\n"
    "struct Bind_ { device uint *p; uint bytes, pad_; };\n"
    "struct Params { uint4 attr[32]; float4 fix; float4 frag; uint4 base; uint4 swz[16]; uint4 sb[32]; };\n";

static const char MSL_TEXTURES[] =
    "inline uint layer_(float l, uint n) { return min(uint(max(rint(l), 0.0f)), n - 1u); }   /* rounded and clamped, as Direct3D's */\n"
    "inline int2 near_(int2 o) { return clamp(o, int2(-8), int2(7)); }   /* the part of a gather's offset Metal applies */\n";

/* Wrappers giving Metal's textures the methods the code calls by HLSL's names, one for each
 * kind of texture declared (msl_kind). Coordinates come packed as for HLSL (the layer last, a
 * float); Metal takes the layer apart, as a uint. 1D textures are 2D ones (Metal's 1D ones
 * have no mips) sampled along the middle of their one row; Loads out of range read 0, as
 * Direct3D's do; gather offsets past Metal's -8..7 move the coordinates. */
static const struct MslTexture {
    const char *name, *type;
    const char *ct, *cx, *layer, *offset;   /* HLSL's coordinates, Metal's, its layer argument, a texel offset (NULL: none) */
    const char *lod_ct, *lod_cx;            /* TMML's coordinates (NULL: no TMML) */
    const char *load;                       /* Load (NULL: none) */
    u8 gathers, shadow, dims;               /* gathers 1: with offsets, 2: without; dims: GetDimensions */
} MSL_TEX[] = {
    {"Tex1D_", "texture2d<float>", "float", "float2(c, 0.5f)", "", "int2(o.x, 0)", "float", "float2(c, 0.5f)",
     "  float4 Load(int2 c) const { uint l = uint(c.y);\n"
     "    return l < t.get_num_mip_levels() && uint(c.x) < t.get_width(l) ? t.read(uint2(uint(c.x), 0u), l) : float4(0.0f); }\n", 0, 0, 0},
    {"Tex1DArray_", "texture2d_array<float>", "float2", "float2(c.x, 0.5f)", ", layer_(c.y, t.get_array_size())", "int2(o.x, 0)",
     NULL, NULL, NULL, 0, 0, 0},
    {"Tex2D_", "texture2d<float>", "float2", "c", "", "o", "float2", "c",
     "  float4 Load(int3 c) const { uint l = uint(c.z);\n"
     "    return l < t.get_num_mip_levels() && uint(c.x) < t.get_width(l) && uint(c.y) < t.get_height(l) ? t.read(uint2(c.xy), l) : float4(0.0f); }\n",
     1, 0, 0},
    {"Tex2DArray_", "texture2d_array<float>", "float3", "c.xy", ", layer_(c.z, t.get_array_size())", "o", "float2", "c",
     "  float4 Load(int4 c) const { uint l = uint(c.w);\n"
     "    return l < t.get_num_mip_levels() && uint(c.x) < t.get_width(l) && uint(c.y) < t.get_height(l) && uint(c.z) < t.get_array_size()\n"
     "      ? t.read(uint2(c.xy), uint(c.z), l) : float4(0.0f); }\n", 1, 0, 0},
    {"Tex3D_", "texture3d<float>", "float3", "c", "", "int3(o, 0)", "float3", "c",
     "  float4 Load(int4 c) const { uint l = uint(c.w);\n"
     "    return l < t.get_num_mip_levels() && uint(c.x) < t.get_width(l) && uint(c.y) < t.get_height(l) && uint(c.z) < t.get_depth(l)\n"
     "      ? t.read(uint3(c.xyz), l) : float4(0.0f); }\n", 0, 0, 0},
    {"TexCube_", "texturecube<float>", "float3", "c", "", NULL, "float3", "c", NULL, 2, 0, 1},
    {"TexCubeArray_", "texturecube_array<float>", "float4", "c.xyz", ", layer_(c.w, t.get_array_size())", NULL, NULL, NULL, NULL, 2, 0, 1},
    {"Shadow1D_", "depth2d<float>", "float", "float2(c, 0.5f)", "", "int2(o.x, 0)", NULL, NULL, NULL, 0, 1, 0},
    {"Shadow1DArray_", "depth2d_array<float>", "float2", "float2(c.x, 0.5f)", ", layer_(c.y, t.get_array_size())", "int2(o.x, 0)",
     NULL, NULL, NULL, 0, 1, 0},
    {"Shadow2D_", "depth2d<float>", "float2", "c", "", "o", NULL, NULL, NULL, 1, 1, 0},
    {"Shadow2DArray_", "depth2d_array<float>", "float3", "c.xy", ", layer_(c.z, t.get_array_size())", "o", NULL, NULL, NULL, 1, 1, 0},
    {"ShadowCube_", "depthcube<float>", "float3", "c", "", NULL, NULL, NULL, NULL, 2, 1, 1},
    {"ShadowCubeArray_", "depthcube_array<float>", "float4", "c.xyz", ", layer_(c.w, t.get_array_size())", NULL, NULL, NULL, NULL, 2, 1, 1},
};

/* The wrapper for a declared texture: the type HLSL declares (TEX_ARRAY_3D: a 2D texture; a
 * shadow 3D texture has no compare, as in HLSL). */
static u32 msl_kind(u32 type, int shadow) {
    static const u8 plain[8] = {0, 1, 2, 3, 4, 2, 5, 6}, depth[8] = {7, 8, 9, 10, 4, 9, 11, 12};
    return (shadow ? depth : plain)[type & 7];
}

static void msl_texture(Str *out, const struct MslTexture *t, int pixel) {
    static const char *const comp[4][2] = {{"Red", "x"}, {"Green", "y"}, {"Blue", "z"}, {"Alpha", "w"}};
    const char *ct = t->ct, *cx = t->cx, *ly = t->layer, *ox = t->offset;
    put(out, "struct %s {\n  %s t;\n", t->name, t->type);
    if (t->gathers == 1)
        put(out, "  float2 at_(float2 c, int2 o) const { return c + float2(o - near_(o)) / float2(t.get_width(), t.get_height()); }\n");
    if (!t->shadow) {
        put(out, "  float4 SampleLevel(sampler s, %s c, float l) const { return t.sample(s, %s%s, level(l)); }\n", ct, cx, ly);
        if (ox) put(out, "  float4 SampleLevel(sampler s, %s c, float l, int2 o) const { return t.sample(s, %s%s, level(l), %s); }\n", ct, cx, ly, ox);
        if (pixel) {
            put(out, "  float4 Sample(sampler s, %s c) const { return t.sample(s, %s%s); }\n", ct, cx, ly);
            if (ox) put(out, "  float4 Sample(sampler s, %s c, int2 o) const { return t.sample(s, %s%s, %s); }\n", ct, cx, ly, ox);
            put(out, "  float4 SampleBias(sampler s, %s c, float b) const { return t.sample(s, %s%s, bias(b)); }\n", ct, cx, ly);
            if (ox) put(out, "  float4 SampleBias(sampler s, %s c, float b, int2 o) const { return t.sample(s, %s%s, bias(b), %s); }\n", ct, cx, ly, ox);
            if (t->lod_ct) {
                put(out, "  float CalculateLevelOfDetail(sampler s, %s c) const { return t.calculate_clamped_lod(s, %s); }\n", t->lod_ct, t->lod_cx);
                put(out, "  float CalculateLevelOfDetailUnclamped(sampler s, %s c) const { return t.calculate_unclamped_lod(s, %s); }\n", t->lod_ct, t->lod_cx);
            }
        }
        for (u32 i = 0; i < 4 && t->gathers; i++) {
            if (t->gathers == 1) {
                put(out, "  float4 Gather%s(sampler s, %s c) const { return t.gather(s, %s%s, int2(0), component::%s); }\n", comp[i][0], ct, cx, ly, comp[i][1]);
                put(out, "  float4 Gather%s(sampler s, %s c, int2 o) const { return t.gather(s, at_(%s, o)%s, near_(o), component::%s); }\n",
                    comp[i][0], ct, cx, ly, comp[i][1]);
            } else put(out, "  float4 Gather%s(sampler s, %s c) const { return t.gather(s, %s%s, component::%s); }\n", comp[i][0], ct, cx, ly, comp[i][1]);
        }
        if (t->load) put(out, "%s", t->load);
    } else {
        /* (HLSL's SampleCmpLevelZero and SampleCmp, with the sampler's LOD bias: lodb_) */
        put(out, "  float4 SampleCmpLevel(sampler s, %s c, float r, float l) const { return float4(t.sample_compare(s, %s%s, r, level(l))); }\n",
            ct, cx, ly);
        if (ox) put(out, "  float4 SampleCmpLevel(sampler s, %s c, float r, float l, int2 o) const { return float4(t.sample_compare(s, %s%s, r, level(l), %s)); }\n",
                    ct, cx, ly, ox);
        if (pixel) {
            put(out, "  float4 SampleCmpBias(sampler s, %s c, float r, float b) const { return float4(t.sample_compare(s, %s%s, r, bias(b))); }\n",
                ct, cx, ly);
            if (ox) put(out, "  float4 SampleCmpBias(sampler s, %s c, float r, float b, int2 o) const { return float4(t.sample_compare(s, %s%s, r, bias(b), %s)); }\n",
                        ct, cx, ly, ox);
        }
        if (t->gathers) put(out, "  float4 GatherCmp(sampler s, %s c, float r) const { return t.gather_compare(s, %s%s, r); }\n", ct, cx, ly);
        if (t->gathers == 1)
            put(out, "  float4 GatherCmp(sampler s, %s c, float r, int2 o) const { return t.gather_compare(s, at_(%s, o)%s, r, near_(o)); }\n", ct, cx, ly);
    }
    if (t->dims)
        put(out, "  void GetDimensions(uint m, thread float &w, thread float &h, thread float &l) const {\n"
                 "    w = t.get_width(m); h = t.get_height(m); l = t.get_num_mip_levels(); }\n");
    put(out, "};\n");
}

/* MSL has no global resources: the program and its helpers are member functions of Shader_,
 * whose members are the resources; the entry point fills it from its arguments (the layout
 * gpu_shader.h gives) and runs it. Buffers are found by their GPU addresses in a table. */
static void msl_out(Ctx *c, Str *out) {
    ShaderInfo *info = c->info;
    int stage = c->stage;
    const char *body = c->body.s ? c->body.s : "";
    put(out, "%s", MSL_PRELUDE);
    put(out, "struct Binds_ { Bind_ cb[18]; Bind_ gm[16];%s };\n", stage == SHADER_VERTEX ? " Bind_ vb[32];" : "");
    if (info->ntex) put(out, "%s", MSL_TEXTURES);
    u32 kinds = 0;
    for (u32 k = 0; k < info->ntex; k++) {
        u32 kind = msl_kind(info->tex[k].type, info->tex[k].shadow);
        if (!(kinds >> kind & 1)) msl_texture(out, &MSL_TEX[kind], stage == SHADER_PIXEL);
        kinds |= 1u << kind;
    }
    u32 nsmp = shader_msl_samplers(info), defaults = 0;   /* (Metal has 16 samplers: past them, Direct3D's default sampler) */
    for (u32 k = nsmp; k < info->ntex; k++) defaults |= info->tex[k].shadow ? 2u : 1u;
    if (defaults & 1) put(out, "constexpr sampler smp_default_(address::clamp_to_edge, filter::linear, mip_filter::linear);\n");
    if (defaults & 2) put(out, "constexpr sampler smp_default_cmp_(address::clamp_to_edge, filter::linear, mip_filter::linear, compare_func::never);\n");
    if (strstr(body, "lop3(")) put(out, "%s", LOP3);
    if (stage == SHADER_VERTEX) put(out, VERTEX_FETCH, "RawBuf", "");
    if (stage == SHADER_COMPUTE) put(out, "#define SMEM_WORDS %uu\n", c->shared / 4 ? c->shared / 4 : 1);
    if (stage == SHADER_COMPUTE || c->uses_lmem) put(out, "#define LMEM_WORDS 256u\n");

    /* the interface between the stages: the vertex shader's outputs, of which the pixel shader
     * declares those it reads (Metal matches them by name) */
    u32 target = 0, flags = 0, ps_inputs = info->inputs & c->vs_outputs;
    if (stage == SHADER_VERTEX) {
        put(out, "struct VSOut {\n  float4 pos [[position, invariant]];\n  float clipw [[user(clipw)]];\n");
        for (u32 n = 0; n < 32; n++)
            if (c->out_attr_mask >> n & 1) put(out, "  float4 a%u [[user(attr%u)]];\n", n, n);
        put(out, "  float psize_ [[point_size]];\n};\n");
    } else if (stage == SHADER_PIXEL) {
        target = *(const u32 *)(c->prog + 0x48);
        flags = *(const u32 *)(c->prog + 0x4C);
        put(out, "struct PSIn {\n  float4 pos [[position]];\n  float clipw [[user(clipw)]];\n");
        for (u32 n = 0; n < 32; n++) {
            if (!(ps_inputs >> n & 1)) continue;
            u32 mode = interp_mode(c->prog, n);
            put(out, "  float4 a%u [[user(attr%u)%s]];\n", n, n, mode == 1 ? ", flat" : mode == 3 ? ", center_no_perspective" : "");
        }
        put(out, "};\n");
        put(out, "struct PSOut {\n");
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) put(out, "  float4 c%u;\n", rt);
        if (flags & 2) put(out, "  float depth;\n");
        put(out, "};\n");
        /* what is written: each render target's output typed as the renderer's attachment is;
         * with dual-source blending (which the renderer asks for from its blend state alone),
         * target 1's output as target 0's second source - 0 when the program writes no target
         * 1, and 0 as target 0's first when it writes no target 0, since Metal refuses a pipeline
         * blending with Source1 whose program does not write both (D3D draws, reading undefined
         * values: 0 in practice) */
        put(out, "constant uint rt_types_ [[function_constant(%u)]];   /* 2 bits a render target: float (0), uint (1), int (2) */\n"
                 "constant uint rtt_ = is_function_constant_defined(rt_types_) ? rt_types_ : 0u;\n"
                 "constant bool dual_ = (rtt_ & 0x%xu) != 0u;\n", SHADER_MSL_RT_TYPES, SHADER_MSL_DUAL_SOURCE);
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                const char *d = rt == 1 ? "!dual_ && " : "";
                put(out, "constant bool c%uf_ = %s(rtt_ >> %u & 3u) == 0u, c%uu_ = %s(rtt_ >> %u & 3u) == 1u, c%ui_ = %s(rtt_ >> %u & 3u) == 2u;\n",
                    rt, d, 2 * rt, rt, d, 2 * rt, rt, d, 2 * rt);
            }
        put(out, "struct PSOut_ {\n");
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15)
                put(out, "  float4 c%uf [[color(%u), function_constant(c%uf_)]];\n  uint4 c%uu [[color(%u), function_constant(c%uu_)]];\n"
                         "  int4 c%ui [[color(%u), function_constant(c%ui_)]];\n", rt, rt, rt, rt, rt, rt, rt, rt, rt);
        if (!(target & 15)) put(out, "  float4 c0s [[color(0), function_constant(dual_)]];\n");   /* (only a float target blends) */
        put(out, "  float4 c1s [[color(0), index(1), function_constant(dual_)]];\n");
        if (flags & 2) put(out, "  float depth [[depth(any)]];\n");
        put(out, "};\n");
    }

    /* the program, with its resources */
    Str init = {0};   /* (the entry point's initialiser for them, in the same order) */
    int binds = 0;
    put(out, "struct Shader_ {\n  constant Params &params;\n");
    put(&init, "  Shader_ s_ = {params");
    for (u32 b = 0; b < 18; b++)
        if (info->cbuf_mask >> b & 1) {
            put(out, "  RawBuf cb%u;\n", b);
            put(&init, ", {bt.cb[%u].p, bt.cb[%u].bytes}", b, b);
            binds = 1;
        }
    for (u32 k = 0; k < info->sb_count; k++) {
        put(out, "  %s gm%u;\n", stage == SHADER_COMPUTE ? "RWRawBuf" : "RawBuf", k);
        put(&init, ", {bt.gm[%u].p, bt.gm[%u].bytes}", k, k);
        binds = 1;
    }
    for (u32 n = 0; n < 32 && stage == SHADER_VERTEX; n++)
        if (info->inputs >> n & 1) {
            put(out, "  RawBuf vb%u;\n", n);
            put(&init, ", {bt.vb[%u].p, bt.vb[%u].bytes}", n, n);
            binds = 1;
        }
    for (u32 k = 0; k < info->ntex; k++) {
        put(out, "  %s tex%u; sampler smp%u;\n", MSL_TEX[msl_kind(info->tex[k].type, info->tex[k].shadow)].name, k, k);
        if (k < nsmp) put(&init, ", {tex%u}, smp%u", k, k);
        else put(&init, ", {tex%u}, %s", k, info->tex[k].shadow ? "smp_default_cmp_" : "smp_default_");
    }
    if (c->uses_smem) {
        put(out, "  threadgroup uint *smem_;\n");
        put(&init, ", smem_");
    }
    put(&init, "};\n");
    put_swz(out, stage);
    put_scale(c, out);
    /* A sampler's LOD bias, in the bits above a texture's swizzle (13, signed, in 1/256ths: the
     * TSC's): the renderer's, where Metal's samplers have none. sample() adds it to every LOD a
     * sample or compare takes - implicit, biased, explicit and level 0 alike, as Direct3D's
     * MipLODBias and Metal's lodBias (macOS 26) are - not to gathers'; so 0 changes nothing. */
    if (info->ntex)
        put(out, "float lodb_(uint k) { return float(as_type<int>(params.swz[%u + (k >> 2)][k & 3u] << 7) >> 19) / 256.0f; }\n",
            stage == SHADER_VERTEX ? 0 : 8);
    if (c->uses_gmem) put_gmem(c, out);
    if (stage == SHADER_VERTEX) {
        for (u32 n = 0; n < 32; n++)
            if (info->inputs >> n & 1) put_fetch(out, n);
        put_finish(out, 1);
        put(out, "VSOut run(uint vid, uint iid) {\n  VSOut o = {}; o.pos.w = 1.0f;\n");
        for (u32 n = 0; n < 32; n++)
            if (info->inputs >> n & 1) put(out, "  uint4 va%u = fetch%u(vid, iid);\n", n, n);
    } else if (stage == SHADER_PIXEL) {
        if (c->alpha_test) put(out, "%s", ALPHA_PASS);
        put(out, "PSOut run(PSIn i, bool front) {\n");
    }
    else put(out, "void run(uint3 tid_, uint3 cta_) {\n");
    put_registers(c, out);
    if (c->uses_lmem) put(out, "  uint lmem_[LMEM_WORDS];\n");   /* (in every stage: HLSL's has it in compute alone) */
    put(out, "%s}\n};\n", body);

    /* the entry point */
    Str args = {0};
    put(&args, "constant Params &params [[buffer(%u)]]", SHADER_MSL_PARAMS);
    if (binds) put(&args, ", constant Binds_ &bt [[buffer(%u)]]", SHADER_MSL_BINDS);
    for (u32 k = 0; k < info->ntex; k++) {
        put(&args, ", %s tex%u [[texture(%u)]]", MSL_TEX[msl_kind(info->tex[k].type, info->tex[k].shadow)].type, k, k);
        if (k < nsmp) put(&args, ", sampler smp%u [[sampler(%u)]]", k, k);
    }
    if (stage == SHADER_VERTEX) {
        put(out, "vertex VSOut vs_main(uint vid [[vertex_id]], uint iid [[instance_id]], %s) {\n%s  return s_.run(vid, iid);\n}\n", args.s, init.s);
    } else if (stage == SHADER_PIXEL) {
        put(out, "fragment PSOut_ ps_main(PSIn i [[stage_in]], bool front [[front_facing]], %s) {\n%s  PSOut o = s_.run(i, front);\n  PSOut_ r;\n",
            args.s, init.s);
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15)
                put(out, "  if (c%uf_) r.c%uf = o.c%u;\n  if (c%uu_) r.c%uu = as_type<uint4>(o.c%u);\n  if (c%ui_) r.c%ui = as_type<int4>(o.c%u);\n",
                    rt, rt, rt, rt, rt, rt, rt, rt, rt);
        if (!(target & 15)) put(out, "  if (dual_) r.c0s = float4(0.0f);\n");
        put(out, target >> 4 & 15 ? "  if (dual_) r.c1s = o.c1;\n" : "  if (dual_) r.c1s = float4(0.0f);\n");
        if (flags & 2) put(out, "  r.depth = o.depth;\n");
        put(out, "  return r;\n}\n");
    } else {
        u32 lx = c->local[0] ? c->local[0] : 1, ly = c->local[1] ? c->local[1] : 1, lz = c->local[2] ? c->local[2] : 1;
        put(out, "[[max_total_threads_per_threadgroup(%u)]]\n"
                 "kernel void cs_main(uint3 tid_ [[thread_position_in_threadgroup]], uint3 cta_ [[threadgroup_position_in_grid]], %s) {\n",
            lx * ly * lz, args.s);
        if (c->uses_smem) put(out, "  threadgroup uint smem_[SMEM_WORDS];\n");
        put(out, "%s  s_.run(tid_, cta_);\n}\n", init.s);
    }
    free(init.s);
    free(args.s);
}

/* ---- translation ----------------------------------------------------------------------------- */
static char *translate(int lang, const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user,
                       const u32 local[3], u32 shared, int alpha_test) {
    init_table();
    memset(info, 0, sizeof *info);
    info->stage = stage;
    Ctx ctx = {0}, *c = &ctx;
    c->lang = lang;
    c->msl = lang == SHADER_MSL;
    c->four_gathers = lang != SHADER_HLSL_FXC;
    c->lod_long = c->sb_words = lang == SHADER_HLSL_VKD3D;
    if (local) { c->local[0] = local[0]; c->local[1] = local[1]; c->local[2] = local[2]; }
    c->shared = shared;
    c->alpha_test = alpha_test;
    c->read_cbuf = read;
    c->read_user = user;
    c->stage = stage;
    c->prog = program;
    c->info = info;
    c->vs_outputs = vs_outputs;
    c->indent = 1;
    decode_program(c, program, size);
    emit_body(c);
    finish_info(c);

    Str out = {0};
    put(&out, "// Maxwell %s shader, %u instructions\n", stage == SHADER_VERTEX ? "vertex" : stage == SHADER_PIXEL ? "pixel" : "compute", c->n);
    if (c->msl) msl_out(c, &out);
    else hlsl_out(c, &out);
    free(c->body.s);
    free(c->ins);
    free(c->leader);
    return out.s;
}

char *shader_translate(int lang, const u8 *program, u32 size, int stage, u32 vs_outputs, const u32 local[3], u32 shared,
                       ShaderInfo *info, CbufReader *read, void *user) {
    return translate(lang, program, size, stage, vs_outputs, info, read, user, stage == SHADER_COMPUTE ? local : NULL, shared, 0);
}

char *shader_translate_alpha_tested(int lang, const u8 *program, u32 size, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(lang, program, size, SHADER_PIXEL, vs_outputs, info, read, user, NULL, 0, 1);
}

char *shader_hlsl(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info) {
    return shader_hlsl_ex(program, size, stage, vs_outputs, info, NULL, NULL);
}

char *shader_hlsl_ex(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(HLSL_DEFAULT, program, size, stage, vs_outputs, info, read, user, NULL, 0, 0);
}

char *shader_hlsl_alpha_tested(const u8 *program, u32 size, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(HLSL_DEFAULT, program, size, SHADER_PIXEL, vs_outputs, info, read, user, NULL, 0, 1);
}

char *shader_hlsl_cs(const u8 *program, u32 size, const u32 local[3], u32 shared, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(HLSL_DEFAULT, program, size, SHADER_COMPUTE, 0, info, read, user, local, shared, 0);
}

/* Does the program read its pixel's position, or step in the texels of what it samples (texel
 * offsets, texel numbers)? Exactly those instructions take the render scale into account
 * (ps_input, emit_tex, emit_tld4, emit_tlds): their translation differs from the one before
 * render scaling, so they get cache files of their own (gpu_shader_cache.h shader_cache_hash).
 * Very few do: one pixel program of the game reads its position, two gather with offsets. */
int shader_uses_pixels(const u8 *program, u32 size, int stage) {
    init_table();
    Ctx c = {0};
    decode_program(&c, program, size);
    int uses = 0;
    for (u32 i = 0; i < c.n && !uses; i++) {
        u64 w = c.ins[i].w;
        switch (c.ins[i].op) {
        case OP_IPA: { u32 attr = BITS(w, 30, 8) * 4; uses = stage == SHADER_PIXEL && (attr == 0x70 || attr == 0x74); break; }
        case OP_TEX: uses = BITS(w, 54, 1) != 0; break;                              /* AOFFI */
        case OP_TLD4: uses = BITS(w, 54, 2) == 1 || BITS(w, 54, 2) == 2; break;     /* one offset, or four */
        case OP_TLD4S: uses = BITS(w, 51, 1) != 0; break;                            /* AOFFI */
        case OP_TLDS: uses = 1; break;
        default: break;
        }
    }
    free(c.ins);
    return uses;
}

/* Does the program use the SSY/PBK stacks? Their translation changed (resolve_stacks): such
 * programs get cache files of their own too. */
int shader_uses_stacks(const u8 *program, u32 size) {
    init_table();
    Ctx c = {0};
    decode_program(&c, program, size);
    int uses = 0;
    for (u32 i = 0; i < c.n && !uses; i++) uses = c.ins[i].op == OP_SSY || c.ins[i].op == OP_PBK;
    free(c.ins);
    return uses;
}

u32 shader_size(const u8 *program, u32 max) {
    init_table();
    Ctx c = {0};
    decode_program(&c, program, max);
    u32 end = c.n ? c.ins[c.n - 1].addr + 8 : 0x50;
    free(c.ins);
    return end;
}
