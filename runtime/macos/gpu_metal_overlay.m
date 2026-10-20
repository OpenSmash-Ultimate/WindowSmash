/* The debug overlay for the Metal renderer: frame rate, a graph of recent frame times and the
 * renderer's statistics, drawn in the corner of each presented frame; and, while it is open, the
 * settings menu (gpu_menu.c), with the same quads - gpu_overlay.c's, drawn with Metal.
 *
 * Text comes from the glyph atlas the build draws (overlay_font.h: a monospaced font, printable
 * ASCII, plus a solid cell for rectangles), at twice the overlay's own text size so the menu's
 * larger text stays sharp; everything is quads, fetched by the vertex shader from a buffer of
 * the frame's own (one of three: no more frames are with the GPU at once). */
#define _CRT_SECURE_NO_WARNINGS
#include "gpu_metal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gpu_menu.h"
#include "gpu_overlay.h"

#define CELL_W 24                 /* the atlas's cells */
#define CELL_H 48
#define TEXT_W 12                 /* the overlay's own letters: half size */
#define TEXT_H 24
#define COLS 16
#define ATLAS_W (CELL_W * COLS)
#define ATLAS_H (CELL_H * 6)      /* ' '..'~', then the solid cell (127) */
#define MAX_QUADS 8192
#define HISTORY 240
#define FRAMES 3

typedef struct Vertex { float x, y, u, v; unsigned color; } Vertex;

static id<MTLLibrary> lib;
static id<MTLTexture> atlas;
static id<MTLSamplerState> sampler;
static id<MTLBuffer> vbufs[FRAMES];
static id<MTLDepthStencilState> no_depth;
static struct { MTLPixelFormat format; id<MTLRenderPipelineState> pso; } pipes[4];
static int state;   /* 0 not yet, 1 ready, -1 failed */
static unsigned frame;

static Vertex verts[MAX_QUADS * 6];
static unsigned nverts;

static const char MSL[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct I { packed_float2 p; packed_float2 uv; uint c; };\n"   /* (Vertex: 20 bytes) */
    "struct O { float4 p [[position]]; float2 uv; float4 c; };\n"
    "vertex O vs(uint id [[vertex_id]], const device I *v [[buffer(0)]], constant float2 &inv_size [[buffer(1)]]) {\n"
    "  O o; I i = v[id]; o.p = float4(i.p * inv_size * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = i.uv; o.c = unpack_unorm4x8_to_float(i.c); return o; }\n"
    "fragment float4 ps(O i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(i.c.rgb, i.c.a * t.sample(s, i.uv).r); }\n";

/* The glyphs: generated at build time (tools/mac/make_overlay_font.m), coverage in one channel. */
#include "overlay_font.h"
static int make_atlas(id<MTLDevice> dev) {
    static unsigned char coverage[ATLAS_W * ATLAS_H];
    _Static_assert(sizeof overlay_font == sizeof coverage, "overlay_font.h is not the atlas's size: build it again (tools/mac/build_mac.py)");
    memcpy(coverage, overlay_font, sizeof coverage);
    int solid = 127 - 32;   /* (filled away from its edges: scaled-down text samples next to its cell, and 'o' is above) */
    for (int y = 4; y < CELL_H - 4; y++) memset(coverage + (solid / COLS * CELL_H + y) * ATLAS_W + solid % COLS * CELL_W + 4, 255, CELL_W - 8);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:ATLAS_W height:ATLAS_H mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    if (!(atlas = [dev newTextureWithDescriptor:td])) return 0;
    [atlas replaceRegion:MTLRegionMake2D(0, 0, ATLAS_W, ATLAS_H) mipmapLevel:0 withBytes:coverage bytesPerRow:ATLAS_W];
    return 1;
}

static int init(id<MTLDevice> dev) {
    NSError *err = nil;
    if (!(lib = [dev newLibraryWithSource:@(MSL) options:nil error:&err])) {
        fprintf(stderr, "[overlay] %s\n", err.localizedDescription.UTF8String);
        return 0;
    }
    for (int i = 0; i < FRAMES; i++)
        if (!(vbufs[i] = [dev newBufferWithLength:sizeof verts options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined])) return 0;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sampler = [dev newSamplerStateWithDescriptor:sd];
    no_depth = [dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    return sampler && no_depth && make_atlas(dev);
}

static id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, MTLPixelFormat format) {
    for (int i = 0; i < 4; i++)
        if (pipes[i].pso && pipes[i].format == format) return pipes[i].pso;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"ps"];
    MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
    a.pixelFormat = format;
    a.blendingEnabled = YES;
    a.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    a.sourceAlphaBlendFactor = MTLBlendFactorOne;
    a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) { fprintf(stderr, "[overlay] %s\n", err.localizedDescription.UTF8String); return nil; }
    for (int i = 0; i < 4; i++)
        if (!pipes[i].pso) { pipes[i].format = format; pipes[i].pso = pso; break; }
    return pso;
}

