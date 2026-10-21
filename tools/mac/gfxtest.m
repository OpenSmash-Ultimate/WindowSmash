/* The Metal renderer (runtime/macos/gpu_metal.m) end to end, without a window or the game: the
 * renderer as the game's GPU command processor (gpu.c) drives it - the 3D engine's registers at a
 * clear, a draw, a 2D blit, a compute launch, the copy engine's hooks, a present - on guest memory
 * where the game's is (RT_MEMBASE up, in the write-watched alias area, through the real Win32
 * shim: runtime/linux/win32.c and runtime/macos), with Maxwell programs assembled here and
 * translated by the real translator (runtime/gpu_shader.c). Each check reads back what the GPU
 * made - a render target's pixels, a buffer's words in guest memory, the screenshot the F3 key
 * saves - and compares it with what the hardware would have made:
 *   clears: a whole target, some channels of one, depth and stencil
 *   draws: a triangle from a vertex stream in guest memory, another by 8-bit indices, points,
 *     depth and stencil tests, a texture in block-linear guest memory (RGBA8; the 16-bit packed
 *     formats, whose bits Metal keeps another way; BC1 of a size not a multiple of 4; Z24 depth),
 *     that texture again after the CPU wrote it (the write watch), arrays, cube maps and 3D
 *     textures (each as its own type, an array as 2D too), render targets as textures (the depth
 *     target as depth; a draw's own target, read as nothing), constants kept on the GPU and
 *     changed between two draws by the command stream (each draw sees its own)
 *   blending, scissor, write mask, culling; sRGB and integer render targets; dual-source
 *     blending (with one target, and with two bound, which Metal does not take as they are)
 *   the 2D engine: a scaled copy between render targets
 *   upload memory over its budget (chunks given up while allocations kept for reuse name them)
 *   compute: a program writing a storage buffer (back in guest memory while it is learnt, kept
 *     on the GPU after: brought back by a flush, read by a draw as its vertices, copied by the
 *     copy engine); a written range merged with a region the same dispatch reads; the table of
 *     resident regions full; a region sharing its 16 KiB host page with the CPU's data, which the
 *     CPU writes (the write watch then reports the region's pages too: the GPU's results stay)
 *   many frames of many draws (upload memory recycled as the GPU finishes, work committed as it grows)
 *   presenting into a CAMetalLayer of its own (offscreen), with the overlay; the screenshot; the
 *     overlay itself, drawn into a target of its own; the disk cache's MSL
 *   render scale: targets made twice the game's size (and 1.5 times), with viewports, scissors,
 *     the fragment position and texel numbers in the game's pixels; the picture presented at its
 *     size and shrunk into the window in linear light (SSBU_CAPTURE and SSBU_CAPTURE_WINDOW's files)
 * Run it with MTL_DEBUG_LAYER=1 (and MTL_SHADER_VALIDATION=1) as well: Metal then checks every
 * call; and with DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib (Guard Malloc: a use of freed
 * memory crashes - set by env, not through a shell script, which drops DYLD_ variables). The
 * upload budget's test takes 1.5 GB of memory. Exits 1 if anything fails.
 *
 * Build, from the repository root (the overlay's glyphs first, as tools/mac/build_mac.py makes them):
 *   mkdir -p /tmp/gfxtest && clang -arch arm64 -mmacosx-version-min=14.0 -fobjc-arc -O1 tools/mac/make_overlay_font.m \
 *     -framework CoreText -framework CoreGraphics -framework Foundation -o /tmp/gfxtest/font && /tmp/gfxtest/font /tmp/gfxtest/overlay_font.h
 *   clang -arch arm64 -mmacosx-version-min=14.0 -mcpu=apple-m1 -fobjc-arc -fno-strict-aliasing -ffp-contract=off -O1 -w \
 *     -I runtime/macos/include -I runtime/linux/include -I runtime -I /tmp/gfxtest \
 *     tools/mac/gfxtest.m runtime/macos/gpu_metal.m runtime/macos/gpu_metal_overlay.m runtime/gpu_shader.c runtime/gpu_texture.c \
 *     runtime/linux/win32.c runtime/macos/memory.c runtime/macos/platform.c runtime/macos/fiber.c runtime/macos/fiber_arm64.S \
 *     -framework Metal -framework QuartzCore -framework CoreGraphics -framework Foundation -o /tmp/gfxtest/gfxtest
 *   /tmp/gfxtest/gfxtest [-v]          (works in a folder of its own under $TMPDIR: shader cache, screenshot;
 *                                       GFXTEST_DIR=<dir>: that one - run twice, the second start prewarms) */
#define _CRT_SECURE_NO_WARNINGS
#include "macos/gpu_metal.h"
#include <dirent.h>
#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- what the rest of the runtime gives the renderer, here --------------------------------- */
const char *rt_data_dir;
u64 gpu_stat_batches;
static CAMetalLayer *test_layer;
static int verbose;

u64 rt_now_ns(void) {
    static mach_timebase_info_data_t tb;
    static u64 t0;
    if (!tb.denom) { mach_timebase_info(&tb); t0 = mach_absolute_time(); }
    return (mach_absolute_time() - t0) * tb.numer / tb.denom;
}
u64 rt_clock_ns(void) { return rt_now_ns(); }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fprintf(stderr, "\n");
    _exit(2);
}
void watchdog_present(void) {}
void hitch_present(void) {}
void profile_start(double seconds) { (void)seconds; }
int profile_running(void) { return 0; }
void net_status(char *out, size_t n) { if (n) out[0] = 0; }
int pad_describe(char *buf, int size) { if (size) buf[0] = 0; return 0; }
HWND GetForegroundWindow(void) { return NULL; }
void *rt_window(void) { return NULL; }
void *rt_metal_layer(void) { return (__bridge void *)test_layer; }
int display_take_size(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh) {   /* (gpu_display.c's: 16:9, centred) */
    if ((unsigned long long)w * 9 > (unsigned long long)h * 16) { *fh = h; *fw = h * 16 / 9; }
    else { *fw = w; *fh = w * 9 / 16; }
    *x = (w - *fw) / 2;
    *y = (h - *fh) / 2;
}
int display_latency(void) { return 2; }
float display_render_scale(void) { return 1.0f; }   /* (test_scale changes the renderer's: gfx_metal_set_render_scale) */
int display_window_mode(void) { return DISPLAY_WINDOWED; }
void display_set_tearing(int supported) { (void)supported; }
unsigned display_present_flags(void) { return 0; }

/* Guest memory: GPU virtual addresses VA0 + o are guest ALIAS_BASE + o (host G2H of that). */
#define GSIZE (64u << 20)
#define VA0 0x500000000ull
u64 gpu_va_to_guest(u64 va) { return va >= VA0 && va < VA0 + GSIZE ? va - VA0 + ALIAS_BASE : 0; }
static u8 *mem(u64 va) { return P(u8, gpu_va_to_guest(va)); }
static void put32(u64 va, u32 v) { memcpy(mem(va), &v, 4); }
static u32 get32(u64 va) { u32 v; memcpy(&v, mem(va), 4); return v; }

int rt_ww_fault(void *address);   /* (macos/memory.c) a write the write watch takes */
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    if ((sig == SIGBUS || sig == SIGSEGV) && rt_ww_fault(si->si_addr)) return;
    signal(sig, SIG_DFL);   /* (anything else: the default, a crash) */
}

/* the renderer's statistics (gpu_metal.m) */
extern u64 gfx_stat_drawn, gfx_stat_dispatched, gfx_stat_skipped, gfx_stat_shaders, gfx_stat_shader_failures, gfx_stat_presented, gfx_stat_commits,
    gfx_stat_gpu_errors, gfx_stat_gpu_waits, gfx_stat_residents_given_up;
void gfx_hotkey(int n);

/* ---- checks ---------------------------------------------------------------------------------- */
static int checks, failures;
static void check(int ok, const char *fmt, ...) {
    checks++;
    if (ok && !verbose) return;
    va_list a;
    va_start(a, fmt);
    printf("  %s: ", ok ? "ok" : "FAILED");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
    if (!ok) failures++;
}

static int near(u32 a, u32 b, u32 tolerance) { return a > b ? a - b <= tolerance : b - a <= tolerance; }

/* RGBA8 pixel (x, y) of a render target (or its 32-bit depth) */
static u8 rt_pixels[4u << 20];
static u32 rt_w, rt_h, rt_fmt;
static int read_rt(u64 va, int depth) { return gfx_metal_read_surface(gpu_va_to_guest(va), depth, rt_pixels, sizeof rt_pixels, &rt_w, &rt_h, &rt_fmt); }
static u32 px(u32 x, u32 y) { u32 v; memcpy(&v, rt_pixels + ((size_t)y * rt_w + x) * 4, 4); return v; }
static float dpx(u32 x, u32 y) { float v; memcpy(&v, rt_pixels + ((size_t)y * rt_w + x) * 4, 4); return v; }
static int rgba_near(u32 got, u32 want, u32 tolerance) {
    for (int c = 0; c < 32; c += 8)
        if (!near(got >> c & 0xFF, want >> c & 0xFF, tolerance)) return 0;
    return 1;
}
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

/* ---- Maxwell programs (the encodings of tools/mac/msltest.m's assembler) ------------------------ */
static const struct { const char *name, *pattern; } ISA[] = {
    {"ALD", "1110111111011---"}, {"AST", "1110111111110---"}, {"EXIT", "111000110000----"}, {"IPA", "11100000--------"},
    {"IADD_reg", "0101110000010---"}, {"ISCADD_reg", "0101110000011---"}, {"LDG", "1110111011010---"}, {"STG", "1110111011011---"},
    {"MOV_reg", "0101110010011---"}, {"MOV_cbuf", "0100110010011---"}, {"MOV32I", "000000010000----"}, {"NOP", "0101000010110---"},
    {"S2R", "1111000011001---"}, {"SHL_imm", "0011100-01001---"}, {"TEX", "11000-----------"}, {"TLDS", "1101-01---------"},
};
#define B_(v, pos, n) (((u64)(v) & ((1ull << (n)) - 1)) << (pos))
#define D_(r) B_(r, 0, 8)
#define A_(r) B_(r, 8, 8)
#define BR_(r) B_(r, 20, 8)
#define IMM_(v) (B_((u32)(v), 20, 19) | B_((u32)(v) >> 31, 56, 1))
#define CB_(bank, off) (B_(bank, 34, 5) | B_((off) / 4, 20, 14))
#define I32_(v) B_(v, 20, 32)
#define RZ_ 255u
#define SB_(k) (SB_FIRST + 16 * (k))

typedef struct Prog { u8 hdr[0x50]; u64 w[64]; u32 n; u8 bytes[0x400]; u32 size; } Prog;

static u64 opcode(const char *name) {
    for (u32 i = 0; i < sizeof ISA / sizeof *ISA; i++)
        if (!strcmp(ISA[i].name, name)) {
            u64 v = 0;
            for (int b = 0; b < 16; b++) v = v << 1 | (ISA[i].pattern[b] == '1');
            return v << 48;
        }
    rt_fatal("no instruction %s", name);
}
static void I(Prog *p, const char *name, u64 fields) { p->w[p->n++] = opcode(name) | B_(7, 16, 3) | fields; }
static u32 addr_of(u32 i) { return 0x50 + 8 * (4 * (i / 3) + 1 + i % 3); }   /* (every fourth word: scheduling) */
static void assemble(Prog *p) {
    I(p, "EXIT", B_(15, 0, 5));
    u32 groups = (p->n + 2) / 3 + 1;
    p->size = 0x50 + groups * 32;
    memset(p->bytes, 0, sizeof p->bytes);
    memcpy(p->bytes, p->hdr, 0x50);
    u64 nop = opcode("NOP") | B_(7, 16, 3);
    for (u32 i = 0; i < 3 * (groups - 1); i++) { u64 w = i < p->n ? p->w[i] : nop; memcpy(p->bytes + addr_of(i), &w, 8); }
    u64 pad = 0xE2400FFFFF87000Full;   /* the 'BRA to self' the compiler pads with */
    memcpy(p->bytes + 0x50 + 32 * (groups - 1) + 8, &pad, 8);
}
static void hdr32(Prog *p, u32 off, u32 v) { memcpy(p->hdr + off, &v, 4); }
static void ald(Prog *p, u32 r, u32 attr) { I(p, "ALD", D_(r) | A_(RZ_) | B_(attr, 20, 10) | B_(3, 47, 2)); }   /* 4 words */
static void ast(Prog *p, u32 r, u32 attr) { I(p, "AST", D_(r) | A_(RZ_) | B_(attr, 20, 10) | B_(3, 47, 2)); }
static void ipa(Prog *p, u32 r, u32 attr) { I(p, "IPA", D_(r) | A_(RZ_) | BR_(RZ_) | B_(attr / 4, 30, 8)); }

