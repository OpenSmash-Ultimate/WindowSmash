/* Check the shader translator's Metal Shading Language on this Mac (the macOS counterpart of
 * tools/linux/hlsltest.c). A corpus of hand-assembled Maxwell programs, with every instruction
 * form the translator decodes, is translated to MSL and compiled by Metal as the renderer
 * compiles it; some of it then runs on the GPU - compute programs, and a vertex and pixel pair
 * drawing points and a triangle - and their results are checked against the same arithmetic
 * done here in C (bit for bit where it is integer or IEEE-exact; NaN compares, rint, packed
 * halves, out-of-range loads reading 0). The corpus' HLSL can also be written to a file and
 * compared with what an older translator wrote: a change to the MSL dialect must leave the
 * Windows (fxc) and Linux (vkd3d-shader) HLSL byte for byte as it was.
 *
 * Usage: msltest [all]                          the corpus, then the GPU checks
 *        msltest corpus [-v]                    translate and compile the corpus (-v: timings, MSL of failures)
 *        msltest gpu [-v]                       the GPU checks (-v: every value)
 *        msltest print <program>                one program's MSL (msltest list: their names)
 *        msltest dir <dir> [filter] [-v]        programs SSBU_DUMP_SHADERS saved (<stage>_<hash>.bin,
 *                                               C_<hash>_<x>x<y>x<z>_<shared>.bin): translate, compile
 *        msltest dump <dir>                     write the corpus so
 *        msltest fuzz [<n>] [-v]                random programs (every encoding, random fields) to MSL, compiled
 *                                               (<n>: print that one's MSL)
 *        msltest hlsl <fxc|vkd3d|api> <file>    write the corpus' HLSL, and what the translator tells of it
 *        msltest hlsldiff <fxc|vkd3d|api> <file>  compare them with a file written so
 *   (api: through shader_hlsl_ex/_cs, the build's own HLSL - vkd3d-shader's off Windows.) Exits 1
 *   when anything fails.
 *
 * Build, from the repository root:
 *   clang -arch arm64 -mmacosx-version-min=14.0 -fobjc-arc -fno-strict-aliasing -ffp-contract=off -O1 \
 *     -I runtime/macos/include -I runtime/linux/include -I runtime \
 *     tools/mac/msltest.m runtime/gpu_shader.c -framework Metal -framework Foundation -o msltest
 * With -DMSLTEST_HLSL_ONLY only the hlsl modes are built, needing neither Metal nor shader_translate:
 * that builds against an older gpu_shader.c, to write the file hlsldiff compares with (its HLSL is
 * whatever that gpu_shader.c was built for: the form argument is then ignored). */
#define _CRT_SECURE_NO_WARNINGS
#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOL MSLTEST_WINDOWS_BOOL   /* (windows.h's BOOL is not Objective-C's, and its interface means struct) */
#include "gpu_shader.h"
#undef BOOL
#undef interface

#ifndef MSLTEST_HLSL_ONLY
#define GetCurrentThread MSLTEST_carbon_GetCurrentThread   /* (Carbon's, which Foundation brings, is another function) */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#undef GetCurrentThread
#endif

/* ---- the instruction set, as the translator decodes it (opcodes from the top 16 bits) ------ */
static const struct { const char *name, *pattern; } ISA[] = {
    {"ALD", "1110111111011---"}, {"AST", "1110111111110---"}, {"BAR", "1111000010101---"},
    {"BFE_reg", "0101110000000---"}, {"BFE_cbuf", "0100110000000---"}, {"BFE_imm", "0011100-00000---"},
    {"BFI_reg", "0101101111110---"}, {"BFI_rc", "0101001111110---"}, {"BFI_cr", "0100101111110---"}, {"BFI_imm", "0011011-11110---"},
    {"BRA", "111000100100----"}, {"BRK", "111000110100----"}, {"BRX", "111000100101----"},
    {"DEPBAR", "1111000011110---"}, {"EXIT", "111000110000----"},
    {"F2F_reg", "0101110010101---"}, {"F2F_cbuf", "0100110010101---"}, {"F2F_imm", "0011100-10101---"},
    {"F2I_reg", "0101110010110---"}, {"F2I_cbuf", "0100110010110---"}, {"F2I_imm", "0011100-10110---"},
    {"FADD_reg", "0101110001011---"}, {"FADD_cbuf", "0100110001011---"}, {"FADD_imm", "0011100-01011---"}, {"FADD32I", "000010----------"},
    {"FFMA_reg", "010110011-------"}, {"FFMA_rc", "010100011-------"}, {"FFMA_cr", "010010011-------"}, {"FFMA_imm", "0011001-1-------"},
    {"FFMA32I", "000011----------"},
    {"FMNMX_reg", "0101110001100---"}, {"FMNMX_cbuf", "0100110001100---"}, {"FMNMX_imm", "0011100-01100---"},
    {"FMUL_reg", "0101110001101---"}, {"FMUL_cbuf", "0100110001101---"}, {"FMUL_imm", "0011100-01101---"}, {"FMUL32I", "00011110--------"},
    {"CSET", "0101000010011---"}, {"CSETP", "0101000010100---"},
    {"FSET_reg", "01011000--------"}, {"FSET_cbuf", "01001000--------"}, {"FSET_imm", "0011000---------"},
    {"FSETP_reg", "010110111011----"}, {"FSETP_cbuf", "010010111011----"}, {"FSETP_imm", "0011011-1011----"},
    {"HADD2_reg", "0101110100010---"}, {"HADD2_cbuf", "0111101-1-------"}, {"HADD2_imm", "0111101-0-------"}, {"HADD2_32I", "0010110---------"},
    {"HFMA2_reg", "0101110100000---"}, {"HFMA2_rc", "01100---1-------"}, {"HFMA2_cr", "01110---1-------"}, {"HFMA2_imm", "01110---0-------"},
    {"HFMA2_32I", "0010100---------"},
    {"HSET2_reg", "0101110100011---"}, {"HSET2_cbuf", "0111110-1-------"}, {"HSET2_imm", "0111110-0-------"},
    {"HSETP2_reg", "0101110100100---"}, {"HSETP2_cbuf", "0111111-1-------"}, {"HSETP2_imm", "0111111-0-------"},
    {"HMUL2_reg", "0101110100001---"}, {"HMUL2_cbuf", "0111100-1-------"}, {"HMUL2_imm", "0111100-0-------"}, {"HMUL2_32I", "0010101---------"},
    {"I2F_reg", "0101110010111---"}, {"I2F_cbuf", "0100110010111---"}, {"I2F_imm", "0011100-10111---"},
    {"I2I_reg", "0101110011100---"}, {"I2I_cbuf", "0100110011100---"}, {"I2I_imm", "0011100-11100---"},
    {"IADD_reg", "0101110000010---"}, {"IADD_cbuf", "0100110000010---"}, {"IADD_imm", "0011100-00010---"}, {"IADD32I", "0001110---------"},
    {"IMNMX_reg", "0101110000100---"}, {"IMNMX_cbuf", "0100110000100---"}, {"IMNMX_imm", "0011100-00100---"},
    {"IPA", "11100000--------"},
    {"ISCADD_reg", "0101110000011---"}, {"ISCADD_cbuf", "0100110000011---"}, {"ISCADD_imm", "0011100-00011---"}, {"ISCADD32I", "000101----------"},
    {"ISET_reg", "010110110101----"}, {"ISET_cbuf", "010010110101----"}, {"ISET_imm", "0011011-0101----"},
    {"ISETP_reg", "010110110110----"}, {"ISETP_cbuf", "010010110110----"}, {"ISETP_imm", "0011011-0110----"},
    {"KIL", "111000110011----"}, {"LDC", "1110111110010---"}, {"LDG", "1110111011010---"}, {"STG", "1110111011011---"},
    {"LDS", "1110111101001---"}, {"STS", "1110111101011---"}, {"LDL", "1110111101000---"}, {"STL", "1110111101010---"},
    {"LOP_reg", "0101110001000---"}, {"LOP_cbuf", "0100110001000---"}, {"LOP_imm", "0011100-01000---"}, {"LOP32I", "000001----------"},
    {"LOP3_reg", "0101101111100---"}, {"LOP3_cbuf", "0000001---------"}, {"LOP3_imm", "001111----------"},
    {"MEMBAR", "1110111110011---"},
    {"MOV_reg", "0101110010011---"}, {"MOV_cbuf", "0100110010011---"}, {"MOV_imm", "0011100-10011---"}, {"MOV32I", "000000010000----"},
    {"MUFU", "0101000010000---"}, {"NOP", "0101000010110---"}, {"PBK", "111000101010----"},
    {"PSET", "0101000010001---"}, {"PSETP", "0101000010010---"},
    {"R2P_reg", "0101110011110---"}, {"R2P_cbuf", "0100110011110---"}, {"R2P_imm", "0011100-11110---"},
    {"IADD3_reg", "010111001100----"}, {"IADD3_cbuf", "010011001100----"}, {"IADD3_imm", "0011100-1100----"},
    {"TMML", "1101111101011---"},
    {"RRO_reg", "0101110010010---"}, {"RRO_cbuf", "0100110010010---"}, {"RRO_imm", "0011100-10010---"},
    {"S2R", "1111000011001---"},
    {"SEL_reg", "0101110010100---"}, {"SEL_cbuf", "0100110010100---"}, {"SEL_imm", "0011100-10100---"},
    {"SHL_reg", "0101110001001---"}, {"SHL_cbuf", "0100110001001---"}, {"SHL_imm", "0011100-01001---"},
    {"SHR_reg", "0101110000101---"}, {"SHR_cbuf", "0100110000101---"}, {"SHR_imm", "0011100-00101---"},
    {"SSY", "111000101001----"}, {"SYNC", "1111000011111---"},
    {"TEX", "11000-----------"}, {"TEXS", "1101-00---------"}, {"TLD4", "110010----------"}, {"TLD4S", "11011111-0------"},
    {"TLDS", "1101-01---------"}, {"TXQ", "1101111101001---"},
    {"VOTE", "0101000011011---"}, {"VOTE_vtg", "0101000011100---"},
    {"XMAD_reg", "0101101100------"}, {"XMAD_rc", "010100010-------"}, {"XMAD_cr", "0100111---------"}, {"XMAD_imm", "0011011-00------"},
};
#define NISA (sizeof ISA / sizeof ISA[0])

/* ---- an assembler ----------------------------------------------------------------------- */
#define B_(v, pos, n) (((u64)(v) & ((1ull << (n)) - 1)) << (pos))
#define BITS_(w, pos, n) ((u32)(((w) >> (pos)) & ((1ull << (n)) - 1)))
#define D_(r) B_(r, 0, 8)                     /* destination */
#define A_(r) B_(r, 8, 8)                     /* operand A */
#define BR_(r) B_(r, 20, 8)                   /* operand B, a register */
#define CR_(r) B_(r, 39, 8)                   /* operand C (or B of the rc forms), a register */
#define IMM_(v) (B_((u32)(v), 20, 19) | B_((u32)(v) >> 31, 56, 1))    /* 20-bit signed immediate */
#define FIMM_(bits) (B_((u32)(bits) >> 12, 20, 19) | B_((u32)(bits) >> 31, 56, 1))   /* a float's top 20 bits */
#define CB_(bank, off) (B_(bank, 34, 5) | B_((off) / 4, 20, 14))      /* c[bank][off] */
#define I32_(v) B_(v, 20, 32)
#define PPT_ B_(7, 39, 3)                     /* the predicate operand at 39: PT */
#define RZ_ 255u
#define SB_(k) (SB_FIRST + 16 * (k))          /* storage buffer k's descriptor in c[0] */

#define MAXI 1200
typedef struct Prog {
    const char *name;
    int stage, pair;              /* pixel: the corpus index of its vertex program, or -1 */
    u32 local[3], shared;
    u8 hdr[0x50];
    u64 w[MAXI]; const char *op[MAXI]; u32 n;
    struct { const char *name; u32 at; } lab[320], fix[320];
    u32 nlab, nfix;
    u8 *bytes; u32 size;
    u32 cb[18][128];              /* constant buffer words the program's jump tables are in */
} Prog;

static Prog corpus[48];
static u32 ncorpus;

static Prog *prog(const char *name, int stage) {
    Prog *p = &corpus[ncorpus++];
    memset(p, 0, sizeof *p);
    p->name = name;
    p->stage = stage;
    p->pair = -1;
    p->local[0] = p->local[1] = p->local[2] = 1;
    return p;
}

static u64 opcode(const char *name) {
    for (u32 i = 0; i < NISA; i++)
        if (!strcmp(ISA[i].name, name)) {
            u64 v = 0;
            for (int b = 0; b < 16; b++) v = v << 1 | (ISA[i].pattern[b] == '1');
            return v << 48;
        }
    fprintf(stderr, "no instruction %s\n", name);
    exit(2);
}

/* An instruction, guarded by predicate p (7: PT) or its negation. */
static void IG(Prog *p, u32 pred, int neg, const char *name, u64 fields) {
    if (p->n == MAXI) { fprintf(stderr, "%s: too long\n", p->name); exit(2); }
    p->op[p->n] = name;
    p->w[p->n++] = opcode(name) | B_(pred, 16, 3) | B_(neg, 19, 1) | fields;
}
static void I(Prog *p, const char *name, u64 fields) { IG(p, 7, 0, name, fields); }
static void label(Prog *p, const char *name) { p->lab[p->nlab].name = name; p->lab[p->nlab++].at = p->n; }
/* A branch-like instruction (BRA, SSY, PBK) to a label; test 15: always. */
static void jump(Prog *p, u32 pred, int neg, const char *name, const char *to, u32 test) {
    p->fix[p->nfix].name = to; p->fix[p->nfix++].at = p->n;
    IG(p, pred, neg, name, B_(test, 0, 5));
}

/* Every fourth word, from the code's start after the 0x50-byte header, is scheduling control. */
static u32 addr_of(u32 i) { return 0x50 + 8 * (4 * (i / 3) + 1 + i % 3); }

static u32 label_addr(Prog *p, const char *name) {
    for (u32 i = 0; i < p->nlab; i++) if (!strcmp(p->lab[i].name, name)) return addr_of(p->lab[i].at);
    fprintf(stderr, "%s: no label %s\n", p->name, name);
    exit(2);
}

static void assemble(Prog *p) {
    for (u32 f = 0; f < p->nfix; f++) {
        u32 at = p->fix[f].at;
        i32 off = (i32)(label_addr(p, p->fix[f].name) - (addr_of(at) + 8));
        p->w[at] = (p->w[at] & ~B_(0xFFFFFF, 20, 24)) | B_((u32)off, 20, 24);
    }
    u32 groups = (p->n + 2) / 3 + 1;
    p->size = 0x50 + groups * 32;
    p->bytes = calloc(1, p->size);
    memcpy(p->bytes, p->hdr, 0x50);
    u64 nop = opcode("NOP") | B_(7, 16, 3);
    for (u32 i = 0; i < 3 * (groups - 1); i++) {
        u64 w = i < p->n ? p->w[i] : nop;
        memcpy(p->bytes + addr_of(i), &w, 8);
    }
    u64 pad = 0xE2400FFFFF87000Full;   /* the 'BRA to self' the compiler pads with */
    memcpy(p->bytes + 0x50 + 32 * (groups - 1) + 8, &pad, 8);
}

/* the jump tables of BRX: the program's constant buffers */
static u32 read_cbuf(void *user, u32 bank, u32 offset) {
    const Prog *p = user;
    return bank < 18 && offset / 4 < 128 ? p->cb[bank][offset / 4] : 0;
}

/* ---- common pieces of programs --------------------------------------------------------- */
static void hdr32(Prog *p, u32 off, u32 v) { memcpy(p->hdr + off, &v, 4); }
static void exit_(Prog *p) { I(p, "EXIT", B_(15, 0, 5)); }

/* Compute: r0 thread, r1 workgroup (of 32); r2..r5 the first 16 bytes of the thread's input in
 * storage buffer 0 (1 << in_shift bytes for each thread of the grid), r34/r35 their address;
 * r36/r37 the address of its output in storage buffer 1 (1 << out_shift bytes each). */
static void prologue(Prog *p, u32 in_shift, u32 out_shift) {
    I(p, "S2R", D_(0) | B_(0x21, 20, 8));
    I(p, "S2R", D_(1) | B_(0x25, 20, 8));
    I(p, "MOV_cbuf", D_(30) | CB_(0, SB_(0)));
    I(p, "MOV_cbuf", D_(31) | CB_(0, SB_(0) + 4));
    I(p, "MOV_cbuf", D_(32) | CB_(0, SB_(1)));
    I(p, "MOV_cbuf", D_(33) | CB_(0, SB_(1) + 4));
    I(p, "ISCADD_reg", D_(21) | A_(1) | BR_(0) | B_(5, 39, 5));   /* workgroup * 32 + thread */
    I(p, "SHL_imm", D_(20) | A_(21) | IMM_(in_shift));
    I(p, "IADD_reg", D_(34) | A_(30) | BR_(20));
    I(p, "MOV_reg", D_(35) | BR_(31));
    I(p, "LDG", D_(2) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3));
    I(p, "SHL_imm", D_(21) | A_(21) | IMM_(out_shift));
    I(p, "IADD_reg", D_(36) | A_(32) | BR_(21));
    I(p, "MOV_reg", D_(37) | BR_(33));
}
static void store(Prog *p, u32 r, u32 word) { I(p, "STG", D_(r) | A_(36) | B_(1, 45, 1) | B_(4, 48, 3) | B_(4 * word, 20, 24)); }

/* ---- arithmetic as the translated code does it, in C ------------------------------------ */
static float F32(u32 x) { float f; memcpy(&f, &x, 4); return f; }
static u32 U32(float f) { u32 x; memcpy(&x, &f, 4); return x; }
static float h2f(u32 h) { _Float16 x; u16 v = (u16)h; memcpy(&x, &v, 2); return (float)x; }
static u32 f2h(float f) { _Float16 x = (_Float16)f; u16 v; memcpy(&v, &x, 2); return v; }
static float sat(float x) { return x > 1.0f ? 1.0f : x > 0.0f ? x : 0.0f; }   /* (NaN: 0, as saturate's) */
static u32 rev(u32 x) { u32 r = 0; for (int i = 0; i < 32; i++) r |= (x >> i & 1) << (31 - i); return r; }
static u32 lop3(u32 a, u32 b, u32 c, u32 lut) {
    u32 r = 0;
    for (u32 i = 0; i < 8; i++)
        if (lut >> i & 1) r |= (i & 4 ? a : ~a) & (i & 2 ? b : ~b) & (i & 1 ? c : ~c);
    return r;
}
static int fcmp(u32 op, float a, float b) {
    switch (op) {
    case 0: return 0;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b && !isnan(a) && !isnan(b);
    case 6: return a >= b;
    case 7: return !isnan(a) && !isnan(b);
    case 8: return isnan(a) || isnan(b);
    case 9: return !(a >= b);
    case 10: return !(a != b && !isnan(a) && !isnan(b));
    case 11: return !(a > b);
    case 12: return !(a <= b);
    case 13: return a != b;
    case 14: return !(a < b);
    }
    return 1;
}
static i32 f2i(float x) { return isnan(x) ? 0 : x >= 2147483648.0f ? 0x7FFFFFFF : x < -2147483648.0f ? (i32)0x80000000 : (i32)x; }
static u32 f2u(float x) { return isnan(x) || x <= 0.0f ? 0 : x >= 4294967296.0f ? 0xFFFFFFFFu : (u32)x; }
static float fclamp(float x, float lo, float hi) { return fminf(fmaxf(x, lo), hi); }   /* (as Direct3D's and Metal's clamp: NaN gives lo) */
static u32 bfe(u32 base, u32 b, int sg) {
    u32 o = b & 0xFF, n = (b >> 8) & 0xFF;
    if (!sg) return n == 0 || o >= 32 ? 0 : (base >> o) & (n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1);
    if (n == 0) return 0;
    if (o >= 32) return (i32)base < 0 ? 0xFFFFFFFFu : 0;
    u32 e = o + n < 32 ? o + n : 32, m = n < 32 - o ? n : 32 - o;
    return (u32)((i32)(base << (32 - e)) >> (32 - m));
}
static u32 bfi(u32 ins, u32 src, u32 base) {
    u32 o = src & 0xFF, n = (src >> 8) & 0xFF;
    if (o >= 32) return base;
    if (n > 32 - o) n = 32 - o;
    u32 m = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1;
    return (base & ~(m << o)) | ((ins & m) << o);
}
static u32 pack(float lo, float hi) { return f2h(lo) | f2h(hi) << 16; }

/* ---- the corpus ---------------------------------------------------------------------------
 * GPU-checked programs first (their builders also note what to expect), then programs that
 * are only compiled: every texture form in each stage, every remaining instruction encoding. */
typedef u32 Model(u32 a, u32 b, u32 c);
enum { SAME_BITS, SAME_FLOAT, NEAR_ULPS, NEAR_2E6, ANY_ZERO, SAME_HALVES };   /* how a result must match: bits; NaN for NaN; within 4 ulps; to 2e-6; any zero for zero; per half */
typedef struct AluCase { const char *what; Model *model; int match; } AluCase;
static AluCase alu[300];
static u32 nalu;
static int dontcare;   /* (a model: this input's result is not defined) */

static void acase(Prog *p, const char *what, Model *m, int match) {
    store(p, 10, nalu);
    alu[nalu].what = what; alu[nalu].model = m; alu[nalu++].match = match;
}

#define M(name, expr) static u32 name(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return (expr); }
/* Apple's GPUs flush float denormals in arithmetic (as Direct3D does) but not in moves, abs or
 * negation: a float result is checked against the model with operands and result flushed, and
 * without (ftz_mode) */
