/* Maxwell shader programs translated to HLSL (Shader Model 5) for the D3D11 back end, or to
 * Metal Shading Language for the Metal one (its layout: SHADER_MSL below).
 *
 * Resource layout the generated HLSL expects:
 *   t0..t17    constant buffers 0..17 of the stage, as raw buffers
 *   t18..t49   vertex streams 0..31 (vertex shaders), as raw buffers
 *   t64+i      texture i of the shader's texture table, s0+i its sampler
 *   t96+i      storage buffer i (ShaderInfo.sb_offset), as raw buffers (compute: u0+i, writable)
 *   b0         per-draw parameters (vertex attribute formats, viewport fix-up), see DrawParams */
#pragma once
#include "host.h"

enum { SHADER_VERTEX, SHADER_PIXEL, SHADER_COMPUTE };

enum { TEX_1D, TEX_ARRAY_1D, TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_ARRAY_3D, TEX_CUBE, TEX_ARRAY_CUBE };

typedef struct ShaderTexture {
    u32 handle_offset;   /* byte offset of the texture handle in the bindless constant buffer */
    u8 type;             /* TEX_* */
    u8 shadow;           /* depth compare */
} ShaderTexture;

typedef struct ShaderInfo {
    int stage;
    u32 cbuf_mask;                 /* constant buffer slots read */
    u32 cbuf_extent[18];           /* bytes of each read at most (~0u: indexed, so all of it) */
    u32 inputs;                    /* generic attributes read (vertex: from streams; pixel: interpolated) */
    u32 outputs;                   /* vertex: generic attributes written */
    u8 interp[32];                 /* pixel: per generic input, 1 flat, 2 perspective, 3 linear */
    u32 color_mask;                /* pixel: render targets written */
    int writes_depth;
    ShaderTexture tex[32];
    u32 ntex;
    int kills;
    char unsupported[256];         /* first instruction the translator could not handle, if any */
    u32 ntables;                   /* indirect branches' jump tables, read from constant buffers */
    struct { u32 bank, offset, count; } table[4];
    u32 sb_count;                  /* global memory: the storage buffers it may fall in, */
    u32 sb_offset[16];             /* by the offset of their descriptor in constant buffer 0 */
    int stores;                    /* compute: writes global memory */
} ShaderInfo;

/* NVN describes storage buffers in constant buffer 0, 16 bytes each from SB_FIRST: the GPU
 * address (64 bits), then the size. Global memory accesses are resolved against them. */
#define SB_FIRST 0x110
#define SB_END   0x610

/* Translate one program (the 0x50-byte header, then code). For a pixel shader,
 * `vs_outputs` is the generic-attribute mask the paired vertex shader writes: the pixel
 * shader's input signature is made identical to it, as Direct3D requires. Returns
 * malloc'd HLSL source. */
char *shader_hlsl(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info);

/* The same, able to read the stage's constant buffers: indirect branches (BRX) take their
 * targets from a jump table in one, and knowing them keeps the generated code small. */
typedef u32 CbufReader(void *user, u32 bank, u32 offset);
char *shader_hlsl_ex(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user);

/* A pixel shader that also does the fixed-function alpha test (ALPHA_TEST): render target 0's
 * alpha compared with DrawParams.frag[3], the pixel discarded when the outcome (less, equal,
 * greater, or unordered) is not one of the bits in frag[2] (see alpha_test_mask). Draws with the
 * test on use it in place of the plain one. */
char *shader_hlsl_alpha_tested(const u8 *program, u32 size, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user);

/* A compute program (no header: pass it with 0x50 bytes of anything in front, so that its
 * addresses line up with those of the other stages), its workgroup size and shared memory. */
char *shader_hlsl_cs(const u8 *program, u32 size, const u32 local[3], u32 shared, ShaderInfo *info, CbufReader *read, void *user);

/* The alpha test's passing outcomes for a D3D11_COMPARISON_FUNC (NEVER 1 .. ALWAYS 8): bit 0 the
 * alpha less than the reference, 1 equal, 2 greater - the function less one - and 3 unordered
 * (a NaN), which only NOT_EQUAL and ALWAYS pass. */
static inline u32 alpha_test_mask(u32 func) { return (func - 1) | (func == 6 || func == 8 ? 8u : 0u); }

/* Output languages: HLSL for fxc (Windows), HLSL within what vkd3d-shader compiles (Linux, or
 * SSBU_HLSL_FOR_VKD3D; shader_hlsl* write the build's), Metal Shading Language (macOS). */
enum { SHADER_HLSL_FXC, SHADER_HLSL_VKD3D, SHADER_MSL };

/* Any stage in any of them: `local` and `shared` are a compute program's (as shader_hlsl_cs
 * takes them; otherwise ignored), `vs_outputs` a pixel program's. Safe to call from several
 * threads at once in MSL (HLSL names texture results by a count shared by every program). */
char *shader_translate(int lang, const u8 *program, u32 size, int stage, u32 vs_outputs, const u32 local[3], u32 shared,
                       ShaderInfo *info, CbufReader *read, void *user);
/* And a pixel shader with the alpha test, as shader_hlsl_alpha_tested's, in any of them. */
char *shader_translate_alpha_tested(int lang, const u8 *program, u32 size, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user);