/* vertex: position from attribute 0, attribute 1 out as attribute 0 */
static void vs_pass(Prog *v) {
    memset(v, 0, sizeof *v);
    hdr32(v, 0, 0x61 | 3 << 17);
    ald(v, 0, 0x80); ald(v, 4, 0x90);
    ast(v, 0, 0x70); ast(v, 4, 0x80);
    assemble(v);
}
/* vertex: position from attribute 0, constant buffer 3's words at 0xFF0 out as attribute 0 */
static void vs_cbuf(Prog *v) {
    memset(v, 0, sizeof *v);
    hdr32(v, 0, 0x61 | 3 << 17);
    ald(v, 0, 0x80);
    for (u32 k = 0; k < 4; k++) I(v, "MOV_cbuf", D_(4 + k) | CB_(3, 0xFF0 + 4 * k));
    ast(v, 0, 0x70); ast(v, 4, 0x80);
    assemble(v);
}
/* pixel: attribute 0 to target 0 */
static void ps_color(Prog *f) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    f->hdr[0x18] = 0xAA;
    hdr32(f, 0x48, 0xF);
    for (u32 k = 0; k < 4; k++) ipa(f, k, 0x80 + 4 * k);
    assemble(f);
}
/* pixel: attribute 0 to target 0, (0, 0, 0, 0.25) to target 1 (blending's source 1, when it takes one) */
static void ps_dual(Prog *f) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    f->hdr[0x18] = 0xAA;
    hdr32(f, 0x48, 0xFF);
    for (u32 k = 0; k < 4; k++) ipa(f, k, 0x80 + 4 * k);
    float c1[4] = {0, 0, 0, 0.25f};
    for (u32 k = 0; k < 4; k++) { u32 v; memcpy(&v, &c1[k], 4); I(f, "MOV32I", D_(4 + k) | I32_(v)); }
    assemble(f);
}
/* pixel: the texture whose handle is at 0x40 of the bindless constant buffer, at attribute 0's xy */
static void ps_texture(Prog *f) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    f->hdr[0x18] = 0xAA;
    hdr32(f, 0x48, 0xF);
    ipa(f, 0, 0x80); ipa(f, 1, 0x84);
    I(f, "TEX", D_(0) | A_(0) | BR_(RZ_) | B_(TEX_2D, 28, 3) | B_(15, 31, 4) | B_(0x40 / 4, 36, 13));
    assemble(f);
}
/* pixel: the texture at 0x40 (of a declared type) sampled at level 0 where r0..r3 (constants) say */
static void ps_texture_at(Prog *f, u32 type, float c0, float c1, float c2, u32 layer) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    hdr32(f, 0x48, 0xF);
    float c[3] = {c0, c1, c2};
    u32 r = 0;
    if (type == TEX_ARRAY_2D) I(f, "MOV32I", D_(r++) | I32_(layer));   /* (an array's layer comes first, an integer) */
    for (u32 k = 0; k < 3; k++) { u32 v; memcpy(&v, &c[k], 4); I(f, "MOV32I", D_(r++) | I32_(v)); }
    I(f, "TEX", D_(0) | A_(0) | BR_(RZ_) | B_(type, 28, 3) | B_(15, 31, 4) | B_(0x40 / 4, 36, 13) | B_(1, 55, 3));   /* (LOD zero) */
    assemble(f);
}
/* pixel: its position (IPA of the fragment's x and y: in the game's pixels) to target 0's red and green */
static void ps_position(Prog *f) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    hdr32(f, 0x48, 0xF);
    ipa(f, 0, 0x70); ipa(f, 1, 0x74);
    I(f, "MOV32I", D_(2) | I32_(0));
    I(f, "MOV32I", D_(3) | I32_(0x3F800000u));
    assemble(f);
}
/* pixel: texel (5, 9) of the texture at 0x40 (TLDS: a texel by its numbers, at level 0) to target 0 */
static void ps_texel(Prog *f) {
    memset(f, 0, sizeof *f);
    hdr32(f, 0, 0x62 | 3 << 17);
    hdr32(f, 0x48, 0xF);
    I(f, "MOV32I", D_(4) | I32_(5));
    I(f, "MOV32I", D_(5) | I32_(9));
    I(f, "TLDS", D_(0) | A_(4) | BR_(5) | B_(2, 28, 8) | B_(0x40 / 4, 36, 13) | B_(4, 50, 3) | B_(2, 53, 4) | B_(1, 59, 1));
    assemble(f);
}
/* compute (no header, 32 threads a workgroup): each thread copies its 16 bytes of storage
 * buffer 0 to storage buffer 1 */
static void cs_copy(Prog *p) {
    memset(p, 0, sizeof *p);
    I(p, "S2R", D_(0) | B_(0x21, 20, 8));
    I(p, "S2R", D_(1) | B_(0x25, 20, 8));
    I(p, "MOV_cbuf", D_(30) | CB_(0, SB_(0)));
    I(p, "MOV_cbuf", D_(31) | CB_(0, SB_(0) + 4));
    I(p, "MOV_cbuf", D_(32) | CB_(0, SB_(1)));
    I(p, "MOV_cbuf", D_(33) | CB_(0, SB_(1) + 4));
    I(p, "ISCADD_reg", D_(21) | A_(1) | BR_(0) | B_(5, 39, 5));   /* workgroup * 32 + thread */
    I(p, "SHL_imm", D_(20) | A_(21) | IMM_(4));
    I(p, "IADD_reg", D_(34) | A_(30) | BR_(20));
    I(p, "MOV_reg", D_(35) | BR_(31));
    I(p, "LDG", D_(2) | A_(34) | B_(1, 45, 1) | B_(6, 48, 3));    /* 16 bytes */
    I(p, "IADD_reg", D_(36) | A_(32) | BR_(20));
    I(p, "MOV_reg", D_(37) | BR_(33));
    for (u32 k = 0; k < 4; k++) I(p, "STG", D_(2 + k) | A_(36) | B_(1, 45, 1) | B_(4, 48, 3) | B_(4 * k, 20, 24));
    assemble(p);
}

/* ---- the layout of guest memory --------------------------------------------------------------- */
#define CODE    (VA0 + 0x000000)   /* programs (the program region) */
#define RT_A    (VA0 + 0x100000)   /* 64x64 RGBA8 */
#define RT_T    (VA0 + 0x200000)   /* 16x16 RGBA8 */
#define RT_B    (VA0 + 0x300000)   /* 32x32 RGBA8 (blit destination) */
#define ZT      (VA0 + 0x400000)   /* 64x64 Z24S8 */
#define RT_K    (VA0 + 0x500000)   /* 64x64 RGBA8 (kept constants) */
#define VERTS   (VA0 + 0x600000)
#define INDICES (VA0 + 0x610000)
#define TEXELS  (VA0 + 0x700000)
#define TICS    (VA0 + 0x780000)
#define TSCS    (VA0 + 0x781000)
#define BINDLESS (VA0 + 0x782000)
#define CB3     (VA0 + 0x800000)   /* 4 KB constants */
#define CS_CB0  (VA0 + 0x900000)
#define CS_IN   (VA0 + 0x910000)
#define CS_OUT  (VA0 + 0x920000)
#define CS_COPY (VA0 + 0x930000)
#define RT_D    (VA0 + 0xA00000)   /* 64x64 RGBA8 (dual-source blending's second target) */
#define CS_MANY (VA0 + 0x2000000)  /* resident regions, many */
#define CS_HP_IN  (VA0 + 0xC00000)  /* a resident region in a host page with the CPU's data: its input */
#define CS_HP_OUT (VA0 + 0xD00000)  /* and its output */
#define RT_S    (VA0 + 0xE00000)   /* 640x360 RGBA8 at render scale 2: 1280x720 */
#define RT_F    (VA0 + 0xE10000)   /* 64x64 RGBA32 float at 2 */
#define RT_P    (VA0 + 0xE20000)   /* 16x16 RGBA8 at 2, sampled texel by texel */
#define RT_Q    (VA0 + 0xE30000)   /* 16x16 RGBA8 at 2 */
#define RT_X    (VA0 + 0xE40000)   /* 64x64 RGBA8 at 1.5: 96x96 */
enum { VS_PASS = 0x0000, VS_CBUF = 0x0400, PS_COLOR = 0x0800, PS_TEXTURE = 0x0C00, CS_COPY_PROG = 0x1000, PS_ARRAY = 0x1400,
       PS_CUBE_X = 0x1800, PS_CUBE_NZ = 0x1C00, PS_3D = 0x2000, PS_DUAL = 0x2400, PS_POSITION = 0x2800, PS_TEXEL = 0x2C00 };

static u32 regs[0x1000];
static CbBinding cb[STAGES][18];

static void place(u32 offset, const Prog *p, int compute) {
    if (compute) memcpy(mem(CODE + offset), p->bytes + 0x50, p->size - 0x50);   /* (compute programs have no header) */
    else memcpy(mem(CODE + offset), p->bytes, p->size);
}

static u32 fbits(float f) { u32 v; memcpy(&v, &f, 4); return v; }

static void target(u32 i, u64 va, u32 w, u32 h, u32 format) {
    regs[M_CT(i)] = (u32)(va >> 32); regs[M_CT(i) + 1] = (u32)va; regs[M_CT(i) + 2] = w; regs[M_CT(i) + 3] = h; regs[M_CT(i) + 4] = format;
}
static void viewport(u32 w, u32 h) {
    regs[M_VIEWPORT_SCALE(0)] = fbits(w / 2.0f); regs[M_VIEWPORT_SCALE(0) + 1] = fbits(h / 2.0f); regs[M_VIEWPORT_SCALE(0) + 2] = fbits(1);
    regs[M_VIEWPORT_SCALE(0) + 3] = fbits(w / 2.0f); regs[M_VIEWPORT_SCALE(0) + 4] = fbits(h / 2.0f); regs[M_VIEWPORT_SCALE(0) + 5] = fbits(0);
    regs[M_VIEWPORT_Z_CLIP] = 1;   /* z in [0, w]: depth = z / w */
}
/* one render target (and the depth target or none); the programs */
static void setup(u64 rt, u32 w, u32 h, int depth, u32 vs, u32 ps) {
    memset(regs, 0, sizeof regs);
    memset(cb, 0, sizeof cb);
    target(0, rt, w, h, 0xD5);
    regs[M_CT_SELECT] = 1;
    regs[M_CT_WRITE(0)] = 0x1111;
    viewport(w, h);
    if (depth) {
        regs[M_ZT_A] = (u32)(ZT >> 32); regs[M_ZT_A + 1] = (u32)ZT; regs[M_ZT_A + 2] = 0x14;
        regs[M_ZT_SIZE] = 64; regs[M_ZT_SIZE + 1] = 64;
        regs[M_ZT_SELECT] = 1;
    }
    regs[M_PROGRAM_REGION] = (u32)(CODE >> 32); regs[M_PROGRAM_REGION + 1] = (u32)CODE;
    regs[M_PIPELINE(1)] = 1 | 1 << 4; regs[M_PIPELINE(1) + 1] = vs;
    regs[M_PIPELINE(5)] = 1 | 5 << 4; regs[M_PIPELINE(5) + 1] = ps;
    regs[M_BEGIN] = 4;   /* triangles */
}
/* vertex stream 0 at va, stride 32: attribute 0 its first 16 bytes, attribute 1 the next (floats) */
static void stream(u64 va, u32 vertices) {
    regs[M_VERTEX_ATTRIBUTE(0)] = 0 | 0 << 7 | 0x01 << 21 | 7u << 27;
    regs[M_VERTEX_ATTRIBUTE(1)] = 0 | 16 << 7 | 0x01 << 21 | 7u << 27;
    regs[M_VERTEX_STREAM(0)] = 32 | 1 << 12;
    regs[M_VERTEX_STREAM(0) + 1] = (u32)(va >> 32); regs[M_VERTEX_STREAM(0) + 2] = (u32)va;
    u64 limit = va + 32ull * vertices - 1;
    regs[M_VERTEX_STREAM_LIMIT(0)] = (u32)(limit >> 32); regs[M_VERTEX_STREAM_LIMIT(0) + 1] = (u32)limit;
}
static void vertex(u64 va, u32 i, float x, float y, float z, const float a[4]) {
    float v[8] = {x, y, z, 1, a[0], a[1], a[2], a[3]};
    memcpy(mem(va + 32ull * i), v, sizeof v);
}