static int ftz_mode;
static float ftz(float x) { return ftz_mode && fpclassify(x) == FP_SUBNORMAL ? copysignf(0.0f, x) : x; }
static u32 UF(float x) { return U32(ftz(x)); }
#define FA ftz(F32(a))
#define FB ftz(F32(b))
#define FC ftz(F32(c))
static const u32 CB0[8] = {0x12345678u, 0xFFFF0001u, 0x3FC00000u, 0xC0490FDBu, 0x3C00BC00u, 13u, 0x0810u, 0x7FC00000u};
M(m_iadd, a + b) M(m_iadd_na, 0u - a + b) M(m_iadd_nb, a - b) M(m_iadd_po, 0u - a + b + 1u) M(m_iadd_imm, a - 5u) M(m_iadd_cb, a + CB0[1])
M(m_iadd32i, a + 0x10000u) M(m_iadd_x, c + (a + b < a ? 1u : 0u)) M(m_iadd3, a + b + c)
M(m_iadd3_h, (((a & 0xFFFFu) + (b >> 16)) << 16) + c) M(m_iadd3_imm, a - 0x1234u + c) M(m_iadd3_cb, a + CB0[0] + c)
M(m_iscadd, (a << 5) + b) M(m_iscadd_na, ((0u - a) << 5) + b) M(m_iscadd_imm, (a << 3) + 100u) M(m_iscadd32i, (a << 2) + 0x1000u)
M(m_iscadd_cb, (a << 1) + CB0[0])
M(m_imin_s, (i32)a < (i32)b ? a : b) M(m_imax_u, a > b ? a : b) M(m_imin_imm, (i32)a < -100 ? a : (u32)-100) M(m_imin_cb, a < CB0[0] ? a : CB0[0])
M(m_iset_lt, (i32)a < (i32)b ? ~0u : 0u) M(m_iset_ge_bf, a >= b ? 0x3F800000u : 0u) M(m_iset_ne, a != 0 ? ~0u : 0u)
M(m_iset_cb, (i32)a <= (i32)CB0[1] ? ~0u : 0u)
M(m_isetp_sel, (i32)a > (i32)b ? a : b) M(m_isetp_xor, (a == c) != (a > b) ? ~0u : 0u) M(m_isetp_imm, (i32)a >= 32 ? a : c)
M(m_isetp_cb, a < CB0[0] ? 1u : 2u)
M(m_and, a & b) M(m_or_na, ~a | b) M(m_xor_nb, a ^ ~b) M(m_pass_nb, ~b) M(m_lop_z, (a & b) == 0 ? a : c) M(m_lop_imm, a & 0x0F0F0u)
M(m_lop32i, ~a ^ 0x55AA55AAu) M(m_lop_cb, a | CB0[0]) M(m_lop3_96, lop3(a, b, c, 0x96)) M(m_lop3_e8, lop3(a, b, c, 0xE8))
M(m_lop3_ca, lop3(a, b, c, 0xCA)) M(m_lop3_imm, lop3(a, 0x7FFFFu, c, 0x80)) M(m_lop3_cb, lop3(a, CB0[0], c, 0x6A))
M(m_shl, b < 32 ? a << b : 0u) M(m_shl_w, a << (b & 31)) M(m_shl_imm, a << 7) M(m_shl_cb, a << 13)
M(m_shr, b < 32 ? a >> b : 0u) M(m_shr_s, (u32)((i32)a >> (b < 31 ? b : 31))) M(m_shr_w, a >> (b & 31)) M(m_shr_sw, (u32)((i32)a >> (b & 31)))
M(m_shr_rev, b < 32 ? rev(a) >> b : 0u) M(m_shr_imm, (u32)((i32)a >> 4)) M(m_shr_cb, a >> 13)
M(m_bfe_u, bfe(a, b, 0)) M(m_bfe_s, bfe(a, b, 1)) M(m_bfe_imm, bfe(a, 0x0C04, 1)) M(m_bfe_rev, bfe(rev(a), b, 0)) M(m_bfe_cb, bfe(a, CB0[6], 0))
M(m_bfi, bfi(a, b, c)) M(m_bfi_imm, bfi(a, 0x0A06, c)) M(m_bfi_rc, bfi(a, b, CB0[1])) M(m_bfi_cr, bfi(a, CB0[6], c))
M(m_xmad, (a & 0xFFFFu) * (b & 0xFFFFu) + c)
M(m_xmad_h1s, (u32)((i32)a >> 16) * (b & 0xFFFFu) + c)
M(m_xmad_chi_psl, (((a & 0xFFFFu) * (b & 0xFFFFu)) << 16) + (c >> 16))
M(m_xmad_cbcc_mrg, ((((a & 0xFFFFu) * (b & 0xFFFFu)) + (c + (b << 16))) & 0xFFFFu) | (b << 16))
M(m_xmad_clo_sbhb, (a & 0xFFFFu) * (u32)((i32)b >> 16) + (c & 0xFFFFu))
M(m_xmad_x, (a & 0xFFFFu) * (b & 0xFFFFu) + c + (a + b < a ? 1u : 0u))
M(m_xmad_imm, (a & 0xFFFFu) * 0xBEEFu + c) M(m_xmad_rc, (a & 0xFFFFu) * (b >> 16) + CB0[0])
M(m_xmad_cr, (((a & 0xFFFFu) * (CB0[1] & 0xFFFFu)) << 16) + c)
M(m_i2i_s8, (u32)((i32)(a << 16) >> 24)) M(m_i2i_u16, (a >> 16) & 0xFFFFu) M(m_i2i_neg, 0u - a) M(m_i2i_abs, (i32)a < 0 ? 0u - a : a)
M(m_i2i_sat8, (u32)((i32)a < -128 ? -128 : (i32)a > 127 ? 127 : (i32)a)) M(m_i2i_satu16, (u32)((i32)a < 0 ? 0 : (i32)a > 65535 ? 65535 : (i32)a))
M(m_i2i_sel3, (u32)((i32)(a << 24) >> 16)) M(m_i2i_imm, 0xFDu) M(m_i2i_cb, CB0[0] & 0xFFFFu)
M(m_i2f_s, UF((float)(i32)a)) M(m_i2f_u, UF((float)a)) M(m_i2f_u8, UF((float)((a >> 16) & 0xFF))) M(m_i2f_s16n, UF(-(float)((i32)(a << 16) >> 16)))
M(m_i2f_abs, UF((float)(i32)((i32)a < 0 ? 0u - a : a))) M(m_i2f_h, f2h((float)(i32)a)) M(m_i2f_imm, UF(-7.0f)) M(m_i2f_cb, UF((float)CB0[0]))
M(m_f2i_rn, (u32)f2i(rintf(FA))) M(m_f2i_fl, (u32)f2i(floorf(FA))) M(m_f2i_ce, (u32)f2i(ceilf(FA))) M(m_f2i_tr, (u32)f2i(truncf(FA)))
M(m_f2u_tr, f2u(truncf(FA))) M(m_f2i_s16, (u32)f2i(fclamp(rintf(FA), -32768.0f, 32767.0f))) M(m_f2u_16a, f2u(fclamp(rintf(fabsf(FA)), 0.0f, 65535.0f)))
M(m_f2i_h1, (u32)f2i(truncf(h2f(a >> 16)))) M(m_f2i_imm, (u32)-2) M(m_f2i_cb, f2u(floorf(F32(CB0[2]))))
M(m_f2i_max, 0x7FFFFFFFu) M(m_f2u_max, 0xFFFFFFFFu) M(m_f2i_min, 0x80000000u) M(m_f2i_zero, 0u)
M(m_f2f_rn, UF(rintf(FA))) M(m_f2f_fl, UF(floorf(FA))) M(m_f2f_ce, UF(ceilf(FA))) M(m_f2f_tr, UF(truncf(FA))) M(m_f2f_h, f2h(FA))
M(m_f2f_h1, UF(h2f(a >> 16))) M(m_f2f_satn, UF(sat(-FA))) M(m_f2f_abs, UF(fabsf(FA))) M(m_f2f_imm, UF(rintf(2.5f))) M(m_f2f_cb, UF(ceilf(F32(CB0[3]))))
M(m_fadd, UF(FA + FB)) M(m_fadd_nab, UF(-FA + fabsf(FB))) M(m_fadd_sat, UF(sat(FA + FB))) M(m_fadd_imm, UF(FA + 1.5f))
M(m_fadd32i, UF(fabsf(FA) + F32(0x40490FDBu))) M(m_fadd_cb, UF(FA + F32(CB0[3])))
M(m_fmul, UF(FA * FB)) M(m_fmul_d2, UF((FA * 0.5f) * FB)) M(m_fmul_fmz, UF((FA == 0.0f || FB == 0.0f) ? 0.0f : (FA * 8.0f) * FB))
M(m_fmul_satn, UF(sat(FA * -FB))) M(m_fmul32i, UF(FA * F32(0x3EAAAAABu))) M(m_fmul_imm, UF(FA * -2.0f)) M(m_fmul_cb, UF(FA * F32(CB0[2])))
M(m_ffma, UF(fmaf(FA, FB, FC))) M(m_ffma_nc, UF(fmaf(FA, FB, -FC))) M(m_ffma_fmz, (FA == 0.0f || FB == 0.0f) ? c : UF(fmaf(FA, FB, FC)))
M(m_ffma_satnb, UF(sat(fmaf(FA, -FB, FC)))) M(m_ffma_imm, UF(fmaf(FA, 2.0f, FC))) M(m_ffma_rc, UF(fmaf(FA, FB, 1.5f)))
M(m_ffma_cr, UF(fmaf(FA, 1.5f, FC))) M(m_ffma32i, UF(fmaf(FA, 0.5f, -FC)))
M(m_fmin, UF(fminf(FA, FB))) M(m_fmax_na, UF(fmaxf(-FA, FB))) M(m_fmin_imm, UF(fminf(FA, 1.0f))) M(m_fmax_cb, UF(fmaxf(FA, F32(CB0[2]))))
M(m_rcp, UF(1.0f / FA)) M(m_rsq, UF(1.0f / sqrtf(FA))) M(m_sqrt, UF(sqrtf(FA))) M(m_rcp_an, UF(1.0f / -fabsf(FA)))
M(m_ex2, UF((dontcare = fabsf(FA) > 100.0f, exp2f(FA)))) M(m_lg2, UF(log2f(FA)))
M(m_sin, UF((dontcare = !(fabsf(FA) < 100.0f), sinf(FA)))) M(m_cos, UF((dontcare = !(fabsf(FA) < 100.0f), cosf(FA))))
M(m_rro, a) M(m_rro_n, UF(-FB)) M(m_rro_imm, 0x3F800000u) M(m_rro_cb, CB0[2])
M(m_fsetp_psel, fcmp(10, FA, FB) ? ~0u : 0u) M(m_fsetp_imm, FA > 0.0f ? a : b) M(m_fset_bf, fcmp(3, fabsf(FA), -FB) ? 0x3F800000u : 0u)
M(m_fset_imm, fcmp(14, FA, 1.0f) ? ~0u : 0u) M(m_fset_cb, fcmp(5, FA, F32(CB0[7])) ? ~0u : 0u)
M(m_fsetp_cb, fcmp(9, FA, F32(CB0[2])) ? 1u : 0u)
M(m_csetp_lo, a + b >= a ? a : b) M(m_cset_eq, a + b == 0 ? 0x3F800000u : 0u) M(m_cset_lt, (i32)(a - b) < 0 ? ~0u : 0u)
M(m_psetp, ((a < b) != (b < c)) ? 0x3F800000u : 0u) M(m_r2p, (a >> 11) & 1 ? ~0u : 0u) M(m_r2p_reg, (a >> 2) & 1 ? 5u : 6u)
M(m_r2p_cb, (CB0[0] >> 4) & 1 ? 7u : 8u)
M(m_hadd2, pack(h2f(a) + h2f(b), h2f(a >> 16) + h2f(b >> 16)))
M(m_hadd2_nbs, pack(sat(h2f(a) + -h2f(b)), sat(h2f(a >> 16) + -h2f(b))))
M(m_hadd2_f32, UF(h2f(a) + FB))
M(m_hadd2_mrg, (c & 0xFFFF0000u) | f2h(h2f(a) + h2f(b)))
M(m_hadd2_imm, pack(h2f(a) + 1.0f, h2f(a >> 16) + -2.0f))
M(m_hadd2_32i, pack(-h2f(a) + 2.0f, -h2f(a >> 16) + 1.0f))
M(m_hadd2_cb, pack(h2f(a) + 1.5f, h2f(a >> 16) + 1.5f))
M(m_hmul2, pack(fabsf(h2f(a)) * h2f(b), fabsf(h2f(a >> 16)) * h2f(b >> 16)))
M(m_hmul2_32i, pack(h2f(a) * 2.0f, h2f(a >> 16) * 1.0f))
M(m_hmul2_imm, pack(h2f(a) * 1.0f, h2f(a >> 16) * -2.0f))
M(m_hmul2_cb, pack(-h2f(a) * 1.5f, -h2f(a >> 16) * 1.5f))
M(m_hfma2, pack(fmaf(h2f(a), h2f(b), -h2f(c >> 16)), fmaf(h2f(a >> 16), h2f(b >> 16), -h2f(c >> 16))))
M(m_hfma2_32i, pack(fmaf(h2f(a), 1.0f, -h2f(c)), fmaf(h2f(a >> 16), 2.0f, -h2f(c >> 16))))
M(m_hfma2_rc, pack(fmaf(h2f(a), h2f(b), 1.5f), fmaf(h2f(a >> 16), h2f(b >> 16), 1.5f)))
M(m_hfma2_cr, pack(fmaf(h2f(a), 1.5f, h2f(c)), fmaf(h2f(a >> 16), 1.5f, h2f(c >> 16))))
M(m_hfma2_imm, pack(fmaf(h2f(a), 1.0f, h2f(c)), fmaf(h2f(a >> 16), -2.0f, h2f(c >> 16))))
M(m_hset2, (fcmp(1, h2f(a), h2f(b)) ? 0xFFFFu : 0u) | (fcmp(1, h2f(a >> 16), h2f(b >> 16)) ? 0xFFFFu : 0u) << 16)
M(m_hset2_bf, (fcmp(13, h2f(a), h2f(b)) ? 0x3C00u : 0u) | (fcmp(13, h2f(a >> 16), h2f(b >> 16)) ? 0x3C00u : 0u) << 16)
M(m_hset2_imm, (fcmp(4, h2f(a), 1.0f) ? 0xFFFFu : 0u) | (fcmp(4, h2f(a >> 16), -2.0f) ? 0xFFFFu : 0u) << 16)
M(m_hset2_cb, (fcmp(3, h2f(a), 1.5f) ? 0xFFFFu : 0u) | (fcmp(3, h2f(a >> 16), 1.5f) ? 0xFFFFu : 0u) << 16)
M(m_hsetp2, fcmp(6, h2f(a), h2f(b)) && fcmp(6, h2f(a >> 16), h2f(b >> 16)) ? ~0u : 0u)
M(m_hsetp2_and, !(fcmp(2, h2f(a), h2f(b)) && fcmp(2, h2f(a >> 16), h2f(b >> 16))) ? ~0u : 0u)
M(m_hsetp2_imm, fcmp(1, h2f(a), 1.0f) ? 3u : 4u) M(m_hsetp2_cb, fcmp(8, h2f(a >> 16), 1.5f) ? 3u : 4u)
M(m_mov32i, 0xCAFEF00Du) M(m_mov_imm, 0xFFFFFFF9u) M(m_mov_cb, CB0[3]) M(m_sel_imm, 77u) M(m_sel_cb, CB0[0])
M(m_vote, ~0u) M(m_ffma_dbl, UF(fmaf(FA, FB, FA * FB)))