/* ---- drawing ------------------------------------------------------------------------- */
static void quad(float x, float y, float w, float h, int cell, unsigned color) {
    if (nverts + 6 > MAX_QUADS * 6) return;
    float u0 = (float)(cell % COLS * CELL_W) / ATLAS_W, v0 = (float)(cell / COLS * CELL_H) / ATLAS_H;
    float u1 = u0 + (float)CELL_W / ATLAS_W, v1 = v0 + (float)CELL_H / ATLAS_H;
    if (cell == 127 - 32) { u0 = u1 = u0 + 0.5f * CELL_W / ATLAS_W; v0 = v1 = v0 + 0.5f * CELL_H / ATLAS_H; }   /* solid: its middle */
    Vertex a = {x, y, u0, v0, color}, b = {x + w, y, u1, v0, color}, c = {x, y + h, u0, v1, color}, d = {x + w, y + h, u1, v1, color};
    Vertex *o = &verts[nverts];
    o[0] = a; o[1] = b; o[2] = c; o[3] = c; o[4] = b; o[5] = d;
    nverts += 6;
}
static void rect(float x, float y, float w, float h, unsigned color) { quad(x, y, w, h, 127 - 32, color); }

#define RGBA OVL_RGBA

/* Text at (x, y), letters size high, one line per '\n'; returns the y below it. */
static float text_sized(float x, float y, float size, unsigned color, const char *s) {
    float cx = x, w = size / 2, shadow = size >= 32 ? 2.0f : 1.0f;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; y += size; continue; }
        int c = (unsigned char)*s;
        if (c > 32 && c < 127) {
            quad(cx + shadow, y + shadow, w, size, c - 32, RGBA(0, 0, 0, 200));   /* shadow */
            quad(cx, y, w, size, c - 32, color);
        }
        cx += w;
    }
    return y + size;
}

static float text(float x, float y, unsigned color, const char *s) { return text_sized(x, y, TEXT_H, color, s); }

void ovl_rect(float x, float y, float w, float h, unsigned color) { rect(x, y, w, h, color); }
float ovl_text(float x, float y, float size, unsigned color, const char *s) { return text_sized(x, y, size, color, s); }

static int lines_of(const char *s, int *widest) {
    int n = 1, w = 0, cur = 0;
    for (; *s; s++) { if (*s == '\n') { n++; cur = 0; } else if (++cur > w) w = cur; }
    *widest = w;
    return n;
}

/* ---- the frame ---------------------------------------------------------------------- */
static void hud(const char *detail);
static double history[HISTORY];   /* milliseconds per frame, newest at [head - 1] */
static unsigned head, filled;