/* A draw, again until it is drawn (its programs and pipeline compile meanwhile; 20 s at most). Each
 * is a submission of its own (gpu.c counts them): the renderer takes memory a submission reads to
 * be unchanged by the CPU until its end, as the console's GPU reads it later - and the tests change
 * vertices between draws. */
static int draw(int indexed) {
    gpu_stat_batches++;
    for (int i = 0; i < 4000; i++) {
        u64 before = gfx_stat_drawn;
        gfx_draw(regs, (const CbBinding (*)[18])cb, indexed);
        if (gfx_stat_drawn != before) return 1;
        usleep(5000);
    }
    printf("  (a draw was never drawn: %llu shaders ready, %llu failed)\n", gfx_stat_shaders, gfx_stat_shader_failures);
    return 0;
}

static void clear_color(u32 rt_index, float r, float g, float b, float a, u32 channels) {
    regs[0x360] = fbits(r); regs[0x361] = fbits(g); regs[0x362] = fbits(b); regs[0x363] = fbits(a);
    gfx_clear(regs, channels << 2 | rt_index << 6);
}

/* ---- the tests ---------------------------------------------------------------------------------- */
static void test_clears(void) {
    printf("clears\n");
    setup(RT_A, 64, 64, 1, VS_PASS, PS_COLOR);
    clear_color(0, 0.25f, 0.5f, 0.75f, 1.0f, 0xF);
    check(read_rt(RT_A, 0) && rt_w == 64 && rt_h == 64, "the target exists, 64x64 (%ux%u, format %u)", rt_w, rt_h, rt_fmt);
    int all = 1;
    for (u32 y = 0; y < 64; y++) for (u32 x = 0; x < 64; x++) all &= rgba_near(px(x, y), RGBA(64, 128, 191, 255), 1);
    check(all, "every pixel the clear colour (%08x)", px(0, 0));
    clear_color(0, 1.0f, 0.0f, 0.0f, 0.0f, 0x1 | 0x8);   /* red and alpha only */
    read_rt(RT_A, 0);
    check(rgba_near(px(33, 17), RGBA(255, 128, 191, 0), 1), "a clear of red and alpha leaves green and blue (%08x)", px(33, 17));
    regs[0x364] = fbits(0.5f);
    regs[0x368] = 0x37;
    gfx_clear(regs, 3);
    check(read_rt(ZT, 1) && rt_w == 64, "the depth target exists (format %u)", rt_fmt);
    check(dpx(10, 50) == 0.5f, "depth cleared to 0.5 (%g)", dpx(10, 50));
}

static const float RED[4] = {1, 0, 0, 1}, GREEN[4] = {0, 1, 0, 1}, BLUE[4] = {0, 0, 1, 1};

static void test_triangles(void) {
    printf("draws: vertex streams, indices, points, depth and stencil\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    /* the upper-left half (NDC y up: the viewport's y scale is positive, so +1 is the bottom row) */
    vertex(VERTS, 0, -1, -1, 0.5f, GREEN); vertex(VERTS, 1, 1, -1, 0.5f, GREEN); vertex(VERTS, 2, -1, 1, 0.5f, GREEN);
    vertex(VERTS, 3, 1, 1, 0.5f, BLUE);
    stream(VERTS, 4);
    regs[M_VERTEX_ARRAY_START] = 0; regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a triangle drawn");
    read_rt(RT_A, 0);
    check(px(10, 10) == RGBA(0, 255, 0, 255), "inside it: green (%08x)", px(10, 10));
    check(px(60, 60) == RGBA(0, 0, 0, 255), "outside it: the clear colour (%08x)", px(60, 60));
    /* the other half, by 8-bit indices (vertex 3's colour is provoking? no: interpolated - checked near it) */
    u8 ix[3] = {1, 3, 2};
    memcpy(mem(INDICES), ix, 3);
    regs[M_INDEX_BUFFER] = (u32)(INDICES >> 32); regs[M_INDEX_BUFFER + 1] = (u32)INDICES;
    regs[M_INDEX_BUFFER + 4] = 0; regs[M_INDEX_BUFFER + 5] = 0; regs[M_DRAW_INDEX_COUNT] = 3;
    check(draw(1), "an indexed triangle (8-bit indices) drawn");
    read_rt(RT_A, 0);
    check(rgba_near(px(62, 62), RGBA(0, 8, 247, 255), 12), "near its blue vertex: blue (%08x)", px(62, 62));
    check(px(10, 10) == RGBA(0, 255, 0, 255), "the first triangle still there (%08x)", px(10, 10));
    /* points: a pixel each */
    clear_color(0, 0, 0, 0, 1, 0xF);
    vertex(VERTS, 0, (5.5f / 64) * 2 - 1, (50.5f / 64) * 2 - 1, 0.5f, RED);
    regs[M_BEGIN] = 0;
    regs[M_VERTEX_ARRAY_COUNT] = 1;
    check(draw(0), "a point drawn");
    read_rt(RT_A, 0);
    check(px(5, 50) == RGBA(255, 0, 0, 255) && px(6, 50) == RGBA(0, 0, 0, 255) && px(5, 51) == RGBA(0, 0, 0, 255),
          "one pixel, at (5, 50) (%08x; beside it %08x %08x)", px(5, 50), px(6, 50), px(5, 51));

    /* depth (Z24S8, which is 32-bit float depth with stencil here) and stencil */
    setup(RT_A, 64, 64, 1, VS_PASS, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    regs[0x364] = fbits(1.0f); regs[0x368] = 0;
    gfx_clear(regs, 3);
    regs[M_DEPTH_TEST] = 1; regs[M_DEPTH_WRITE] = 1; regs[M_DEPTH_FUNC] = 0x201;   /* less */
    regs[M_STENCIL_TEST] = 1; regs[M_STENCIL_FUNC] = 0x207; regs[M_STENCIL_REF] = 7; regs[M_STENCIL_FUNC_MASK] = 0xFF; regs[M_STENCIL_MASK] = 0xFF;
    regs[M_STENCIL_OP_FAIL] = 0x1E00; regs[M_STENCIL_OP_ZFAIL] = 0x1E00; regs[M_STENCIL_OP_ZPASS] = 0x1E01;   /* replace where drawn */
    vertex(VERTS, 0, -1, -1, 0.25f, RED); vertex(VERTS, 1, 1, -1, 0.25f, RED); vertex(VERTS, 2, -1, 1, 0.25f, RED);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a triangle at depth 0.25 drawn");
    vertex(VERTS, 0, -1, -1, 0.75f, GREEN); vertex(VERTS, 1, 3, -1, 0.75f, GREEN); vertex(VERTS, 2, -1, 3, 0.75f, GREEN);
    regs[M_STENCIL_OP_ZPASS] = 0x1E00;   /* (keep) */
    check(draw(0), "a full-target triangle at depth 0.75 drawn");
    read_rt(RT_A, 0);
    check(px(10, 10) == RGBA(255, 0, 0, 255), "behind the first: hidden (%08x)", px(10, 10));
    check(px(60, 60) == RGBA(0, 255, 0, 255), "elsewhere: drawn (%08x)", px(60, 60));
    read_rt(ZT, 1);
    check(dpx(10, 10) == 0.25f && dpx(60, 60) == 0.75f, "depth 0.25 and 0.75 (%g, %g)", dpx(10, 10), dpx(60, 60));
    /* stencil: the first triangle wrote 7; a nearer full-target one where the stencil is 7 */
    setup(RT_A, 64, 64, 1, VS_PASS, PS_COLOR);
    regs[M_DEPTH_TEST] = 1; regs[M_DEPTH_FUNC] = 0x201;
    regs[M_STENCIL_TEST] = 1; regs[M_STENCIL_FUNC] = 0x202; regs[M_STENCIL_REF] = 7; regs[M_STENCIL_FUNC_MASK] = 0xFF; regs[M_STENCIL_MASK] = 0;
    regs[M_STENCIL_OP_FAIL] = regs[M_STENCIL_OP_ZFAIL] = regs[M_STENCIL_OP_ZPASS] = 0x1E00;
    vertex(VERTS, 0, -1, -1, 0.1f, BLUE); vertex(VERTS, 1, 3, -1, 0.1f, BLUE); vertex(VERTS, 2, -1, 3, 0.1f, BLUE);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a stencil-tested triangle drawn");
    read_rt(RT_A, 0);
    check(px(10, 10) == RGBA(0, 0, 255, 255) && px(60, 60) == RGBA(0, 255, 0, 255), "drawn only where the stencil is 7 (%08x, %08x)", px(10, 10),
          px(60, 60));
}

/* the byte offset of (x bytes, y) in a block-linear image one GOB high a block (gpu_texture.c's layout) */
static u32 gob_offset(u32 x, u32 y) {
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
static u32 block_linear(u32 x, u32 y, u32 row_bytes) {   /* (block height 1 GOB) */
    u32 gobs_x = (row_bytes + 63) / 64;
    return (y / 8) * gobs_x * 512 + (x / 64) * 512 + gob_offset(x, y);
}

/* texture header 1 and sampler 1: a w x h x depth (layers) texture of `format` (component types
 * `type`) and header type tic_type (TIC_*) at TEXELS */
static void texture_header_of(u32 format, u32 type, u32 w, u32 h, u32 tic_type, u32 depth) {
    u32 tic[8] = {0};
    tic[0] = format | type << 7 | type << 10 | type << 13 | type << 16 | (2u | 3u << 3 | 4u << 6 | 5u << 9) << 19;
    tic[1] = (u32)TEXELS;
    tic[2] = (u32)(TEXELS >> 32) | 3u << 21;   /* block linear */
    tic[3] = 0;                                /* one GOB a block; one level */
    tic[4] = (w - 1) | tic_type << 23;
    tic[5] = (h - 1) | (depth - 1) << 16;
    memcpy(mem(TICS + 32), tic, 32);
    u32 tsc[8] = {2 | 2 << 3 | 2 << 6, 1 | 1 << 4 | 1 << 6, 0};   /* clamp; nearest, no mips */
    memcpy(mem(TSCS + 32), tsc, 32);
}
static void texture_header(u32 format, u32 type, u32 w, u32 h) { texture_header_of(format, type, w, h, TIC_2D, 1); }
static void texture_address(u64 va) { put32(TICS + 32 + 4, (u32)va); put32(TICS + 32 + 8, (u32)(va >> 32) | 3u << 21); }

static void textured_setup(void) {
    setup(RT_T, 16, 16, 0, VS_PASS, PS_TEXTURE);
    float uv0[4] = {0, 0, 0, 0}, uv1[4] = {2, 0, 0, 0}, uv2[4] = {0, 2, 0, 0};
    vertex(VERTS, 0, -1, -1, 0.5f, uv0); vertex(VERTS, 1, 3, -1, 0.5f, uv1); vertex(VERTS, 2, -1, 3, 0.5f, uv2);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_TEX_HEADER_POOL] = (u32)(TICS >> 32); regs[M_TEX_HEADER_POOL + 1] = (u32)TICS;
    regs[M_TEX_SAMPLER_POOL] = (u32)(TSCS >> 32); regs[M_TEX_SAMPLER_POOL + 1] = (u32)TSCS;
    regs[M_BINDLESS_TEXTURE] = 2;
    put32(BINDLESS + 0x40, 1 | 1u << 20);   /* texture header 1, sampler 1 */
    cb[STAGE_FRAGMENT][2] = (CbBinding){BINDLESS, 0x100};
}

static u32 texel(u32 x, u32 y, u32 version) { return RGBA(x * 16, y * 16, 128 + version * 64, 255); }

static void write_rgba8_texture(u32 version) {
    for (u32 y = 0; y < 16; y++)
        for (u32 x = 0; x < 16; x++) put32(TEXELS + block_linear(x * 4, y, 64), texel(x, y, version));
}

static void test_textures(void) {
    printf("textures from guest memory\n");
    textured_setup();
    texture_header(0x08, 2, 16, 16);
    write_rgba8_texture(0);
    check(draw(0), "a textured draw");
    read_rt(RT_T, 0);
    int all = 1;
    for (u32 y = 0; y < 16; y++) for (u32 x = 0; x < 16; x++) all &= px(x, y) == texel(x, y, 0);
    check(all, "each pixel its texel of the block-linear RGBA8 texture (%08x at 3,5, want %08x)", px(3, 5), texel(3, 5, 0));

    /* the CPU writes the texture: the write watch sees it, the next frame's draw takes it again */
    write_rgba8_texture(1);
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    check(draw(0), "the draw again, a frame later");
    read_rt(RT_T, 0);
    check(px(7, 9) == texel(7, 9, 1), "the texture as the CPU wrote it since (%08x, want %08x)", px(7, 9), texel(7, 9, 1));

    /* packed 16-bit formats: Metal's bits differ (B4G4R4A4: converted) or match (B5G5R5A1, B5G6R5) */
    static const struct { u32 format; u16 v; u32 want; const char *name; } F[] = {
        {0x12, (u16)(1 | 2 << 4 | 3 << 8 | 4 << 12), RGBA(3 * 17, 2 * 17, 1 * 17, 4 * 17), "B4G4R4A4"},
        {0x14, (u16)(31 | 0 << 5 | 16 << 10 | 1u << 15), RGBA(132, 0, 255, 255), "B5G5R5A1"},
        {0x15, (u16)(0 | 63 << 5 | 31 << 11), RGBA(255, 255, 0, 255), "B5G6R5"},
    };
    for (u32 i = 0; i < 3; i++) {
        texture_header(F[i].format, 2, 16, 16);
        for (u32 y = 0; y < 16; y++) for (u32 x = 0; x < 16; x++) memcpy(mem(TEXELS + block_linear(x * 2, y, 32)), &F[i].v, 2);
        gfx_present(gpu_va_to_guest(RT_T), 16, 16);   /* (a frame: the texture is asked about again) */
        check(draw(0), "a draw with a %s texture", F[i].name);
        read_rt(RT_T, 0);
        check(rgba_near(px(5, 5), F[i].want, 1), "%s: %08x, want %08x", F[i].name, px(5, 5), F[i].want);
    }
    /* BC1, 10x10 (not a multiple of 4: kept so, not padded): red blocks */
    texture_header(0x24, 2, 10, 10);
    for (u32 by = 0; by < 3; by++)
        for (u32 bx = 0; bx < 3; bx++) {
            u8 block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};   /* colour 0 = colour 1 = red (565), every index 0 */
            memcpy(mem(TEXELS + block_linear(bx * 8, by, 24)), block, 8);
        }
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    check(draw(0), "a draw with a BC1 texture");
    read_rt(RT_T, 0);
    check(px(1, 1) == RGBA(255, 0, 0, 255) && px(14, 14) == RGBA(255, 0, 0, 255), "BC1 10x10: red throughout (%08x, %08x)", px(1, 1), px(14, 14));
    /* integers (RGBA8 uint): widened to 32 bits a channel, read as the floats of those bits, into a
     * uint target the same bits again */
    texture_header(0x08, 4, 16, 16);
    for (u32 y = 0; y < 16; y++) for (u32 x = 0; x < 16; x++) put32(TEXELS + block_linear(x * 4, y, 64), RGBA(x, y, 200, 255));
    target(0, RT_K + 0xC0000, 16, 16, 0xC5);   /* RGBA32 uint */
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    check(draw(0), "a draw with an 8-bit uint texture into a uint target");
    read_rt(RT_K + 0xC0000, 0);
    u32 w4[4];
    memcpy(w4, rt_pixels + (9 * 16 + 6) * 16, 16);
    check(w4[0] == 6 && w4[1] == 9 && w4[2] == 200 && w4[3] == 255, "the texels' integers, bit for bit (%u %u %u %u)", w4[0], w4[1], w4[2], w4[3]);
    target(0, RT_T, 16, 16, 0xD5);
    /* depth in guest memory (Z24S8, format 0x29): its 24 bits as a float */
    texture_header(0x29, 2, 16, 16);
    for (u32 y = 0; y < 16; y++) for (u32 x = 0; x < 16; x++) put32(TEXELS + block_linear(x * 4, y, 64), 0xAB000000u | 0x800000u);
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    check(draw(0), "a draw with a Z24S8 texture");
    read_rt(RT_T, 0);
    check(near(px(8, 8) & 0xFF, 128, 1) && (px(8, 8) >> 8 & 0xFFFF) == 0, "Z24 0x800000 reads 0.5 (%08x)", px(8, 8));
}

/* Arrays, cube maps and 3D textures, each sampled as its own type (and an array as 2D: its first layer). */
static void test_texture_types(void) {
    printf("textures: arrays, cube maps, 3D\n");
    textured_setup();
    /* a 4x4 array of 3 layers (512 bytes each: a GOB), layer L red L * 80 */
    texture_header_of(0x08, 2, 4, 4, TIC_2D_ARRAY, 3);
    for (u32 l = 0; l < 3; l++)
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) put32(TEXELS + 512 * l + block_linear(x * 4, y, 16), RGBA(l * 80, 10, 20, 255));
    regs[M_PIPELINE(5) + 1] = PS_ARRAY;
    check(draw(0), "an array texture's layer 2");
    read_rt(RT_T, 0);
    check(px(3, 3) == RGBA(160, 10, 20, 255), "layer 2 (%08x)", px(3, 3));
    regs[M_PIPELINE(5) + 1] = PS_TEXTURE;
    check(draw(0), "the array as a 2D texture");
    read_rt(RT_T, 0);
    check(px(3, 3) == RGBA(0, 10, 20, 255), "its first layer (%08x)", px(3, 3));
    /* a 4x4 cube map, face F red F * 40 */
    texture_header_of(0x08, 2, 4, 4, TIC_CUBE, 1);
    for (u32 f = 0; f < 6; f++)
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) put32(TEXELS + 512 * f + block_linear(x * 4, y, 16), RGBA(f * 40, 200, 0, 255));
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);   /* (a frame: the memory is asked about again) */
    regs[M_PIPELINE(5) + 1] = PS_CUBE_X;
    check(draw(0), "a cube map towards +x");
    read_rt(RT_T, 0);
    check(px(3, 3) == RGBA(0, 200, 0, 255), "face 0 (%08x)", px(3, 3));
    regs[M_PIPELINE(5) + 1] = PS_CUBE_NZ;
    check(draw(0), "a cube map towards -z");
    read_rt(RT_T, 0);
    check(px(3, 3) == RGBA(200, 200, 0, 255), "face 5 (%08x)", px(3, 3));
    /* a 4x4x4 3D texture (one GOB a slice), slice Z red Z * 60 */
    texture_header_of(0x08, 2, 4, 4, TIC_3D, 4);
    for (u32 z = 0; z < 4; z++)
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) put32(TEXELS + 512 * z + block_linear(x * 4, y, 16), RGBA(z * 60, 0, 100, 255));
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    regs[M_PIPELINE(5) + 1] = PS_3D;
    check(draw(0), "a 3D texture");
    read_rt(RT_T, 0);
    check(px(3, 3) == RGBA(120, 0, 100, 255), "slice 2 (%08x)", px(3, 3));
}