/* r10 from the thread's inputs a, b, c (r2, r3, r4), one check each */
static Prog *build_alu(void) {
    Prog *p = prog("cs_alu", SHADER_COMPUTE);
    p->local[0] = 32;
    prologue(p, 4, 10);
    nalu = 0;
    /* integer */
    I(p, "IADD_reg", D_(10) | A_(2) | BR_(3)); acase(p, "IADD", m_iadd, SAME_BITS);
    I(p, "IADD_reg", D_(10) | A_(2) | BR_(3) | B_(1, 49, 1)); acase(p, "IADD -a", m_iadd_na, SAME_BITS);
    I(p, "IADD_reg", D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "IADD -b", m_iadd_nb, SAME_BITS);
    I(p, "IADD_reg", D_(10) | A_(2) | BR_(3) | B_(3, 48, 2)); acase(p, "IADD.PO (as translated: -a + b + 1)", m_iadd_po, SAME_BITS);
    I(p, "IADD_imm", D_(10) | A_(2) | IMM_(-5)); acase(p, "IADD imm", m_iadd_imm, SAME_BITS);
    I(p, "IADD_cbuf", D_(10) | A_(2) | CB_(0, 4)); acase(p, "IADD cbuf", m_iadd_cb, SAME_BITS);
    I(p, "IADD32I", D_(10) | A_(2) | I32_(0x10000)); acase(p, "IADD32I", m_iadd32i, SAME_BITS);
    I(p, "IADD_reg", D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, "IADD_reg", D_(10) | A_(4) | BR_(RZ_) | B_(1, 43, 1)); acase(p, "IADD.CC, IADD.X", m_iadd_x, SAME_BITS);
    I(p, "IADD3_reg", D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "IADD3", m_iadd3, SAME_BITS);
    I(p, "IADD3_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 35, 2) | B_(2, 33, 2) | B_(2, 37, 2)); acase(p, "IADD3 halves, shift", m_iadd3_h, SAME_BITS);
    I(p, "IADD3_imm", D_(10) | A_(2) | IMM_(0x1234) | CR_(4) | B_(1, 50, 1)); acase(p, "IADD3 imm -b", m_iadd3_imm, SAME_BITS);
    I(p, "IADD3_cbuf", D_(10) | A_(2) | CB_(0, 0) | CR_(4)); acase(p, "IADD3 cbuf", m_iadd3_cb, SAME_BITS);
    I(p, "ISCADD_reg", D_(10) | A_(2) | BR_(3) | B_(5, 39, 5)); acase(p, "ISCADD", m_iscadd, SAME_BITS);
    I(p, "ISCADD_reg", D_(10) | A_(2) | BR_(3) | B_(5, 39, 5) | B_(1, 49, 1)); acase(p, "ISCADD -a", m_iscadd_na, SAME_BITS);
    I(p, "ISCADD_imm", D_(10) | A_(2) | IMM_(100) | B_(3, 39, 5)); acase(p, "ISCADD imm", m_iscadd_imm, SAME_BITS);
    I(p, "ISCADD32I", D_(10) | A_(2) | I32_(0x1000) | B_(2, 53, 5)); acase(p, "ISCADD32I", m_iscadd32i, SAME_BITS);
    I(p, "ISCADD_cbuf", D_(10) | A_(2) | CB_(0, 0) | B_(1, 39, 5)); acase(p, "ISCADD cbuf", m_iscadd_cb, SAME_BITS);
    I(p, "IMNMX_reg", D_(10) | A_(2) | BR_(3) | B_(1, 48, 1) | PPT_); acase(p, "IMNMX.S32 min", m_imin_s, SAME_BITS);
    I(p, "IMNMX_reg", D_(10) | A_(2) | BR_(3) | PPT_ | B_(1, 42, 1)); acase(p, "IMNMX.U32 max", m_imax_u, SAME_BITS);
    I(p, "IMNMX_imm", D_(10) | A_(2) | IMM_(-100) | B_(1, 48, 1) | PPT_); acase(p, "IMNMX imm", m_imin_imm, SAME_BITS);
    I(p, "IMNMX_cbuf", D_(10) | A_(2) | CB_(0, 0) | PPT_); acase(p, "IMNMX cbuf", m_imin_cb, SAME_BITS);
    I(p, "ISET_reg", D_(10) | A_(2) | BR_(3) | B_(1, 49, 3) | B_(1, 48, 1) | PPT_); acase(p, "ISET.LT.S32", m_iset_lt, SAME_BITS);
    I(p, "ISET_reg", D_(10) | A_(2) | BR_(3) | B_(6, 49, 3) | B_(1, 44, 1) | PPT_); acase(p, "ISET.GE.U32.BF", m_iset_ge_bf, SAME_BITS);
    I(p, "ISET_imm", D_(10) | A_(2) | IMM_(0) | B_(5, 49, 3) | PPT_); acase(p, "ISET imm NE", m_iset_ne, SAME_BITS);
    I(p, "ISET_cbuf", D_(10) | A_(2) | CB_(0, 4) | B_(3, 49, 3) | B_(1, 48, 1) | PPT_); acase(p, "ISET cbuf LE", m_iset_cb, SAME_BITS);
    I(p, "ISETP_reg", A_(2) | BR_(3) | B_(4, 49, 3) | B_(1, 48, 1) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    I(p, "SEL_reg", D_(10) | A_(2) | BR_(3) | B_(0, 39, 3)); acase(p, "ISETP.GT.S32, SEL", m_isetp_sel, SAME_BITS);
    I(p, "ISETP_reg", A_(2) | BR_(3) | B_(4, 49, 3) | PPT_ | B_(0, 3, 3) | B_(1, 0, 3));
    I(p, "ISETP_reg", A_(2) | BR_(4) | B_(2, 49, 3) | B_(0, 39, 3) | B_(2, 45, 2) | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, "PSET", D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "ISETP.XOR, PSET", m_isetp_xor, SAME_BITS);
    I(p, "ISETP_imm", A_(2) | IMM_(32) | B_(6, 49, 3) | B_(1, 48, 1) | PPT_ | B_(3, 3, 3) | B_(7, 0, 3));
    I(p, "SEL_reg", D_(10) | A_(2) | BR_(4) | B_(3, 39, 3)); acase(p, "ISETP imm", m_isetp_imm, SAME_BITS);
    I(p, "ISETP_cbuf", A_(2) | CB_(0, 0) | B_(1, 49, 3) | PPT_ | B_(7, 3, 3) | B_(4, 0, 3));
    I(p, "MOV32I", D_(11) | I32_(1)); I(p, "MOV32I", D_(12) | I32_(2));
    I(p, "SEL_reg", D_(10) | A_(12) | BR_(11) | B_(4, 39, 3)); acase(p, "ISETP cbuf (second predicate)", m_isetp_cb, SAME_BITS);
    I(p, "LOP_reg", D_(10) | A_(2) | BR_(3) | B_(0, 41, 2)); acase(p, "LOP.AND", m_and, SAME_BITS);
    I(p, "LOP_reg", D_(10) | A_(2) | BR_(3) | B_(1, 41, 2) | B_(1, 39, 1)); acase(p, "LOP.OR ~a", m_or_na, SAME_BITS);
    I(p, "LOP_reg", D_(10) | A_(2) | BR_(3) | B_(2, 41, 2) | B_(1, 40, 1)); acase(p, "LOP.XOR ~b", m_xor_nb, SAME_BITS);
    I(p, "LOP_reg", D_(10) | A_(2) | BR_(3) | B_(3, 41, 2) | B_(1, 40, 1)); acase(p, "LOP.PASS_B ~b", m_pass_nb, SAME_BITS);
    I(p, "LOP_reg", D_(11) | A_(2) | BR_(3) | B_(2, 44, 2) | B_(1, 48, 3));
    I(p, "SEL_reg", D_(10) | A_(2) | BR_(4) | B_(1, 39, 3)); acase(p, "LOP.Z, SEL", m_lop_z, SAME_BITS);
    I(p, "LOP_imm", D_(10) | A_(2) | IMM_(0x0F0F0)); acase(p, "LOP imm", m_lop_imm, SAME_BITS);
    I(p, "LOP32I", D_(10) | A_(2) | I32_(0x55AA55AA) | B_(2, 53, 2) | B_(1, 55, 1)); acase(p, "LOP32I.XOR ~a", m_lop32i, SAME_BITS);
    I(p, "LOP_cbuf", D_(10) | A_(2) | CB_(0, 0) | B_(1, 41, 2)); acase(p, "LOP cbuf OR", m_lop_cb, SAME_BITS);
    I(p, "LOP3_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0x96, 28, 8)); acase(p, "LOP3 0x96", m_lop3_96, SAME_BITS);
    I(p, "LOP3_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0xE8, 28, 8)); acase(p, "LOP3 0xE8", m_lop3_e8, SAME_BITS);
    I(p, "LOP3_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(0xCA, 28, 8)); acase(p, "LOP3 0xCA", m_lop3_ca, SAME_BITS);
    I(p, "LOP3_imm", D_(10) | A_(2) | IMM_(0x7FFFF) | CR_(4) | B_(0x80, 48, 8)); acase(p, "LOP3 imm", m_lop3_imm, SAME_BITS);
    I(p, "LOP3_cbuf", D_(10) | A_(2) | CB_(0, 0) | CR_(4) | B_(0x6A, 48, 8)); acase(p, "LOP3 cbuf", m_lop3_cb, SAME_BITS);
    I(p, "SHL_reg", D_(10) | A_(2) | BR_(3)); acase(p, "SHL", m_shl, SAME_BITS);
    I(p, "SHL_reg", D_(10) | A_(2) | BR_(3) | B_(1, 39, 1)); acase(p, "SHL.W", m_shl_w, SAME_BITS);
    I(p, "SHL_imm", D_(10) | A_(2) | IMM_(7)); acase(p, "SHL imm", m_shl_imm, SAME_BITS);
    I(p, "SHL_cbuf", D_(10) | A_(2) | CB_(0, 20)); acase(p, "SHL cbuf", m_shl_cb, SAME_BITS);
    I(p, "SHR_reg", D_(10) | A_(2) | BR_(3)); acase(p, "SHR.U32", m_shr, SAME_BITS);
    I(p, "SHR_reg", D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "SHR.S32", m_shr_s, SAME_BITS);
    I(p, "SHR_reg", D_(10) | A_(2) | BR_(3) | B_(1, 39, 1)); acase(p, "SHR.W.U32", m_shr_w, SAME_BITS);
    I(p, "SHR_reg", D_(10) | A_(2) | BR_(3) | B_(1, 39, 1) | B_(1, 48, 1)); acase(p, "SHR.W.S32", m_shr_sw, SAME_BITS);
    I(p, "SHR_reg", D_(10) | A_(2) | BR_(3) | B_(1, 40, 1)); acase(p, "SHR.BREV", m_shr_rev, SAME_BITS);
    I(p, "SHR_imm", D_(10) | A_(2) | IMM_(4) | B_(1, 48, 1)); acase(p, "SHR imm S32", m_shr_imm, SAME_BITS);
    I(p, "SHR_cbuf", D_(10) | A_(2) | CB_(0, 20)); acase(p, "SHR cbuf", m_shr_cb, SAME_BITS);
    I(p, "BFE_reg", D_(10) | A_(2) | BR_(3)); acase(p, "BFE.U32", m_bfe_u, SAME_BITS);
    I(p, "BFE_reg", D_(10) | A_(2) | BR_(3) | B_(1, 48, 1)); acase(p, "BFE.S32", m_bfe_s, SAME_BITS);
    I(p, "BFE_imm", D_(10) | A_(2) | IMM_(0x0C04) | B_(1, 48, 1)); acase(p, "BFE imm S32", m_bfe_imm, SAME_BITS);
    I(p, "BFE_reg", D_(10) | A_(2) | BR_(3) | B_(1, 40, 1)); acase(p, "BFE.BREV", m_bfe_rev, SAME_BITS);
    I(p, "BFE_cbuf", D_(10) | A_(2) | CB_(0, 24)); acase(p, "BFE cbuf", m_bfe_cb, SAME_BITS);
    I(p, "BFI_reg", D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "BFI", m_bfi, SAME_BITS);
    I(p, "BFI_imm", D_(10) | A_(2) | IMM_(0x0A06) | CR_(4)); acase(p, "BFI imm", m_bfi_imm, SAME_BITS);
    I(p, "BFI_rc", D_(10) | A_(2) | CR_(3) | CB_(0, 4)); acase(p, "BFI rc", m_bfi_rc, SAME_BITS);
    I(p, "BFI_cr", D_(10) | A_(2) | CB_(0, 24) | CR_(4)); acase(p, "BFI cr", m_bfi_cr, SAME_BITS);
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "XMAD", m_xmad, SAME_BITS);
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 53, 1) | B_(1, 48, 1)); acase(p, "XMAD.S16 H1(a)", m_xmad_h1s, SAME_BITS);
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(2, 50, 3) | B_(1, 36, 1)); acase(p, "XMAD.CHI.PSL", m_xmad_chi_psl, SAME_BITS);
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(4, 50, 3) | B_(1, 37, 1)); acase(p, "XMAD.CBCC.MRG", m_xmad_cbcc_mrg, SAME_BITS);
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 50, 3) | B_(1, 49, 1) | B_(1, 35, 1)); acase(p, "XMAD.CLO S16 H1(b)", m_xmad_clo_sbhb, SAME_BITS);
    I(p, "IADD_reg", D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, "XMAD_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 38, 1)); acase(p, "IADD.CC, XMAD.X", m_xmad_x, SAME_BITS);
    I(p, "XMAD_imm", D_(10) | A_(2) | B_(0xBEEF, 20, 16) | CR_(4)); acase(p, "XMAD imm", m_xmad_imm, SAME_BITS);
    I(p, "XMAD_rc", D_(10) | A_(2) | CR_(3) | CB_(0, 0) | B_(1, 52, 1)); acase(p, "XMAD rc H1(b)", m_xmad_rc, SAME_BITS);
    I(p, "XMAD_cr", D_(10) | A_(2) | CB_(0, 4) | CR_(4) | B_(1, 55, 1)); acase(p, "XMAD cr PSL", m_xmad_cr, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(0, 10, 2) | B_(1, 13, 1) | B_(1, 41, 3) | B_(2, 8, 2) | B_(1, 12, 1)); acase(p, "I2I.S32.S8 b1", m_i2i_s8, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(1, 10, 2) | B_(2, 41, 3) | B_(2, 8, 2)); acase(p, "I2I.U32.U16 h1", m_i2i_u16, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 45, 1)); acase(p, "I2I -x", m_i2i_neg, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 49, 1)); acase(p, "I2I |x|", m_i2i_abs, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(0, 8, 2) | B_(1, 12, 1) | B_(1, 50, 1)); acase(p, "I2I.S8.SAT", m_i2i_sat8, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 8, 2) | B_(1, 50, 1)); acase(p, "I2I.U16.SAT", m_i2i_satu16, SAME_BITS);
    I(p, "I2I_reg", D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 13, 1) | B_(3, 41, 3) | B_(2, 8, 2) | B_(1, 12, 1)); acase(p, "I2I.S16 b3 (shift past 32)", m_i2i_sel3, SAME_BITS);
    I(p, "I2I_imm", D_(10) | IMM_(-3) | B_(2, 10, 2) | B_(1, 13, 1) | B_(0, 8, 2)); acase(p, "I2I imm", m_i2i_imm, SAME_BITS);
    I(p, "I2I_cbuf", D_(10) | CB_(0, 0) | B_(1, 10, 2) | B_(2, 8, 2)); acase(p, "I2I cbuf", m_i2i_cb, SAME_BITS);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2)); acase(p, "I2F.S32", m_i2f_s, SAME_FLOAT);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2)); acase(p, "I2F.U32", m_i2f_u, SAME_FLOAT);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(0, 10, 2) | B_(2, 41, 2) | B_(2, 8, 2)); acase(p, "I2F.U8 b2", m_i2f_u8, SAME_FLOAT);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 13, 1) | B_(1, 45, 1) | B_(2, 8, 2)); acase(p, "I2F.S16 -x", m_i2f_s16n, SAME_FLOAT);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 49, 1) | B_(2, 8, 2)); acase(p, "I2F.S32 |x|", m_i2f_abs, SAME_FLOAT);
    I(p, "I2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 13, 1) | B_(1, 8, 2)); acase(p, "I2F.F16", m_i2f_h, SAME_BITS);
    I(p, "I2F_imm", D_(10) | IMM_(-7) | B_(2, 10, 2) | B_(1, 13, 1) | B_(2, 8, 2)); acase(p, "I2F imm", m_i2f_imm, SAME_FLOAT);
    I(p, "I2F_cbuf", D_(10) | CB_(0, 0) | B_(2, 10, 2) | B_(2, 8, 2)); acase(p, "I2F cbuf", m_i2f_cb, SAME_FLOAT);
    /* conversions from float */
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(0, 39, 2)); acase(p, "F2I.S32 RN", m_f2i_rn, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(1, 39, 2)); acase(p, "F2I.S32 floor", m_f2i_fl, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(2, 39, 2)); acase(p, "F2I.S32 ceil", m_f2i_ce, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.S32 trunc", m_f2i_tr, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(3, 39, 2)); acase(p, "F2I.U32 trunc", m_f2u_tr, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2) | B_(1, 12, 1)); acase(p, "F2I.S16", m_f2i_s16, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2) | B_(1, 45, 1)); acase(p, "F2I.U16 |x|", m_f2u_16a, SAME_BITS);
    I(p, "F2I_reg", D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 41, 1) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.F16 h1 trunc", m_f2i_h1, SAME_BITS);
    I(p, "F2I_imm", D_(10) | FIMM_(0xC0200000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1)); acase(p, "F2I imm -2.5 RN", m_f2i_imm, SAME_BITS);
    I(p, "F2I_cbuf", D_(10) | CB_(0, 8) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 39, 2)); acase(p, "F2I cbuf floor", m_f2i_cb, SAME_BITS);
    /* out of range and NaN operands Metal's compiler knows: saturated as at run time (a plain
     * conversion of a constant out of range is folded to poison, and the store goes) */
    I(p, "F2I_imm", D_(10) | FIMM_(0x7F800000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.S32 imm +inf", m_f2i_max, SAME_BITS);
    I(p, "F2I_imm", D_(10) | FIMM_(0xE0AD7000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.S32 imm -1e20", m_f2i_min, SAME_BITS);
    I(p, "F2I_imm", D_(10) | FIMM_(0x7FC00000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "F2I.S32 imm NaN", m_f2i_zero, SAME_BITS);
    I(p, "F2I_imm", D_(10) | FIMM_(0x60AD7000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(3, 39, 2)); acase(p, "F2I.U32 imm 1e20", m_f2u_max, SAME_BITS);
    I(p, "F2I_imm", D_(10) | FIMM_(0xBF800000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(3, 39, 2)); acase(p, "F2I.U32 imm -1", m_f2i_zero, SAME_BITS);
    I(p, "MOV32I", D_(11) | I32_(0x7F800000));
    I(p, "F2I_reg", D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MOV32I +inf, F2I.S32", m_f2i_max, SAME_BITS);
    I(p, "MUFU", D_(11) | A_(RZ_) | B_(4, 20, 4));   /* (1 / RZ: the translator's own constant, +inf) */
    I(p, "F2I_reg", D_(10) | BR_(11) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 12, 1) | B_(3, 39, 2)); acase(p, "MUFU.RCP RZ, F2I.S32", m_f2i_max, SAME_BITS);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(8, 39, 4)); acase(p, "F2F RN (rint)", m_f2f_rn, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(9, 39, 4)); acase(p, "F2F floor", m_f2f_fl, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(10, 39, 4)); acase(p, "F2F ceil", m_f2f_ce, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(11, 39, 4)); acase(p, "F2F trunc", m_f2f_tr, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(1, 8, 2)); acase(p, "F2F.F16.F32", m_f2f_h, SAME_HALVES);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(1, 10, 2) | B_(1, 41, 1) | B_(2, 8, 2)); acase(p, "F2F.F32.F16 h1", m_f2f_h1, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 50, 1) | B_(1, 45, 1)); acase(p, "F2F.SAT -x", m_f2f_satn, SAME_FLOAT);
    I(p, "F2F_reg", D_(10) | BR_(2) | B_(2, 10, 2) | B_(2, 8, 2) | B_(1, 49, 1)); acase(p, "F2F |x|", m_f2f_abs, SAME_FLOAT);
    I(p, "F2F_imm", D_(10) | FIMM_(0x40200000) | B_(2, 10, 2) | B_(2, 8, 2) | B_(8, 39, 4)); acase(p, "F2F imm 2.5 RN", m_f2f_imm, SAME_FLOAT);
    I(p, "F2F_cbuf", D_(10) | CB_(0, 12) | B_(2, 10, 2) | B_(2, 8, 2) | B_(10, 39, 4)); acase(p, "F2F cbuf ceil", m_f2f_cb, SAME_FLOAT);
    /* float */
    I(p, "FADD_reg", D_(10) | A_(2) | BR_(3)); acase(p, "FADD", m_fadd, SAME_FLOAT);
    I(p, "FADD_reg", D_(10) | A_(2) | BR_(3) | B_(1, 48, 1) | B_(1, 49, 1)); acase(p, "FADD -a |b|", m_fadd_nab, SAME_FLOAT);
    I(p, "FADD_reg", D_(10) | A_(2) | BR_(3) | B_(1, 50, 1)); acase(p, "FADD.SAT", m_fadd_sat, SAME_FLOAT);
    I(p, "FADD_imm", D_(10) | A_(2) | FIMM_(0x3FC00000)); acase(p, "FADD imm", m_fadd_imm, SAME_FLOAT);
    I(p, "FADD32I", D_(10) | A_(2) | I32_(0x40490FDB) | B_(1, 54, 1)); acase(p, "FADD32I |a|", m_fadd32i, SAME_FLOAT);
    I(p, "FADD_cbuf", D_(10) | A_(2) | CB_(0, 12)); acase(p, "FADD cbuf", m_fadd_cb, SAME_FLOAT);
    I(p, "FMUL_reg", D_(10) | A_(2) | BR_(3)); acase(p, "FMUL", m_fmul, SAME_FLOAT);
    I(p, "FMUL_reg", D_(10) | A_(2) | BR_(3) | B_(1, 41, 3)); acase(p, "FMUL.D2", m_fmul_d2, SAME_FLOAT);
    I(p, "FMUL_reg", D_(10) | A_(2) | BR_(3) | B_(4, 41, 3) | B_(2, 44, 2)); acase(p, "FMUL.M8.FMZ", m_fmul_fmz, SAME_FLOAT);
    I(p, "FMUL_reg", D_(10) | A_(2) | BR_(3) | B_(1, 50, 1) | B_(1, 48, 1)); acase(p, "FMUL.SAT -b", m_fmul_satn, SAME_FLOAT);
    I(p, "FMUL32I", D_(10) | A_(2) | I32_(0x3EAAAAAB)); acase(p, "FMUL32I", m_fmul32i, SAME_FLOAT);
    I(p, "FMUL_imm", D_(10) | A_(2) | FIMM_(0xC0000000)); acase(p, "FMUL imm", m_fmul_imm, SAME_FLOAT);
    I(p, "FMUL_cbuf", D_(10) | A_(2) | CB_(0, 8)); acase(p, "FMUL cbuf", m_fmul_cb, SAME_FLOAT);
    I(p, "FFMA_reg", D_(10) | A_(2) | BR_(3) | CR_(4)); acase(p, "FFMA (fused)", m_ffma, SAME_FLOAT);
    I(p, "FFMA_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 49, 1)); acase(p, "FFMA -c", m_ffma_nc, SAME_FLOAT);
    I(p, "FFMA_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(2, 53, 2)); acase(p, "FFMA.FMZ", m_ffma_fmz, SAME_FLOAT);
    I(p, "FFMA_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 50, 1) | B_(1, 48, 1)); acase(p, "FFMA.SAT -b", m_ffma_satnb, SAME_FLOAT);
    I(p, "FFMA_imm", D_(10) | A_(2) | FIMM_(0x40000000) | CR_(4)); acase(p, "FFMA imm", m_ffma_imm, SAME_FLOAT);
    I(p, "FFMA_rc", D_(10) | A_(2) | CR_(3) | CB_(0, 8)); acase(p, "FFMA rc", m_ffma_rc, SAME_FLOAT);
    I(p, "FFMA_cr", D_(10) | A_(2) | CB_(0, 8) | CR_(4)); acase(p, "FFMA cr", m_ffma_cr, SAME_FLOAT);
    I(p, "MOV_reg", D_(10) | BR_(4));
    I(p, "FFMA32I", D_(10) | A_(2) | I32_(0x3F000000) | B_(1, 57, 1)); acase(p, "FFMA32I -c(d)", m_ffma32i, SAME_FLOAT);
    I(p, "FMUL_reg", D_(11) | A_(2) | BR_(3));
    I(p, "FFMA_reg", D_(10) | A_(2) | BR_(3) | CR_(11)); acase(p, "FMUL, FFMA", m_ffma_dbl, SAME_FLOAT);
    I(p, "FMNMX_reg", D_(10) | A_(2) | BR_(3) | PPT_); acase(p, "FMNMX min", m_fmin, ANY_ZERO);
    I(p, "FMNMX_reg", D_(10) | A_(2) | BR_(3) | PPT_ | B_(1, 42, 1) | B_(1, 48, 1)); acase(p, "FMNMX max -a", m_fmax_na, ANY_ZERO);
    I(p, "FMNMX_imm", D_(10) | A_(2) | FIMM_(0x3F800000) | PPT_); acase(p, "FMNMX imm", m_fmin_imm, ANY_ZERO);
    I(p, "FMNMX_cbuf", D_(10) | A_(2) | CB_(0, 8) | PPT_ | B_(1, 42, 1)); acase(p, "FMNMX cbuf max", m_fmax_cb, ANY_ZERO);
    I(p, "MUFU", D_(10) | A_(2) | B_(4, 20, 4)); acase(p, "MUFU.RCP", m_rcp, NEAR_ULPS);
    I(p, "MUFU", D_(10) | A_(2) | B_(5, 20, 4)); acase(p, "MUFU.RSQ", m_rsq, NEAR_ULPS);
    I(p, "MUFU", D_(10) | A_(2) | B_(8, 20, 4)); acase(p, "MUFU.SQRT", m_sqrt, NEAR_ULPS);
    I(p, "MUFU", D_(10) | A_(2) | B_(4, 20, 4) | B_(1, 46, 1) | B_(1, 48, 1)); acase(p, "MUFU.RCP -|a|", m_rcp_an, NEAR_ULPS);
    I(p, "MUFU", D_(10) | A_(2) | B_(2, 20, 4)); acase(p, "MUFU.EX2", m_ex2, NEAR_2E6);
    I(p, "MUFU", D_(10) | A_(2) | B_(3, 20, 4)); acase(p, "MUFU.LG2", m_lg2, NEAR_2E6);
    I(p, "MUFU", D_(10) | A_(2) | B_(1, 20, 4)); acase(p, "MUFU.SIN", m_sin, NEAR_2E6);
    I(p, "MUFU", D_(10) | A_(2) | B_(0, 20, 4)); acase(p, "MUFU.COS", m_cos, NEAR_2E6);
    I(p, "RRO_reg", D_(10) | BR_(2)); acase(p, "RRO", m_rro, SAME_FLOAT);
    I(p, "RRO_reg", D_(10) | BR_(3) | B_(1, 45, 1)); acase(p, "RRO -b", m_rro_n, SAME_FLOAT);
    I(p, "RRO_imm", D_(10) | FIMM_(0x3F800000)); acase(p, "RRO imm", m_rro_imm, SAME_FLOAT);
    I(p, "RRO_cbuf", D_(10) | CB_(0, 8)); acase(p, "RRO cbuf", m_rro_cb, SAME_FLOAT);
    /* compares: every FSET condition, then the predicate forms */
    for (u32 k = 0; k < 16; k++) {
        static char names[16][24];
        snprintf(names[k], sizeof names[k], "FSET cond %u", k);
        I(p, "FSET_reg", D_(10) | A_(2) | BR_(3) | B_(k, 48, 4) | PPT_);
        acase(p, names[k], NULL, SAME_BITS);
        alu[nalu - 1].match = 100 + (int)k;   /* (checked by fcmp itself) */
    }
    I(p, "FSET_reg", D_(10) | A_(2) | BR_(3) | B_(3, 48, 4) | PPT_ | B_(1, 52, 1) | B_(1, 54, 1) | B_(1, 53, 1)); acase(p, "FSET.LE.BF |a| -b", m_fset_bf, SAME_BITS);
    I(p, "FSET_imm", D_(10) | A_(2) | FIMM_(0x3F800000) | B_(14, 48, 4) | PPT_); acase(p, "FSET imm", m_fset_imm, SAME_BITS);
    I(p, "FSET_cbuf", D_(10) | A_(2) | CB_(0, 28) | B_(5, 48, 4) | PPT_); acase(p, "FSET cbuf NaN", m_fset_cb, SAME_BITS);
    I(p, "FSETP_reg", A_(2) | BR_(3) | B_(10, 48, 4) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, "PSET", D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "FSETP, PSET", m_fsetp_psel, SAME_BITS);
    I(p, "FSETP_imm", A_(2) | FIMM_(0) | B_(4, 48, 4) | PPT_ | B_(5, 3, 3) | B_(7, 0, 3));
    I(p, "SEL_reg", D_(10) | A_(2) | BR_(3) | B_(5, 39, 3)); acase(p, "FSETP imm, SEL", m_fsetp_imm, SAME_BITS);
    I(p, "FSETP_cbuf", A_(2) | CB_(0, 8) | B_(9, 48, 4) | PPT_ | B_(7, 3, 3) | B_(6, 0, 3));
    I(p, "MOV32I", D_(11) | I32_(0)); I(p, "MOV32I", D_(12) | I32_(1));
    I(p, "SEL_reg", D_(10) | A_(11) | BR_(12) | B_(6, 39, 3)); acase(p, "FSETP cbuf (second)", m_fsetp_cb, SAME_BITS);
    I(p, "IADD_reg", D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, "CSETP", B_(17, 8, 5) | PPT_ | B_(3, 3, 3) | B_(7, 0, 3));
    I(p, "SEL_reg", D_(10) | A_(2) | BR_(3) | B_(3, 39, 3)); acase(p, "IADD.CC, CSETP.LO", m_csetp_lo, SAME_BITS);
    I(p, "IADD_reg", D_(11) | A_(2) | BR_(3) | B_(1, 47, 1));
    I(p, "CSET", D_(10) | B_(2, 8, 5) | PPT_ | B_(1, 44, 1)); acase(p, "IADD.CC, CSET.EQ.BF", m_cset_eq, SAME_BITS);
    I(p, "IADD_reg", D_(11) | A_(2) | BR_(3) | B_(1, 48, 1) | B_(1, 47, 1));
    I(p, "CSET", D_(10) | B_(1, 8, 5) | PPT_); acase(p, "IADD.CC -b, CSET.LT", m_cset_lt, SAME_BITS);
    I(p, "ISETP_reg", A_(2) | BR_(3) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    I(p, "ISETP_reg", A_(3) | BR_(4) | B_(1, 49, 3) | PPT_ | B_(1, 3, 3) | B_(7, 0, 3));
    I(p, "PSETP", B_(0, 12, 3) | B_(1, 29, 3) | B_(2, 24, 2) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    I(p, "PSET", D_(10) | B_(2, 12, 3) | B_(7, 29, 3) | PPT_ | B_(1, 44, 1)); acase(p, "PSETP.XOR, PSET.BF", m_psetp, SAME_BITS);
    I(p, "R2P_imm", A_(2) | IMM_(0x7F00) | B_(1, 41, 2));
    I(p, "PSET", D_(10) | B_(3, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "R2P b1, PSET", m_r2p, SAME_BITS);
    I(p, "MOV32I", D_(11) | I32_(0x7F));
    I(p, "R2P_reg", A_(2) | BR_(11));
    I(p, "MOV32I", D_(12) | I32_(5)); I(p, "MOV32I", D_(13) | I32_(6));
    I(p, "SEL_reg", D_(10) | A_(12) | BR_(13) | B_(2, 39, 3)); acase(p, "R2P reg, SEL", m_r2p_reg, SAME_BITS);
    I(p, "R2P_cbuf", A_(30) | CB_(0, 0) | B_(0, 41, 2));   /* (r30: the input address - its predicates, overwritten by:) */
    I(p, "R2P_imm", A_(RZ_) | IMM_(0) | B_(0, 41, 2));
    I(p, "MOV_cbuf", D_(14) | CB_(0, 0));
    I(p, "R2P_reg", A_(14) | BR_(14));
    I(p, "MOV32I", D_(12) | I32_(7)); I(p, "MOV32I", D_(13) | I32_(8));
    I(p, "SEL_reg", D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "R2P cbuf mask, SEL", m_r2p_cb, SAME_BITS);
    /* half pairs */
    I(p, "HADD2_reg", D_(10) | A_(2) | BR_(3)); acase(p, "HADD2", m_hadd2, SAME_HALVES);
    I(p, "HADD2_reg", D_(10) | A_(2) | BR_(3) | B_(1, 31, 1) | B_(1, 32, 1) | B_(2, 28, 2)); acase(p, "HADD2 -b.H0_H0 SAT", m_hadd2_nbs, SAME_HALVES);
    I(p, "HADD2_reg", D_(10) | A_(2) | BR_(3) | B_(1, 28, 2) | B_(1, 49, 2)); acase(p, "HADD2 b F32, merge F32", m_hadd2_f32, NEAR_ULPS);
    I(p, "MOV_reg", D_(10) | BR_(4));
    I(p, "HADD2_reg", D_(10) | A_(2) | BR_(3) | B_(2, 49, 2)); acase(p, "HADD2 merge H0", m_hadd2_mrg, SAME_HALVES);
    I(p, "HADD2_imm", D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1)); acase(p, "HADD2 imm", m_hadd2_imm, SAME_HALVES);
    I(p, "HADD2_32I", D_(10) | A_(2) | I32_(0x3C004000) | B_(1, 56, 1)); acase(p, "HADD2_32I -a", m_hadd2_32i, SAME_HALVES);
    I(p, "HADD2_cbuf", D_(10) | A_(2) | CB_(0, 8)); acase(p, "HADD2 cbuf", m_hadd2_cb, SAME_HALVES);
    I(p, "HMUL2_reg", D_(10) | A_(2) | BR_(3) | B_(1, 44, 1)); acase(p, "HMUL2 |a|", m_hmul2, SAME_HALVES);
    I(p, "HMUL2_32I", D_(10) | A_(2) | I32_(0x3C004000)); acase(p, "HMUL2_32I", m_hmul2_32i, SAME_HALVES);
    I(p, "HMUL2_imm", D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1)); acase(p, "HMUL2 imm", m_hmul2_imm, SAME_HALVES);
    I(p, "HMUL2_cbuf", D_(10) | A_(2) | CB_(0, 8) | B_(1, 43, 1)); acase(p, "HMUL2 cbuf -a", m_hmul2_cb, SAME_HALVES);
    I(p, "HFMA2_reg", D_(10) | A_(2) | BR_(3) | CR_(4) | B_(1, 30, 1) | B_(3, 35, 2)); acase(p, "HFMA2 -c.H1_H1", m_hfma2, SAME_HALVES);
    I(p, "MOV_reg", D_(10) | BR_(4));
    I(p, "HFMA2_32I", D_(10) | A_(2) | I32_(0x40003C00) | B_(1, 52, 1)); acase(p, "HFMA2_32I -c(d)", m_hfma2_32i, SAME_HALVES);
    I(p, "HFMA2_rc", D_(10) | A_(2) | CR_(3) | CB_(0, 8)); acase(p, "HFMA2 rc", m_hfma2_rc, SAME_HALVES);
    I(p, "HFMA2_cr", D_(10) | A_(2) | CB_(0, 8) | CR_(4)); acase(p, "HFMA2 cr", m_hfma2_cr, SAME_HALVES);
    I(p, "HFMA2_imm", D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | CR_(4)); acase(p, "HFMA2 imm", m_hfma2_imm, SAME_HALVES);
    I(p, "HSET2_reg", D_(10) | A_(2) | BR_(3) | B_(1, 35, 4) | PPT_); acase(p, "HSET2.LT", m_hset2, SAME_BITS);
    I(p, "HSET2_reg", D_(10) | A_(2) | BR_(3) | B_(13, 35, 4) | PPT_ | B_(1, 49, 1)); acase(p, "HSET2.NEU.BF", m_hset2_bf, SAME_BITS);
    I(p, "HSET2_imm", D_(10) | A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | B_(4, 49, 4) | PPT_); acase(p, "HSET2 imm", m_hset2_imm, SAME_BITS);
    I(p, "HSET2_cbuf", D_(10) | A_(2) | CB_(0, 8) | B_(3, 49, 4) | PPT_); acase(p, "HSET2 cbuf", m_hset2_cb, SAME_BITS);
    I(p, "HSETP2_reg", A_(2) | BR_(3) | B_(6, 35, 4) | PPT_ | B_(1, 3, 3) | B_(2, 0, 3));
    I(p, "PSET", D_(10) | B_(1, 12, 3) | B_(2, 29, 3) | PPT_); acase(p, "HSETP2.GE, PSET.AND", m_hsetp2, SAME_BITS);
    I(p, "HSETP2_reg", A_(2) | BR_(3) | B_(2, 35, 4) | PPT_ | B_(1, 49, 1) | B_(7, 3, 3) | B_(3, 0, 3));
    I(p, "PSET", D_(10) | B_(3, 12, 3) | B_(7, 29, 3) | PPT_); acase(p, "HSETP2.EQ.H_AND (second)", m_hsetp2_and, SAME_BITS);
    I(p, "HSETP2_imm", A_(2) | B_(0xF0, 20, 9) | B_(0x100, 30, 9) | B_(1, 56, 1) | B_(1, 49, 4) | PPT_ | B_(4, 3, 3) | B_(7, 0, 3));
    I(p, "MOV32I", D_(12) | I32_(3)); I(p, "MOV32I", D_(13) | I32_(4));
    I(p, "SEL_reg", D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "HSETP2 imm", m_hsetp2_imm, SAME_BITS);
    I(p, "HSETP2_cbuf", A_(2) | CB_(0, 8) | B_(8, 49, 4) | PPT_ | B_(7, 3, 3) | B_(4, 0, 3));
    I(p, "SEL_reg", D_(10) | A_(12) | BR_(13) | B_(4, 39, 3)); acase(p, "HSETP2 cbuf (high half)", m_hsetp2_cb, SAME_BITS);
    /* moves, selects, votes */
    I(p, "MOV32I", D_(10) | I32_(0xCAFEF00D)); acase(p, "MOV32I", m_mov32i, SAME_BITS);
    I(p, "MOV_imm", D_(10) | IMM_(-7)); acase(p, "MOV imm", m_mov_imm, SAME_BITS);
    I(p, "MOV_cbuf", D_(10) | CB_(0, 12)); acase(p, "MOV cbuf", m_mov_cb, SAME_BITS);
    I(p, "SEL_imm", D_(10) | A_(2) | IMM_(77) | PPT_ | B_(1, 42, 1)); acase(p, "SEL imm", m_sel_imm, SAME_BITS);
    I(p, "SEL_cbuf", D_(10) | A_(2) | CB_(0, 0) | PPT_ | B_(1, 42, 1)); acase(p, "SEL cbuf", m_sel_cb, SAME_BITS);
    I(p, "VOTE", D_(10)); acase(p, "VOTE", m_vote, SAME_BITS);
    I(p, "VOTE_vtg", 0); I(p, "NOP", 0); I(p, "DEPBAR", 0); I(p, "MEMBAR", 0); I(p, "BAR", 0);
    exit_(p);
    for (u32 i = 0; i < 8; i++) p->cb[0][i] = CB0[i];
    return p;
}

/* Branches: a loop, SSY/SYNC, PBK/BRK, an indirect branch through a jump table, a condition-code
 * branch; the last thread leaves at once (a guarded EXIT inside the dispatch loop). */
static Prog *build_flow(void) {
    Prog *p = prog("cs_flow", SHADER_COMPUTE);
    p->local[0] = 32;
    prologue(p, 4, 6);
    I(p, "ISETP_imm", A_(0) | IMM_(31) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    IG(p, 6, 0, "EXIT", B_(15, 0, 5));
    I(p, "MOV32I", D_(10) | I32_(0));                                   /* r10 = 1 + 2 + ... + (a & 15) */
    I(p, "LOP_imm", D_(12) | A_(2) | IMM_(15));
    label(p, "loop");
    I(p, "ISETP_imm", A_(12) | IMM_(0) | B_(2, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    jump(p, 0, 0, "BRA", "done", 15);
    I(p, "IADD_reg", D_(10) | A_(10) | BR_(12));
    I(p, "IADD_imm", D_(12) | A_(12) | IMM_(-1));
    jump(p, 7, 0, "BRA", "loop", 15);
    label(p, "done");
    store(p, 10, 0);
    jump(p, 7, 0, "SSY", "join", 15);                                   /* r11 = a odd ? 1 : 2 */
    I(p, "LOP_imm", D_(13) | A_(2) | IMM_(1));
    I(p, "ISETP_imm", A_(13) | IMM_(0) | B_(5, 49, 3) | PPT_ | B_(1, 3, 3) | B_(7, 0, 3));
    jump(p, 1, 0, "BRA", "odd", 15);
    I(p, "MOV32I", D_(11) | I32_(2));
    I(p, "SYNC", B_(15, 0, 5));
    label(p, "odd");
    I(p, "MOV32I", D_(11) | I32_(1));
    I(p, "SYNC", B_(15, 0, 5));
    label(p, "join");
    store(p, 11, 1);
    I(p, "MOV32I", D_(14) | I32_(0));                                   /* r14 counts to b & 7, BRK leaving */
    I(p, "LOP_imm", D_(15) | A_(3) | IMM_(7));
    jump(p, 7, 0, "PBK", "after", 15);
    label(p, "loop2");
    I(p, "ISETP_reg", A_(14) | BR_(15) | B_(6, 49, 3) | PPT_ | B_(2, 3, 3) | B_(7, 0, 3));
    IG(p, 2, 0, "BRK", B_(15, 0, 5));
    I(p, "IADD_imm", D_(14) | A_(14) | IMM_(1));
    jump(p, 7, 0, "BRA", "loop2", 15);
    label(p, "after");
    store(p, 14, 2);
    I(p, "LOP_imm", D_(16) | A_(4) | IMM_(3));                          /* r18 = 100 + (c & 3), by BRX */
    I(p, "IMNMX_imm", D_(16) | A_(16) | IMM_(3) | PPT_);
    I(p, "SHL_imm", D_(16) | A_(16) | IMM_(2));
    I(p, "LDC", D_(17) | A_(16) | B_(0x40, 20, 16) | B_(2, 36, 5) | B_(4, 48, 3));
    u32 brx = p->n;
    I(p, "BRX", A_(17) | B_(15, 0, 5));
    static const char *const cases[4] = {"case0", "case1", "case2", "case3"};
    for (u32 k = 0; k < 4; k++) {
        label(p, cases[k]);
        I(p, "MOV32I", D_(18) | I32_(100 + k));
        if (k < 3) jump(p, 7, 0, "BRA", "joinx", 15);
    }
    label(p, "joinx");
    store(p, 18, 3);
    I(p, "IADD_reg", D_(20) | A_(2) | BR_(3) | B_(1, 47, 1));           /* r19 = a + b carries ? 1 : 2 */
    I(p, "MOV32I", D_(19) | I32_(1));
    jump(p, 7, 0, "BRA", "carried", 22);
    I(p, "MOV32I", D_(19) | I32_(2));
    label(p, "carried");
    store(p, 19, 4);
    exit_(p);
    for (u32 k = 0; k < 4; k++) p->cb[2][0x40 / 4 + k] = label_addr(p, cases[k]) - (addr_of(brx) + 8);
    return p;
}

/* Memory: constant buffer 3 at every size and out of range, global memory loads and stores of
 * every size (outside the storage buffers: 0), shared and local memory, special registers. Two
 * workgroups of 32; the grid's last thread leaves first (a guarded EXIT, straight code). */
static const u8 *cb3_bytes(void) {
    static u8 b[64];
    for (u32 i = 0; i < 64; i++) b[i] = (u8)(i * 37 + 11);
    return b;
}
static Prog *build_mem(void) {
    Prog *p = prog("cs_mem", SHADER_COMPUTE);
    p->local[0] = 32;
    p->shared = 512;
    prologue(p, 4, 8);
    I(p, "ISCADD_reg", D_(22) | A_(1) | BR_(0) | B_(5, 39, 5));
    I(p, "ISETP_imm", A_(22) | IMM_(63) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
    IG(p, 6, 0, "EXIT", B_(15, 0, 5));
    I(p, "LOP_imm", D_(12) | A_(2) | IMM_(0x7F));
    for (u32 s = 0; s < 5; s++) { I(p, "LDC", D_(10) | A_(12) | B_(3, 36, 5) | B_(s, 48, 3)); store(p, 10, s); }
    I(p, "LDC", D_(10) | A_(12) | B_(3, 36, 5) | B_(5, 48, 3)); store(p, 10, 5); store(p, 11, 6);
    I(p, "LDC", D_(10) | A_(RZ_) | B_(0x10, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3)); store(p, 10, 7);
    I(p, "LDC", D_(10) | A_(12) | B_(0xFFFC, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3)); store(p, 10, 8);
    I(p, "LOP_imm", D_(13) | A_(3) | IMM_(0x3F));
    I(p, "IADD_reg", D_(40) | A_(30) | BR_(13));
    I(p, "MOV_reg", D_(41) | BR_(31));
    for (u32 s = 0; s < 5; s++) { I(p, "LDG", D_(10) | A_(40) | B_(1, 45, 1) | B_(s, 48, 3)); store(p, 10, 9 + s); }
    I(p, "LDG", D_(10) | A_(40) | B_(1, 45, 1) | B_(5, 48, 3)); store(p, 10, 14); store(p, 11, 15);
    I(p, "IADD32I", D_(42) | A_(30) | I32_(0x100000));
    I(p, "MOV_reg", D_(43) | BR_(31));
    I(p, "LDG", D_(10) | A_(42) | B_(1, 45, 1) | B_(4, 48, 3)); store(p, 10, 16);   /* past the buffer */
    I(p, "MOV_reg", D_(44) | BR_(40));
    I(p, "IADD32I", D_(45) | A_(31) | I32_(1));
    I(p, "LDG", D_(10) | A_(44) | B_(1, 45, 1) | B_(4, 48, 3)); store(p, 10, 17);   /* another high word */
    I(p, "LDG", D_(10) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(16, 20, 24));
    store(p, 10, 18); store(p, 11, 19); store(p, 12, 20); store(p, 13, 21);
    I(p, "MOV32I", D_(14) | I32_(0xAAAAAAAA));
    store(p, 14, 22); store(p, 14, 23);
    I(p, "LOP_imm", D_(15) | A_(0) | IMM_(3));
    I(p, "IADD_reg", D_(46) | A_(36) | BR_(15));
    I(p, "MOV_reg", D_(47) | BR_(37));
    I(p, "STG", D_(3) | A_(46) | B_(1, 45, 1) | B_(0, 48, 3) | B_(4 * 22, 20, 24));
    I(p, "STG", D_(4) | A_(36) | B_(1, 45, 1) | B_(2, 48, 3) | B_(4 * 23 + 2, 20, 24));
    I(p, "STG", D_(2) | A_(36) | B_(1, 45, 1) | B_(5, 48, 3) | B_(4 * 24, 20, 24));
    I(p, "STG", D_(2) | A_(36) | B_(1, 45, 1) | B_(6, 48, 3) | B_(4 * 28, 20, 24));
    I(p, "SHL_imm", D_(50) | A_(0) | IMM_(4));                         /* shared memory: the thread's 16 bytes */
    I(p, "STS", D_(2) | A_(50) | B_(4, 48, 3));
    I(p, "STS", D_(3) | A_(50) | B_(0, 48, 3) | B_(1, 20, 24));
    I(p, "LDS", D_(10) | A_(50) | B_(4, 48, 3)); store(p, 10, 32);
    I(p, "STS", D_(5) | A_(50) | B_(4, 48, 3) | B_(4, 20, 24));
    I(p, "STS", D_(4) | A_(50) | B_(2, 48, 3) | B_(6, 20, 24));
    I(p, "LDS", D_(10) | A_(50) | B_(3, 48, 3) | B_(6, 20, 24)); store(p, 10, 33);
    I(p, "LDS", D_(10) | A_(50) | B_(0, 48, 3) | B_(3, 20, 24)); store(p, 10, 34);
    I(p, "LDS", D_(10) | A_(50) | B_(1, 48, 3) | B_(1, 20, 24)); store(p, 10, 35);
    I(p, "LDS", D_(10) | A_(50) | B_(2, 48, 3) | B_(5, 20, 24)); store(p, 10, 36);
    I(p, "STS", D_(2) | A_(50) | B_(5, 48, 3) | B_(8, 20, 24));
    I(p, "LDS", D_(10) | A_(50) | B_(5, 48, 3) | B_(8, 20, 24)); store(p, 10, 37); store(p, 11, 38);
    I(p, "STS", D_(2) | A_(50) | B_(6, 48, 3));
    I(p, "LDS", D_(10) | A_(50) | B_(6, 48, 3)); store(p, 10, 39); store(p, 11, 40); store(p, 12, 41); store(p, 13, 42);
    I(p, "STL", D_(4) | A_(RZ_) | B_(4, 48, 3) | B_(0x20, 20, 24));    /* local memory */
    I(p, "STL", D_(2) | A_(RZ_) | B_(0, 48, 3) | B_(0x21, 20, 24));
    I(p, "LDL", D_(10) | A_(RZ_) | B_(4, 48, 3) | B_(0x20, 20, 24)); store(p, 10, 43);
    I(p, "LDL", D_(10) | A_(RZ_) | B_(1, 48, 3) | B_(0x21, 20, 24)); store(p, 10, 44);
    I(p, "STL", D_(2) | A_(RZ_) | B_(5, 48, 3) | B_(0x40, 20, 24));
    I(p, "LDL", D_(10) | A_(RZ_) | B_(5, 48, 3) | B_(0x40, 20, 24)); store(p, 10, 45); store(p, 11, 46);
    static const u32 srs[7] = {0x25, 0x28, 0x22, 18, 0x20, 0, 0x50};
    for (u32 i = 0; i < 7; i++) { I(p, "S2R", D_(10) | B_(srs[i], 20, 8)); store(p, 10, 47 + i); }
    exit_(p);
    memcpy(p->cb[3], cb3_bytes(), 64);
    return p;
}

/* Textures, in a compute program (explicit LOD): 2D with mips, 2D array, depth compares, cube,
 * 3D, 1D (a 2D texture), an integer texture's bits through a float view; samples, texel
 * fetches in and out of range, gathers with offsets in Metal's -8..7 and past it, TMML.
 * Inputs, 128 bytes a thread: r2..r5 u v lod -, r6..r9 x y mip layer, r22..r25 offsets,
 * r60..r63 layer u4 v4 -, r64..r67 cube direction, depth reference, r68..r71 3D x y z, x & 3. */
enum { H_2D = 0x10, H_ARRAY = 0x14, H_DEPTH = 0x18, H_CUBE = 0x1C, H_3D = 0x20, H_1D = 0x24, H_UINT = 0x28 };
static u64 TEXS_(u32 da, u32 a, u32 b, u32 db, u32 handle, u32 swz, u32 enc) {
    return D_(da) | A_(a) | BR_(b) | B_(db, 28, 8) | B_(handle / 4, 36, 13) | B_(swz, 50, 3) | B_(enc, 53, 4) | B_(1, 59, 1);
}
static u64 TEX_(u32 d, u32 a, u32 m, u32 type, u32 mask, u32 handle, u32 dc, u32 aoffi, u32 blod) {
    return D_(d) | A_(a) | BR_(m) | B_(type, 28, 3) | B_(mask, 31, 4) | B_(handle / 4, 36, 13) | B_(dc, 50, 1) | B_(aoffi, 54, 1) | B_(blod, 55, 3);
}
static u64 TLD4_(u32 d, u32 a, u32 m, u32 type, u32 handle, u32 dc, u32 offmode, u32 comp) {
    return D_(d) | A_(a) | BR_(m) | B_(type, 28, 3) | B_(15, 31, 4) | B_(handle / 4, 36, 13) | B_(dc, 50, 1) | B_(offmode, 54, 2) | B_(comp, 56, 2);
}
static u64 TLD4S_(u32 da, u32 a, u32 b, u32 db, u32 handle, u32 dc, u32 aoffi, u32 comp) {
    return D_(da) | A_(a) | BR_(b) | B_(db, 28, 8) | B_(handle / 4, 36, 13) | B_(dc, 50, 1) | B_(aoffi, 51, 1) | B_(comp, 52, 2);
}
static Prog *build_tex(void) {
    Prog *p = prog("cs_tex", SHADER_COMPUTE);
    p->local[0] = 16;
    prologue(p, 7, 9);
    I(p, "LDG", D_(6) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(16, 20, 24));
    I(p, "LDG", D_(22) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(32, 20, 24));
    I(p, "LDG", D_(60) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(48, 20, 24));
    I(p, "LDG", D_(64) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(64, 20, 24));
    I(p, "LDG", D_(68) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3) | B_(80, 20, 24));
    u32 j = 0;
#define STORE4() do { store(p, 10, j); store(p, 11, j + 1); store(p, 12, j + 2); store(p, 13, j + 3); j += 4; } while (0)
    I(p, "TEXS", TEXS_(10, 2, 3, 12, H_2D, 4, 2)); STORE4();                            /* 0: TEXS 2D, LOD 0 */
    I(p, "TEX", TEX_(10, 2, 4, TEX_2D, 15, H_2D, 0, 0, 3)); STORE4();                    /* 4: TEX 2D, LOD 1 */
    I(p, "TEX", TEX_(10, 2, 22, TEX_2D, 15, H_2D, 0, 1, 1)); STORE4();                   /* 8: TEX 2D, offset */
    I(p, "TLDS", TEXS_(10, 6, 7, 12, H_2D, 4, 2)); STORE4();                            /* 12: TLDS 2D */
    I(p, "TLDS", TEXS_(10, 6, 8, 12, H_2D, 4, 12)); STORE4();                           /* 16: TLDS 2D at a mip */
    I(p, "TLD4", TLD4_(10, 2, RZ_, TEX_2D, H_2D, 0, 0, 0)); STORE4();                   /* 20: gather R */
    I(p, "TLD4", TLD4_(10, 2, 23, TEX_2D, H_2D, 0, 1, 1)); STORE4();                    /* 24: gather G, offset */
    I(p, "TLD4", TLD4_(10, 2, 24, TEX_2D, H_2D, 0, 2, 2)); STORE4();                    /* 28: gather B, an offset a texel */
    I(p, "TLD4S", TLD4S_(10, 2, 23, 12, H_2D, 0, 1, 3)); STORE4();                      /* 32: TLD4S A, offset */
    I(p, "TEX", TEX_(10, 60, RZ_, TEX_ARRAY_2D, 15, H_ARRAY, 0, 0, 1)); STORE4();        /* 36: TEX 2D array */
    I(p, "TLDS", TEXS_(10, 60, 6, 12, H_ARRAY, 4, 8)); STORE4();                        /* 40: TLDS 2D array */
    I(p, "TEX", TEX_(10, 61, 67, TEX_2D, 1, H_DEPTH, 1, 0, 1)); store(p, 10, j++);      /* 44: depth compare */
    I(p, "TLD4S", TLD4S_(10, 61, 67, 12, H_DEPTH, 1, 0, 0)); STORE4();                  /* 45: gather compare */
    I(p, "TEXS", TEXS_(10, 61, 67, RZ_, H_DEPTH, 0, 6)); store(p, 10, j++);             /* 49: TEXS compare */
    I(p, "TEX", TEX_(10, 64, RZ_, TEX_CUBE, 15, H_CUBE, 0, 0, 1)); STORE4();            /* 50: cube */
    I(p, "TLD4", TLD4_(10, 64, RZ_, TEX_CUBE, H_CUBE, 0, 0, 0)); STORE4();              /* 54: cube gather */
    I(p, "TLDS", TEXS_(10, 68, 70, 12, H_3D, 4, 7)); STORE4();                          /* 58: TLDS 3D */
    I(p, "TEXS", TEXS_(10, 2, RZ_, 12, H_1D, 4, 0)); STORE4();                          /* 62: TEXS 1D */
    I(p, "TLDS", TEXS_(10, 71, 7, RZ_, H_UINT, 0, 2)); store(p, 10, j++);               /* 66: integer bits */
    I(p, "TMML", D_(10) | A_(2) | B_(TEX_2D, 28, 3) | B_(3, 31, 4) | B_(H_2D / 4, 36, 13)); store(p, 10, j++);   /* 67 */
#undef STORE4
    exit_(p);
    return p;
}

/* A vertex and pixel pair drawing points: the vertex shader passes each vertex's attribute 1
 * (formats vary by draw) and its index to the pixel shader, which writes them (flat) to a uint
 * target, its position to a float one, and a depth; it discards vertex 3's pixel. */
static void build_render(void) {
    Prog *v = prog("vs_points", SHADER_VERTEX);
    hdr32(v, 0, 0x61 | 3 << 17);
    I(v, "ALD", D_(0) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    I(v, "ALD", D_(4) | A_(RZ_) | B_(0x90, 20, 10) | B_(3, 47, 2));
    I(v, "ALD", D_(8) | A_(RZ_) | B_(0x2FC, 20, 10) | B_(0, 47, 2));
    I(v, "AST", D_(0) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    I(v, "AST", D_(4) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    I(v, "AST", D_(8) | A_(RZ_) | B_(0x90, 20, 10) | B_(0, 47, 2));
    exit_(v);
    Prog *f = prog("ps_points", SHADER_PIXEL);
    f->pair = (int)(v - corpus);
    hdr32(f, 0, 0x62 | 3 << 17);
    f->hdr[0x18] = 0x55; f->hdr[0x19] = 0x55;      /* attributes 0, 1: flat */
    hdr32(f, 0x48, 0xFF);                          /* targets 0 and 1, all four components */
    hdr32(f, 0x4C, 2);                             /* and depth */
    for (u32 k = 0; k < 4; k++) I(f, "IPA", D_(k) | A_(RZ_) | BR_(RZ_) | B_((0x80 + 4 * k) / 4, 30, 8));
    for (u32 k = 0; k < 4; k++) I(f, "IPA", D_(4 + k) | A_(RZ_) | BR_(RZ_) | B_((0x70 + 4 * k) / 4, 30, 8));
    I(f, "IPA", D_(8) | A_(RZ_) | BR_(RZ_) | B_(0x90 / 4, 30, 8));
    I(f, "ISETP_imm", A_(8) | IMM_(3) | B_(2, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
    IG(f, 0, 0, "KIL", B_(15, 0, 5));
    I(f, "MOV32I", D_(9) | I32_(0x3E800000));
    exit_(f);
    /* a triangle over the target: position (flipped) and facing */
    Prog *t = prog("vs_triangle", SHADER_VERTEX);
    I(t, "ALD", D_(0) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    I(t, "AST", D_(0) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(t);
    Prog *q = prog("ps_position", SHADER_PIXEL);
    q->pair = (int)(t - corpus);
    hdr32(q, 0x48, 0xF);
    I(q, "IPA", D_(0) | A_(RZ_) | BR_(RZ_) | B_(0x70 / 4, 30, 8));
    I(q, "IPA", D_(1) | A_(RZ_) | BR_(RZ_) | B_(0x74 / 4, 30, 8));
    I(q, "IPA", D_(2) | A_(RZ_) | BR_(RZ_) | B_(0x78 / 4, 30, 8));
    I(q, "IPA", D_(3) | A_(RZ_) | BR_(RZ_) | B_(0x3FC / 4, 30, 8));
    exit_(q);
    /* two targets, the second blending as the first's second source */
    Prog *d = prog("ps_dual_source", SHADER_PIXEL);
    d->pair = (int)(t - corpus);
    hdr32(d, 0x48, 0xFF);
    static const u32 c[8] = {0x3F000000, 0x3E800000, 0x3F800000, 0x3F800000, 0x40000000, 0x40000000, 0x3F000000, 0x3F800000};
    for (u32 k = 0; k < 8; k++) I(d, "MOV32I", D_(k) | I32_(c[k]));
    exit_(d);
    /* blending with source 1 too, but writing target 0 alone (source 1: 0), or target 1 alone
     * (source 0: 0) - Metal needs both written to blend so */
    Prog *d0 = prog("ps_one_target", SHADER_PIXEL);
    d0->pair = (int)(t - corpus);
    hdr32(d0, 0x48, 0xF);
    for (u32 k = 0; k < 4; k++) I(d0, "MOV32I", D_(k) | I32_(c[k]));
    exit_(d0);
    Prog *d1 = prog("ps_target1_only", SHADER_PIXEL);
    d1->pair = (int)(t - corpus);
    hdr32(d1, 0x48, 0xF0);
    for (u32 k = 0; k < 4; k++) I(d1, "MOV32I", D_(k) | I32_(c[k]));
    exit_(d1);
    /* implicit-LOD samples over the 8x8 target (LOD 0) with the sampler's LOD bias: a texture's
     * colour to target 0, a compare with 0.5 to target 1 */
    Prog *b = prog("ps_lod_bias", SHADER_PIXEL);
    b->pair = (int)(t - corpus);
    hdr32(b, 0x48, 0xFF);
    I(b, "IPA", D_(8) | A_(RZ_) | BR_(RZ_) | B_(0x70 / 4, 30, 8));
    I(b, "IPA", D_(9) | A_(RZ_) | BR_(RZ_) | B_(0x74 / 4, 30, 8));
    I(b, "FMUL32I", D_(8) | A_(8) | I32_(0x3E000000));
    I(b, "FMUL32I", D_(9) | A_(9) | I32_(0x3E000000));
    I(b, "MOV32I", D_(10) | I32_(0x3F000000));
    I(b, "TEXS", TEXS_(0, 8, 9, 2, H_2D, 4, 1));
    I(b, "TEX", TEX_(4, 8, 10, TEX_2D, 1, H_DEPTH, 1, 0, 0));
    for (u32 k = 5; k < 8; k++) I(b, "MOV32I", D_(k) | I32_(0));
    exit_(b);
}

/* ---- programs that are only compiled ------------------------------------------------------ */
/* Every texture form: TEXS encodings, TEX on each type at each LOD mode with and without offsets
 * and compares, TLDS encodings, TLD4 offset modes, TLD4S, TMML. With `loop`, in a dispatch loop
 * (no derivatives: LOD 0), else straight. */
static void texture_forms(Prog *p, int loop) {
    if (loop) {
        I(p, "ISETP_imm", A_(1) | IMM_(0) | B_(2, 49, 3) | PPT_ | B_(6, 3, 3) | B_(7, 0, 3));
        jump(p, 6, 0, "BRA", "skip", 15);
    }
    /* (one texture a type, so that a type's every form shares a slot) */
#define HT_(type) (0x50 + 4 * (type))
    static const u8 texs_type[14] = {TEX_1D, TEX_2D, TEX_2D, TEX_2D, TEX_2D, TEX_2D, TEX_2D, TEX_ARRAY_2D, TEX_ARRAY_2D, TEX_ARRAY_2D,
                                     TEX_3D, TEX_3D, TEX_CUBE, TEX_CUBE};
    for (u32 i = 0; i < 14; i++) I(p, "TEXS", TEXS_(10, 2, 4, i & 1 ? RZ_ : 12, HT_(texs_type[i]), i & 7, i));
    static const u8 types[] = {TEX_1D, TEX_ARRAY_1D, TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_CUBE, TEX_ARRAY_CUBE};
    for (u32 t = 0; t < sizeof types; t++)
        for (u32 blod = 0; blod < 8; blod++) {
            if (blod == 4 || blod == 5) continue;
            for (u32 v = 0; v < 4; v++) {   /* offset, compare */
                u32 aoffi = v & 1, dc = v >> 1;
                if (dc && types[t] == TEX_3D) continue;   /* (no 3D depth textures) */
                I(p, "TEX", TEX_(10, 2, 8, types[t], 15, HT_(types[t]), dc, aoffi, blod));
            }
        }
    static const u8 tlds_enc[] = {0, 1, 2, 4, 5, 7, 8, 12}, tlds_type[] = {TEX_1D, TEX_1D, TEX_2D, TEX_2D, TEX_2D, TEX_3D, TEX_ARRAY_2D, TEX_2D};
    for (u32 i = 0; i < sizeof tlds_enc; i++) I(p, "TLDS", TEXS_(10, 2, 4, i & 1 ? RZ_ : 12, HT_(tlds_type[i]), i & 7, tlds_enc[i]));
    static const u8 gtypes[] = {TEX_2D, TEX_ARRAY_2D, TEX_CUBE, TEX_ARRAY_CUBE};
    for (u32 t = 0; t < sizeof gtypes; t++)
        for (u32 off = 0; off < 3; off++)
            for (u32 dc = 0; dc < 2; dc++) {
                if (off && gtypes[t] >= TEX_CUBE) continue;   /* (cube maps take no offsets) */
                I(p, "TLD4", TLD4_(10, 2, 8, gtypes[t], HT_(gtypes[t]), dc, off, (off + dc) & 3));
            }
    for (u32 v = 0; v < 4; v++) I(p, "TLD4S", TLD4S_(10, 2, 4, 12, HT_(TEX_2D), v >> 1, v & 1, v));
    static const u8 ltypes[] = {TEX_1D, TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_CUBE};
    for (u32 t = 0; t < sizeof ltypes; t++)
        I(p, "TMML", D_(10) | A_(2) | B_(ltypes[t], 28, 3) | B_(3, 31, 4) | B_(HT_(ltypes[t]) / 4, 36, 13));
#undef HT_
    if (loop) label(p, "skip");
}
static void build_compiled(void) {
    /* the shader program of tools: a vertex and pixel pair (the scratch programs of the port's study) */
    Prog *v = prog("vs_basic", SHADER_VERTEX);
    hdr32(v, 0, 0x61 | 3 << 17);
    I(v, "ALD", D_(0) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    I(v, "ALD", D_(4) | A_(RZ_) | B_(0x90, 20, 10) | B_(3, 47, 2));
    I(v, "LDC", D_(8) | A_(4) | B_(0x10, 20, 16) | B_(3, 36, 5) | B_(4, 48, 3));
    I(v, "AST", D_(0) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    I(v, "AST", D_(4) | A_(RZ_) | B_(0x80, 20, 10) | B_(3, 47, 2));
    jump(v, 7, 0, "BRA", "end", 15);
    label(v, "end");
    exit_(v);
    Prog *f = prog("ps_basic", SHADER_PIXEL);
    f->pair = (int)(v - corpus);
    hdr32(f, 0, 0x62 | 3 << 17);
    f->hdr[0x18] = 0xAA;
    hdr32(f, 0x48, 0xF);
    I(f, "IPA", D_(0) | A_(RZ_) | BR_(RZ_) | B_(0x80 / 4, 30, 8));
    I(f, "IPA", D_(1) | A_(RZ_) | BR_(RZ_) | B_(0x84 / 4, 30, 8));
    I(f, "TEXS", TEXS_(0, 0, 1, 2, 0x40, 4, 1));
    I(f, "TLD4", TLD4_(4, 0, 8, TEX_2D, 0x44, 0, 2, 1));
    IG(f, 0, 0, "KIL", B_(15, 0, 5));
    exit_(f);

    /* every texture form in a pixel shader, straight (derivatives) and in a dispatch loop */
    Prog *t = prog("ps_textures", SHADER_PIXEL);
    hdr32(t, 0x48, 0xF);
    texture_forms(t, 0);
    exit_(t);
    Prog *tl = prog("ps_textures_loop", SHADER_PIXEL);
    hdr32(tl, 0x48, 0xF);
    texture_forms(tl, 1);
    exit_(tl);
    /* in a vertex shader too, with every attribute in and out, and global memory */
    Prog *va = prog("vs_attributes", SHADER_VERTEX);
    for (u32 n = 0; n < 32; n++) {
        I(va, "ALD", D_(100) | A_(RZ_) | B_(0x80 + 16 * n, 20, 10) | B_(3, 47, 2));
        I(va, "AST", D_(100) | A_(RZ_) | B_(0x80 + 16 * n, 20, 10) | B_(3, 47, 2));
    }
    I(va, "ALD", D_(2) | A_(RZ_) | B_(0x2F8, 20, 10) | B_(1, 47, 2));     /* instance, vertex id */
    I(va, "AST", D_(2) | A_(RZ_) | B_(0x6C, 20, 10) | B_(0, 47, 2));      /* (point size: not forwarded) */
    I(va, "MOV_cbuf", D_(30) | CB_(0, SB_(0)));
    I(va, "MOV_cbuf", D_(31) | CB_(0, SB_(0) + 4));
    I(va, "LDG", D_(4) | A_(30) | B_(1, 45, 1) | B_(4, 48, 3));
    texture_forms(va, 0);
    I(va, "AST", D_(4) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(va);
    /* more textures than Metal has samplers */
    Prog *mt = prog("ps_many_textures", SHADER_PIXEL);
    hdr32(mt, 0x48, 0xF);
    for (u32 k = 0; k < 20; k++) I(mt, "TEXS", TEXS_(0, 0, 1, 2, 0x40 + 4 * k, 4, k & 1 ? 4 : 1));
    exit_(mt);
    /* eight targets and depth; inputs of every interpolation; global memory; IPA's forms */
    Prog *o8 = prog("ps_outputs", SHADER_PIXEL);
    o8->pair = (int)(va - corpus);
    hdr32(o8, 0x48, 0xFFFFFFFF);
    hdr32(o8, 0x4C, 2);
    for (u32 n = 0; n < 8; n++) o8->hdr[0x18 + n] = (u8)(n % 3 == 0 ? 0x55 : n % 3 == 1 ? 0xAA : 0xFF);
    I(o8, "IPA", D_(9) | A_(RZ_) | BR_(RZ_) | B_(0x7C / 4, 30, 8));
    for (u32 n = 0; n < 8; n++) {
        I(o8, "IPA", D_(4 * n) | A_(RZ_) | BR_(9) | B_((0x80 + 16 * n) / 4, 30, 8) | B_(n & 1, 54, 2) | B_(n >> 2 & 1, 51, 1));
        I(o8, "IPA", D_(4 * n + 1) | A_(RZ_) | BR_(RZ_) | B_((0x84 + 16 * n) / 4, 30, 8));
    }
    for (u32 sys = 0; sys < 6; sys++) {
        static const u32 at[6] = {0x70, 0x74, 0x78, 0x3FC, 0x2E0, 0x300};
        I(o8, "IPA", D_(40 + sys) | A_(RZ_) | BR_(RZ_) | B_(at[sys] / 4, 30, 8));
    }
    I(o8, "MOV_cbuf", D_(50) | CB_(0, SB_(0)));
    I(o8, "MOV_cbuf", D_(51) | CB_(0, SB_(0) + 4));
    I(o8, "LDG", D_(2) | A_(50) | B_(1, 45, 1) | B_(6, 48, 3));
    I(o8, "LDG", D_(6) | A_(50) | B_(1, 45, 1) | B_(1, 48, 3) | B_(0xFFFFFF, 20, 24));
    exit_(o8);
    Prog *nd = prog("ps_no_outputs", SHADER_PIXEL);
    IG(nd, 0, 1, "KIL", B_(15, 0, 5));
    exit_(nd);
    Prog *dz = prog("ps_depth_only", SHADER_PIXEL);
    hdr32(dz, 0x4C, 2);
    I(dz, "IPA", D_(1) | A_(RZ_) | BR_(RZ_) | B_(0x78 / 4, 30, 8));
    I(dz, "KIL", B_(1, 0, 5));   /* (a condition-code test) */
    exit_(dz);
    /* every flow test of a branch, nested SSY and PBK, in a vertex shader */
    Prog *cf = prog("vs_flow", SHADER_VERTEX);
    I(cf, "IADD_reg", D_(1) | A_(2) | BR_(3) | B_(1, 47, 1));
    for (u32 test = 0; test < 32; test++) {
        static char names[32][8];
        snprintf(names[test], sizeof names[test], "t%u", test);
        jump(cf, 7, 0, "BRA", names[test], test);
        I(cf, "IADD32I", D_(4) | A_(4) | I32_(test));
        label(cf, names[test]);
    }
    jump(cf, 7, 0, "SSY", "s_out", 15);
    jump(cf, 7, 0, "PBK", "p_out", 15);
    label(cf, "in");
    I(cf, "ISETP_imm", A_(4) | IMM_(100) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(1, 0, 3));
    IG(cf, 0, 0, "SYNC", B_(15, 0, 5));
    IG(cf, 1, 0, "BRK", B_(15, 0, 5));
    I(cf, "IADD32I", D_(4) | A_(4) | I32_(7));
    IG(cf, 0, 1, "EXIT", B_(15, 0, 5));
    jump(cf, 7, 0, "BRA", "in", 15);
    label(cf, "p_out");
    I(cf, "MOV32I", D_(5) | I32_(1));
    label(cf, "s_out");
    I(cf, "AST", D_(4) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(cf);
    /* indirect branches no table explains: every instruction a block */
    Prog *bx = prog("cs_brx_open", SHADER_COMPUTE);
    I(bx, "S2R", D_(0) | B_(0x21, 20, 8));
    I(bx, "SHL_imm", D_(0) | A_(0) | IMM_(3));
    I(bx, "BRX", A_(0) | B_(15, 0, 5));
    for (u32 k = 0; k < 8; k++) I(bx, "IADD32I", D_(2) | A_(2) | I32_(k));
    exit_(bx);
    /* a long dispatch loop (the compile time of a big program) */
    Prog *big = prog("cs_big", SHADER_COMPUTE);
    big->local[0] = 64; big->local[1] = 2;
    big->shared = 4096;
    for (u32 k = 0; k < 250; k++) {
        static char names[250][8];
        snprintf(names[k], sizeof names[k], "b%u", k);
        I(big, "XMAD_reg", D_(2 + k % 8) | A_(2 + (k + 1) % 8) | BR_(2 + (k + 3) % 8) | CR_(2 + (k + 5) % 8));
        I(big, "ISETP_imm", A_(2 + k % 8) | IMM_(k) | B_(1, 49, 3) | PPT_ | B_(0, 3, 3) | B_(7, 0, 3));
        jump(big, 0, 0, "BRA", names[k], 15);
        I(big, "STS", D_(2 + k % 8) | A_(2) | B_(4, 48, 3) | B_(4 * k, 20, 24));
        label(big, names[k]);
    }
    exit_(big);
    /* what the translator leaves out: each is a comment, and the program still compiles */
    Prog *un = prog("ps_unsupported", SHADER_PIXEL);
    hdr32(un, 0x48, 0x1);
    I(un, "TXQ", D_(0) | A_(1));
    I(un, "R2P_imm", A_(2) | IMM_(0x7F) | B_(1, 40, 1));
    I(un, "MUFU", D_(0) | A_(1) | B_(6, 20, 4));
    I(un, "LDC", D_(0) | A_(1) | B_(1, 44, 2));
    I(un, "STG", D_(0) | A_(4) | B_(1, 45, 1) | B_(4, 48, 3));
    I(un, "LDS", D_(0) | A_(4) | B_(4, 48, 3));
    I(un, "ALD", D_(0) | A_(RZ_) | B_(0x80, 20, 10));
    I(un, "AST", D_(0) | A_(RZ_) | B_(0x80, 20, 10));
    I(un, "TEX", TEX_(0, 2, 8, 5, 15, 0x40, 0, 0, 0));
    I(un, "TEXS", TEXS_(0, 2, 4, 12, 0x40, 0, 14));
    I(un, "TLDS", TEXS_(0, 2, 4, 12, 0x40, 0, 3));
    I(un, "TLD4", TLD4_(0, 2, 8, TEX_1D, 0x44, 0, 0, 0));
    I(un, "TMML", D_(0) | A_(2) | B_(TEX_ARRAY_CUBE, 28, 3) | B_(1, 31, 4) | B_(0x48 / 4, 36, 13));
    I(un, "ISETP_reg", A_(2) | BR_(3) | B_(1, 49, 3) | PPT_ | B_(1, 43, 1) | B_(0, 3, 3) | B_(7, 0, 3));   /* (.X: a plain compare) */
    exit_(un);
    Prog *uv = prog("vs_unsupported", SHADER_VERTEX);
    I(uv, "IPA", D_(0) | A_(RZ_) | BR_(RZ_) | B_(0x80 / 4, 30, 8));
    I(uv, "ALD", D_(0) | A_(5) | B_(0x80, 20, 10));
    I(uv, "S2R", D_(0) | B_(0x50, 20, 8));
    I(uv, "AST", D_(0) | A_(RZ_) | B_(0x70, 20, 10) | B_(3, 47, 2));
    exit_(uv);
}

static void build_corpus(void) {
    if (ncorpus) return;
    build_alu();
    build_flow();
    build_mem();
    build_tex();
    build_render();
    build_compiled();
    for (u32 i = 0; i < ncorpus; i++) assemble(&corpus[i]);
}

/* The corpus as SSBU_DUMP_SHADERS writes programs (msltest dir reads them). */
static int dump_corpus(const char *dir) {
    for (u32 i = 0; i < ncorpus; i++) {
        Prog *p = &corpus[i];
        char path[2048];
        u64 h = 1469598103934665603ull;   /* (FNV-1a, as the renderer names them) */
        for (u32 k = 0; k < p->size; k++) h = (h ^ p->bytes[k]) * 1099511628211ull;
        if (p->stage == SHADER_COMPUTE)
            snprintf(path, sizeof path, "%s/C_%016llx_%ux%ux%u_%u.bin", dir, (unsigned long long)h, p->local[0], p->local[1], p->local[2], p->shared);
        else snprintf(path, sizeof path, "%s/%u_%016llx.bin", dir, p->stage == SHADER_PIXEL ? 5 : 1, (unsigned long long)h);
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite(p->bytes, 1, p->size, f);
        fclose(f);
    }
    printf("%u programs written to %s\n", ncorpus, dir);
    return 0;
}

#ifndef MSLTEST_HLSL_ONLY
static Prog *find_prog(const char *name) {
    for (u32 i = 0; i < ncorpus; i++) if (!strcmp(corpus[i].name, name)) return &corpus[i];
    return NULL;
}
#endif
static const char *stage_name(int stage) { return stage == SHADER_VERTEX ? "vertex" : stage == SHADER_PIXEL ? "pixel" : "compute"; }

/* ---- HLSL: written, and compared with what an older translator wrote ----------------------- */
static char *hlsl_of(int form, Prog *p, u32 vs_outputs, ShaderInfo *info) {
#ifdef MSLTEST_HLSL_ONLY
    (void)form;
    return p->stage == SHADER_COMPUTE ? shader_hlsl_cs(p->bytes, p->size, p->local, p->shared, info, read_cbuf, p)
                                      : shader_hlsl_ex(p->bytes, p->size, p->stage, vs_outputs, info, read_cbuf, p);
#else
    if (form == 2)
        return p->stage == SHADER_COMPUTE ? shader_hlsl_cs(p->bytes, p->size, p->local, p->shared, info, read_cbuf, p)
                                          : shader_hlsl_ex(p->bytes, p->size, p->stage, vs_outputs, info, read_cbuf, p);
    return shader_translate(form ? SHADER_HLSL_VKD3D : SHADER_HLSL_FXC, p->bytes, p->size, p->stage, vs_outputs, p->local, p->shared, info,
                            read_cbuf, p);
#endif
}

static void put_info(FILE *f, const ShaderInfo *i) {
    fprintf(f, "info: stage %d cbufs %08x extents", i->stage, i->cbuf_mask);
    for (u32 b = 0; b < 18; b++) fprintf(f, " %x", i->cbuf_extent[b]);
    fprintf(f, "\ninfo: inputs %08x outputs %08x interp", i->inputs, i->outputs);
    for (u32 n = 0; n < 32; n++) fprintf(f, "%u", i->interp[n]);
    fprintf(f, " colours %02x depth %d kills %d stores %d\ninfo: %u textures", i->color_mask, i->writes_depth, i->kills, i->stores, i->ntex);
    for (u32 k = 0; k < i->ntex; k++) fprintf(f, " %x/%u/%u", i->tex[k].handle_offset, i->tex[k].type, i->tex[k].shadow);
    fprintf(f, "\ninfo: %u tables", i->ntables);
    for (u32 t = 0; t < i->ntables; t++) fprintf(f, " c%u+%x*%u", i->table[t].bank, i->table[t].offset, i->table[t].count);
    fprintf(f, ", %u storage buffers", i->sb_count);
    for (u32 k = 0; k < i->sb_count; k++) fprintf(f, " %x", i->sb_offset[k]);
    fprintf(f, "\ninfo: unsupported: %s\n", i->unsupported);
}

/* Random programs, the same every run: each instruction one of the instruction set's encodings
 * with random fields (now and then a random word), random headers, constant buffers (jump
 * tables), workgroups. Translated to HLSL only: they reach the translator's every path. */
#define NFUZZ 900
static u64 fuzz_state = 0x9E3779B97F4A7C15ull;
static u64 fuzz_next(void) {   /* xorshift64* */
    fuzz_state ^= fuzz_state >> 12; fuzz_state ^= fuzz_state << 25; fuzz_state ^= fuzz_state >> 27;
    return fuzz_state * 0x2545F4914F6CDD1Dull;
}
static void fuzz_program(Prog *p, u32 i) {
    memset(p, 0, sizeof *p);
    p->name = "fuzz";
    p->stage = (int)(i % 3);
    p->pair = -1;
    for (u32 k = 0; k < 0x50; k += 8) { u64 r = fuzz_next(); memcpy(p->hdr + k, &r, 8); }
    for (u32 b = 0; b < 18; b++) for (u32 k = 0; k < 128; k++) p->cb[b][k] = (u32)fuzz_next() & (k & 1 ? 0x3F8u : 0xFFFFFFFFu);
    p->local[0] = 1 + (u32)(fuzz_next() % 64); p->local[1] = 1 + (u32)(fuzz_next() % 4); p->local[2] = 1;
    p->shared = (u32)(fuzz_next() % 3) * 1024;
    u32 n = 4 + (u32)(fuzz_next() % 90);
    for (u32 k = 0; k < n; k++) {
        u64 r = fuzz_next(), w;
        if (r % 23 == 0) w = fuzz_next();
        else {
            const char *pat = ISA[r % NISA].pattern;
            u64 fixed = 0, mask = 0;
            for (int bit = 0; bit < 16; bit++) {
                fixed = fixed << 1 | (pat[bit] == '1');
                mask = mask << 1 | (pat[bit] != '-');
            }
            w = (fuzz_next() & ~(mask << 48)) | fixed << 48;
            if (fuzz_next() % 3) w = (w & ~B_(15, 16, 4)) | B_(7, 16, 3);   /* (mostly unguarded) */
            if (fuzz_next() % 4 == 0) w = (w & ~B_(0xFFFFFF, 20, 24)) | B_((u32)(fuzz_next() % 64) * 8 - 128, 20, 24);   /* (near branches) */
        }
        /* constant buffers 0..17, as the hardware has (the translator notes extents of 18
         * banks: one past them would be written outside ShaderInfo.cbuf_extent) */
        w = (w & ~B_(31, 34, 5)) | B_(BITS_(w, 34, 5) % 18, 34, 5);
        if ((w >> 48 & 0xFFF8) == (opcode("LDC") >> 48)) w = (w & ~B_(31, 36, 5)) | B_(BITS_(w, 36, 5) % 18, 36, 5);
        p->w[p->n] = w;
        p->op[p->n++] = "?";
    }
    p->w[p->n] = opcode("EXIT") | B_(7, 16, 3) | B_(15, 0, 5);
    p->op[p->n++] = "EXIT";
    assemble(p);
}

/* Every program in order (a pixel program after its vertex program's translation, for the
 * outputs it is keyed by), into a file: the same calls in the same order as any other build of
 * this, as HLSL names texture results by a count over all programs. Then the random ones. */
static int hlsl_write(int form, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    static Prog fz;
    fuzz_state = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < NFUZZ; i++) {
        fuzz_program(&fz, i);
        ShaderInfo info;
        u32 outs = (u32)fuzz_next();
        char *text = hlsl_of(form, &fz, outs, &info);
        fprintf(f, "==== fuzz %u (%s, vertex outputs %08x)\n", i, stage_name(fz.stage), outs);
        put_info(f, &info);
        fputs(text ? text : "(none)\n", f);
        free(text);
        free(fz.bytes);
    }
    for (u32 i = 0; i < ncorpus; i++) {
        Prog *p = &corpus[i];
        ShaderInfo info;
        u32 outs = 0xFFFFFFFFu;
        if (p->stage == SHADER_PIXEL && p->pair >= 0) {
            free(hlsl_of(form, &corpus[p->pair], 0, &info));
            outs = info.outputs;
        }
        char *text = hlsl_of(form, p, outs, &info);
        fprintf(f, "==== %s (%s, vertex outputs %08x)\n", p->name, stage_name(p->stage), outs);
        put_info(f, &info);
        fputs(text ? text : "(none)\n", f);
        free(text);
    }
    fclose(f);
    return 0;
}

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseeko(f, 0, SEEK_END);
    size_t size = (size_t)ftello(f);
    fseeko(f, 0, SEEK_SET);
    char *s = malloc(size + 1);
    size_t got = fread(s, 1, size, f);
    s[got] = 0;
    fclose(f);
    if (n) *n = got;
    return s;
}

static int hlsl_diff(int form, const char *baseline) {
    char now[1024];
    snprintf(now, sizeof now, "%s.now", baseline);
    if (hlsl_write(form, now)) return 1;
    size_t na, nb;
    char *a = slurp(baseline, &na), *b = slurp(now, &nb);
    if (!a || !b) { fprintf(stderr, "cannot read %s\n", a ? now : baseline); return 1; }
    if (na == nb && !memcmp(a, b, na)) {
        printf("HLSL (%s): %u programs and %u random ones, %zu bytes, identical to %s\n", form == 0 ? "fxc" : form == 1 ? "vkd3d" : "api", ncorpus,
               NFUZZ, na, baseline);
        remove(now);
        return 0;
    }
    size_t at = 0, line = 1, start = 0;
    while (at < na && at < nb && a[at] == b[at]) { if (a[at] == '\n') { line++; start = at + 1; } at++; }
    const char *prog_name = "?";
    for (size_t k = start; k > 0; k--)
        if (!strncmp(a + k, "==== ", 5) && (k == 0 || a[k - 1] == '\n')) { prog_name = a + k; break; }
    printf("HLSL (%s) DIFFERS from %s (now in %s) at line %zu, in %.60s\n  was: %.200s\n  now: %.200s\n", form == 0 ? "fxc" : form == 1 ? "vkd3d" : "api",
           baseline, now, line, prog_name, a + start, b + start);
    return 1;
}

#ifndef MSLTEST_HLSL_ONLY
/* ---- Metal ------------------------------------------------------------------------------------ */
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static int verbose;

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

/* As the renderer must compile: safe math (Metal's fast math breaks NaN compares), invariant
 * positions. floor14: also as on macOS 14 (MSL 3.1, which has no mathMode). */
static MTLCompileOptions *options(int floor14) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.preserveInvariance = YES;
    if (floor14) {
        o.languageVersion = MTLLanguageVersion3_1;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    } else if (@available(macOS 15.0, *)) o.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return o;
}

static id<MTLLibrary> compile(const char *src, int floor14, double *ms, NSString **error) {
    NSError *err = nil;
    double t0 = now_ms();
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:options(floor14) error:&err];
    if (ms) *ms = now_ms() - t0;
    if (error) *error = lib ? nil : err.localizedDescription;
    else if (!lib) printf("  compile: %s\n", err.localizedDescription.UTF8String);
    return lib;
}

/* ps_main, specialised for the render targets' kinds (2 bits each: 0 float, 1 uint, 2 int) */
static id<MTLFunction> function(id<MTLLibrary> lib, int stage, u32 rt_types) {
    if (stage != SHADER_PIXEL) return [lib newFunctionWithName:stage == SHADER_VERTEX ? @"vs_main" : @"cs_main"];
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    [cv setConstantValue:&rt_types type:MTLDataTypeUInt atIndex:SHADER_MSL_RT_TYPES];
    NSError *err = nil;
    id<MTLFunction> f = [lib newFunctionWithName:@"ps_main" constantValues:cv error:&err];
    if (!f) printf("  specialise: %s\n", err.localizedDescription.UTF8String);
    return f;
}

static char *msl_of(Prog *p, u32 vs_outputs, ShaderInfo *info) {
    return shader_translate(SHADER_MSL, p->bytes, p->size, p->stage, vs_outputs, p->local, p->shared, info, read_cbuf, p);
}
static u32 outputs_of(Prog *p) {
    if (p->stage != SHADER_PIXEL || p->pair < 0) return 0xFFFFFFFFu;
    ShaderInfo info;
    free(msl_of(&corpus[p->pair], 0, &info));
    return info.outputs;
}

/* Did every instruction decode as the one assembled? (the text notes each: "// addr: word ... NAME") */
static int check_decoding(const Prog *p, const char *text) {
    int bad = 0;
    for (u32 i = 0; i < p->n; i++) {
        char key[64];
        snprintf(key, sizeof key, "// %04x: %016llx ", addr_of(i), (unsigned long long)p->w[i]);
        const char *at = strstr(text, key);
        if (!at) {
            if (!strcmp(p->op[i], "EXIT") && i + 1 == p->n) continue;   /* (a straight program's final EXIT is its end) */
            printf("  %s: %04x %s not in the text\n", p->name, addr_of(i), p->op[i]);
            bad = 1;
            continue;
        }
        const char *end = strchr(at, '\n'), *name = at + strlen(key);
        name = strchr(name, ' ') + 1;   /* (after the guard) */
        size_t len = strcspn(name, " \n");
        if (len != strlen(p->op[i]) || strncmp(name, p->op[i], len) || name > end) {
            printf("  %s: %04x decoded as %.*s, not %s\n", p->name, addr_of(i), (int)len, name, p->op[i]);
            bad = 1;
        }
    }
    return bad;
}

/* The corpus: translated, every instruction decoded as assembled, compiled (and as for macOS 14),
 * specialised, made into pipelines - a compute pipeline taking its workgroup, a render pipeline
 * of each pixel program with its vertex program (the interface must match). */
static int run_corpus(void) {
    int failed = 0;
    u32 used[NISA] = {0};
    double total = 0;
    for (u32 i = 0; i < ncorpus; i++) {
        Prog *p = &corpus[i];
        for (u32 k = 0; k < p->n; k++)
            for (u32 s = 0; s < NISA; s++) if (!strcmp(p->op[k], ISA[s].name)) used[s]++;
        ShaderInfo info;
        u32 outs = outputs_of(p);
        double t0 = now_ms();
        char *text = msl_of(p, outs, &info);
        double translate_ms = now_ms() - t0;
        int bad = check_decoding(p, text);
        ShaderInfo hinfo;   /* (what the translator tells of a program does not depend on the language) */
        free(shader_translate(SHADER_HLSL_VKD3D, p->bytes, p->size, p->stage, outs, p->local, p->shared, &hinfo, read_cbuf, p));
        if (memcmp(&hinfo, &info, sizeof info)) { printf("  %s: ShaderInfo differs from HLSL's\n", p->name); bad = 1; }
        double ms = 0, ms14 = 0;
        NSString *err = nil, *err14 = nil;
        id<MTLLibrary> lib = compile(text, 0, &ms, &err);
        id<MTLLibrary> lib14 = compile(text, 1, &ms14, &err14);
        total += ms;
        const char *what = "";
        if (!lib || !lib14) { bad = 1; what = "compile"; }
        else {
            double t1 = now_ms();
            id<MTLFunction> fn = function(lib, p->stage, 0);
            if (verbose && p->stage == SHADER_PIXEL) printf("  %s: ps_main specialised in %.2f ms\n", p->name, now_ms() - t1);
            if (!fn) { bad = 1; what = "function"; }
            else if (p->stage == SHADER_COMPUTE) {
                NSError *e = nil;
                id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:fn error:&e];
                u32 want = p->local[0] * p->local[1] * p->local[2];
                if (!cps || cps.maxTotalThreadsPerThreadgroup < want) {
                    bad = 1; what = "pipeline";
                    printf("  %s: %s\n", p->name, cps ? "takes fewer threads than its workgroup" : e.localizedDescription.UTF8String);
                }
            } else if (p->stage == SHADER_PIXEL && p->pair >= 0) {
                ShaderInfo vinfo;
                char *vtext = msl_of(&corpus[p->pair], 0, &vinfo);
                id<MTLLibrary> vlib = compile(vtext, 0, NULL, NULL);
                free(vtext);
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [vlib newFunctionWithName:@"vs_main"];
                d.fragmentFunction = fn;
                for (u32 rt = 0; rt < 8; rt++)
                    if (info.color_mask >> rt & 1) d.colorAttachments[rt].pixelFormat = MTLPixelFormatRGBA16Float;
                if (info.writes_depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                NSError *e = nil;
                if (![dev newRenderPipelineStateWithDescriptor:d error:&e]) {
                    bad = 1; what = "pipeline";
                    printf("  %s with %s: %s\n", p->name, corpus[p->pair].name, e.localizedDescription.UTF8String);
                }
            }
        }
        printf("%-20s %-8s %4u instructions, %6zu bytes of MSL, %5.2f ms to translate, %6.1f ms to compile%s%s%s%s\n", p->name, stage_name(p->stage),
               p->n, strlen(text), translate_ms, ms, bad ? " FAILED " : "", what, info.unsupported[0] ? "; leaves out: " : "", info.unsupported);
        if (err || err14) printf("  %s\n", (err ? err : err14).UTF8String);
        if (bad && verbose) printf("%s\n", text);
        failed |= bad;
        free(text);
    }
    /* the same text when translated on many threads at once (the renderer's compiler threads) */
    __block int differ = 0;
    char **texts = calloc(ncorpus, sizeof *texts);
    u32 *outs = calloc(ncorpus, sizeof *outs);
    for (u32 i = 0; i < ncorpus; i++) {
        ShaderInfo info;
        outs[i] = outputs_of(&corpus[i]);
        texts[i] = msl_of(&corpus[i], outs[i], &info);
    }
    double t0 = now_ms();
    dispatch_apply(ncorpus * 16, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t n) {
        ShaderInfo info;
        u32 i = (u32)(n % ncorpus);
        char *text = msl_of(&corpus[i], outs[i], &info);
        if (strcmp(text, texts[i])) __atomic_store_n(&differ, 1, __ATOMIC_RELAXED);
        free(text);
    });
    printf("translated %u times on several threads (%.0f ms): %s\n", ncorpus * 16, now_ms() - t0, differ ? "TEXT DIFFERS" : "the same text");
    failed |= differ;
    for (u32 i = 0; i < ncorpus; i++) free(texts[i]);
    free(texts);
    free(outs);
    int missing = 0;
    for (u32 s = 0; s < NISA; s++)
        if (!used[s]) { if (!missing++) printf("not in the corpus:"); printf(" %s", ISA[s].name); }
    if (missing) printf("\n");
    printf("corpus: %u programs, every one of the %u encodings%s, %.0f ms compiling: %s\n", ncorpus, (u32)NISA, missing ? " but those" : "",
           total, failed ? "FAILED" : "ok");
    return failed;
}

/* ---- running programs ---------------------------------------------------------------------- */
static id<MTLBuffer> buffer(const void *data, u32 size) {
    id<MTLBuffer> b = [dev newBufferWithLength:size ? size : 16 options:MTLResourceStorageModeShared];
    if (data) memcpy(b.contents, data, size);
    return b;
}

static int checks, failures;
static void check(const char *what, int index, u32 got, u32 want, int ok) {
    checks++;
    if (ok && !verbose) return;
    if (!ok) failures++;
    if (!ok && failures > 60 && !verbose) return;
    printf("  %s %-32s [%2d] got %08x want %08x%s\n", ok ? "  " : "!!", what, index, got, want, ok ? "" : "  MISMATCH");
}
static int same_float(u32 got, u32 want, int match) {
    float g = F32(got), w = F32(want);
    if (got == want || (isnan(g) && isnan(w))) return 1;
    if (match == ANY_ZERO) return fabsf(g) < FLT_MIN && fabsf(w) < FLT_MIN;   /* (zeros and denormals: either) */
    if (match == NEAR_ULPS) return !isnan(g) && !isnan(w) && (i64)got - (i64)want <= 4 && (i64)want - (i64)got <= 4 && (got ^ want) >> 31 == 0;
    if (match == NEAR_2E6) return fabsf(g - w) <= 2e-6f * fmaxf(1.0f, fabsf(w)) || (isinf(g) && g == w);
    if (match == SAME_HALVES) {
        for (int h = 0; h < 2; h++) {
            u32 a = got >> (16 * h) & 0xFFFF, b = want >> (16 * h) & 0xFFFF;
            if (a != b && !(isnan(h2f(a)) && isnan(h2f(b)))) return 0;
        }
        return 1;
    }
    return 0;
}

/* A compute program on the GPU, as the Metal renderer would run it: constant buffers trimmed to
 * what the program reads; storage buffers found by the descriptors in c[0] (0: in, 1: out);
 * textures by their handle's offset. */
typedef struct Kernel {
    Prog *p;
    u32 groups;
    const void *in; u32 in_size;
    void *out; u32 out_size;
    u32 cb_size[18];
    u32 handles[8]; id<MTLTexture> tex[8]; id<MTLSamplerState> smp[8];
    u32 lod_bias[8];   /* (the sampler's, as the renderer passes it before macOS 26: 13 bits, 1/256ths) */
    u32 scale[8];      /* (a render target's render scale, 4.12, as the renderer passes it: 0 for 1) */
    u32 ntex;
} Kernel;

#define VA_IN 0x510000000ull
#define VA_OUT 0x520000000ull
static int run_kernel(Kernel *k) {
    Prog *p = k->p;
    u32 desc[2][4] = {{(u32)VA_IN, (u32)(VA_IN >> 32), k->in_size, 0}, {(u32)VA_OUT, (u32)(VA_OUT >> 32), k->out_size, 0}};
    memcpy(&p->cb[0][SB_(0) / 4], desc, sizeof desc);
    if (!k->cb_size[0]) k->cb_size[0] = SB_(2);
    ShaderInfo info;
    char *text = msl_of(p, 0, &info);
    id<MTLLibrary> lib = compile(text, 0, NULL, NULL);
    if (!lib) { printf("%s\n", text); free(text); return 1; }
    free(text);
    NSError *e = nil;
    id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:function(lib, SHADER_COMPUTE, 0) error:&e];
    if (!cps) { printf("  pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    DrawParams params;
    ShaderBinds binds;
    memset(&params, 0, sizeof params);
    params.base[2] = params.base[3] = 0x3F800000u;   /* 1 / the render scale (ps_input): 1, as the renderer's */
    memset(&binds, 0, sizeof binds);
    NSMutableArray *keep = [NSMutableArray array];
    for (u32 b = 0; b < 18; b++) {
        if (!(info.cbuf_mask >> b & 1)) continue;
        u32 size = k->cb_size[b] < info.cbuf_extent[b] ? k->cb_size[b] : info.cbuf_extent[b];
        id<MTLBuffer> buf = buffer(p->cb[b], sizeof p->cb[b]);
        [keep addObject:buf];
        binds.cb[b] = (ShaderBind){buf.gpuAddress, size, 0};
    }
    id<MTLBuffer> in = buffer(k->in, k->in_size), out = buffer(k->out, k->out_size);
    for (u32 s = 0; s < info.sb_count; s++) {
        const u32 *d = &p->cb[0][info.sb_offset[s] / 4];
        u64 va = (u64)d[1] << 32 | d[0];
        id<MTLBuffer> buf = va == VA_IN ? in : va == VA_OUT ? out : nil;
        params.sb[0][s][0] = d[0]; params.sb[0][s][1] = d[1]; params.sb[0][s][2] = d[2];
        if (buf) binds.gm[s] = (ShaderBind){buf.gpuAddress, d[2], 0};
    }
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:cps];
    [enc setBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
    [enc setBytes:&binds length:shader_binds_size(SHADER_COMPUTE) atIndex:SHADER_MSL_BINDS];
    for (id<MTLBuffer> b in keep) [enc useResource:b usage:MTLResourceUsageRead];
    [enc useResource:in usage:MTLResourceUsageRead];
    [enc useResource:out usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (u32 t = 0; t < info.ntex; t++) {
        u32 h = 0;
        while (h < k->ntex && k->handles[h] != info.tex[t].handle_offset) h++;
        if (h == k->ntex) { printf("  no texture for handle %#x\n", info.tex[t].handle_offset); return 1; }
        params.swz[1][t] = 2 | 3 << 3 | 4 << 6 | 5 << 9 | (k->lod_bias[h] & 0x1FFF) << 12;   /* (x, y, z, w) */
        params.sb[1][t >> 1][3] |= (k->scale[h] & 0xFFFF) << ((t & 1) * 16);
        [enc setTexture:k->tex[h] atIndex:t];
        if (t < shader_msl_samplers(&info)) [enc setSamplerState:k->smp[h] atIndex:t];
    }
    [enc setBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
    [enc dispatchThreadgroups:MTLSizeMake(k->groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(p->local[0], p->local[1], p->local[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { printf("  %s: %s\n", p->name, cb.error.localizedDescription.UTF8String); return 1; }
    memcpy(k->out, out.contents, k->out_size);
    return 0;
}

/* thread inputs: integers' edges, floats' (NaN, infinities, denormals, halves), mixed */
static const u32 IN_A[32] = {0x00000000, 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x80000001, 0x0000001F, 0x00000020,
                             0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000, 0x00FF00FF, 0x3F800000, 0xBF800000, 0x40200000,
                             0xC0200000, 0x40600000, 0x7FC00000, 0x7F800000, 0xFF800000, 0x00000003, 0x4F000000, 0x4F800000,
                             0xCF000001, 0x3EAAAAAB, 0x447A0000, 0x3C003C00, 0xC000BC00, 0x7BFF3555, 0x3F000000, 0xBF000000};
static const u32 IN_B[32] = {0x00000000, 0x00000005, 0x0000001F, 0x00000020, 0x00000021, 0xFFFFFFFF, 0x80000000, 0x00000010,
                             0x00000808, 0x00002000, 0x0000081C, 0x00000820, 0x00000C04, 0x3F800000, 0x40000000, 0x7FC00000,
                             0xBF800000, 0x40200000, 0x3F800000, 0x7F800000, 0x7F800000, 0x00000004, 0x4F000000, 0x00000001,
                             0x00001810, 0x3EAAAAAB, 0xC47A0000, 0x40003C00, 0x3C00C000, 0x7C00FC00, 0x00000000, 0x3F000000};
static const u32 IN_C[32] = {0x00000000, 0xFFFFFFFF, 0x00010000, 0x12345678, 0x80000000, 0x7FFFFFFF, 0x00000001, 0xFFFF8000,
                             0x55555555, 0x0F0F0F0F, 0x3F800000, 0xBF800000, 0x40490FDB, 0x7FC00000, 0x00800000, 0x00000007,
                             0x3C003C00, 0x00000002, 0xC1200000, 0x41200000, 0x7F7FFFFF, 0x00000003, 0x80000000, 0x00000100,
                             0x0000FFFF, 0x3F000000, 0xDEADBEEF, 0x3C00BC00, 0x40004000, 0x00000000, 0x12345678, 0x87654321};

static int gpu_alu(void) {
    Prog *p = find_prog("cs_alu");
    static u32 in[64 * 4], out[32 * 256];
    for (u32 t = 0; t < 32; t++) { in[4 * t] = IN_A[t]; in[4 * t + 1] = IN_B[t]; in[4 * t + 2] = IN_C[t]; in[4 * t + 3] = t; }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 1, .in = in, .in_size = 32 * 16, .out = out, .out_size = sizeof out};
    if (run_kernel(&k)) return 1;
    int before = failures;
    for (u32 c = 0; c < nalu; c++)
        for (u32 t = 0; t < 32; t++) {
            u32 a = IN_A[t], b = IN_B[t], cc = IN_C[t], got = out[t * 256 + c], want = 0;
            int ok = 0;
            for (ftz_mode = 0; ftz_mode < 2 && !ok; ftz_mode++) {   /* (denormals kept, then flushed) */
                dontcare = 0;
                if (alu[c].match >= 100) {
                    want = fcmp((u32)alu[c].match - 100, FA, FB) ? ~0u : 0u;
                    ok = got == want;
                } else {
                    u32 w = alu[c].model(a, b, cc);
                    if (!ftz_mode) want = w;
                    ok = alu[c].match == SAME_BITS ? got == w : same_float(got, w, alu[c].match);
                    if (dontcare) ok = 1;
                }
            }
            ftz_mode = 0;
            check(alu[c].what, (int)t, got, want, ok);
        }
    printf("gpu cs_alu: %u checks of 32 threads each: %s\n", nalu, failures == before ? "ok" : "FAILED");
    return failures != before;
}

static int gpu_flow(void) {
    Prog *p = find_prog("cs_flow");
    static u32 in[32 * 4], out[32 * 16];
    for (u32 t = 0; t < 32; t++) { in[4 * t] = IN_A[t]; in[4 * t + 1] = IN_B[t]; in[4 * t + 2] = IN_C[t] ^ t; in[4 * t + 3] = 0; }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 1, .in = in, .in_size = sizeof in, .out = out, .out_size = sizeof out, .cb_size = {[2] = 256}};
    if (run_kernel(&k)) return 1;
    int before = failures;
    for (u32 t = 0; t < 32; t++) {
        u32 a = in[4 * t], b = in[4 * t + 1], c = in[4 * t + 2], n = a & 15, cx = c & 3;
        u32 want[5] = {n * (n + 1) / 2, a & 1 ? 1u : 2u, b & 7, 100 + cx, a + b < a ? 1u : 2u};
        static const char *const what[5] = {"loop", "SSY/SYNC", "PBK/BRK", "BRX jump table", "branch on carry"};
        for (u32 i = 0; i < 5; i++) {
            u32 w = t == 31 ? 0xCDCDCDCDu : want[i];
            check(t == 31 ? "(left first: nothing written)" : what[i], (int)t, out[t * 16 + i], w, out[t * 16 + i] == w);
        }
    }
    printf("gpu cs_flow: branches, 32 threads: %s\n", failures == before ? "ok" : "FAILED");
    return failures != before;
}

static u32 rd(const u8 *m, u32 size, u32 off, u32 bytes) {   /* little-endian, 0 past the end */
    u32 v = 0;
    for (u32 i = 0; i < bytes; i++) v |= (off + i < size ? m[off + i] : 0u) << (8 * i);
    return v;
}
static u32 sext(u32 v, u32 bits) { return (u32)((i32)(v << (32 - bits)) >> (32 - bits)); }

static int gpu_mem(void) {
    Prog *p = find_prog("cs_mem");
    static u32 in[64 * 4], out[64 * 64];
    for (u32 t = 0; t < 64; t++) {
        in[4 * t] = IN_A[t & 31] ^ (t >> 5) * 0x5A5A5A5Au;
        in[4 * t + 1] = IN_B[(t * 7) & 31] + t;
        in[4 * t + 2] = IN_C[t & 31] ^ (t * 0x01010101u);
        in[4 * t + 3] = t * 0x11111111u;
    }
    memset(out, 0xCD, sizeof out);
    Kernel k = {.p = p, .groups = 2, .in = in, .in_size = sizeof in, .out = out, .out_size = sizeof out, .cb_size = {[3] = 64}};
    if (run_kernel(&k)) return 1;
    int before = failures;
    const u8 *cb3 = cb3_bytes(), *m = (const u8 *)in;
    for (u32 t = 0; t < 64; t++) {
        u32 a = in[4 * t], b = in[4 * t + 1], c = in[4 * t + 2], d = in[4 * t + 3], want[54];
        const u32 *got = out + t * 64;
        u32 idx = a & 0x7F, w = idx < 64 ? rd(cb3, 64, idx & ~3u, 4) : 0, sh = (idx & 3) * 8;
        want[0] = (w >> sh) & 0xFF;
        want[1] = sext((w >> sh) & 0xFF, 8);
        want[2] = (w >> sh) & 0xFFFF;
        want[3] = sext((w >> sh) & 0xFFFF, 16);
        want[4] = w;
        want[5] = idx < 64 ? rd(cb3, 64, idx & ~7u, 4) : 0;
        want[6] = (idx & ~7u) + 4 < 64 ? rd(cb3, 64, (idx & ~7u) + 4, 4) : 0;
        want[7] = rd(cb3, 64, 0x10, 4);
        u32 back = idx - 4;
        want[8] = back < 64 ? rd(cb3, 64, back & ~3u, 4) : 0;
        u32 off = b & 0x3F, gw = rd(m, sizeof in, off & ~3u, 4), gsh = (off & 3) * 8;
        want[9] = (gw >> gsh) & 0xFF;
        want[10] = sext((gw >> gsh) & 0xFF, 8);
        want[11] = (gw >> gsh) & 0xFFFF;
        want[12] = sext((gw >> gsh) & 0xFFFF, 16);
        want[13] = rd(m, sizeof in, off & ~3u, 4);
        want[14] = want[13];
        want[15] = rd(m, sizeof in, (off & ~3u) + 4, 4);
        want[16] = 0;   /* past storage buffer 0 */
        want[17] = 0;   /* another high word: no storage buffer */
        for (u32 i = 0; i < 4; i++) want[18 + i] = t < 63 ? in[4 * (t + 1) + i] : 0;
        u32 bsh = (t & 3) * 8;
        want[22] = (0xAAAAAAAAu & ~(0xFFu << bsh)) | ((b & 0xFF) << bsh);
        want[23] = (0xAAAAAAAAu & 0xFFFF) | (c & 0xFFFF) << 16;
        want[24] = a; want[25] = b;
        for (u32 i = 26; i < 28; i++) want[i] = 0xCDCDCDCDu;
        want[28] = a; want[29] = b; want[30] = c; want[31] = d;
        u32 s0 = (a & ~0xFF00u) | (b & 0xFF) << 8;
        want[32] = s0;
        want[33] = sext(c & 0xFFFF, 16);
        want[34] = s0 >> 24;
        want[35] = sext(b & 0xFF, 8);
        want[36] = (d >> 8 & 0xFF) | (c & 0xFF) << 8;   /* (bytes 5 and 6: d's, then c's) */
        want[37] = a; want[38] = b;
        want[39] = a; want[40] = b; want[41] = c; want[42] = d;
        want[43] = (c & ~0xFF00u) | (a & 0xFF) << 8;
        want[44] = sext(a & 0xFF, 8);
        want[45] = a; want[46] = b;
        want[47] = t >> 5;
        want[48] = 32 | 1 << 16 | 1 << 26;
        want[49] = 0;
        want[50] = 0x3F800000;
        want[51] = t & 31;
        want[52] = t & 31;
        want[53] = 0;
        static const char *const what[54] = {"LDC.U8", "LDC.S8", "LDC.U16", "LDC.S16", "LDC.32", "LDC.64 lo", "LDC.64 hi", "LDC c[3][0x10]",
            "LDC [r - 4]", "LDG.U8", "LDG.S8", "LDG.U16", "LDG.S16", "LDG.32", "LDG.64 lo", "LDG.64 hi", "LDG past the buffer", "LDG other high word",
            "LDG.128 x", "LDG.128 y", "LDG.128 z", "LDG.128 w", "STG.U8", "STG.U16", "STG.64 lo", "STG.64 hi", "(unwritten)", "(unwritten)",
            "STG.128 x", "STG.128 y", "STG.128 z", "STG.128 w", "STS, STS.U8, LDS", "STS.U16, LDS.S16", "LDS.U8", "LDS.S8", "LDS.U16",
            "STS.64, LDS.64 lo", "LDS.64 hi", "STS.128, LDS.128 x", "LDS.128 y", "LDS.128 z", "LDS.128 w", "STL, STL.U8, LDL", "LDL.S8",
            "STL.64, LDL.64 lo", "LDL.64 hi", "S2R CTAID.X", "S2R local size", "S2R TID.Y", "S2R SR18", "S2R TID", "S2R lane", "S2R (unknown)"};
        for (u32 i = 0; i < 54; i++) {
            u32 wv = t == 63 ? 0xCDCDCDCDu : want[i];
            check(t == 63 ? "(left first: nothing written)" : what[i], (int)t, got[i], wv, got[i] == wv);
        }
    }
    printf("gpu cs_mem: memory, 2 workgroups of 32: %s\n", failures == before ? "ok" : "FAILED");
    return failures != before;
}

/* ---- textures ---------------------------------------------------------------------------- */
static id<MTLTexture> texture(MTLTextureType type, MTLPixelFormat format, u32 w, u32 h, u32 d, u32 layers, u32 mips) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = type; td.pixelFormat = format;
    td.width = w; td.height = h; td.depth = d; td.arrayLength = layers; td.mipmapLevelCount = mips;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.storageMode = format == MTLPixelFormatDepth32Float ? MTLStorageModePrivate : MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}
typedef struct V4 { float x, y, z, w; } V4;
static const V4 Z4;
static V4 t2d(u32 x, u32 y, u32 m) { return (V4){(float)(x + 8 * y + 100 * m), 0.5f + (float)x, 0.25f + (float)y, (float)m + 1.0f}; }
static V4 tarr(u32 x, u32 y, u32 l) { return (V4){(float)(x + 4 * y + 1000 * l), (float)l, 7, 8}; }
static float tdepth(u32 x, u32 y) { return (float)(x + 4 * y) / 16.0f; }
static float tdepth1(u32 x, u32 y) { return 1.0f - (float)(x + 2 * y) / 4.0f; }   /* (its mip 1, 2x2) */
static V4 tcube(u32 f) { return (V4){(float)f, (float)(2 * f), (float)(3 * f), 9}; }
static V4 t3d(u32 x, u32 y, u32 z) { return (V4){(float)x, (float)y, (float)z, (float)(x + 4 * y + 16 * z)}; }
static V4 t1d(u32 x) { return (V4){(float)(3 * x), 1, 2, 3}; }
static u32 tuint(u32 x, u32 y) { static const u32 v[4] = {5, 0x7FC00001u, 0x80000001u, 0x00800000u}; return v[x & 3] ^ (y * 0x01000000u); }
static float comp(V4 v, u32 c) { return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w; }
static i32 clampi(i32 v, i32 lo, i32 hi) { return v < lo ? lo : v > hi ? hi : v; }
/* a gather on the 2D texture's level 0 at texel (x, y)'s centre, offset (ox, oy), clamping */
static V4 gather2d(i32 x, i32 y, i32 ox, i32 oy, u32 c) {
    i32 i0 = clampi(x + ox, 0, 7), i1 = clampi(x + ox + 1, 0, 7), j0 = clampi(y + oy, 0, 7), j1 = clampi(y + oy + 1, 0, 7);
    return (V4){comp(t2d((u32)i0, (u32)j1, 0), c), comp(t2d((u32)i1, (u32)j1, 0), c), comp(t2d((u32)i1, (u32)j0, 0), c), comp(t2d((u32)i0, (u32)j0, 0), c)};
}

static int gpu_tex(void) {
    Prog *p = find_prog("cs_tex");
    enum { T = 16 };
    static u32 in[T * 32], out[T * 128];
    for (u32 t = 0; t < T; t++) {
        u32 *r = in + t * 32, tx = t & 7, ty = t >> 3;
        float *f = (float *)r;
        f[0] = ((float)tx + 0.5f) / 8; f[1] = ((float)ty + 0.5f) / 8; f[2] = 1.0f;
        r[4] = t - 2; r[5] = ty; r[6] = t & 1; r[7] = t & 3;
        i32 ox = (i32)(t % 5) - 2, oy = (i32)(t % 3) - 1, ox6 = 3 * (i32)t - 20, oy6 = 7 - (i32)t;
        r[8] = ((u32)ox & 15) | ((u32)oy & 15) << 4;
        r[9] = ((u32)ox6 & 63) | ((u32)oy6 & 63) << 8;
        for (u32 i = 0; i < 4; i++) {
            i32 px = (i32)((t + i * 5) % 23) - 11, py = (i32)((t * 3 + i) % 19) - 9;
            r[10 + i / 2] |= (((u32)px & 63) | ((u32)py & 63) << 8) << (16 * (i & 1));
        }
        r[12] = t % 4; f[13] = ((float)(t & 3) + 0.5f) / 4; f[14] = ((float)((t >> 2) & 3) + 0.5f) / 4;
        static const float dir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        f[16] = dir[t % 6][0]; f[17] = dir[t % 6][1]; f[18] = dir[t % 6][2];
        f[19] = ((float)((t * 5) % 16) + 0.5f) / 16;
        r[20] = t & 3; r[21] = (t >> 2) & 3; r[22] = t & 1 ? 1 : 5; r[23] = (t - 2) & 3;
    }
    memset(out, 0xCD, sizeof out);
    /* the textures */
    id<MTLTexture> t2 = texture(MTLTextureType2D, MTLPixelFormatRGBA32Float, 8, 8, 1, 1, 2);
    for (u32 m = 0; m < 2; m++) {
        V4 px[64];
        u32 s = 8 >> m;
        for (u32 y = 0; y < s; y++) for (u32 x = 0; x < s; x++) px[y * s + x] = t2d(x, y, m);
        [t2 replaceRegion:MTLRegionMake2D(0, 0, s, s) mipmapLevel:m withBytes:px bytesPerRow:s * 16];
    }
    id<MTLTexture> ta = texture(MTLTextureType2DArray, MTLPixelFormatRGBA32Float, 4, 4, 1, 3, 1);
    for (u32 l = 0; l < 3; l++) {
        V4 px[16];
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) px[y * 4 + x] = tarr(x, y, l);
        [ta replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 slice:l withBytes:px bytesPerRow:64 bytesPerImage:0];
    }
    id<MTLTexture> td = texture(MTLTextureType2D, MTLPixelFormatDepth32Float, 4, 4, 1, 1, 2);
    {
        float px[16 + 4];
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) px[y * 4 + x] = tdepth(x, y);
        for (u32 y = 0; y < 2; y++) for (u32 x = 0; x < 2; x++) px[16 + y * 2 + x] = tdepth1(x, y);
        id<MTLBuffer> b = buffer(px, sizeof px);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:64 sourceSize:MTLSizeMake(4, 4, 1)
                 toTexture:td destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl copyFromBuffer:b sourceOffset:64 sourceBytesPerRow:8 sourceBytesPerImage:16 sourceSize:MTLSizeMake(2, 2, 1)
                 toTexture:td destinationSlice:0 destinationLevel:1 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    }
    id<MTLTexture> tc = texture(MTLTextureTypeCube, MTLPixelFormatRGBA32Float, 2, 2, 1, 1, 1);
    for (u32 f = 0; f < 6; f++) {
        V4 px[4] = {tcube(f), tcube(f), tcube(f), tcube(f)};
        [tc replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:f withBytes:px bytesPerRow:32 bytesPerImage:0];
    }
    id<MTLTexture> t3 = texture(MTLTextureType3D, MTLPixelFormatRGBA32Float, 4, 4, 4, 1, 1);
    {
        V4 px[64];
        for (u32 z = 0; z < 4; z++) for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) px[z * 16 + y * 4 + x] = t3d(x, y, z);
        [t3 replaceRegion:MTLRegionMake3D(0, 0, 0, 4, 4, 4) mipmapLevel:0 slice:0 withBytes:px bytesPerRow:64 bytesPerImage:256];
    }
    id<MTLTexture> t1 = texture(MTLTextureType2D, MTLPixelFormatRGBA32Float, 8, 1, 1, 1, 1);
    {
        V4 px[8];
        for (u32 x = 0; x < 8; x++) px[x] = t1d(x);
        [t1 replaceRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0 withBytes:px bytesPerRow:128];
    }
    id<MTLTexture> tu = texture(MTLTextureType2D, MTLPixelFormatR32Uint, 4, 4, 1, 1, 1);
    {
        u32 px[16];
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) px[y * 4 + x] = tuint(x, y);
        [tu replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
    }
    id<MTLTexture> tuf = [tu newTextureViewWithPixelFormat:MTLPixelFormatR32Float];   /* (a float view: the shader declares float) */
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNearest;
    sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> nearest = [dev newSamplerStateWithDescriptor:sd];
    sd.compareFunction = MTLCompareFunctionLessEqual;
    id<MTLSamplerState> compare = [dev newSamplerStateWithDescriptor:sd];
    Kernel k = {.p = p, .groups = 1, .in = in, .in_size = sizeof in, .out = out, .out_size = sizeof out,
                .handles = {H_2D, H_ARRAY, H_DEPTH, H_CUBE, H_3D, H_1D, H_UINT},
                .tex = {t2, ta, td, tc, t3, t1, tuf}, .smp = {nearest, nearest, compare, nearest, nearest, nearest, nearest}, .ntex = 7};
    int before = failures;
    /* four times: then with a LOD bias of -1 on the 2D texture's sampler, which moves the
     * explicit LOD 1 to 0 and leaves level 0's samples (clamped there), texel fetches and gathers
     * as they were; then with +1 on it and on the depth texture's, which moves level 0's samples
     * and compares - every sample outside a pixel shader's derivatives - to mip 1, as Direct3D's
     * MipLODBias and Metal's own lodBias do, and the explicit LOD 1 to 2 (clamped to 1); then with
     * the 2D texture a render target drawn at render scale 2, whose texel offsets and numbers are
     * the game's: twice as many of its own (sc) */
    static const char *const bias_name[4] = {"", " (LOD bias -1)", " (LOD bias +1)", " (render scale 2)"};
    for (int biased = 0; biased < 4; biased++) {
        k.lod_bias[0] = biased == 1 ? (u32)-256 : biased == 2 ? 256 : 0;
        k.lod_bias[2] = biased == 2 ? 256 : 0;
        k.scale[0] = biased == 3 ? 2 * 4096 : 0;
        i32 sc = biased == 3 ? 2 : 1;
        memset(out, 0xCD, sizeof out);
        if (run_kernel(&k)) return 1;
        for (u32 t = 0; t < T; t++) {
            const u32 *r = in + t * 32, *got = out + t * 128;
            const float *f = (const float *)r;
            u32 tx = t & 7, ty = t >> 3;
            float want[68];
            V4 v;
            u32 wbits[68];
            int bits[68] = {0};
#define PUT4(at, val) do { V4 v_ = (val); want[at] = v_.x; want[(at) + 1] = v_.y; want[(at) + 2] = v_.z; want[(at) + 3] = v_.w; } while (0)
            PUT4(0, biased == 2 ? t2d(tx >> 1, ty >> 1, 1) : t2d(tx, ty, 0));
            PUT4(4, biased == 1 ? t2d(tx, ty, 0) : t2d(tx >> 1, ty >> 1, 1));
            i32 ox = (i32)(t % 5) - 2, oy = (i32)(t % 3) - 1;
            if (biased == 2) PUT4(8, t2d((u32)clampi((i32)(tx >> 1) + ox, 0, 3), (u32)clampi((i32)(ty >> 1) + oy, 0, 3), 1));
            else PUT4(8, t2d((u32)clampi((i32)tx + sc * ox, 0, 7), (u32)clampi((i32)ty + sc * oy, 0, 7), 0));
            i32 ix = (i32)r[4], iy = (i32)r[5], mip = (i32)r[6];
            i32 hx = sc * ix, hy = sc * iy;   /* (the 2D texture's: at 2, texel v's centre is its 2v + 1, so 2v) */
            PUT4(12, hx >= 0 && hx < 8 ? t2d((u32)hx, (u32)hy, 0) : Z4);
            i32 ms = 8 >> mip;
            PUT4(16, hx >= 0 && hx < ms && hy < ms ? t2d((u32)hx, (u32)hy, (u32)mip) : Z4);
            PUT4(20, gather2d((i32)tx, (i32)ty, 0, 0, 0));
            i32 ox6 = 3 * (i32)t - 20, oy6 = 7 - (i32)t;
            PUT4(24, gather2d((i32)tx, (i32)ty, sc * ox6, sc * oy6, 1));
            for (u32 i = 0; i < 4; i++) {
                i32 px = (i32)((t + i * 5) % 23) - 11, py = (i32)((t * 3 + i) % 19) - 9;
                want[28 + i] = comp(gather2d((i32)tx, (i32)ty, sc * px, sc * py, 2), i);
            }
            PUT4(32, gather2d((i32)tx, (i32)ty, sc * ox6, sc * oy6, 3));
            u32 l = r[12], ux = t & 3, uy = (t >> 2) & 3;
            PUT4(36, tarr(ux, uy, l < 3 ? l : 2));
            PUT4(40, ix >= 0 && ix < 4 && l < 3 ? tarr((u32)ix, (u32)iy, l) : Z4);
            float ref = f[19];
            want[44] = ref <= (biased == 2 ? tdepth1(ux >> 1, uy >> 1) : tdepth(ux, uy)) ? 1.0f : 0.0f;
            {   /* the footprint at a texel's centre: it, right, below */
                i32 i0 = (i32)ux, i1 = clampi((i32)ux + 1, 0, 3), j0 = (i32)uy, j1 = clampi((i32)uy + 1, 0, 3);
                want[45] = ref <= tdepth((u32)i0, (u32)j1); want[46] = ref <= tdepth((u32)i1, (u32)j1);
                want[47] = ref <= tdepth((u32)i1, (u32)j0); want[48] = ref <= tdepth((u32)i0, (u32)j0);
            }
            want[49] = want[44];
            PUT4(50, tcube(t % 6));
            v = tcube(t % 6);
            PUT4(54, ((V4){v.x, v.x, v.x, v.x}));
            PUT4(58, r[22] < 4 ? t3d(r[20], r[21], r[22]) : Z4);
            PUT4(62, t1d(tx));
            wbits[66] = tuint(r[23], r[5]); bits[66] = 1;
            wbits[67] = 0; bits[67] = 1;
#undef PUT4
            static const char *const what[68] = {"TEXS 2D", "", "", "", "TEX 2D LOD 1", "", "", "", "TEX 2D offset", "", "", "",
                "TLDS 2D", "", "", "", "TLDS 2D mip", "", "", "", "TLD4 R", "", "", "", "TLD4 G offset", "", "", "", "TLD4 B offset/texel", "", "", "",
                "TLD4S A offset", "", "", "", "TEX 2D array", "", "", "", "TLDS 2D array", "", "", "", "TEX compare", "TLD4S compare", "", "", "",
                "TEXS compare", "TEX cube", "", "", "", "TLD4 cube", "", "", "", "TLDS 3D", "", "", "", "TEXS 1D", "", "", "",
                "TLDS integer bits", "TMML (no derivatives: 0)"};
            for (u32 i = 0; i < 68; i++) {
                u32 wv = bits[i] ? wbits[i] : U32(want[i]);
                char name[64];
                u32 base = i;
                while (base && !what[base][0]) base--;
                snprintf(name, sizeof name, "%s%s .%c", what[base], bias_name[biased], "xyzw"[(i - base) & 3]);
                check(name, (int)t, got[i], wv, got[i] == wv);
            }
        }
    }
    printf("gpu cs_tex: textures, 16 threads, without and with LOD biases -1 and +1, and at render scale 2: %s\n",
           failures == before ? "ok" : "FAILED");
    return failures != before;
}

/* ---- drawing ---------------------------------------------------------------------------- */
/* decode_attr, in C: a vertex attribute's components from raw bytes */
static void decode_attr(const u8 *m, u32 size, u32 addr, u32 fmt, u32 o[4]) {
    u32 sz = (fmt >> 21) & 0x3F, type = (fmt >> 27) & 7, n = 4, bits = 32, packed = 0;
    switch (sz) {
    case 0x01: n = 4; bits = 32; break; case 0x02: n = 3; bits = 32; break; case 0x03: n = 4; bits = 16; break;
    case 0x04: n = 2; bits = 32; break; case 0x05: n = 3; bits = 16; break; case 0x0A: n = 4; bits = 8; break;
    case 0x0F: n = 2; bits = 16; break; case 0x12: n = 1; bits = 32; break; case 0x13: n = 3; bits = 8; break;
    case 0x18: n = 2; bits = 8; break; case 0x1B: n = 1; bits = 16; break; case 0x1D: n = 1; bits = 8; break;
    case 0x32: n = 2; bits = 8; break; case 0x33: n = 4; bits = 8; break; case 0x34: n = 1; bits = 8; break;
    case 0x30: n = 4; bits = 10; packed = 1; break;
    default: o[0] = o[1] = o[2] = 0; o[3] = 0x3F800000; return;
    }
    u32 base = addr & ~3u, sh = (addr & 3) * 8, w[5];
    for (u32 i = 0; i < 5; i++) w[i] = (base >> 2) + i < size >> 2 ? rd(m, size, base + 4 * i, 4) : 0;
    int is_float = type == 7 || type == 1 || type == 2 || type == 5 || type == 6;
    o[0] = o[1] = o[2] = 0; o[3] = is_float ? 0x3F800000u : 1u;
    for (u32 i = 0; i < n; i++) {
        u32 cb = packed && i == 3 ? 2 : bits, bo = sh + (packed ? i * 10 : i * bits), wi = bo >> 5, s = bo & 31, v = w[wi] >> s;
        if (s + cb > 32) v |= w[wi + 1] << (32 - s);
        if (cb < 32) v &= (1u << cb) - 1;
        i32 sv = cb < 32 ? (i32)(v << (32 - cb)) >> (32 - cb) : (i32)v;
        float scale = cb < 32 ? (float)((1u << cb) - 1) : 4294967295.0f;
        u32 r = v;
        switch (type) {
        case 1: r = U32(fmaxf((float)sv / (scale * 0.5f - 0.5f), -1.0f)); break;
        case 2: r = U32((float)v / scale); break;
        case 3: r = (u32)sv; break;
        case 5: r = U32((float)v); break;
        case 6: r = U32((float)sv); break;
        case 7: r = cb == 16 ? U32(h2f(v)) : v; break;
        }
        o[i] = r;
    }
    if (fmt >> 31) { u32 x = o[0]; o[0] = o[2]; o[2] = x; }
}

static id<MTLRenderPipelineState> pipeline(Prog *vs, Prog *ps, u32 rt_types, const MTLPixelFormat *formats, u32 ntargets, int depth,
                                           MTLPrimitiveTopologyClass topology, ShaderInfo *vinfo, ShaderInfo *pinfo) {
    char *vt = msl_of(vs, 0, vinfo);
    char *pt = msl_of(ps, vinfo->outputs, pinfo);
    id<MTLLibrary> vl = compile(vt, 0, NULL, NULL), pl = compile(pt, 0, NULL, NULL);
    free(vt);
    free(pt);
    if (!vl || !pl) return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = function(vl, SHADER_VERTEX, 0);
    d.fragmentFunction = function(pl, SHADER_PIXEL, rt_types);
    for (u32 i = 0; i < ntargets; i++) d.colorAttachments[i].pixelFormat = formats[i];
    if (depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.inputPrimitiveTopology = topology;
    NSError *e = nil;
    id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!rps) printf("  pipeline: %s\n", e.localizedDescription.UTF8String);
    return rps;
}

static id<MTLTexture> target(MTLPixelFormat format, u32 w, u32 h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = format == MTLPixelFormatDepth32Float ? MTLStorageModePrivate : MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}

/* Points: each pixel of an 8x1 target gets one vertex's attribute 1, decoded (through a uint
 * target: bits), its position (float target) and a depth; vertex 3 discards. Every format. */
static int gpu_points(void) {
    Prog *vs = find_prog("vs_points"), *ps = find_prog("ps_points");
    static const MTLPixelFormat formats[2] = {MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Float};
    ShaderInfo vinfo, pinfo;
    id<MTLRenderPipelineState> rps = pipeline(vs, ps, 1 /* target 0: uint */, formats, 2, 1, MTLPrimitiveTopologyClassPoint, &vinfo, &pinfo);
    if (!rps) return 1;
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dd];
    float pos[8][4];
    for (u32 i = 0; i < 8; i++) { pos[i][0] = ((float)i + 0.5f) / 4 - 1; pos[i][1] = 0; pos[i][2] = 0.5f; pos[i][3] = 1; }
    u8 data[8 * 20 + 16];
    for (u32 i = 0; i < sizeof data; i++) data[i] = (u8)(i * 53 + 7 + (i >> 3) * 29);
    const u32 vsize = 8 * 20;
    static const struct { const char *name; u32 size, type, offset, swap, constant; } F[] = {
        {"R32G32B32A32 float", 0x01, 7, 0, 0, 0}, {"R8G8B8A8 unorm", 0x0A, 2, 0, 0, 0}, {"R8G8B8A8 snorm", 0x0A, 1, 0, 0, 0},
        {"R8G8B8A8 uint", 0x0A, 4, 0, 0, 0}, {"R8G8B8A8 sint", 0x0A, 3, 0, 0, 0}, {"R8G8B8A8 uscaled", 0x0A, 5, 0, 0, 0},
        {"R8G8B8A8 sscaled", 0x0A, 6, 0, 0, 0}, {"R16G16 float", 0x0F, 7, 0, 0, 0}, {"A2B10G10R10 unorm", 0x30, 2, 0, 0, 0},
        {"R8 unorm", 0x1D, 2, 0, 0, 0}, {"R32 uint", 0x12, 4, 0, 0, 0}, {"R8G8B8 unorm, offset 1", 0x13, 2, 1, 0, 0},
        {"R16G16B16 snorm, offset 3", 0x05, 1, 3, 0, 0}, {"B8G8R8A8 unorm (swap)", 0x0A, 2, 4, 1, 0}, {"constant", 0x0A, 2, 0, 0, 1},
        {"R32G32B32 float, offset 9", 0x02, 7, 9, 0, 0}, {"R16G16B16A16 sint", 0x03, 3, 0, 0, 0}, {"R32G32 uint", 0x04, 4, 4, 0, 0},
        {"R16 float", 0x1B, 7, 2, 0, 0}, {"R8G8 snorm", 0x18, 1, 1, 0, 0}, {"unknown size", 0x3F, 2, 0, 0, 0},
    };
    id<MTLBuffer> pbuf = buffer(pos, sizeof pos), abuf = buffer(data, sizeof data);
    id<MTLTexture> rt0 = target(MTLPixelFormatRGBA32Uint, 8, 1), rt1 = target(MTLPixelFormatRGBA32Float, 8, 1), dt = target(MTLPixelFormatDepth32Float, 8, 1);
    id<MTLBuffer> depth_out = buffer(NULL, 64);
    int before = failures;
    for (u32 fi = 0; fi < sizeof F / sizeof F[0]; fi++) {
        u32 fmt = F[fi].size << 21 | F[fi].type << 27 | F[fi].offset << 7 | F[fi].swap << 31 | F[fi].constant << 6 | 1;
        DrawParams params;
        ShaderBinds binds;
        memset(&params, 0, sizeof params);
        params.base[2] = params.base[3] = 0x3F800000u;   /* 1 / the render scale (ps_input): 1, as the renderer's */
        memset(&binds, 0, sizeof binds);
        params.attr[0][0] = 0x01 << 21 | 7u << 27; params.attr[0][1] = 16; params.attr[0][3] = 1;
        params.attr[1][0] = fmt; params.attr[1][1] = 20; params.attr[1][3] = 1;
        params.fix[0] = params.fix[1] = params.fix[2] = 1;
        params.frag[1] = 1;
        binds.vb[0] = (ShaderBind){pbuf.gpuAddress, sizeof pos, 0};
        binds.vb[1] = (ShaderBind){abuf.gpuAddress, vsize, 0};
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt0; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[1].texture = rt1; rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].clearColor = MTLClearColorMake(-1, -1, -1, -1); rp.colorAttachments[1].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = dt; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:rps];
        [enc setDepthStencilState:dss];
        [enc setVertexBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc setVertexBytes:&binds length:shader_binds_size(SHADER_VERTEX) atIndex:SHADER_MSL_BINDS];
        [enc setFragmentBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc setFragmentBytes:&binds length:shader_binds_size(SHADER_PIXEL) atIndex:SHADER_MSL_BINDS];
        [enc useResource:pbuf usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [enc useResource:abuf usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:8];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 1, 1)
                   toBuffer:depth_out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:32];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("  draw: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        u32 c0[8][4], c1[8][4];
        [rt0 getBytes:c0 bytesPerRow:sizeof c0 fromRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0];
        [rt1 getBytes:c1 bytesPerRow:sizeof c1 fromRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0];
        const float *dz = depth_out.contents;
        for (u32 i = 0; i < 8; i++) {
            u32 want[4] = {0, 0, 0, 0};
            if (F[fi].constant) { want[3] = 0x3F800000; }
            else decode_attr(data, vsize, i * 20 + F[fi].offset, fmt, want);
            if (i == 3) memset(want, 0, sizeof want);   /* (discarded: the clear colour) */
            char name[64];
            for (u32 c = 0; c < 4; c++) {
                snprintf(name, sizeof name, "%s .%c", F[fi].name, "xyzw"[c]);
                check(name, (int)i, c0[i][c], want[c], F[fi].type == 7 ? same_float(c0[i][c], want[c], SAME_FLOAT) : c0[i][c] == want[c]);
            }
            if (fi) continue;
            float wp[4] = {(float)i + 0.5f, 0.5f, 0.5f, 1.0f};
            if (i == 3) wp[0] = wp[1] = wp[2] = wp[3] = -1;
            for (u32 c = 0; c < 4; c++) {
                snprintf(name, sizeof name, "position .%c", "xyzw"[c]);
                check(name, (int)i, c1[i][c], U32(wp[c]), c1[i][c] == U32(wp[c]));
            }
            check("depth written", (int)i, U32(dz[i]), U32(i == 3 ? 1.0f : 0.25f), dz[i] == (i == 3 ? 1.0f : 0.25f));
        }
    }
    printf("gpu points: %zu vertex formats through flat attributes, a uint target, depth, discard: %s\n", sizeof F / sizeof F[0],
           failures == before ? "ok" : "FAILED");
    return failures != before;
}

/* A triangle over a 4x4 target, clockwise in normalised device coordinates (y up): the pixel
 * shader's position (y flipped by DrawParams.frag) and facing, drawn with clockwise and then
 * counter-clockwise front faces; then culling front faces, with clockwise ones front. */
static int gpu_triangle(void) {
    Prog *vs = find_prog("vs_triangle"), *ps = find_prog("ps_position");
    static const MTLPixelFormat formats[1] = {MTLPixelFormatRGBA32Uint};
    ShaderInfo vinfo, pinfo;
    id<MTLRenderPipelineState> rps = pipeline(vs, ps, 1, formats, 1, 0, MTLPrimitiveTopologyClassUnspecified, &vinfo, &pinfo);
    if (!rps) return 1;
    float pos[3][4] = {{-1, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}, {3, -1, 0.5f, 1}};
    id<MTLBuffer> pbuf = buffer(pos, sizeof pos);
    id<MTLTexture> rt = target(MTLPixelFormatRGBA32Uint, 4, 4);
    int before = failures;
    u32 facing[2] = {0, 0}, culled = 0;
    for (u32 pass = 0; pass < 3; pass++) {
        DrawParams params;
        ShaderBinds binds;
        memset(&params, 0, sizeof params);
        params.base[2] = params.base[3] = 0x3F800000u;   /* 1 / the render scale (ps_input): 1, as the renderer's */
        memset(&binds, 0, sizeof binds);
        params.attr[0][0] = 0x01 << 21 | 7u << 27; params.attr[0][1] = 16; params.attr[0][3] = 1;
        params.fix[0] = params.fix[1] = params.fix[2] = 1;
        params.frag[0] = 1; params.frag[1] = 4;
        binds.vb[0] = (ShaderBind){pbuf.gpuAddress, sizeof pos, 0};
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(7, 7, 7, 7); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:rps];
        [enc setFrontFacingWinding:pass == 1 ? MTLWindingCounterClockwise : MTLWindingClockwise];
        if (pass == 2) [enc setCullMode:MTLCullModeFront];
        [enc setVertexBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc setVertexBytes:&binds length:shader_binds_size(SHADER_VERTEX) atIndex:SHADER_MSL_BINDS];
        [enc setFragmentBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc useResource:pbuf usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("  draw: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        u32 c[16][4];
        [rt getBytes:c bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
        if (pass == 2) {   /* (all culled: the clear colour) */
            for (u32 i = 0; i < 16; i++) culled += c[i][0] == 7 && c[i][3] == 7;
            check("culled as front", 0, culled, 16, culled == 16);
            continue;
        }
        for (u32 i = 0; i < 16; i++) {
            u32 x = i & 3, y = i >> 2;
            check("position x", (int)i, c[i][0], U32((float)x + 0.5f), c[i][0] == U32((float)x + 0.5f));
            check("position y (flipped)", (int)i, c[i][1], U32(4.0f - ((float)y + 0.5f)), c[i][1] == U32(4.0f - ((float)y + 0.5f)));
            check("position z", (int)i, c[i][2], U32(0.5f), c[i][2] == U32(0.5f));
            check("facing: all or none", (int)i, c[i][3], c[0][3], c[i][3] == c[0][3] && (c[i][3] == 0 || c[i][3] == ~0u));
        }
        facing[pass] = c[0][3];
    }
    check("facing follows the winding", 0, facing[1], ~facing[0], facing[1] == ~facing[0]);
    printf("gpu triangle: position, flip, facing (clockwise in NDC with MTLWindingClockwise: %s, %s): %s\n", facing[0] ? "front" : "back",
           culled == 16 ? "culled as front" : "NOT culled as front", failures == before ? "ok" : "FAILED");
    return failures != before;
}

/* Dual-source blending (specialised with SHADER_MSL_DUAL_SOURCE) into a target cleared to
 * (0.25, 0.5, 0.75, 1): source 1 is target 1's output, or 0 when the program writes no target 1;
 * a program writing no target 0 gives 0 as source 0. Metal refuses a pipeline blending with
 * Source1 whose program does not write both, as the renderer asks for it from the blend state
 * alone: programs writing no colour at all must give one too (as the renderer would, its
 * attachment's writeMask none). */
typedef struct DualCase {
    const char *ps, *what;
    MTLBlendFactor src, src_a, dst, dst_a;
    float want[4];   /* (no draw: NaN) */
} DualCase;
static int gpu_dual(void) {
    static const DualCase cases[] = {
        {"ps_dual_source", "source 0 x source 1", MTLBlendFactorSource1Color, MTLBlendFactorSource1Alpha, MTLBlendFactorZero, MTLBlendFactorZero,
         {1.0f, 0.5f, 0.5f, 1.0f}},
        {"ps_one_target", "target 0 alone: source 0 + d x (1 - 0)", MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Color,
         MTLBlendFactorOneMinusSource1Alpha, {0.75f, 0.75f, 1.75f, 2.0f}},
        {"ps_target1_only", "target 1 alone: 0 + d x (1 - source 1)", MTLBlendFactorOne, MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Color,
         MTLBlendFactorOneMinusSource1Alpha, {0.125f, 0.375f, 0.0f, 0.0f}},
        {"ps_no_outputs", "no target", MTLBlendFactorSourceAlpha, MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Alpha, MTLBlendFactorZero, {NAN}},
        {"ps_depth_only", "depth alone", MTLBlendFactorSourceAlpha, MTLBlendFactorOne, MTLBlendFactorOneMinusSource1Alpha, MTLBlendFactorZero, {NAN}},
    };
    Prog *vs = find_prog("vs_triangle");
    float pos[3][4] = {{-1, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}, {3, -1, 0.5f, 1}};
    id<MTLBuffer> pbuf = buffer(pos, sizeof pos);
    int before = failures;
    for (u32 n = 0; n < sizeof cases / sizeof cases[0]; n++) {
        const DualCase *dc = &cases[n];
        Prog *ps = find_prog(dc->ps);
        char *vt, *pt;
        ShaderInfo vinfo, pinfo;
        vt = msl_of(vs, 0, &vinfo);
        pt = msl_of(ps, vinfo.outputs, &pinfo);
        id<MTLLibrary> vl = compile(vt, 0, NULL, NULL), pl = compile(pt, 0, NULL, NULL);
        free(vt);
        free(pt);
        if (!vl || !pl) return 1;
        int draw = !isnan(dc->want[0]);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = function(vl, SHADER_VERTEX, 0);
        d.fragmentFunction = function(pl, SHADER_PIXEL, SHADER_MSL_DUAL_SOURCE);
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        d.colorAttachments[0].writeMask = draw ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = dc->src;
        d.colorAttachments[0].sourceAlphaBlendFactor = dc->src_a;
        d.colorAttachments[0].destinationRGBBlendFactor = dc->dst;
        d.colorAttachments[0].destinationAlphaBlendFactor = dc->dst_a;
        if (pinfo.writes_depth) d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *e = nil;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
        char what[96];
        snprintf(what, sizeof what, "%s: a pipeline", dc->what);
        check(what, (int)n, rps != nil, 1, rps != nil);
        if (!rps) { printf("  %s: %s\n", dc->ps, e.localizedDescription.UTF8String); continue; }
        if (!draw) continue;
        id<MTLTexture> rt = target(MTLPixelFormatRGBA16Float, 2, 2);
        DrawParams params;
        ShaderBinds binds;
        memset(&params, 0, sizeof params);
        params.base[2] = params.base[3] = 0x3F800000u;   /* 1 / the render scale (ps_input): 1, as the renderer's */
        memset(&binds, 0, sizeof binds);
        params.attr[0][0] = 0x01 << 21 | 7u << 27; params.attr[0][1] = 16; params.attr[0][3] = 1;
        params.fix[0] = params.fix[1] = params.fix[2] = 1;
        binds.vb[0] = (ShaderBind){pbuf.gpuAddress, sizeof pos, 0};
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:rps];
        [enc setVertexBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc setVertexBytes:&binds length:shader_binds_size(SHADER_VERTEX) atIndex:SHADER_MSL_BINDS];
        [enc setFragmentBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc useResource:pbuf usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("  draw: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        u16 px[4][4];
        [rt getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
        for (u32 i = 0; i < 4; i++)
            for (u32 k = 0; k < 4; k++) check(dc->what, (int)(i * 4 + k), px[i][k], f2h(dc->want[k]), px[i][k] == f2h(dc->want[k]));
    }
    printf("gpu dual source: blending with target 1's output, with no target 1 (0), no target 0, no colour: %s\n",
           failures == before ? "ok" : "FAILED");
    return failures != before;
}

/* Implicit-LOD sampling in a pixel shader (derivatives: LOD 0 over an 8x8 target and texture),
 * without and with the sampler's LOD bias +1 as the renderer passes it before macOS 26: the
 * colour and the compare come from mip 1 with it. */
static V4 tbias(u32 x, u32 y, u32 m) { return (V4){(float)(x + 8 * y + 100 * m), (float)m, 1, 2}; }
static float tbias_depth(u32 x, u32 y, u32 m) { return m ? 1.0f - (float)(x + 4 * y) / 16.0f : (float)(x + 8 * y) / 64.0f; }
static int gpu_lod_bias(void) {
    Prog *vs = find_prog("vs_triangle"), *ps = find_prog("ps_lod_bias");
    static const MTLPixelFormat formats[2] = {MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Float};
    ShaderInfo vinfo, pinfo;
    id<MTLRenderPipelineState> rps = pipeline(vs, ps, 0, formats, 2, 0, MTLPrimitiveTopologyClassUnspecified, &vinfo, &pinfo);
    if (!rps) return 1;
    id<MTLTexture> tc = texture(MTLTextureType2D, MTLPixelFormatRGBA32Float, 8, 8, 1, 1, 2);
    id<MTLTexture> td = texture(MTLTextureType2D, MTLPixelFormatDepth32Float, 8, 8, 1, 1, 2);
    {
        float dp[64 + 16];
        for (u32 m = 0; m < 2; m++) {
            V4 px[64];
            u32 s = 8 >> m;
            for (u32 y = 0; y < s; y++)
                for (u32 x = 0; x < s; x++) { px[y * s + x] = tbias(x, y, m); dp[(m ? 64 : 0) + y * s + x] = tbias_depth(x, y, m); }
            [tc replaceRegion:MTLRegionMake2D(0, 0, s, s) mipmapLevel:m withBytes:px bytesPerRow:s * 16];
        }
        id<MTLBuffer> b = buffer(dp, sizeof dp);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:32 sourceBytesPerImage:256 sourceSize:MTLSizeMake(8, 8, 1)
                 toTexture:td destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl copyFromBuffer:b sourceOffset:256 sourceBytesPerRow:16 sourceBytesPerImage:64 sourceSize:MTLSizeMake(4, 4, 1)
                 toTexture:td destinationSlice:0 destinationLevel:1 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    }
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.mipFilter = MTLSamplerMipFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> nearest = [dev newSamplerStateWithDescriptor:sd];
    sd.compareFunction = MTLCompareFunctionLessEqual;
    id<MTLSamplerState> compare = [dev newSamplerStateWithDescriptor:sd];
    float pos[3][4] = {{-1, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}, {3, -1, 0.5f, 1}};
    id<MTLBuffer> pbuf = buffer(pos, sizeof pos);
    id<MTLTexture> rt0 = target(MTLPixelFormatRGBA32Float, 8, 8), rt1 = target(MTLPixelFormatRGBA32Float, 8, 8);
    int before = failures;
    for (u32 biased = 0; biased < 2; biased++) {
        DrawParams params;
        ShaderBinds binds;
        memset(&params, 0, sizeof params);
        params.base[2] = params.base[3] = 0x3F800000u;   /* 1 / the render scale (ps_input): 1, as the renderer's */
        memset(&binds, 0, sizeof binds);
        params.attr[0][0] = 0x01 << 21 | 7u << 27; params.attr[0][1] = 16; params.attr[0][3] = 1;
        params.fix[0] = params.fix[1] = params.fix[2] = 1;
        params.frag[0] = 1; params.frag[1] = 8;
        binds.vb[0] = (ShaderBind){pbuf.gpuAddress, sizeof pos, 0};
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt0; rp.colorAttachments[1].texture = rt1;
        for (u32 i = 0; i < 2; i++) { rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].storeAction = MTLStoreActionStore; }
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:rps];
        for (u32 t = 0; t < pinfo.ntex; t++) {
            int depth = pinfo.tex[t].handle_offset == H_DEPTH;
            params.swz[1][t] = 2 | 3 << 3 | 4 << 6 | 5 << 9 | (biased ? 256u : 0u) << 12;
            [enc setFragmentTexture:depth ? td : tc atIndex:t];
            [enc setFragmentSamplerState:depth ? compare : nearest atIndex:t];
        }
        [enc setVertexBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc setVertexBytes:&binds length:shader_binds_size(SHADER_VERTEX) atIndex:SHADER_MSL_BINDS];
        [enc setFragmentBytes:&params length:sizeof params atIndex:SHADER_MSL_PARAMS];
        [enc useResource:pbuf usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { printf("  draw: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        static float c0[64][4], c1[64][4];
        [rt0 getBytes:c0 bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        [rt1 getBytes:c1 bytesPerRow:128 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        for (u32 i = 0; i < 64; i++) {
            u32 x = i & 7, y = 7 - (i >> 3), m = biased;   /* (y flipped: the texture's row) */
            V4 w = tbias(x >> m, y >> m, m);
            float wc = 0.5f <= tbias_depth(x >> m, y >> m, m) ? 1.0f : 0.0f;
            const char *what = biased ? "implicit sample, LOD bias +1: mip 1" : "implicit sample: mip 0";
            for (u32 k = 0; k < 4; k++) check(what, (int)(i * 4 + k), U32(c0[i][k]), U32(comp(w, k)), c0[i][k] == comp(w, k));
            check(biased ? "implicit compare, LOD bias +1: mip 1" : "implicit compare: mip 0", (int)i, U32(c1[i][0]), U32(wc), c1[i][0] == wc);
        }
    }
    printf("gpu lod bias: a pixel shader's implicit samples and compares, without and with the sampler's LOD bias: %s\n",
           failures == before ? "ok" : "FAILED");
    return failures != before;
}

static int run_gpu(void) {
    int bad = 0;
    printf("device: %s\n", dev.name.UTF8String);
    bad |= gpu_alu();
    bad |= gpu_flow();
    bad |= gpu_mem();
    bad |= gpu_tex();
    bad |= gpu_points();
    bad |= gpu_triangle();
    bad |= gpu_dual();
    bad |= gpu_lod_bias();
    printf("gpu: %d checks, %d failed%s\n", checks, failures, failures > 60 ? " (the first 60 shown)" : "");
    return bad;
}

/* The random programs in MSL: each must compile, but where HLSL's could not either (what Direct3D
 * and Metal both refuse: compares on 3D textures, offsets on cube maps), and so be reported. */
static int run_fuzz(int only) {
    static Prog fz;
    fuzz_state = 0x9E3779B97F4A7C15ull;
    int compiled = 0, failed = 0, expected = 0, infos_differ = 0;
    double total = 0;
    for (u32 i = 0; i < NFUZZ; i++) {
        fuzz_program(&fz, i);
        ShaderInfo info;
        u32 outs = (u32)fuzz_next();
        char *text = msl_of(&fz, outs, &info);
        ShaderInfo hinfo;
        free(shader_translate(SHADER_HLSL_FXC, fz.bytes, fz.size, fz.stage, outs, fz.local, fz.shared, &hinfo, read_cbuf, &fz));
        if (memcmp(&hinfo, &info, sizeof info)) { printf("fuzz %u: ShaderInfo differs from HLSL's\n", i); infos_differ++; }
        if (only >= 0) {   /* (one program's MSL) */
            if ((int)i == only) { fputs(text, stdout); return 0; }
            free(text);
            free(fz.bytes);
            continue;
        }
        double ms;
        NSString *err = nil;
        id<MTLLibrary> lib = compile(text, 0, &ms, &err);
        total += ms;
        if (lib && function(lib, fz.stage, 0)) compiled++;
        else {
            const char *e = err ? strstr(err.UTF8String, "error: ") : NULL;
            char first[300] = "no function";
            if (e) snprintf(first, sizeof first, "%.*s", (int)strcspn(e, "\n"), e);
            /* what HLSL cannot say either: a compare on a 3D texture; a gather offset on a cube map;
             * a texture's slot taken by another type (past 32); discard, or the thread's index,
             * outside the stages that have them; a register past the last (RZ + 1) */
            int also_hlsl = info.ntex == 32;
            for (u32 k = 0; k < info.ntex; k++) also_hlsl |= info.tex[k].shadow && info.tex[k].type == TEX_3D;
            also_hlsl |= (strstr(first, "no matching member function") || strstr(first, "too many arguments")) &&
                         (strstr(text, "TexCube") || strstr(text, "ShadowCube"));
            also_hlsl |= fz.stage != SHADER_PIXEL && strstr(text, "discard_fragment") != NULL;
            also_hlsl |= fz.stage != SHADER_COMPUTE && (strstr(first, "'tid_'") || strstr(first, "'cta_'"));
            also_hlsl |= strstr(first, "'r256'") != NULL;
            if (also_hlsl) expected++;
            else {
                failed++;
                printf("fuzz %u (%s): %s\n", i, stage_name(fz.stage), first);
                if (verbose) printf("%s\n", text);
            }
        }
        free(text);
        free(fz.bytes);
    }
    printf("fuzz: %u random programs in MSL, %d compiled (%.0f ms), %d refused as HLSL would be too, %d FAILED; ShaderInfo as HLSL's: %s\n",
           NFUZZ, compiled, total, expected, failed, infos_differ ? "NOT ALWAYS" : "always");
    return failed != 0 || infos_differ;
}

/* ---- dumped programs ------------------------------------------------------------------------- */
static int run_dir(const char *dir, const char *filter) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    struct dirent *e;
    int total = 0, ok = 0;
    double ms_total = 0, ms_max = 0;
    char slowest[256] = "";
    static u8 buf[0x40000 + 0x50];
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".bin") || (filter && !strstr(e->d_name, filter))) continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        u32 n = (u32)fread(buf, 1, sizeof buf, f);
        fclose(f);
        int stage = e->d_name[0] == '5' ? SHADER_PIXEL : e->d_name[0] == 'C' ? SHADER_COMPUTE : SHADER_VERTEX;
        u32 local[3] = {1, 1, 1}, shared = 0, outs = 0;
        if (stage == SHADER_COMPUTE) {
            const char *u = strchr(e->d_name + 2, '_');
            if (u) sscanf(u + 1, "%ux%ux%u_%u", &local[0], &local[1], &local[2], &shared);
        }
        ShaderInfo info;
        if (stage == SHADER_PIXEL) {   /* (keyed by what it reads, as hlsltest does) */
            free(shader_translate(SHADER_MSL, buf, n, stage, 0xFFFFFFFFu, NULL, 0, &info, NULL, NULL));
            outs = info.inputs;
        }
        char *text = shader_translate(SHADER_MSL, buf, n, stage, outs, local, shared, &info, NULL, NULL);
        double ms;
        NSString *err = nil;
        id<MTLLibrary> lib = compile(text, 0, &ms, &err);
        total++;
        ms_total += ms;
        if (ms > ms_max) { ms_max = ms; snprintf(slowest, sizeof slowest, "%s", e->d_name); }
        if (lib && function(lib, stage, 0)) ok++;
        else {
            printf("%s: FAILED%s%s\n", e->d_name, info.unsupported[0] ? " (leaves out " : "", info.unsupported[0] ? info.unsupported : "");
            if (err) printf("  %.2000s\n", err.UTF8String);
            if (verbose) printf("%s\n", text);
        }
        free(text);
    }
    closedir(d);
    printf("%d programs: %d compiled (%.0f ms, slowest %.0f ms %s)\n", total, ok, ms_total, ms_max, slowest);
    return ok != total;
}
#endif

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "all";
    build_corpus();
    if (!strcmp(mode, "hlsl") || !strcmp(mode, "hlsldiff")) {
        if (argc < 4) { fprintf(stderr, "usage: msltest %s <fxc|vkd3d|api> <file>\n", mode); return 2; }
        int form = !strcmp(argv[2], "fxc") ? 0 : !strcmp(argv[2], "vkd3d") ? 1 : 2;
        return mode[4] ? hlsl_diff(form, argv[3]) : hlsl_write(form, argv[3]);
    }
    if (!strcmp(mode, "dump")) {
        if (argc < 3) { fprintf(stderr, "usage: msltest dump <dir>\n"); return 2; }
        return dump_corpus(argv[2]);
    }
    if (!strcmp(mode, "list")) {
        for (u32 i = 0; i < ncorpus; i++) printf("%s (%s, %u instructions)\n", corpus[i].name, stage_name(corpus[i].stage), corpus[i].n);
        return 0;
    }
#ifdef MSLTEST_HLSL_ONLY
    fprintf(stderr, "built with MSLTEST_HLSL_ONLY: only the hlsl modes\n");
    return 2;
#else
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "-v")) verbose = 1;
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        queue = [dev newCommandQueue];
        if (!strcmp(mode, "print")) {
            Prog *p = argc > 2 ? find_prog(argv[2]) : NULL;
            if (!p) { fprintf(stderr, "no program %s (msltest list)\n", argc > 2 ? argv[2] : ""); return 2; }
            ShaderInfo info;
            char *text = msl_of(p, outputs_of(p), &info);
            fputs(text, stdout);
            free(text);
            return 0;
        }
        if (!strcmp(mode, "dir")) {
            if (argc < 3) { fprintf(stderr, "usage: msltest dir <dir> [filter] [-v]\n"); return 2; }
            return run_dir(argv[2], argc > 3 && strcmp(argv[3], "-v") ? argv[3] : NULL);
        }
        if (!strcmp(mode, "fuzz")) return run_fuzz(argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : -1);
        int bad = 0;
        if (!strcmp(mode, "corpus") || !strcmp(mode, "all")) bad |= run_corpus();
        if (!strcmp(mode, "gpu") || !strcmp(mode, "all")) bad |= run_gpu();
        else if (strcmp(mode, "corpus")) { fprintf(stderr, "unknown mode %s\n", mode); return 2; }
        return bad;
    }
#endif
}