/* ---- Metal (SHADER_MSL) -------------------------------------------------------------------
 * Entry points vs_main, ps_main, cs_main, each with, in its stage's argument tables:
 *   buffer(0)     DrawParams (set*Bytes)
 *   buffer(1)     ShaderBinds: the stage's raw buffers by GPU address and size (set*Bytes, its first
 *                 shader_binds_size(stage) bytes); each buffer made resident (useResource, a
 *                 residency set or a heap). Words past a size read 0, and are not written.
 *   texture(k)    texture k of ShaderInfo.tex, of the type shader_msl_texture says
 *   sampler(k)    its sampler, for k < shader_msl_samplers(info)
 * A pixel shader's outputs are typed by function constant SHADER_MSL_RT_TYPES (a uint, 2 bits a
 * render target: 0 float, 1 uint, 2 int - the attachment's format's kind - and SHADER_MSL_DUAL_SOURCE
 * when blending takes source 1: target 1's output, or 0 without one, is then attachment 0's second
 * source, and a program writing no target 0 writes 0 as its first - so any program takes it): ps_main
 * must always be specialised (newFunctionWithName:constantValues:), with 0 for float targets alone.
 * Metal's pipeline validation, which aborts the process even without MTL_DEBUG_LAYER, has two rules
 * for source 1: a pipeline whose blending takes it has no colour attachment but 0, and Source1
 * factors are attachment 0's alone. So macos/gpu_metal.m's dual_source() sets SHADER_MSL_DUAL_SOURCE
 * only when target 0's blend takes source 1 and the program writes target 1; such a draw goes into
 * target 0 (and depth) alone, in a render pass of its own. Any other Source1 factor is read with
 * source 1 as 0 (Source1* becomes Zero, OneMinusSource1* One).
 * vs_main writes [[point_size]] (1): its pipelines' inputPrimitiveTopology must stay unspecified
 * (or be points). Compile with MTLMathModeSafe (fastMathEnabled = NO before macOS 15) and
 * preserveInvariance. tools/mac/msltest.m binds and runs programs so, as an example. */
#define SHADER_MSL_PARAMS 0
#define SHADER_MSL_BINDS 1
#define SHADER_MSL_SAMPLERS 16
#define SHADER_MSL_RT_TYPES 0
#define SHADER_MSL_DUAL_SOURCE 0x10000u
typedef struct ShaderBind {
    u64 address;           /* MTLBuffer.gpuAddress + offset, a multiple of 4 (0 with size 0: none) */
    u32 size, pad;         /* bytes the shader may read (compute storage buffers: and write) */
} ShaderBind;
typedef struct ShaderBinds {
    ShaderBind cb[18];     /* constant buffers 0..17 */
    ShaderBind gm[16];     /* storage buffers (ShaderInfo.sb_offset; compute: writable) */
    ShaderBind vb[32];     /* vertex shaders: each generic attribute's vertex stream */
} ShaderBinds;
static inline u32 shader_binds_size(int stage) { return stage == SHADER_VERTEX ? (u32)sizeof(ShaderBinds) : 34 * (u32)sizeof(ShaderBind); }

/* How many of a program's textures have a sampler argument (texture k < this: sampler(k)): all of
 * them up to Metal's 16. With more, the rest share default samplers declared in the shader
 * (linear, clamping to the edge; a comparing one for shadow textures, comparing never, as
 * Direct3D's default) - which count against the 16, so fewer arguments then. */
static inline u32 shader_msl_samplers(const ShaderInfo *info) {
    if (info->ntex <= SHADER_MSL_SAMPLERS) return info->ntex;
    for (u32 n = SHADER_MSL_SAMPLERS - 1;; n--) {
        u32 kinds = 0;
        for (u32 k = n; k < info->ntex; k++) kinds |= info->tex[k].shadow ? 2u : 1u;
        if (n + (kinds & 1) + (kinds >> 1) <= SHADER_MSL_SAMPLERS) return n;
    }
}

/* The type texture k is declared with: TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_CUBE or TEX_ARRAY_CUBE
 * (1D textures are 2D ones: Metal's 1D textures have no mips), a depth texture (depth2d...) when
 * it is a shadow one, but for 3D (none in Metal: compares on it fail to compile, as in HLSL). */
static inline int shader_msl_texture(const ShaderTexture *t) {
    static const signed char type[8] = {TEX_2D, TEX_ARRAY_2D, TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_2D, TEX_CUBE, TEX_ARRAY_CUBE};
    return type[t->type & 7];
}

/* Per-draw parameters (cbuffer b0), shared by both stages. */
typedef struct DrawParams {
    u32 attr[32][4];       /* vertex attribute: format word, stream stride, instance divisor (0: per vertex), stream bound */
    float fix[4];          /* clip-space fix-up: x sign, y sign, z scale, z offset */
    float frag[4];         /* fragment y: flip (nonzero), render target height; alpha test: passing outcomes (bits, not a float), reference */
    u32 base[4];           /* base vertex, base instance; 1 / the render targets' render scale in x, y (float bits) */
    u32 swz[2][32];        /* per texture of each stage (vertex, pixel): component sources, 3 bits each; a render target's render
                            * scale (4.12, 0: 1) in bits 16-31 - MSL: bits 12-24 the sampler's LOD bias, and the scale in sb */
    u32 sb[2][16][4];      /* per storage buffer of each stage: GPU address low, high, size; MSL: in the fourth word, textures 2i
                            * and 2i + 1's render scale, as swz has it in HLSL (the low half texture 2i's) */
} DrawParams;

/* Bytes of a program up to its end (header included), reading at most `max`. */
u32 shader_size(const u8 *program, u32 max);
/* Does the program read its pixel's position or step in texels (its translation then takes the
 * render scale into account)? */
int shader_uses_pixels(const u8 *program, u32 size, int stage);
/* Does the program use the SSY/PBK stacks (translated without them since)? */
int shader_uses_stacks(const u8 *program, u32 size);