/* Render targets as textures: the depth target the depth test wrote (Z24S8: 32-bit float depth
 * here), read as depth; and a target sampled by a draw into it, which reads nothing (Direct3D
 * unbinds a bound target's view). */
static void test_targets_as_textures(void) {
    printf("render targets as textures\n");
    textured_setup();
    setup(RT_A, 64, 64, 0, VS_PASS, PS_TEXTURE);
    float uv0[4] = {0, 0, 0, 0}, uv1[4] = {2, 0, 0, 0}, uv2[4] = {0, 2, 0, 0};
    vertex(VERTS, 0, -1, -1, 0.5f, uv0); vertex(VERTS, 1, 3, -1, 0.5f, uv1); vertex(VERTS, 2, -1, 3, 0.5f, uv2);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_TEX_HEADER_POOL] = (u32)(TICS >> 32); regs[M_TEX_HEADER_POOL + 1] = (u32)TICS;
    regs[M_TEX_SAMPLER_POOL] = (u32)(TSCS >> 32); regs[M_TEX_SAMPLER_POOL + 1] = (u32)TSCS;
    regs[M_BINDLESS_TEXTURE] = 2;
    cb[STAGE_FRAGMENT][2] = (CbBinding){BINDLESS, 0x100};
    texture_header(0x29, 2, 64, 64);   /* (Z24S8, the depth target's size) */
    texture_address(ZT);
    check(draw(0), "a draw sampling the depth target");
    read_rt(RT_A, 0);
    check(rgba_near(px(10, 10), RGBA(64, 0, 0, 255), 1) && rgba_near(px(60, 60), RGBA(191, 0, 0, 255), 1),
          "its depth in red, 0.25 and 0.75, as Direct3D reads it (%08x, %08x)", px(10, 10), px(60, 60));
    texture_header(0x08, 2, 64, 64);
    texture_address(RT_A);
    check(draw(0), "a draw sampling its own target");
    read_rt(RT_A, 0);
    check(px(30, 30) == 0, "it reads nothing (%08x)", px(30, 30));
}