void overlay_present_metal(id<MTLDevice> dev, id<MTLRenderCommandEncoder> enc, MTLPixelFormat format, unsigned x0, unsigned y0, unsigned w,
                           unsigned h, int show, const char *detail) {
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        history[head] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        head = (head + 1) % HISTORY;
        if (filled < HISTORY) filled++;
    }
    last = now;
    int menu = menu_visible();
    if (!show && !menu) return;
    if (!state) state = init(dev) ? 1 : -1;
    if (state < 0) return;
    id<MTLRenderPipelineState> pso = pipeline(dev, format);
    if (!pso) return;
    nverts = 0;
    if (show && !menu_blocks_input()) hud(detail);   /* (an open menu instead of the overlay) */
    if (menu) menu_draw((float)w, (float)h);
    if (!nverts) return;

    /* draw it all */
    id<MTLBuffer> vbuf = vbufs[frame++ % FRAMES];
    memcpy(vbuf.contents, verts, nverts * sizeof *verts);
    float inv[2] = {1.0f / (float)w, 1.0f / (float)h};
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:no_depth];
    [enc setCullMode:MTLCullModeNone];
    [enc setViewport:(MTLViewport){x0, y0, w, h, 0, 1}];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:inv length:sizeof inv atIndex:1];
    [enc setFragmentTexture:atlas atIndex:0];
    [enc setFragmentSamplerState:sampler atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:nverts];
}

/* The overlay: the last second's frame rate and a graph of the last 240 frames' times. */
static void hud(const char *detail) {
    /* the last second: frames, slowest and fastest */
    double total = 0, worst = 0, best = 1e9;
    unsigned frames = 0;
    for (unsigned i = 0; i < filled && total < 1000.0; i++) {
        double ms = history[(head + HISTORY - 1 - i) % HISTORY];
        total += ms; frames++;
        if (ms > worst) worst = ms;
        if (ms < best) best = ms;
    }
    double fps = total > 0 ? frames * 1000.0 / total : 0, last_ms = filled ? history[(head + HISTORY - 1) % HISTORY] : 0;
    char line[256];
    float x = 16, y = 16, gw = 2.0f * HISTORY, gh = 100;   /* graph: 2 px a frame; 100 px = 50 ms */
    int detail_w = 0, detail_lines = detail ? lines_of(detail, &detail_w) : 0;
    float panel_w = gw + 16, panel_h = 2 * TEXT_H + 8 + gh + 12 + detail_lines * TEXT_H;
    if (detail_w * TEXT_W + 16 > panel_w) panel_w = (float)(detail_w * TEXT_W + 16);
    rect(x - 8, y - 6, panel_w, panel_h, RGBA(0, 0, 0, 150));
    unsigned fps_color = fps >= 57 ? RGBA(120, 255, 120, 255) : fps >= 45 ? RGBA(255, 230, 90, 255) : RGBA(255, 100, 90, 255);
    snprintf(line, sizeof line, "%5.1f FPS  %5.1f ms", fps, last_ms);
    y = text(x, y, fps_color, line);
    snprintf(line, sizeof line, "last second: %.1f-%.1f ms", frames ? best : 0, worst);
    y = text(x, y, RGBA(230, 230, 230, 255), line);
    y += 6;
    /* the graph: a bar per frame, its height its time; lines at 60 and 30 frames a second */
    float scale = gh / 50.0f;
    rect(x, y, gw, gh, RGBA(20, 20, 20, 160));
    for (unsigned i = 0; i < filled; i++) {
        double ms = history[(head + HISTORY - filled + i) % HISTORY];
        float bh = (float)(ms > 50 ? 50 : ms) * scale;
        unsigned c = ms <= 17.5 ? RGBA(90, 220, 90, 230) : ms <= 34 ? RGBA(240, 210, 60, 230) : RGBA(240, 70, 60, 240);
        rect(x + gw - 2.0f * (filled - i), y + gh - bh, 2, bh, c);
    }
    rect(x, y + gh - 16.67f * scale, gw, 1, RGBA(255, 255, 255, 140));
    rect(x, y + gh - 33.33f * scale, gw, 1, RGBA(255, 255, 255, 70));
    y += gh + 6;
    if (detail) text(x, y, RGBA(210, 225, 255, 255), detail);
}