static void test_kept_constants(void) {
    printf("constants kept on the GPU, changed by the command stream between draws\n");
    setup(RT_K, 64, 64, 0, VS_CBUF, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    float left[4] = {0}, right[4] = {0};
    vertex(VERTS, 0, -1, -1, 0.5f, left); vertex(VERTS, 1, 0, -1, 0.5f, left); vertex(VERTS, 2, -1, 1, 0.5f, left);
    vertex(VERTS, 3, 0.01f, -1, 0.5f, right); vertex(VERTS, 4, 1, -1, 0.5f, right); vertex(VERTS, 5, 1, 1, 0.5f, right);
    stream(VERTS, 6);
    memset(mem(CB3), 0, 0x1000);
    memcpy(mem(CB3 + 0xFF0), RED, 16);
    cb[STAGE_VERTEX][3] = (CbBinding){CB3, 0x1000};
    regs[M_VERTEX_ARRAY_START] = 0; regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a draw with 4 KB of constants (made ready)");
    clear_color(0, 0, 0, 0, 1, 0xF);
    gpu_stat_batches++;   /* (a submission: the write watch is asked again) */
    check(draw(0), "the left triangle, its colour from the constants");
    memcpy(mem(CB3 + 0xFF0), BLUE, 16);   /* (a constant load in the command stream: guest memory, then the hook) */
    gfx_stream_wrote(CB3 + 0xFF0, 16);
    regs[M_VERTEX_ARRAY_START] = 3;
    check(draw(0), "the right triangle, after the command stream changed them");
    read_rt(RT_K, 0);
    check(px(5, 5) == RGBA(255, 0, 0, 255), "the left one red, as the constants were for it (%08x)", px(5, 5));
    check(px(60, 5) == RGBA(0, 0, 255, 255), "the right one blue (%08x)", px(60, 5));
    /* the CPU writes them (seen by the write watch at the next submission) */
    memcpy(mem(CB3 + 0xFF0), GREEN, 16);
    gpu_stat_batches++;
    check(draw(0), "the right triangle again, after the CPU wrote the constants");
    read_rt(RT_K, 0);
    check(px(60, 5) == RGBA(0, 255, 0, 255), "green now (%08x)", px(60, 5));
}

static void test_state(void) {
    printf("draws: blending, scissor, write mask, culling, sRGB and integer targets\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0, 1, 0, 1, 0xF);
    float half_red[4] = {1, 0, 0, 0.5f};
    vertex(VERTS, 0, -1, -1, 0.5f, half_red); vertex(VERTS, 1, 3, -1, 0.5f, half_red); vertex(VERTS, 2, -1, 3, 0.5f, half_red);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_BLEND_ENABLE(0)] = 1;
    regs[M_BLEND_COLOR_OP] = 0x8006; regs[M_BLEND_COLOR_SRC] = 0x4302; regs[M_BLEND_COLOR_DST] = 0x4303;   /* add; source alpha, 1 - source alpha */
    regs[M_SCISSOR(0)] = 1; regs[M_SCISSOR(0) + 1] = 8 | 40u << 16; regs[M_SCISSOR(0) + 2] = 16 | 48u << 16;
    regs[M_CT_WRITE(0)] = 0x0111;   /* not alpha */
    check(draw(0), "a blended, scissored, masked draw");
    read_rt(RT_A, 0);
    check(rgba_near(px(20, 20), RGBA(128, 128, 0, 255), 1), "inside the scissor: half red over green, alpha kept (%08x)", px(20, 20));
    check(px(4, 20) == RGBA(0, 255, 0, 255) && px(20, 50) == RGBA(0, 255, 0, 255), "outside it: untouched (%08x, %08x)", px(4, 20), px(20, 50));
    /* culling back faces: the triangle runs clockwise on the target (as Direct3D sees it) */
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    vertex(VERTS, 0, -1, -1, 0.5f, RED); vertex(VERTS, 1, 3, -1, 0.5f, RED); vertex(VERTS, 2, -1, 3, 0.5f, RED);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    stream(VERTS, 3);
    regs[M_CULL_ENABLE] = 1; regs[M_CULL_FACE] = 0x405;
    regs[M_FRONT_FACE] = 0x900;
    check(draw(0), "a draw with back faces culled, front faces clockwise in GL's terms");
    read_rt(RT_A, 0);
    check(px(30, 30) == RGBA(0, 0, 0, 255), "culled (%08x)", px(30, 30));
    regs[M_FRONT_FACE] = 0x901;
    check(draw(0), "again with front faces counter-clockwise");
    read_rt(RT_A, 0);
    check(px(30, 30) == RGBA(255, 0, 0, 255), "drawn (%08x)", px(30, 30));
    /* an sRGB target: the bytes stored are sRGB */
    setup(RT_K + 0x80000, 64, 64, 0, VS_PASS, PS_COLOR);
    target(0, RT_K + 0x80000, 64, 64, 0xD6);
    float grey[4] = {0.5f, 0.5f, 0.5f, 1};
    vertex(VERTS, 0, -1, -1, 0.5f, grey); vertex(VERTS, 1, 3, -1, 0.5f, grey); vertex(VERTS, 2, -1, 3, 0.5f, grey);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a draw into an sRGB target");
    read_rt(RT_K + 0x80000, 0);
    check(rgba_near(px(9, 9), RGBA(188, 188, 188, 255), 1), "0.5 stored as sRGB 188 (%08x)", px(9, 9));
    /* an integer target: the program's bits, as Direct3D writes a float output to it */
    setup(RT_K + 0xC0000, 16, 16, 0, VS_PASS, PS_COLOR);
    target(0, RT_K + 0xC0000, 16, 16, 0xC5);   /* RGBA32 uint */
    float ints[4] = {1.0f, -2.0f, 0.5f, 3.0f};
    vertex(VERTS, 0, -1, -1, 0.5f, ints); vertex(VERTS, 1, 3, -1, 0.5f, ints); vertex(VERTS, 2, -1, 3, 0.5f, ints);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a draw into a uint target");
    read_rt(RT_K + 0xC0000, 0);
    u32 got[4];
    memcpy(got, rt_pixels + (5 * 16 + 5) * 16, 16);
    check(got[0] == 0x3F800000u && got[1] == 0xC0000000u && got[2] == 0x3F000000u && got[3] == 0x40400000u, "its words the floats' bits (%08x %08x %08x %08x)",
          got[0], got[1], got[2], got[3]);
}

/* Dual-source blending (target 0 blended by the program's second source: its target 1 output).
 * Metal takes it with one colour attachment alone, and aborts the process at the pipeline
 * otherwise: a draw with more targets bound draws into target 0 alone, and a source-1 factor on
 * another target reads source 1 as 0. */
static void test_dual_source(void) {
    printf("dual-source blending\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_DUAL);
    target(1, RT_D, 64, 64, 0xD5);
    regs[M_CT_WRITE(1)] = 0x1111;
    clear_color(0, 0, 1, 0, 1, 0xF);   /* green */
    clear_color(1, 0, 0, 1, 1, 0xF);   /* blue */
    vertex(VERTS, 0, -1, -1, 0.5f, RED); vertex(VERTS, 1, 3, -1, 0.5f, RED); vertex(VERTS, 2, -1, 3, 0.5f, RED);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_BLEND_ENABLE(0)] = 1;
    regs[M_BLEND_COLOR_OP] = 0x8006; regs[M_BLEND_COLOR_SRC] = 0xC902; regs[M_BLEND_COLOR_DST] = 0xC903;   /* add; source 1 alpha, 1 - it */
    check(draw(0), "a dual-source draw into one target");
    read_rt(RT_A, 0);
    check(rgba_near(px(20, 20), RGBA(64, 191, 0, 255), 1), "red over green by source 1's alpha, 0.25 (%08x)", px(20, 20));
    /* the same with a second target bound, blending alike (one blend state for all) */
    clear_color(0, 0, 1, 0, 1, 0xF);
    regs[M_CT_SELECT] = 2 | 1 << 7;
    regs[M_BLEND_ENABLE(1)] = 1;
    check(draw(0), "a dual-source draw with two targets bound");
    read_rt(RT_A, 0);
    check(rgba_near(px(20, 20), RGBA(64, 191, 0, 255), 1), "target 0 blended as before (%08x)", px(20, 20));
    read_rt(RT_D, 0);
    check(px(20, 20) == RGBA(0, 0, 255, 255), "target 1 not drawn (%08x)", px(20, 20));
    /* source 1 on target 1 alone (blend state per target), target 0 not blended */
    clear_color(1, 0, 0, 1, 1, 0xF);
    regs[M_BLEND_ENABLE(0)] = 0;
    regs[M_BLEND_STATE_PER_TARGET] = 1;
    u32 b = M_BLEND_PER_TARGET(1);
    regs[b] = 0; regs[b + 1] = 0x8006; regs[b + 2] = 0xC902; regs[b + 3] = 0xC903;
    check(draw(0), "a draw with source 1 in target 1's blending");
    read_rt(RT_A, 0);
    check(px(20, 20) == RGBA(255, 0, 0, 255), "target 0 red (%08x)", px(20, 20));
    read_rt(RT_D, 0);
    check(px(20, 20) == RGBA(0, 0, 255, 255), "target 1 as it was: source 1 read as 0 (%08x)", px(20, 20));
}

/* Upload memory over its budget (1.5 GB in flight): chunks too small for a big upload are given
 * up for it, while allocations kept for reuse still name them - found dead, and made again. */
static void test_arena(void) {
    printf("upload memory over its budget\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    /* attribute 1 from a small stream of its own (stream 1): kept for reuse */
    vertex(VERTS, 0, -1, -1, 0.5f, GREEN); vertex(VERTS, 1, 3, -1, 0.5f, GREEN); vertex(VERTS, 2, -1, 3, 0.5f, GREEN);
    regs[M_VERTEX_ATTRIBUTE(0)] = 0 | 0 << 7 | 0x01 << 21 | 7u << 27;
    regs[M_VERTEX_ATTRIBUTE(1)] = 1 | 16 << 7 | 0x01 << 21 | 7u << 27;
    for (u32 j = 0; j < 2; j++) {
        regs[M_VERTEX_STREAM(j)] = 32 | 1 << 12;
        regs[M_VERTEX_STREAM(j) + 1] = (u32)(VERTS >> 32); regs[M_VERTEX_STREAM(j) + 2] = (u32)VERTS;
        regs[M_VERTEX_STREAM_LIMIT(j)] = (u32)((VERTS + 95) >> 32); regs[M_VERTEX_STREAM_LIMIT(j) + 1] = (u32)(VERTS + 95);
    }
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a draw, attribute 1 from a small stream");
    gfx_present(gpu_va_to_guest(RT_A), 64, 64);
    read_rt(RT_A, 0);   /* (waits for the GPU: every chunk idle) */
    /* attribute 0 from a 60 MB stream, attribute 1 off, over and over in one command buffer */
    u64 big = VA0 + 0x400000, waits0 = gfx_stat_gpu_waits;
    regs[M_VERTEX_STREAM(0) + 1] = (u32)(big >> 32); regs[M_VERTEX_STREAM(0) + 2] = (u32)big;
    regs[M_VERTEX_STREAM_LIMIT(0)] = (u32)((big + 0x3C00000 - 1) >> 32); regs[M_VERTEX_STREAM_LIMIT(0) + 1] = (u32)(big + 0x3C00000 - 1);
    regs[M_VERTEX_STREAM(1)] = 32;
    for (int i = 0; i < 30; i++) { gpu_stat_batches++; gfx_draw(regs, (const CbBinding (*)[18])cb, 0); }
    gfx_present(gpu_va_to_guest(RT_A), 64, 64);
    check(gfx_stat_gpu_waits > waits0, "30 draws of 60 MB each: past the budget (%llu GPU waits)", gfx_stat_gpu_waits - waits0);
    /* the first draw again: attribute 1's slot asks about its allocation, in a chunk given up */
    regs[M_VERTEX_STREAM(0)] = 32 | 1 << 12;
    regs[M_VERTEX_STREAM(0) + 1] = (u32)(VERTS >> 32); regs[M_VERTEX_STREAM(0) + 2] = (u32)VERTS;
    regs[M_VERTEX_STREAM_LIMIT(0)] = (u32)((VERTS + 95) >> 32); regs[M_VERTEX_STREAM_LIMIT(0) + 1] = (u32)(VERTS + 95);
    regs[M_VERTEX_STREAM(1)] = 32 | 1 << 12;
    clear_color(0, 0, 0, 0, 1, 0xF);
    check(draw(0), "the small draw again");
    read_rt(RT_A, 0);
    check(px(10, 10) == RGBA(0, 255, 0, 255), "its colour from attribute 1, uploaded again (%08x)", px(10, 10));
}

static void test_blit(void) {
    printf("the 2D engine\n");
    textured_setup();
    texture_header(0x08, 2, 16, 16);
    write_rgba8_texture(0);
    gfx_present(gpu_va_to_guest(RT_T), 16, 16);
    check(draw(0), "the source drawn");
    u32 r[0x400] = {0};
    r[0x80] = 0xD5; r[0x86] = 32; r[0x87] = 32; r[0x88] = (u32)(RT_B >> 32); r[0x89] = (u32)RT_B;
    r[0x8C] = 0xD5; r[0x92] = 16; r[0x93] = 16; r[0x94] = (u32)(RT_T >> 32); r[0x95] = (u32)RT_T;
    r[0x22C] = 0; r[0x22D] = 0; r[0x22E] = 32; r[0x22F] = 32;
    r[0x230] = 0x80000000u; r[0x231] = 0; r[0x232] = 0x80000000u; r[0x233] = 0;   /* half a source pixel a destination pixel */
    gfx_blit2d(r);
    check(read_rt(RT_B, 0) && rt_w == 32, "the destination made, 32x32");
    int all = 1;
    for (u32 y = 0; y < 32; y++) for (u32 x = 0; x < 32; x++) all &= px(x, y) == texel(x / 2, y / 2, 0);
    check(all, "the source scaled up twice, nearest (%08x at 9,13, want %08x)", px(9, 13), texel(4, 6, 0));
}

static void dispatch(u32 groups) {
    u32 q[64] = {0};
    q[8] = CS_COPY_PROG;
    q[12] = groups; q[13] = 1 | 1 << 16;
    q[17] = 0;
    q[18] = 32u << 16; q[19] = 1 | 1 << 16;
    q[20] = 1;   /* constant buffer 0 */
    q[29] = (u32)CS_CB0; q[30] = (u32)(CS_CB0 >> 32) | 0x1000u << 15;
    u32 r[0x1000] = {0};
    r[M_PROGRAM_REGION] = (u32)(CODE >> 32); r[M_PROGRAM_REGION + 1] = (u32)CODE;
    gpu_stat_batches++;
    for (int i = 0; i < 4000; i++) {
        u64 before = gfx_stat_dispatched;
        gfx_dispatch(r, q);
        if (gfx_stat_dispatched != before) return;
        usleep(5000);
    }
    check(0, "a dispatch was never run");
}

static void test_compute(void) {
    printf("compute\n");
    enum { N = 64 };   /* threads: 2 workgroups of 32, 16 bytes each */
    u32 sb[8] = {(u32)CS_IN, (u32)(CS_IN >> 32), N * 16, 0, (u32)CS_OUT, (u32)(CS_OUT >> 32), N * 16, 0};
    memset(mem(CS_CB0), 0, 0x1000);
    memcpy(mem(CS_CB0 + SB_FIRST), sb, sizeof sb);
    /* positions of a triangle over the target, then words the copy should take as they are */
    float tri[3][4] = {{-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}};
    for (u32 i = 0; i < N * 4; i++) put32(CS_IN + 4 * i, 0x1000 + i);
    memcpy(mem(CS_IN), tri, sizeof tri);
    memset(mem(CS_OUT), 0, N * 16);
    dispatch(2);
    int same = !memcmp(mem(CS_OUT), mem(CS_IN), N * 16);
    check(same, "while learnt: the results in guest memory (word 40: %#x, want %#x)", get32(CS_OUT + 160), get32(CS_IN + 160));
    for (int i = 0; i < 8; i++) dispatch(2);   /* (learnt after 8 runs: what it writes stays on the GPU) */
    tri[0][0] = -0.5f;
    memcpy(mem(CS_IN), tri, sizeof tri);
    put32(CS_IN + 4 * 77, 0xC0FFEE);
    put32(CS_OUT + 4 * 77, 0);
    gpu_stat_batches++;
    dispatch(2);
    check(get32(CS_OUT + 4 * 77) == 0, "learnt: the results stay on the GPU (guest memory: %#x)", get32(CS_OUT + 4 * 77));
    gfx_resident_flush(CS_OUT, N * 16);
    check(get32(CS_OUT + 4 * 77) == 0xC0FFEE && !memcmp(mem(CS_OUT), tri, sizeof tri), "a flush brings them back (%#x)", get32(CS_OUT + 4 * 77));
    /* a draw whose vertices are those results (read from the GPU's copy) */
    put32(CS_IN + 4 * 77, 0xBEEF);
    gpu_stat_batches++;
    dispatch(2);
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 1, 1, 1, 1, 0xF);
    regs[M_VERTEX_ATTRIBUTE(0)] = 0 | 0 << 7 | 0x01 << 21 | 7u << 27;
    regs[M_VERTEX_STREAM(0)] = 16 | 1 << 12;
    regs[M_VERTEX_STREAM(0) + 1] = (u32)(CS_OUT >> 32); regs[M_VERTEX_STREAM(0) + 2] = (u32)CS_OUT;
    regs[M_VERTEX_STREAM_LIMIT(0)] = (u32)((CS_OUT + 48 - 1) >> 32); regs[M_VERTEX_STREAM_LIMIT(0) + 1] = (u32)(CS_OUT + 48 - 1);
    regs[M_VERTEX_ATTRIBUTE(1)] = 1u << 6 | 0x01 << 21 | 7u << 27;   /* (constant: (0, 0, 0, 1)) */
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    check(draw(0), "a draw with compute's results as its vertices");
    read_rt(RT_A, 0);
    check(px(2, 30) == RGBA(255, 255, 255, 255) && px(20, 30) == RGBA(0, 0, 0, 255),
          "drawn (the constant attribute: black) where the results put it (%08x outside, %08x inside)", px(2, 30), px(20, 30));
    check(get32(CS_OUT + 4 * 77) == 0xC0FFEE, "(guest memory still the old results: %#x)", get32(CS_OUT + 4 * 77));
    /* the copy engine: from the GPU's results to elsewhere, on the GPU, then back */
    check(gfx_copy_resident(CS_COPY, CS_OUT, N * 16) == 1, "the copy engine copies resident data on the GPU");
    check(get32(CS_COPY + 4 * 77) == 0, "(not in guest memory yet: %#x)", get32(CS_COPY + 4 * 77));
    gfx_resident_flush(CS_COPY, N * 16);
    check(get32(CS_COPY + 4 * 77) == 0xBEEF, "the copy, brought back (%#x)", get32(CS_COPY + 4 * 77));
    gfx_resident_flush(CS_OUT, N * 16);
    check(get32(CS_OUT + 4 * 77) == 0xBEEF, "and the results (%#x)", get32(CS_OUT + 4 * 77));
}

/* A dispatch writing a range that partly overlaps a region it reads: the regions are merged before
 * either is bound, and both buffers are bound as parts of the merged one. */
static void test_dispatch_merge(void) {
    printf("compute: a written range over part of a region read\n");
    enum { N = 64 };
    u8 was[512];
    memcpy(was, mem(CS_COPY), sizeof was);   /* (the region's data: test_compute brought it back) */
    u64 dst = CS_COPY + 512;
    u32 sb[8] = {(u32)CS_COPY, (u32)(CS_COPY >> 32), N * 16, 0, (u32)dst, (u32)(dst >> 32), N * 16, 0};
    memcpy(mem(CS_CB0 + SB_FIRST), sb, sizeof sb);
    u64 dispatched0 = gfx_stat_dispatched;
    dispatch(2);
    check(gfx_stat_dispatched == dispatched0 + 1, "a dispatch reading CS_COPY's region, writing 512 bytes on");
    gfx_resident_flush(CS_COPY, 512 + N * 16);
    u32 want;
    memcpy(&want, was + 4 * 77, 4);
    check(!memcmp(mem(dst), was, sizeof was), "the region's first 512 bytes, copied after them (their word 77: %#x, want %#x)", get32(dst + 4 * 77), want);
}

/* The table of resident regions full (512): a region made gives up the one used longest ago, not
 * one the call making it uses - a dispatch's region it has bound already (once freed under it:
 * the driver crashed), or a copy's source. */
static void test_resident_full(void) {
    printf("compute: the table of resident regions full\n");
    enum { N = 64 };
    int copied = 0;
    for (u32 i = 0; i < 510; i++) copied += gfx_copy_resident(CS_MANY + 0x100ull * i, CS_OUT, 16);   /* (with CS_OUT's and CS_COPY's: 512) */
    check(copied == 510, "510 regions more, copies of resident data (%d)", copied);
    u64 out = CS_MANY + 0x100000;
    memset(mem(out), 0x77, N * 16);
    u32 sb[8] = {(u32)CS_OUT, (u32)(CS_OUT >> 32), N * 16, 0, (u32)out, (u32)(out >> 32), N * 16, 0};
    memcpy(mem(CS_CB0 + SB_FIRST), sb, sizeof sb);
    dispatch(2);
    gfx_resident_flush(out, N * 16);
    check(!memcmp(mem(out), mem(CS_OUT), N * 16), "a dispatch reading a region into a new one, the table full: copied (word 77: %#x, want %#x)",
          get32(out + 4 * 77), get32(CS_OUT + 4 * 77));
    u64 given_up0 = gfx_stat_residents_given_up;
    copied = 0;
    for (u32 i = 0; i < 64; i++) copied += gfx_copy_resident(CS_MANY + 0x200000 + 0x100ull * i, CS_OUT, 16);
    check(copied == 64 && gfx_stat_residents_given_up == given_up0 + 64, "64 more: as many regions used longest ago given up (%llu), the source kept (%d copied)",
          gfx_stat_residents_given_up - given_up0, copied);
}

/* A resident region (8 KiB, a guest page into a 16 KiB host page) whose host page holds the CPU's
 * data too, which the CPU writes while the GPU's results are newer than guest memory: a write
 * anywhere in a host page reports all its guest pages (macos/memory.c), the region's as well -
 * which must not take guest memory's stale words over the GPU's. at: a host page's offset; the
 * CPU writes at + neighbour (in the next host page: the control). */
static void test_resident_host_page(u64 at, u64 neighbour, const char *what) {
    enum { BYTES = 0x2000 };
    u64 in = CS_HP_IN + at, out = CS_HP_OUT + at + 0x1000;
    printf("compute: a resident region, the CPU writing %s\n", what);
    u32 sb[8] = {(u32)in, (u32)(in >> 32), BYTES, 0, (u32)out, (u32)(out >> 32), BYTES, 0};
    memset(mem(CS_CB0), 0, 0x1000);
    memcpy(mem(CS_CB0 + SB_FIRST), sb, sizeof sb);
    for (u32 i = 0; i < BYTES / 4; i++) put32(in + 4 * i, 0xA0000000u + i);
    memset(mem(out), 0, BYTES);
    dispatch(BYTES / 16 / 32);
    gfx_resident_flush(out, BYTES);
    check(get32(out + 4 * 1000) == 0xA0000000u + 1000, "%s: the first results, brought back (%#x)", what, get32(out + 4 * 1000));
    for (u32 i = 0; i < BYTES / 4; i++) put32(in + 4 * i, 0xB0000000u + i);
    dispatch(BYTES / 16 / 32);
    check(get32(out + 4 * 1000) == 0xA0000000u + 1000, "%s: (the new ones on the GPU alone: %#x in guest memory)", what, get32(out + 4 * 1000));
    put32(CS_HP_OUT + at + neighbour, 0x12345678);   /* (the CPU's own data, outside the region) */
    gpu_stat_batches++;
    gfx_resident_flush(out, BYTES);
    u32 w0 = get32(out), w1000 = get32(out + 4 * 1000), w2047 = get32(out + 4 * 2047);
    check(w0 == 0xB0000000u && w1000 == 0xB0000000u + 1000 && w2047 == 0xB0000000u + 2047,
          "%s: the GPU's results kept (%#x %#x %#x, want %#x %#x %#x)", what, w0, w1000, w2047, 0xB0000000u, 0xB0000000u + 1000, 0xB0000000u + 2047);
}

/* Many frames of many draws, each reading vertices of its own (nothing to reuse), between two
 * targets in turn: upload memory and passes go round, and the work is committed as it grows. */
static void test_stress(void) {
    printf("many draws\n");
    enum { FRAMES = 12, DRAWS = 2500 };
    u64 commits0 = gfx_stat_commits, drawn0 = gfx_stat_drawn, t0 = rt_now_ns();
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    u32 last[2] = {0, 0};
    for (u32 f = 0; f < FRAMES; f++) {
        for (u32 i = 0; i < DRAWS; i++) {
            u32 n = f * DRAWS + i, t = (i / 100) & 1;
            target(0, t ? RT_K : RT_A, 64, 64, 0xD5);
            float c[4] = {(float)(n % 251) / 255.0f, (float)(n % 241) / 255.0f, (float)(f + 1) / 255.0f, 1};
            u64 va = VERTS + 0x2000 + (u64)(n % 512) * 96;
            vertex(va, 0, -1, -1, 0.5f, c); vertex(va, 1, 3, -1, 0.5f, c); vertex(va, 2, -1, 3, 0.5f, c);
            stream(va, 3);
            regs[M_VERTEX_ARRAY_START] = 0; regs[M_VERTEX_ARRAY_COUNT] = 3;
            gpu_stat_batches++;
            gfx_draw(regs, (const CbBinding (*)[18])cb, 0);
            last[t] = RGBA(n % 251, n % 241, f + 1, 255);
        }
        gfx_present(gpu_va_to_guest(RT_A), 64, 64);
    }
    double secs = (rt_now_ns() - t0) / 1e9;
    check(gfx_stat_drawn - drawn0 == FRAMES * DRAWS, "%u draws drawn (%llu), in %.2f s", FRAMES * DRAWS, gfx_stat_drawn - drawn0, secs);
    check(gfx_stat_commits - commits0 > FRAMES, "committed as the work grew: %llu command buffers for %u frames", gfx_stat_commits - commits0, FRAMES);
    read_rt(RT_A, 0);
    u32 a = px(32, 32);
    read_rt(RT_K, 0);
    u32 k = px(32, 32);
    check(a == last[0] && k == last[1], "each target's last draw shows (%08x, want %08x; %08x, want %08x)", a, last[0], k, last[1]);
    check(gfx_stat_gpu_errors == 0, "no command buffer failed (%llu)", gfx_stat_gpu_errors);
}

/* A BMP as gpu_metal.m writes them (24-bit, top row first: a negative height): its size, and its
 * pixel (x, y) as RGBA, alpha 255. */
static u8 *bmp;
static i32 bmp_w, bmp_h;
static int read_bmp(const char *path) {
    free(bmp);
    bmp = NULL;
    bmp_w = bmp_h = 0;
    FILE *f = fopen(path, "rb");
    u8 hdr[54];
    int ok = f && fread(hdr, 1, 54, f) == 54 && hdr[0] == 'B' && hdr[1] == 'M';
    i32 w = 0, h = 0;
    if (ok) { memcpy(&w, hdr + 18, 4); memcpy(&h, hdr + 22, 4); }
    size_t n = ((size_t)w * 3 + 3) / 4 * 4 * (size_t)(h < 0 ? -h : h);
    ok = ok && w > 0 && n && (bmp = malloc(n)) && fread(bmp, 1, n, f) == n;
    if (f) fclose(f);
    if (ok) { bmp_w = w; bmp_h = h; }
    return ok;
}
static u32 bmp_px(u32 x, u32 y) {
    if (!bmp || x >= (u32)bmp_w || y >= (u32)(bmp_h < 0 ? -bmp_h : bmp_h)) return 0;
    const u8 *p = bmp + ((size_t)bmp_w * 3 + 3) / 4 * 4 * (bmp_h < 0 ? y : (u32)bmp_h - 1 - y) + x * 3;
    return RGBA(p[2], p[1], p[0], 255);
}

/* Render scale: targets made twice the game's size (gpu_metal.m's render_scale, as display.ini's
 * render_scale = 2 makes every one; 1.5 for a scissor's rounding). What the game gives in its own
 * pixels - viewports, scissors, the fragment position, texel numbers - lands on the targets' own;
 * the picture is presented at its size, and shrunk into a smaller window in linear light. Its
 * present is the run's first, whose picture and window SSBU_CAPTURE saves (main). Only targets of
 * their own: those of the other tests are made at 1 after it. */
static void test_scale(void) {
    printf("render scale\n");
    static const float WHITE[4] = {1, 1, 1, 1};
    const u32 white = RGBA(255, 255, 255, 255), black = RGBA(0, 0, 0, 255), red = RGBA(255, 0, 0, 255);
    remove("captures/frame_000.bmp");
    remove("captures/window_000.bmp");
    gfx_metal_set_render_scale(2.0f);
    /* a viewport of the game's x 0 to 10.5 (of 640): host pixels 0-20 */
    setup(RT_S, 640, 360, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    check(read_rt(RT_S, 0) && rt_w == 1280 && rt_h == 720, "a 640x360 target made at 1280x720 (%ux%u)", rt_w, rt_h);
    vertex(VERTS, 0, -1, -1, 0.5f, WHITE); vertex(VERTS, 1, 3, -1, 0.5f, WHITE); vertex(VERTS, 2, -1, 3, 0.5f, WHITE);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_VIEWPORT_SCALE(0)] = fbits(5.25f); regs[M_VIEWPORT_SCALE(0) + 3] = fbits(5.25f);
    check(draw(0), "a draw into a viewport 10.5 pixels wide");
    read_rt(RT_S, 0);
    check(px(0, 300) == white && px(20, 300) == white && px(21, 300) == black && px(20, 719) == white,
          "the viewport is host pixels 0-20 (%08x %08x %08x)", px(0, 300), px(20, 300), px(21, 300));
    /* a scissor of the game's x 100-110, y 20-30: host 200-220, 40-60 */
    viewport(640, 360);
    regs[M_SCISSOR(0)] = 1; regs[M_SCISSOR(0) + 1] = 100 | 110 << 16; regs[M_SCISSOR(0) + 2] = 20 | 30 << 16;
    vertex(VERTS, 0, -1, -1, 0.5f, RED); vertex(VERTS, 1, 3, -1, 0.5f, RED); vertex(VERTS, 2, -1, 3, 0.5f, RED);
    check(draw(0), "a scissored draw");
    read_rt(RT_S, 0);
    check(px(200, 40) == red && px(219, 59) == red && px(199, 50) == black && px(220, 50) == black && px(210, 39) == black &&
          px(210, 60) == black, "the scissor is host pixels 200-219, 40-59 (%08x %08x; outside %08x %08x %08x %08x)", px(200, 40),
          px(219, 59), px(199, 50), px(220, 50), px(210, 39), px(210, 60));

    /* the fragment position, in the game's pixels: host pixel (21, 40)'s centre is (10.75, 20.25);
     * with the origin at the lower left, y counts up from the game's 64 */
    setup(RT_F, 64, 64, 0, VS_PASS, PS_POSITION);
    target(0, RT_F, 64, 64, 0xC0);   /* RGBA32 float */
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    float at[4] = {0};
    for (int lower_left = 0; lower_left < 2; lower_left++) {
        regs[M_WINDOW_ORIGIN] = lower_left;
        check(draw(0), "a draw writing its position%s", lower_left ? ", the origin at the lower left" : "");
        check(read_rt(RT_F, 0) && rt_w == 128, "into a 64x64 float target, 128x128 (%ux%u)", rt_w, rt_h);
        memcpy(at, rt_pixels + ((size_t)40 * rt_w + 21) * 16, 16);
        check(at[0] == 10.75f && at[1] == (lower_left ? 64 - 20.25f : 20.25f), "host pixel (21, 40) is the game's (%g, %g)", at[0], at[1]);
    }

    /* a texel by its numbers (TLDS) from a scaled target: the game's texel, whatever the target's own */
    textured_setup();
    target(0, RT_P, 16, 16, 0xD5);   /* (not textured_setup's RT_T: the other tests make that one at 1) */
    texture_header(0x08, 2, 16, 16);
    write_rgba8_texture(0);
    check(draw(0), "a 16x16 texture drawn into a 16x16 target, 32x32");
    read_rt(RT_P, 0);
    check(rt_w == 32 && px(10, 18) == texel(5, 9, 0) && px(11, 19) == texel(5, 9, 0) && px(9, 17) == texel(4, 8, 0),
          "each texel 2x2 host pixels (%08x %08x %08x)", px(10, 18), px(11, 19), px(9, 17));
    regs[M_PIPELINE(5) + 1] = PS_TEXEL;
    target(0, RT_Q, 16, 16, 0xD5);
    texture_address(RT_P);
    check(draw(0), "a draw fetching texel (5, 9) of that target");
    read_rt(RT_Q, 0);
    check(px(0, 0) == texel(5, 9, 0) && px(31, 31) == texel(5, 9, 0), "the game's texel (5, 9): %08x, want %08x (the host's (5, 9) is %08x)",
          px(0, 0), texel(5, 9, 0), texel(2, 4, 0));

    /* at 1.5 a 64x64 target is 96x96, and a scissor's edges go outwards: x 11-21 is 16.5-31.5, so 16-31 */
    gfx_metal_set_render_scale(1.5f);
    setup(RT_X, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    vertex(VERTS, 0, -1, -1, 0.5f, RED); vertex(VERTS, 1, 3, -1, 0.5f, RED); vertex(VERTS, 2, -1, 3, 0.5f, RED);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_SCISSOR(0)] = 1; regs[M_SCISSOR(0) + 1] = 11 | 21 << 16; regs[M_SCISSOR(0) + 2] = 0 | 64 << 16;
    check(draw(0), "a scissored draw at 1.5");
    read_rt(RT_X, 0);
    check(rt_w == 96 && px(15, 50) == black && px(16, 50) == red && px(31, 50) == red && px(32, 50) == black,
          "96x96 (%ux%u), the scissor host pixels 16-31 (%08x %08x %08x %08x)", rt_w, rt_h, px(15, 50), px(16, 50), px(31, 50), px(32, 50));
    gfx_metal_set_render_scale(1.0f);

    /* presented: the picture at its own size, 1280x720, and the 640x360 window's, each of its pixels
     * two of the picture's averaged in linear light - at the viewport's edge, white and black: 188,
     * not 128 */
    gfx_present(gpu_va_to_guest(RT_S), 640, 360);
    check(read_bmp("captures/frame_000.bmp") && bmp_w == 1280 && bmp_h == -720, "the picture saved at 1280x720 (%dx%d)", bmp_w, bmp_h);
    check(bmp_px(20, 300) == white && bmp_px(21, 300) == black && bmp_px(200, 40) == red && bmp_px(199, 40) == black,
          "pixel for pixel the target (%08x %08x %08x %08x)", bmp_px(20, 300), bmp_px(21, 300), bmp_px(200, 40), bmp_px(199, 40));
    check(read_bmp("captures/window_000.bmp") && bmp_w == 640 && bmp_h == -360, "the window saved, 640x360 (%dx%d)", bmp_w, bmp_h);
    check(bmp_px(9, 300) == white && rgba_near(bmp_px(10, 300), RGBA(188, 188, 188, 255), 2) && bmp_px(11, 300) == black,
          "shrunk in linear light: white, the edge %08x (188: half the light), black (%08x, %08x)", bmp_px(10, 300), bmp_px(9, 300),
          bmp_px(11, 300));
    check(bmp_px(100, 20) == red && bmp_px(109, 29) == red && bmp_px(99, 25) == black && bmp_px(110, 25) == black,
          "the scissored block at half its size (%08x %08x; beside it %08x %08x)", bmp_px(100, 20), bmp_px(109, 29), bmp_px(99, 25),
          bmp_px(110, 25));
}

/* The game's picture into the layer, and the F3 screenshot of it. */
static void test_present(void) {
    printf("presenting\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    clear_color(0, 0.25f, 0.5f, 0.75f, 1.0f, 0xF);
    u64 presented0 = gfx_stat_presented, t0 = rt_now_ns();
    for (int i = 0; i < 10; i++) gfx_present(gpu_va_to_guest(RT_A), 64, 64);
    double ms = (rt_now_ns() - t0) / 1e6;
    check(gfx_stat_presented == presented0 + 10, "ten frames presented");
    check(ms < 1000, "without waiting for drawables that never come back (%.1f ms for ten; a layer in no window)", ms);
    gfx_hotkey(2);   /* F3 */
    gfx_present(gpu_va_to_guest(RT_A), 64, 64);
    DIR *d = opendir("screenshots");
    char path[600] = "";
    for (struct dirent *e; d && (e = readdir(d));)
        if (strstr(e->d_name, ".bmp")) snprintf(path, sizeof path, "screenshots/%s", e->d_name);
    if (d) closedir(d);
    check(path[0] != 0, "F3 saved a screenshot (%s)", path);
    check(read_bmp(path) && bmp_w == 64 && bmp_h == -64, "a 64x64 BMP: the picture is the presented target's size (%dx%d)", bmp_w, bmp_h);
    check(rgba_near(bmp_px(32, 32), RGBA(64, 128, 191, 255), 1), "the picture's middle the target's colour (%08x)", bmp_px(32, 32));
}

/* The overlay (gpu_metal_overlay.m) over a white target of its own: its panel darkens it, its text
 * is in colour. */
static void test_overlay(void) {
    printf("the overlay\n");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:640 height:360 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    for (int frame = 0; frame < 2; frame++) {   /* (the second has a frame time to show) */
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> b = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [b renderCommandEncoderWithDescriptor:rp];
        overlay_present_metal(dev, e, MTLPixelFormatBGRA8Unorm, 0, 0, 640, 360, 1, "gfxtest\ndetails");
        [e endEncoding];
        [b commit];
        [b waitUntilCompleted];
        usleep(16000);
    }
    static u32 pix[640 * 360];
    [t getBytes:pix bytesPerRow:640 * 4 fromRegion:MTLRegionMake2D(0, 0, 640, 360) mipmapLevel:0];
    u32 panel = pix[200 * 640 + 300], outside = pix[350 * 640 + 630];
    check(outside == 0xFFFFFFFFu, "outside the panel: untouched (%08x)", outside);
    check((panel & 0xFF) < 160 && (panel & 0xFF) > 60, "the panel darkens what is under it (%08x)", panel);
    int coloured = 0;
    for (u32 y = 16; y < 40; y++)
        for (u32 x = 16; x < 250; x++) {
            u32 v = pix[y * 640 + x], b = v & 0xFF, g = v >> 8 & 0xFF, r = v >> 16 & 0xFF;
            coloured += (g > b + 40 || r > b + 40);   /* (the FPS line: green, yellow or red) */
        }
    check(coloured > 50, "its text drawn (%d coloured pixels in the first line)", coloured);
}

/* The disk cache: each program's MSL, as compiled (the next start's prewarm compiles them). */
static void test_cache(void) {
    printf("the shader cache\n");
    DIR *d = opendir("shadercache");
    int files = 0, good = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "m", 1) || !strstr(e->d_name, ".metal") || strstr(e->d_name, ".tmp")) continue;
        files++;
        char path[600], line[64] = "";
        snprintf(path, sizeof path, "shadercache/%s", e->d_name);
        FILE *f = fopen(path, "r");
        if (f && fgets(line, sizeof line, f) && !strncmp(line, "// Maxwell ", 11)) good++;
        if (f) fclose(f);
    }
    if (d) closedir(d);
    check(files == 13 && good == files, "%d MSL files written (13: 12 programs - ps_color for both vertex programs, whose outputs are "
          "alike - and ps_color's alpha-tested variant), %d of them whole", files, good);
}

/* ---- what upstream fixed in both renderers: the alpha test, the shared write watch, unmaps ---- */
extern u64 gfx_stat_unmap_dropped, gfx_stat_unmapped;
void gfx_gpu_unmapped(u64 va, u64 size);
void gfx_guest_unmapped(u64 addr, u64 size);

/* the alpha test: a draw (wait: again until it is discarded - the plain shader draws while the
 * alpha-tested variant and its pipeline compile) */
static u32 alpha_draw(int on, float alpha, u32 func, float ref, int wait) {
    float c[4] = {0, 1, 0, alpha};
    vertex(VERTS, 0, -1, -1, 0.5f, c); vertex(VERTS, 1, 3, -1, 0.5f, c); vertex(VERTS, 2, -1, 3, 0.5f, c);
    stream(VERTS, 3);
    regs[M_VERTEX_ARRAY_START] = 0; regs[M_VERTEX_ARRAY_COUNT] = 3;
    regs[M_ALPHA_TEST] = (u32)on; regs[M_ALPHA_FUNC] = func; regs[M_ALPHA_REF] = fbits(ref);
    u32 got = 0;
    for (int i = 0; i < 400; i++) {
        clear_color(0, 0, 0, 0, 0, 0xF);
        draw(0);
        read_rt(RT_A, 0);
        got = px(30, 30);
        if (!wait || got == RGBA(0, 0, 0, 0)) break;
        usleep(5000);
    }
    return got;
}

static void test_alpha(void) {
    printf("the alpha test\n");
    setup(RT_A, 64, 64, 0, VS_PASS, PS_COLOR);
    u32 a = alpha_draw(1, 0.25f, 0x204, 0.5f, 1);   /* greater */
    check(a == RGBA(0, 0, 0, 0), "alpha 0.25, greater than 0.5: discarded (%08x)", a);
    a = alpha_draw(1, 0.75f, 0x204, 0.5f, 0);
    check(rgba_near(a, RGBA(0, 255, 0, 191), 1), "alpha 0.75, greater than 0.5: drawn (%08x)", a);
    a = alpha_draw(1, 0.75f, 0x201, 0.5f, 0);   /* less */
    check(a == RGBA(0, 0, 0, 0), "alpha 0.75, less than 0.5: discarded (%08x)", a);
    a = alpha_draw(1, 0.25f, 0x201, 0.5f, 0);
    check(rgba_near(a, RGBA(0, 255, 0, 64), 1), "alpha 0.25, less than 0.5: drawn (%08x)", a);
    a = alpha_draw(1, 0.5f, 0x202, 0.5f, 0);   /* equal */
    check(rgba_near(a, RGBA(0, 255, 0, 128), 1), "alpha 0.5, equal to 0.5: drawn (%08x)", a);
    a = alpha_draw(1, 0.5f, 0x205, 0.5f, 0);   /* not equal */
    check(a == RGBA(0, 0, 0, 0), "alpha 0.5, not equal to 0.5: discarded (%08x)", a);
    a = alpha_draw(1, 0.25f, 0x207, 0.5f, 0);   /* always */
    check(rgba_near(a, RGBA(0, 255, 0, 64), 1), "always: drawn (%08x)", a);
    a = alpha_draw(1, 0.75f, 0x200, 0.5f, 0);   /* never */
    check(a == RGBA(0, 0, 0, 0), "never: discarded (%08x)", a);
    a = alpha_draw(0, 0.25f, 0x204, 0.5f, 0);
    check(rgba_near(a, RGBA(0, 255, 0, 64), 1), "the test off: drawn (%08x)", a);
    regs[M_ALPHA_TEST] = 0;
}

/* two kept constant buffers over one page: the CPU's write is news to both */
static void test_shared_news(void) {
    printf("two kept constant buffers on one page, the CPU writing it\n");
    setup(RT_K, 64, 64, 0, VS_CBUF, PS_COLOR);
    clear_color(0, 0, 0, 0, 1, 0xF);
    float none[4] = {0};
    vertex(VERTS, 0, -1, -1, 0.5f, none); vertex(VERTS, 1, 0, -1, 0.5f, none); vertex(VERTS, 2, -1, 1, 0.5f, none);
    vertex(VERTS, 3, 0.01f, -1, 0.5f, none); vertex(VERTS, 4, 1, -1, 0.5f, none); vertex(VERTS, 5, 1, 1, 0.5f, none);
    stream(VERTS, 6);
    memset(mem(CB3 - 0x1000), 0, 0x3000);
    memcpy(mem(CB3 + 0xFF0), RED, 16);    /* buffer A: [CB3, CB3 + 0x1000), its colour at +0xFF0 */
    memcpy(mem(CB3 + 0xFE0), BLUE, 16);   /* buffer B: [CB3 - 0x10, CB3 + 0xFF0), its colour at +0xFF0 = CB3 + 0xFE0 */
    for (int round = 0; round < 2; round++) {
        gpu_stat_batches++;
        clear_color(0, 0, 0, 0, 1, 0xF);
        cb[STAGE_VERTEX][3] = (CbBinding){CB3, 0x1000};
        regs[M_VERTEX_ARRAY_START] = 0; regs[M_VERTEX_ARRAY_COUNT] = 3;
        draw(0);
        cb[STAGE_VERTEX][3] = (CbBinding){CB3 - 0x10, 0x1000};
        regs[M_VERTEX_ARRAY_START] = 3;
        draw(0);
    }
    read_rt(RT_K, 0);
    check(px(5, 5) == RGBA(255, 0, 0, 255) && px(60, 5) == RGBA(0, 0, 255, 255), "red and blue (%08x %08x)", px(5, 5), px(60, 5));
    memcpy(mem(CB3 + 0xFF0), GREEN, 16);   /* the CPU writes both colours (one page) */
    memcpy(mem(CB3 + 0xFE0), GREEN, 16);
    gpu_stat_batches++;
    clear_color(0, 0, 0, 0, 1, 0xF);
    cb[STAGE_VERTEX][3] = (CbBinding){CB3, 0x1000};
    regs[M_VERTEX_ARRAY_START] = 0;
    draw(0);
    cb[STAGE_VERTEX][3] = (CbBinding){CB3 - 0x10, 0x1000};
    regs[M_VERTEX_ARRAY_START] = 3;
    draw(0);
    read_rt(RT_K, 0);
    check(px(5, 5) == RGBA(0, 255, 0, 255), "the first buffer asked takes the write (%08x)", px(5, 5));
    check(px(60, 5) == RGBA(0, 255, 0, 255), "and the second, on the same page, too (%08x)", px(60, 5));
}

/* unmap notices: what was made from the memory is dropped, and made again at its next use */
static void test_unmapped(void) {
    printf("memory taken away\n");
    u64 before = gfx_stat_unmap_dropped;
    gfx_gpu_unmapped(VA0, GSIZE);
    check(gfx_stat_unmap_dropped > before, "a GPU unmap drops what was made there (%llu)", gfx_stat_unmap_dropped - before);
    before = gfx_stat_unmap_dropped;
    gfx_guest_unmapped(ALIAS_BASE, GSIZE);
    check(gfx_stat_unmap_dropped == before, "nothing left to drop (%llu)", gfx_stat_unmap_dropped - before);
    /* compute again (learnt: the results stay on the GPU), in a region made anew */
    enum { N = 64 };
    u32 sb[8] = {(u32)CS_IN, (u32)(CS_IN >> 32), N * 16, 0, (u32)CS_OUT, (u32)(CS_OUT >> 32), N * 16, 0};
    memset(mem(CS_CB0), 0, 0x1000);
    memcpy(mem(CS_CB0 + SB_FIRST), sb, sizeof sb);
    for (u32 i = 0; i < N * 4; i++) put32(CS_IN + 4 * i, 0x5000 + i);
    memset(mem(CS_OUT), 0, N * 16);
    dispatch(2);
    check(get32(CS_OUT + 4 * 9) == 0, "the results on the GPU (%#x in guest memory)", get32(CS_OUT + 4 * 9));
    gfx_resident_flush(CS_OUT, N * 16);
    check(get32(CS_OUT + 4 * 9) == 0x5009, "brought back from the region made anew (%#x)", get32(CS_OUT + 4 * 9));
    before = gfx_stat_unmap_dropped;
    gfx_guest_unmapped(gpu_va_to_guest(CS_OUT), 0x1000);
    check(gfx_stat_unmap_dropped > before, "guest memory taken away under it drops it (%llu)", gfx_stat_unmap_dropped - before);
    for (u32 i = 0; i < N * 4; i++) put32(CS_IN + 4 * i, 0x6000 + i);
    dispatch(2);
    gfx_resident_flush(CS_OUT, N * 16);
    check(get32(CS_OUT + 4 * 9) == 0x6009, "and the next dispatch makes it again (%#x)", get32(CS_OUT + 4 * 9));
    test_kept_constants();   /* (its buffers dropped above: made again) */
}

int main(int argc, char **argv) {
    @autoreleasepool {
        verbose = argc > 1 && !strcmp(argv[1], "-v");
        setvbuf(stdout, NULL, _IONBF, 0);
        if (!rt_guest_window()) return 2;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
        /* guest memory as the kernel reserves the alias area (write-watched), its start committed */
        if (!VirtualAlloc(G2H(ALIAS_BASE), ALIAS_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE) ||
            !VirtualAlloc(G2H(ALIAS_BASE), GSIZE, MEM_COMMIT, PAGE_READWRITE)) {
            fprintf(stderr, "cannot map guest memory\n");
            return 2;
        }
        /* a folder of its own: the shader cache (rt_data_dir/../shadercache) and the screenshot */
        char dir[512];
        const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        snprintf(dir, sizeof dir, "%s/gfxtest.%d", tmp, (int)getpid());
        if (getenv("GFXTEST_DIR")) snprintf(dir, sizeof dir, "%s", getenv("GFXTEST_DIR"));   /* (again: the prewarm takes the cache) */
        mkdir(dir, 0755);
        if (chdir(dir)) return 2;
        mkdir("modules", 0755);
        rt_data_dir = "modules";
        printf("gfxtest in %s\n", dir);
        test_layer = [CAMetalLayer layer];   /* (as macos/window.m makes the window's: BGRA8 in sRGB) */
        test_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        test_layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        test_layer.framebufferOnly = NO;
        test_layer.drawableSize = CGSizeMake(640, 360);
        Prog p;
        vs_pass(&p); place(VS_PASS, &p, 0);
        vs_cbuf(&p); place(VS_CBUF, &p, 0);
        ps_color(&p); place(PS_COLOR, &p, 0);
        ps_texture(&p); place(PS_TEXTURE, &p, 0);
        cs_copy(&p); place(CS_COPY_PROG, &p, 1);
        ps_texture_at(&p, TEX_ARRAY_2D, 0.5f, 0.5f, 0, 2); place(PS_ARRAY, &p, 0);
        ps_texture_at(&p, TEX_CUBE, 1, 0, 0, 0); place(PS_CUBE_X, &p, 0);
        ps_texture_at(&p, TEX_CUBE, 0, 0, -1, 0); place(PS_CUBE_NZ, &p, 0);
        ps_texture_at(&p, TEX_3D, 0.5f, 0.5f, 0.625f, 0); place(PS_3D, &p, 0);
        ps_dual(&p); place(PS_DUAL, &p, 0);
        ps_position(&p); place(PS_POSITION, &p, 0);
        ps_texel(&p); place(PS_TEXEL, &p, 0);
        /* SSBU_CAPTURE: the picture and the window of the run's first present (test_scale's) */
        mkdir("captures", 0755);
        setenv("SSBU_CAPTURE", "captures", 1);
        setenv("SSBU_CAPTURE_WINDOW", "1", 1);
        setenv("SSBU_CAPTURE_EVERY", "0", 1);
        setenv("SSBU_CAPTURE_MAX", "1", 1);
        test_scale();
        test_clears();
        test_triangles();
        test_textures();
        test_texture_types();
        test_targets_as_textures();
        test_kept_constants();
        test_state();
        test_dual_source();
        test_blit();
        test_arena();
        test_compute();
        test_dispatch_merge();
        test_resident_full();
        test_resident_host_page(0x20000, 0x4100, "in the next host page");
        test_resident_host_page(0x60000, 0x100, "in the same host page");
        test_alpha();
        test_shared_news();
        test_unmapped();
        test_stress();
        test_present();
        test_overlay();
        test_cache();
        printf("%d checks, %d failed (%llu draws, %llu skipped while compiling)\n", checks, failures, gfx_stat_drawn, gfx_stat_skipped);
    }
    fflush(stdout);
    _exit(failures ? 1 : 0);
}
