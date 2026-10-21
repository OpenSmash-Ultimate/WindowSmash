/* The rendering back end: Direct3D 11.
 *
 * The game's render targets live on the host GPU: each is a D3D11 texture keyed by the guest
 * address the console would have rendered into. When the display shows a buffer, the
 * matching texture is drawn into the window.
 *
 * Every entry point takes the back end's lock: GPU work arrives from the game's submitting
 * threads, frames from the display's vsync thread. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "debug_profile.h"
#include "display.h"
#include "gpu_backend.h"
#include "gpu_menu.h"
#include "gpu_overlay.h"
#include "net.h"
#include "pad.h"
#include "gpu_shader.h"
#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#ifdef _WIN32
#include <ctype.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#else
#include <errno.h>
#include <sys/uio.h>   /* process_vm_readv */
#include <unistd.h>
#endif

static HWND window;
static ID3D11Device *dev, *compile_dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *swapchain;
static UINT swapchain_flags;
/* The game's picture at its own size (1920x1080 times the render scale), before it is fitted
 * into the window (screenshots read it): its bytes as the game's framebuffer has them (sRGB),
 * read plainly (final_srv) or as sRGB (final_srv_srgb: shrinking it averages in linear light). */
static ID3D11Texture2D *final_tex;
static ID3D11RenderTargetView *final_rtv;
static ID3D11ShaderResourceView *final_srv, *final_srv_srgb;
static u32 final_w, final_h;
static ID3D11VertexShader *blit_vs;
static ID3D11PixelShader *blit_ps, *blit2d_ps, *blit_alpha_ps, *decode_ps, *down_ps;
static ID3D11Buffer *down_cb;
static ID3D11SamplerState *linear_sampler;
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static int ready;

u64 gfx_stat_clears, gfx_stat_presented;

/* ---- surfaces: render targets on the host ------------------------------------------- */
/* Render scale (display.ini render_scale): every render target is made this many times the
 * game's size in each direction, and the draw state given in the game's pixels (viewports,
 * scissors, blits, the fragment position) is scaled to match. 1 = the game's own size. Fixed at
 * the device's start (see init). */
static float render_scale = 1.0f;

typedef struct Surface {
    u64 guest;
    u32 width, height, format, depth;     /* the game's size: what the surface is known and matched by */
    u32 hw, hh;                           /* the host texture's size (the game's, scaled) */
    float sx, sy;                         /* hw / width, hh / height */
    DXGI_FORMAT storage, view;            /* the texture's format, and how it is rendered to and sampled */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
    ID3D11ShaderResourceView *srv_other;  /* 8-bit colour: the other reading of the bytes (sRGB / plain) */
    u64 used;
} Surface;

static Surface surfaces[2048];
static u32 nsurfaces;
static void bury(void *object);   /* (released after the frame is presented) */
static u64 tick;

/* Maxwell render target formats (color) to DXGI. */
static DXGI_FORMAT color_format(u32 f) {
    switch (f) {
    case 0xC0: case 0xC3: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 0xC1: case 0xC4: return DXGI_FORMAT_R32G32B32A32_SINT;
    case 0xC2: case 0xC5: return DXGI_FORMAT_R32G32B32A32_UINT;
    case 0xC6: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 0xC7: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 0xC8: return DXGI_FORMAT_R16G16B16A16_SINT;
    case 0xC9: return DXGI_FORMAT_R16G16B16A16_UINT;
    case 0xCA: case 0xCE: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 0xCB: return DXGI_FORMAT_R32G32_FLOAT;
    case 0xCC: return DXGI_FORMAT_R32G32_SINT;
    case 0xCD: return DXGI_FORMAT_R32G32_UINT;
    case 0xCF: case 0xE6: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 0xD0: case 0xE7: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 0xD1: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 0xD2: return DXGI_FORMAT_R10G10B10A2_UINT;
    case 0xD5: case 0xF9: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 0xD6: case 0xFA: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 0xD7: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case 0xD8: return DXGI_FORMAT_R8G8B8A8_SINT;
    case 0xD9: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 0xDA: return DXGI_FORMAT_R16G16_UNORM;
    case 0xDB: return DXGI_FORMAT_R16G16_SNORM;
    case 0xDC: return DXGI_FORMAT_R16G16_SINT;
    case 0xDD: return DXGI_FORMAT_R16G16_UINT;
    case 0xDE: return DXGI_FORMAT_R16G16_FLOAT;
    case 0xE0: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 0xE3: return DXGI_FORMAT_R32_SINT;
    case 0xE4: return DXGI_FORMAT_R32_UINT;
    case 0xE5: return DXGI_FORMAT_R32_FLOAT;
    case 0xE8: return DXGI_FORMAT_B5G6R5_UNORM;
    case 0xE9: case 0xF8: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 0xEA: return DXGI_FORMAT_R8G8_UNORM;
    case 0xEB: return DXGI_FORMAT_R8G8_SNORM;
    case 0xEC: return DXGI_FORMAT_R8G8_SINT;
    case 0xED: return DXGI_FORMAT_R8G8_UINT;
    case 0xEE: return DXGI_FORMAT_R16_UNORM;
    case 0xEF: return DXGI_FORMAT_R16_SNORM;
    case 0xF0: return DXGI_FORMAT_R16_SINT;
    case 0xF1: return DXGI_FORMAT_R16_UINT;
    case 0xF2: return DXGI_FORMAT_R16_FLOAT;
    case 0xF3: return DXGI_FORMAT_R8_UNORM;
    case 0xF4: return DXGI_FORMAT_R8_SNORM;
    case 0xF5: return DXGI_FORMAT_R8_SINT;
    case 0xF6: return DXGI_FORMAT_R8_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

/* Maxwell depth formats: the texture's typeless storage, and how it is used as depth. */
static DXGI_FORMAT depth_format(u32 f, DXGI_FORMAT *typeless) {
    switch (f) {
    case 0x0A: *typeless = DXGI_FORMAT_R32_TYPELESS; return DXGI_FORMAT_D32_FLOAT;              /* Z32_FLOAT */
    case 0x13: *typeless = DXGI_FORMAT_R16_TYPELESS; return DXGI_FORMAT_D16_UNORM;              /* Z16_UNORM */
    case 0x14: case 0x15: case 0x16:                                                            /* Z24 + S8 variants */
        *typeless = DXGI_FORMAT_R24G8_TYPELESS; return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 0x19: *typeless = DXGI_FORMAT_R32G8X24_TYPELESS; return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;  /* Z32 + S8 */
    }
    *typeless = DXGI_FORMAT_UNKNOWN;
    return DXGI_FORMAT_UNKNOWN;
}

static void release_views(Surface *s) {
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->srv_other) ID3D11ShaderResourceView_Release(s->srv_other);
    s->rtv = NULL; s->dsv = NULL; s->srv = NULL; s->srv_other = NULL;
}

static void release_surface(Surface *s) {
    release_views(s);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
}

/* 8-bit colour is stored typeless: the hardware writes and reads the same bytes as sRGB or as
 * plain values according to each use's format (render target, texture header), and a surface
 * keeps its contents across such changes. */
static DXGI_FORMAT storage_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
    }
}
static int is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
static DXGI_FORMAT other_gamma(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

/* How a depth surface is sampled (shadow maps): its depth as the red channel. */
static DXGI_FORMAT depth_sampled(DXGI_FORMAT dsv) {
    switch (dsv) {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static ID3D11ShaderResourceView *make_srv(ID3D11Texture2D *tex, DXGI_FORMAT f) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *v = NULL;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, &sd, &v))) return NULL;
    return v;
}

/* The views of a surface rendered as `view` (a colour format, or a depth one). */
static void make_views(Surface *s, DXGI_FORMAT view) {
    s->view = view;
    if (s->depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {view, D3D11_DSV_DIMENSION_TEXTURE2D, 0};
        ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->tex, &dd, &s->dsv);
        DXGI_FORMAT sampled = depth_sampled(view);
        if (sampled != DXGI_FORMAT_UNKNOWN) s->srv = make_srv(s->tex, sampled);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = view;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s->tex, &rd, &s->rtv);
        s->srv = make_srv(s->tex, view);
    }
}

/* A surface's bytes read as sRGB (srgb) or as plain values, for an 8-bit colour surface; its
 * own view otherwise. */
static ID3D11ShaderResourceView *surface_srv(Surface *s, int srgb) {
    DXGI_FORMAT other = other_gamma(s->view);
    if (other == DXGI_FORMAT_UNKNOWN || srgb == is_srgb(s->view)) return s->srv;
    if (!s->srv_other) s->srv_other = make_srv(s->tex, other);
    return s->srv_other ? s->srv_other : s->srv;
}

static void warn_format(const char *what, u32 f) {
    static u32 seen[64], n;
    for (u32 i = 0; i < n; i++) if (seen[i] == (f | (what[0] << 24))) return;
    if (n < 64) seen[n++] = f | (what[0] << 24);
    fprintf(stderr, "[gfx] unsupported %s format %#x\n", what, f);
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
 * or depth), so a colour target and the depth target drawn with it always match, as Direct3D
 * requires. A target too big to scale stays at the game's size. */
static void scaled_size(u32 w, u32 h, u32 *hw, u32 *hh) {
    *hw = w; *hh = h;
    if (render_scale == 1.0f || scale_skipped(w, h)) return;
    float x = (float)w * render_scale + 0.5f, y = (float)h * render_scale + 0.5f;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > 16384 || y > 16384) return;
    *hw = (u32)x; *hh = (u32)y;
}

/* The host texture standing for the render target at `guest`, created on first use. */
static Surface *surface(u64 guest, u32 width, u32 height, u32 format, int depth) {
    if (!guest || !width || !height || width > 16384 || height > 16384) return NULL;
    Surface *free_slot = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex) { if (!free_slot) free_slot = s; continue; }
        if (s->guest == guest && s->depth == depth) {
            if (s->width == width && s->height == height && s->format == format) { s->used = ++tick; return s; }
            DXGI_FORMAT now = depth ? DXGI_FORMAT_UNKNOWN : color_format(format);
            if (s->width == width && s->height == height && now != DXGI_FORMAT_UNKNOWN && storage_of(now) == s->storage) {
                bury(s->rtv); bury(s->srv); bury(s->srv_other);   /* the same bytes, now sRGB or plain: new views (the old may be bound) */
                s->rtv = NULL; s->srv = NULL; s->srv_other = NULL;
                make_views(s, now);
                s->format = format;
                s->used = ++tick;
                return s;
            }
            release_surface(s);   /* the memory now holds a different surface */
            if (!free_slot) free_slot = s;
        }
    }
    DXGI_FORMAT view, storage;
    if (depth) view = depth_format(format, &storage);
    else storage = storage_of(view = color_format(format));
    if (view == DXGI_FORMAT_UNKNOWN) { warn_format(depth ? "depth" : "color", format); return NULL; }

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
    D3D11_TEXTURE2D_DESC td = {hw, hh, 1, 1, storage, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET), 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->tex))) { s->tex = NULL; return NULL; }
    s->depth = depth;
    s->storage = storage;
    make_views(s, view);
    s->guest = guest; s->width = width; s->height = height; s->format = format;
    s->hw = hw; s->hh = hh;
    s->sx = (float)hw / (float)width; s->sy = (float)hh / (float)height;
    s->used = ++tick;
    return s;
}

/* ---- window and device ------------------------------------------------------------ */
/* Debug keys, taken at the next present: F1 the overlay, F2 its details, F3 a screenshot,
 * F4 a log of the next frame's draws and its render targets, F5 depth sampling on/off, F6 a
 * 10-second sampling profile. */
static volatile LONG hotkeys;

#ifdef _WIN32
/* The settings menu's keys (gpu_menu.c): Esc always, the others while it is open. */
static int menu_keys(UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CHAR) {
        if (!menu_is_open()) return 0;
        menu_char((unsigned)w);
        return 1;
    }
    if (msg != WM_KEYDOWN) return 0;
    int repeat = (l & (1 << 30)) != 0, key = 0;
    switch (w) {
    case VK_ESCAPE: if (!repeat) key = MENU_KEY_ESC; break;
    case VK_UP: key = MENU_KEY_UP; break;
    case VK_DOWN: key = MENU_KEY_DOWN; break;
    case VK_LEFT: key = MENU_KEY_LEFT; break;
    case VK_RIGHT: key = MENU_KEY_RIGHT; break;
    case VK_RETURN: if (!repeat) key = MENU_KEY_ENTER; break;
    case VK_BACK: key = MENU_KEY_BACKSPACE; break;
    case VK_DELETE: key = MENU_KEY_DELETE; break;
    case VK_TAB: if (!repeat) key = GetKeyState(VK_SHIFT) & 0x8000 ? MENU_KEY_SHIFT_TAB : MENU_KEY_TAB; break;
    }
    if (!key || (key != MENU_KEY_ESC && !menu_is_open())) return 0;
    menu_key(key);
    return 1;
}

static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    LRESULT result;
    if (display_message(h, msg, w, l, &result)) return result;   /* size, full screen, pointer (gpu_display.c) */
    if (msg == WM_CLOSE) { fprintf(stderr, "[gfx] window closed\n"); gfx_closing(); fflush(stderr); ExitProcess(0); }
    if (msg == WM_KEYDOWN && w >= VK_F1 && w <= VK_F6 && !(l & (1 << 30))) InterlockedOr(&hotkeys, 1 << (w - VK_F1));
    if (menu_keys(msg, w, l)) return 0;
    return DefWindowProcW(h, msg, w, l);
}

static DWORD WINAPI window_thread(LPVOID done) {
    SetThreadDescription(GetCurrentThread(), L"window");
    WNDCLASSW wc = {CS_HREDRAW | CS_VREDRAW, window_proc, 0, 0, GetModuleHandleW(NULL), NULL,
                    LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"ssbu"};
    RegisterClassW(&wc);
    window = display_create_window(wc.hInstance, L"ssbu", L"Super Smash Bros. Ultimate (native)");
    SetEvent((HANDLE)done);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
#else
/* Linux: the window is SDL's, made and run by the main thread (linux/window.c), which passes
 * the debug keys on here; DXVK takes the SDL_Window as the swap chain's HWND. */
void *rt_window(void);
void gfx_hotkey(int n) { InterlockedOr(&hotkeys, 1 << n); }
#endif

/* The picture's texture at w x h (the presented surface's host size), made again when that
 * changes. Typeless: written and saved as the plain bytes, shrunk through an sRGB view. */
static int make_final(u32 w, u32 h) {
    if (final_tex && final_w == w && final_h == h) return 1;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv, *srv_srgb;
    D3D11_TEXTURE2D_DESC fd = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &fd, NULL, &tex))) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &rd, &rtv))) { ID3D11Texture2D_Release(tex); return 0; }
    srv = make_srv(tex, DXGI_FORMAT_R8G8B8A8_UNORM);
    srv_srgb = make_srv(tex, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (!srv || !srv_srgb) {
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (srv_srgb) ID3D11ShaderResourceView_Release(srv_srgb);
        ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex);
        return 0;
    }
    if (final_tex) { bury(final_rtv); bury(final_srv); bury(final_srv_srgb); bury(final_tex); }
    final_tex = tex; final_rtv = rtv; final_srv = srv; final_srv_srgb = srv_srgb;
    final_w = w; final_h = h;
    return 1;
}

static ID3DBlob *compile_hlsl(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors))) {
        fprintf(stderr, "[gfx] shader compile failed: %s\n", errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        rt_fatal("cannot compile a built-in shader");
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static const char BLIT_HLSL[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
    "float4 psa(V i) : SV_Target { float a = t.Sample(s, i.uv).a; return float4(a, a, a, 1); }\n"   /* debug: alpha as grey */
    "cbuffer K : register(b2) { float4 kcolor; };\n"
    "float4 psclear(V i) : SV_Target { return kcolor; }\n"   /* a clear of some channels: drawn, with a write mask */
    /* the 2D engine's blit: destination pixel p reads source point src0 + (p - dst0) * step */
    "cbuffer R : register(b1) { float4 src0_step; float4 dst0_inv; };\n"
    "float4 ps2d(V i) : SV_Target { return t.Sample(s, (src0_step.xy + (i.pos.xy - dst0_inv.xy) * src0_step.zw) * dst0_inv.zw); }\n"
    /* the picture shrunk into the window (render scale): each window pixel the average of the
     * picture's pixels under it, n x n bilinear taps spread over its footprint (read and written
     * through sRGB views: averaged in linear light) */
    "cbuffer D : register(b3) { float4 dsize; float4 dtaps; };\n"   /* picture w, h, 1/w, 1/h; taps x, y; footprint x, y */
    "float4 psdown(V i) : SV_Target { float2 c = i.uv * dsize.xy - dtaps.zw * 0.5f; float3 sum = 0;\n"
    "  [loop] for (int y = 0; y < (int)dtaps.y; y++) [loop] for (int x = 0; x < (int)dtaps.x; x++)\n"
    "    sum += t.SampleLevel(s, (c + dtaps.zw * (float2(x, y) + 0.5f) / dtaps.xy) * dsize.zw, 0).rgb;\n"
    "  return float4(sum / (dtaps.x * dtaps.y), 1); }\n";

/* (get_texture) one level of a block-compressed texture, texel by texel, into its exact-size copy */
static const char DECODE_HLSL[] =
    "Texture2DArray<float4> src : register(t0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 main(V i) : SV_Target { return src.Load(int4(int2(i.pos.xy), 0, 0)); }\n";

static void start_compilers(void);                 /* (see "shaders") */
static DWORD WINAPI prewarm_main(void *unused);
static int warm_mode;                              /* (see "first draws") 0 off, 1 the warm after the game, 2 also at start */
static LUID warm_adapter;
static void warm_start(void);
#ifdef _WIN32
#define WARM_GAME "Local\\ssbu-game-running"
static HANDLE game_mutex;                          /* (WARM_GAME) held while the game runs: ssbu --warm-shaders stops for it */
#endif

/* Direct3D 11 is DXVK's: always on Linux; on Windows when d3d11.dll is not the system's (the
 * DXVK test builds of tools/linux/wintest.py). */
static int on_dxvk(void) {
#ifdef _WIN32
    wchar_t path[MAX_PATH], sys[MAX_PATH];
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return m && n && n < MAX_PATH && GetModuleFileNameW(m, path, MAX_PATH) && _wcsnicmp(path, sys, n) != 0;
#else
    return 1;
#endif
}

#ifdef _WIN32
/* ---- the graphics driver's threads: big stacks --------------------------------------------
 * NVIDIA's driver starts its worker threads (which compile shaders to GPU code, among their
 * work) with 64 KB stacks, and its compiler (nvgpucomp64.dll) recurses over a program's blocks:
 * some of the game's programs run that out, and the stack overflow ends the game the moment the
 * driver compiles one - players saw it when an effect first showed (the Daybreak item,
 * Sephiroth's specials). The compile device below does not cover every compile the rendering
 * device makes. So every thread a driver DLL (one from the system's DriverStore) starts gets a
 * 16 MB stack reservation: as each DLL loads, its CreateThread import is pointed here.
 * SSBU_DRIVER_STACKS=0: the driver's own sizes. */
#define DRIVER_STACK (16ull << 20)

static HANDLE WINAPI driver_create_thread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags,
                                          LPDWORD id) {
    if (stack < DRIVER_STACK) {
        stack = DRIVER_STACK;
        flags |= STACK_SIZE_PARAM_IS_A_RESERVATION;
    }
    return CreateThread(sa, stack, fn, arg, flags, id);
}

static void driver_imports(BYTE *base) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *name = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk), *slot = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; name->u1.AddressOfData; name++, slot++) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) || strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + name->u1.AddressOfData))->Name, "CreateThread"))
                continue;
            DWORD old;
            if (!VirtualProtect(&slot->u1.Function, sizeof slot->u1.Function, PAGE_READWRITE, &old)) continue;
            slot->u1.Function = (ULONG_PTR)driver_create_thread;
            VirtualProtect(&slot->u1.Function, sizeof slot->u1.Function, old, &old);
        }
    }
}

typedef struct { USHORT length, max; WCHAR *text; } NtString;
typedef struct { ULONG flags; const NtString *path, *name; void *base; ULONG size; } DllLoaded;

/* A DLL's path (len characters) is a driver's: in the system's DriverStore. */
static int in_driver_store(const WCHAR *path, size_t len) {
    static const WCHAR store[] = L"\\DriverStore\\";
    const size_t n = sizeof store / sizeof *store - 1;
    for (size_t i = 0; i + n <= len; i++)
        if (!_wcsnicmp(path + i, store, n)) return 1;
    return 0;
}

static void CALLBACK on_dll_loaded(ULONG reason, const DllLoaded *dll, void *unused) {
    (void)unused;
    if (reason != 1) return;   /* (LDR_DLL_NOTIFICATION_REASON_LOADED) */
    if (in_driver_store(dll->path->text, dll->path->length / sizeof(WCHAR))) driver_imports(dll->base);
}

static void driver_stacks(void) {
    static void *cookie;
    if (getenv("SSBU_DRIVER_STACKS") && !atoi(getenv("SSBU_DRIVER_STACKS"))) return;
    LONG (NTAPI *reg)(ULONG, void (CALLBACK *)(ULONG, const DllLoaded *, void *), void *, void **) =
        (LONG (NTAPI *)(ULONG, void (CALLBACK *)(ULONG, const DllLoaded *, void *), void *, void **))(void *)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (!reg || reg(0, on_dll_loaded, NULL, &cookie) < 0) fprintf(stderr, "[gfx] note: the driver's threads keep their own stack size\n");
}

/* ---- the graphics card ---------------------------------------------------------------------
 * Laptops with two graphics chips make the processor's own (Intel's) the system's default, and
 * the game used to draw with that. display.ini's gpu: "auto" takes the card Windows calls
 * high-performance (the NVIDIA or AMD chip beside it; on a PC with one card, that card), any
 * other text the first card whose name contains it ("intel", "radeon", "4060"), "warp" Windows'
 * software renderer (slow: for tests). The two exports ask the NVIDIA and AMD drivers of such
 * laptops for their chip too, for whatever else would take the default. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
/* IDXGIFactory6's interface ID (dxgi1_6.h's), here rather than IID_IDXGIFactory6 from dxguid.lib:
 * older Windows SDKs' dxguid.lib lacks it though their dxgi1_6.h declares it, and players building
 * with such an SDK had the link stop on "undefined symbol: IID_IDXGIFactory6". */
static const IID IID_IDXGIFactory6_ = {0xC1B6694F, 0xFF09, 0x44A9, {0xB0, 0x3C, 0x77, 0x90, 0x0A, 0x0A, 0x1D, 0x17}};

static void adapter_name(const DXGI_ADAPTER_DESC1 *d, char *out, size_t size) {
    size_t i = 0;
    for (; i + 1 < size && d->Description[i]; i++) out[i] = d->Description[i] < 128 ? (char)d->Description[i] : '?';
    out[i] = 0;
}

/* The graphics cards' names, fastest first, without the software ones (the settings menu). */
int gfx_adapter_names(char names[][64], int max) {
    IDXGIFactory1 *f1;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) return 0;
    IDXGIFactory6 *f6 = NULL;
    if (FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6_, (void **)&f6))) f6 = NULL;
    int n = 0;
    for (UINT i = 0; n < max; i++) {
        IDXGIAdapter1 *a = NULL;
        HRESULT hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&a)
                        : IDXGIFactory1_EnumAdapters1(f1, i, &a);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            char name[128];
            adapter_name(&d, name, sizeof name);
            int again = 0;
            for (int k = 0; k < n; k++) again |= !strcmp(names[k], name);
            if (!again) snprintf(names[n++], 64, "%s", name);
        }
        IDXGIAdapter1_Release(a);
    }
    if (f6) IDXGIFactory6_Release(f6);
    IDXGIFactory1_Release(f1);
    return n;
}

/* The adapter to create the device on (referenced), or NULL for the system's default; *warp:
 * the software renderer was asked for. */
static IDXGIAdapter1 *pick_adapter(int *warp) {
    const char *want = display_gpu();
    *warp = !_stricmp(want, "warp");
    if (*warp) return NULL;
    int any = !*want || !_stricmp(want, "auto");
    IDXGIFactory1 *f1;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) return NULL;
    IDXGIFactory6 *f6 = NULL;
    if (FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory6_, (void **)&f6))) f6 = NULL;   /* (before Windows 10 1803: the system's order) */
    char w[64];
    size_t k = 0;
    for (; k + 1 < sizeof w && want[k]; k++) w[k] = (char)tolower((unsigned char)want[k]);
    w[k] = 0;
    IDXGIAdapter1 *fastest = NULL, *named = NULL;   /* (fastest first) */
    char list[1024] = "";
    DXGI_ADAPTER_DESC1 seen[8];
    UINT nseen = 0;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *a = NULL;
        HRESULT hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&a)
                        : IDXGIFactory1_EnumAdapters1(f1, i, &a);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 d;
        if (FAILED(IDXGIAdapter1_GetDesc1(a, &d))) { IDXGIAdapter1_Release(a); continue; }
        int again = 0;   /* (a card can be listed twice, e.g. once more for a virtual display) */
        for (UINT j = 0; j < nseen; j++)
            again |= seen[j].VendorId == d.VendorId && seen[j].DeviceId == d.DeviceId && seen[j].SubSysId == d.SubSysId &&
                     seen[j].DedicatedVideoMemory == d.DedicatedVideoMemory && !wcscmp(seen[j].Description, d.Description);
        if (again) { IDXGIAdapter1_Release(a); continue; }
        if (nseen < 8) seen[nseen++] = d;
        char name[128], lower[128];
        adapter_name(&d, name, sizeof name);
        for (k = 0; k < sizeof lower; k++) lower[k] = (char)tolower((unsigned char)name[k]);
        int software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        size_t l = strlen(list);
        snprintf(list + l, sizeof list - l, "%s%s (vendor %#x, %.1f GB%s)", l ? "; " : "", name, d.VendorId,
                 (double)d.DedicatedVideoMemory / (1 << 30), software ? ", software" : "");
        if (!software && !any && !named && strstr(lower, w)) { named = a; IDXGIAdapter1_AddRef(a); }
        if (!software && !fastest) fastest = a;
        else IDXGIAdapter1_Release(a);
    }
    if (f6) IDXGIFactory6_Release(f6);
    IDXGIFactory1_Release(f1);
    fprintf(stderr, "[gfx] graphics cards: %s\n", list[0] ? list : "none listed");
    if (any) return fastest;
    if (!named) { fprintf(stderr, "[gfx] display.ini: no graphics card's name contains '%s': using the fastest\n", want); return fastest; }
    if (fastest) IDXGIAdapter1_Release(fastest);
    return named;
}

/* SSBU_D3D_DEBUG=1: Direct3D's debug layer (Windows' optional "Graphics Tools" feature) checks
 * every call; its complaints go to the log at each present - each kind the first 3 times, then
 * every 1000th - as uses of the API one driver forgives and another does not. */
static ID3D11InfoQueue *debug_queue;

static void debug_messages(void) {
    if (!debug_queue) return;
    static struct { D3D11_MESSAGE_ID id; u32 count; } seen[512];
    UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(debug_queue);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(ID3D11InfoQueue_GetMessage(debug_queue, i, NULL, &len)) || !len) continue;
        D3D11_MESSAGE *m = malloc(len);
        if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(debug_queue, i, m, &len))) {
            u32 h = (u32)m->ID % 512, probe = 0;
            while (seen[h].count && seen[h].id != m->ID && probe++ < 512) h = (h + 1) % 512;
            seen[h].id = m->ID;
            u32 c = ++seen[h].count;
            if (c <= 3 || c % 1000 == 0)
                fprintf(stderr, "[d3d debug] %s #%d (%u so far): %.*s\n", m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" : "warning",
                        (int)m->ID, c, (int)m->DescriptionByteLength, m->pDescription);
        }
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(debug_queue);
}
#else
static void debug_messages(void) {}
#endif

static BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&lock);
#ifdef _WIN32
    driver_stacks();   /* (before Direct3D loads the driver) */
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, done, 0, NULL);
    WaitForSingleObject(done, INFINITE);
#else
    window = (HWND)rt_window();
#endif
    if (!window) { fprintf(stderr, "[gfx] cannot create the window\n"); return TRUE; }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (getenv("SSBU_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    /* SSBU_D3D_DRIVER_THREADS=0: the driver does its work on the thread that calls it (as AMD's
     * and Intel's mostly do) - for measuring what the renderer costs such a driver */
    UINT render_flags = getenv("SSBU_D3D_DRIVER_THREADS") && !atoi(getenv("SSBU_D3D_DRIVER_THREADS"))
                        ? D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS : 0;
    IDXGIAdapter1 *chosen = NULL;
    int warp = 0;
#ifdef _WIN32
    chosen = pick_adapter(&warp);
#endif
    HRESULT made = D3D11CreateDevice((IDXGIAdapter *)chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
                                     NULL, flags | render_flags, levels, 2, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (chosen) IDXGIAdapter1_Release(chosen);
    if (FAILED(made) && (chosen || warp))   /* (then the system's default) */
        made = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags | render_flags, levels, 2, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(made)) {
        fprintf(stderr, "[gfx] cannot create a Direct3D 11 device (%#lx)%s\n", (unsigned long)made,
                flags & D3D11_CREATE_DEVICE_DEBUG ? ": SSBU_D3D_DEBUG needs Windows' optional Graphics Tools feature" : "");
        return TRUE;
    }
#ifdef _WIN32
    if ((flags & D3D11_CREATE_DEVICE_DEBUG) && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11InfoQueue, (void **)&debug_queue))) {
        D3D11_MESSAGE_SEVERITY quiet[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
        D3D11_INFO_QUEUE_FILTER filter;
        memset(&filter, 0, sizeof filter);
        filter.DenyList.NumSeverities = 2;
        filter.DenyList.pSeverityList = quiet;
        ID3D11InfoQueue_AddStorageFilterEntries(debug_queue, &filter);
        ID3D11InfoQueue_SetMessageCountLimit(debug_queue, 4096);
    }
#endif
    IDXGIDevice *dxgi_dev; IDXGIAdapter *adapter; IDXGIFactory2 *factory;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev);
    IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    /* NVIDIA's: a second device, with the driver's internal threading off, that compiles each
     * shader first on our compiler threads (see "shaders"). SSBU_COMPILE_DEVICE=0/1 overrides. */
    DXGI_ADAPTER_DESC ad;
    memset(&ad, 0, sizeof ad);
    int nvidia = SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)) && ad.VendorId == 0x10DE;
    char name[128];
    {
        size_t i = 0;
        for (; i < 127 && ad.Description[i]; i++) name[i] = ad.Description[i] < 128 ? (char)ad.Description[i] : '?';
        name[i] = 0;
    }
#ifdef _WIN32
    {   /* (and what its driver says it can do: for reports from other cards) */
        LARGE_INTEGER umd;
        D3D11_FEATURE_DATA_THREADING th;
        memset(&umd, 0, sizeof umd);
        memset(&th, 0, sizeof th);
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_THREADING, &th, sizeof th);
        IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd);
        unsigned fl = (unsigned)ID3D11Device_GetFeatureLevel(dev);
        fprintf(stderr, "[gfx] drawing with %s (vendor %#x, driver %u.%u.%u.%u, feature level %u.%u, concurrent creates %s, command lists %s)\n",
                name, ad.VendorId, (unsigned)(umd.HighPart >> 16), (unsigned)(umd.HighPart & 0xFFFF), (unsigned)((u32)umd.LowPart >> 16),
                (unsigned)(umd.LowPart & 0xFFFF), fl >> 12 & 15, fl >> 8 & 15, th.DriverConcurrentCreates ? "yes" : "no",
                th.DriverCommandLists ? "yes" : "no");
    }
#else
    {   /* (DXVK: which Vulkan device it chose) */
        fprintf(stderr, "[gfx] Direct3D 11 through DXVK on %s (vendor %#x)\n", name, ad.VendorId);
        if (ad.VendorId == 0x10005 || strstr(name, "llvmpipe") || strstr(name, "lavapipe"))
            fprintf(stderr, "[gfx] note: this is a software renderer (no Vulkan GPU driver found): expect a low frame rate\n");
    }
    nvidia = 0;   /* (the compile device is for NVIDIA's Direct3D driver; DXVK compiles its own way) */
#endif
    const char *cd = getenv("SSBU_COMPILE_DEVICE");
    if ((cd ? atoi(cd) : nvidia) &&
        FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                                 levels, 2, D3D11_SDK_VERSION, &compile_dev, NULL, NULL)))
        compile_dev = NULL;
    /* AMD's: a program's GPU code is made by the driver's own threads, from its disk cache when
     * that has it (see "first draws"). SSBU_SHADER_WARM=0/1/2 overrides. */
    const char *wv = getenv("SSBU_SHADER_WARM");
    warm_mode = wv ? atoi(wv) : ad.VendorId == 0x1002 && !on_dxvk() ? 1 : 0;
    warm_adapter = ad.AdapterLuid;
    /* The back buffer is the window's inside (display.ini sizes the window). Where the system
     * allows it, the swap chain may tear, which VRR pacing asks for at each Present. */
    IDXGIFactory5 *factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&factory5))) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
        IDXGIFactory5_Release(factory5);
    }
    swapchain_flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    display_set_tearing(tearing != FALSE);
    DXGI_SWAP_CHAIN_DESC1 sd = {0, 0, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, swapchain_flags};
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)dev, window, &sd, NULL, NULL, &swapchain))) {
        fprintf(stderr, "[gfx] cannot create the swap chain\n");
        return TRUE;
    }
    /* full screen is gpu_display.c's (a borderless window), never DXGI's own */
    IDXGIFactory2_MakeWindowAssociation(factory, window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    IDXGIDevice1 *dxgi_dev1;
    if (SUCCEEDED(IDXGIDevice_QueryInterface(dxgi_dev, &IID_IDXGIDevice1, (void **)&dxgi_dev1))) {
        IDXGIDevice1_SetMaximumFrameLatency(dxgi_dev1, (UINT)display_latency());
        IDXGIDevice1_Release(dxgi_dev1);
    }
    IDXGIFactory2_Release(factory); IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi_dev);

    ID3DBlob *vs = compile_hlsl(BLIT_HLSL, "vs", "vs_5_0"), *ps = compile_hlsl(BLIT_HLSL, "ps", "ps_5_0");
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &blit_vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit_ps);
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    if ((ps = compile_hlsl(BLIT_HLSL, "psa", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit_alpha_ps);
        ID3D10Blob_Release(ps);
    }
    if ((ps = compile_hlsl(BLIT_HLSL, "ps2d", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit2d_ps);
        ID3D10Blob_Release(ps);
    }
    if ((ps = compile_hlsl(DECODE_HLSL, "main", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &decode_ps);
        ID3D10Blob_Release(ps);
    }
    D3D11_SAMPLER_DESC smp = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(dev, &smp, &linear_sampler);
    if ((ps = compile_hlsl(BLIT_HLSL, "psdown", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &down_ps);
        ID3D10Blob_Release(ps);
    }
    D3D11_BUFFER_DESC dcb = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Device_CreateBuffer(dev, &dcb, NULL, &down_cb);
    /* The render scale (display.ini render_scale; auto: the picture's height in the window as it
     * is now, over the game's 1080), fixed from here on: render targets are made at it. */
    float scale = display_render_scale();
    DXGI_SWAP_CHAIN_DESC1 scd;
    if (scale == 0) {
        scale = 1;
        if (SUCCEEDED(IDXGISwapChain1_GetDesc1(swapchain, &scd))) {
            unsigned fx, fy, fw, fh;
            display_fit(scd.Width, scd.Height, &fx, &fy, &fw, &fh);
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
    if (render_scale != 1.0f)
        fprintf(stderr, "[gfx] render scale %.3g%s: the game's 1920x1080 drawn at %ux%u\n", render_scale,
                display_render_scale() == 0 ? " (auto)" : "", pw, ph);
    start_compilers();
    if (warm_mode) warm_start();
    /* The prewarm is for a driver that compiles a shader when it is created (NVIDIA's Direct3D:
     * 190 ms at first use). DXVK queues a Vulkan pipeline library per shader on its own workers
     * instead: the whole cache at once kept them busy for minutes and the first frame waited
     * behind them (5,948 shaders: 600 CPU-seconds, no frame for over 30 s). So under DXVK it is
     * off unless SSBU_SHADER_PREWARM=1. */
    const char *prewarm = getenv("SSBU_SHADER_PREWARM");
    if (prewarm ? atoi(prewarm) != 0 : !on_dxvk()) CloseHandle(CreateThread(NULL, 0, prewarm_main, NULL, 0, NULL));
    else if (!prewarm) fprintf(stderr, "[gfx] prewarm: off under DXVK (SSBU_SHADER_PREWARM=1 turns it on)\n");
#ifdef _WIN32
    game_mutex = CreateMutexA(NULL, FALSE, WARM_GAME);
#endif
    ready = 1;
    fprintf(stderr, "[gfx] Direct3D 11 ready%s\n", compile_dev ? " (shaders compile on a device of their own first)" : "");
    return TRUE;
}

/* Time spent in the renderer (the outermost enter to its leave), for the overlay - from
 * rt_clock_ns, as this is twice a draw (clock.h: the system timer can be slow). */
static u32 enter_depth;
static u64 enter_t0;
u64 gfx_stat_busy_ns, gfx_stat_upload_bytes, gfx_stat_gpu_waits, gfx_stat_composed, gfx_stat_upload_kind[4];
static struct { u32 size, bank, extent; char shader[24]; } gfx_stat_biggest_cb;

static volatile DWORD last_enter_thread;
DWORD gfx_lock_owner(void) { return ready ? (DWORD)(uintptr_t)lock.OwningThread : 0; }
DWORD gfx_last_enter_thread(void) { return last_enter_thread; }

static int enter(void) {
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!ready) return 0;
    EnterCriticalSection(&lock);
    last_enter_thread = GetCurrentThreadId();
    if (!enter_depth++) enter_t0 = rt_clock_ns();
    return 1;
}

/* GPU addresses and guest memory taken away, whose cached data is not dropped yet ([lo, hi);
 * hi 0: none). Dropped as the outermost entry point leaves (below: forget_unmapped). */
typedef struct Span { u64 lo, hi; } Span;
static Span unmapped_va, unmapped_guest;
static void forget_unmapped(void);

static void leave(void) {
    if (enter_depth == 1 && (unmapped_va.hi || unmapped_guest.hi)) forget_unmapped();
    if (!--enter_depth) gfx_stat_busy_ns += rt_clock_ns() - enter_t0;
    LeaveCriticalSection(&lock);
}

static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* ---- the 3D engine's work -------------------------------------------------------- */
#define RT(i, field) regs[0x200 + (i) * 0x10 + (field)]

static void clear_channels(Surface *s, const float color[4], u32 mask);   /* (below: it draws) */
static void forget_draw_state(void);
static int draw_logging;   /* (below) */
static void note_sampled(Surface *s);   /* (F4, below) */
static void save_sampled(void);

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
            if (((arg >> 2) & 0xF) == 0xF) ID3D11DeviceContext_ClearRenderTargetView(ctx, s->rtv, color);
            else clear_channels(s, color, (arg >> 2) & 0xF);   /* only some channels: a masked draw */
        }
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) ID3D11DeviceContext_ClearDepthStencilView(ctx, s->dsv, (arg & 1 ? D3D11_CLEAR_DEPTH : 0) | (arg & 2 ? D3D11_CLEAR_STENCIL : 0),
                                                         f32_of(regs[0x364]), (UINT8)regs[0x368]);
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
    static int shown;
    if (shown++ < 0)
        for (u32 j = 0; j < 6; j++)
            fprintf(stderr, "[gfx] pipeline %u: %08x %08x %08x %08x %08x (region %#llx)\n", j, regs[M_PIPELINE(j)],
                    regs[M_PIPELINE(j) + 1], regs[M_PIPELINE(j) + 2], regs[M_PIPELINE(j) + 3], regs[M_PIPELINE(j) + 4], region);
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

/* ---- shaders ------------------------------------------------------------------------ */
/* Translated at their first draw (on the submitting thread, well under a millisecond), then
 * made ready by compiler threads of our own: from the DXBC an earlier run kept on disk, else
 * from fxc's (seconds on large programs, minutes on some dispatch loops) - cache loads first.
 * Left to itself NVIDIA's driver compiles to GPU code after Create*Shader returns, on threads of
 * its own (see driver_stacks), and the first draw with the shader waits for that (the hitch when
 * a move is first used). So a second device, with the driver's
 * threading off, compiles each one first - inside Create*Shader, on our thread, with our stack;
 * the rendering device keeps its threading (without it the renderer took twice as long a
 * frame). AMD's driver compiles at a program's first draw instead (see "first draws"). A draw whose
 * shaders are not ready is skipped. The disk cache's names, and fxc's flags, are
 * gpu_shader_cache.h's (shared with tools/shaderaot.c). */

typedef struct Shader {
    u64 hash, table_hash;
    u32 vs_outputs;
    int stage;
    volatile LONG state;   /* 0 compiling, 1 ready, 2 failed */
    void *obj;             /* ID3D11VertexShader, ID3D11PixelShader or ID3D11ComputeShader */
    char *hlsl;            /* while compiling */
    char name[64];
    ShaderInfo info;
    u32 written_mask, learn_runs;   /* compute: storage buffers it was seen to write, over its first runs */
    struct Shader *next_job;
    int cached;            /* an earlier run compiled it (its job is a load) */
    struct Prewarm *prewarm;   /* a prewarm job (not a shader of the game yet): the entry it fills */
    int drawn;             /* a draw (or launch) has used it: its first one was timed (first_draw_done) */
} Shader;

static Shader **shader_cache;
static u32 shader_count, shader_cap;
u64 gfx_stat_shaders, gfx_stat_shader_failures;
static volatile LONG compiles_pending;

/* The compiler threads: a queue of cache loads, one of compiles. Big stacks (the driver's
 * compiler recurses over a program's blocks), below the game's threads in priority. */
#define COMPILERS_MAX 16
#define COMPILER_STACK (64ull << 20)
enum { STEP_IDLE, STEP_CACHE, STEP_FXC, STEP_DRIVER };
static SRWLOCK jobs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE jobs_cv = CONDITION_VARIABLE_INIT;
/* [0] loads from the disk cache, [1] compiles, [2] prewarm (the disk cache at start; never
 * taken by the first thread, which stays free for the game's own shaders) */
static struct { Shader *head, *tail; } job_queue[3];
static struct Compiler {
    volatile DWORD thread;
    Shader *volatile job;
    volatile LONG step;
    volatile u64 since_ms;
    const void *volatile dxbc;   /* what the driver is compiling (STEP_DRIVER) */
    volatile size_t dxbc_size;
} compilers[COMPILERS_MAX];
static u32 ncompilers;

static const char *cache_dir(void) {
    static char dir[1024];
    if (!dir[0]) {   /* SSBU_SHADER_CACHE: another directory (an empty one: a first run's compiles) */
        if (getenv("SSBU_SHADER_CACHE")) snprintf(dir, sizeof dir, "%s", getenv("SSBU_SHADER_CACHE"));
        else snprintf(dir, sizeof dir, "%s/../shadercache", rt_data_dir);
        CreateDirectoryA(dir, NULL);
    }
    return dir;
}

static void cache_path(const Shader *s, char *path, size_t n) {
    shader_cache_file(path, n, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash);
}

/* A file the ahead-of-time compiler wrote (dated 2001), now used by the game: dated now, so the
 * next start's prewarm makes it. */
static void cache_touch(const char *path) {
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME written, now;
    if (GetFileTime(h, NULL, NULL, &written) && ((u64)written.dwHighDateTime << 32 | written.dwLowDateTime) < SHADER_CACHE_PLAYED) {
        GetSystemTimeAsFileTime(&now);
        SetFileTime(h, NULL, NULL, &now);
    }
    CloseHandle(h);
}

static HRESULT create_on(ID3D11Device *d, int stage, const void *code, size_t size, void **obj) {
    return stage == SHADER_VERTEX ? ID3D11Device_CreateVertexShader(d, code, size, NULL, (ID3D11VertexShader **)obj)
         : stage == SHADER_PIXEL  ? ID3D11Device_CreatePixelShader(d, code, size, NULL, (ID3D11PixelShader **)obj)
                                  : ID3D11Device_CreateComputeShader(d, code, size, NULL, (ID3D11ComputeShader **)obj);
}

/* (on a compiler thread) the driver's compile of DXBC, noted for crash reports: on the compile
 * device, which compiles right here (its object is not needed: the rendering device finds the
 * GPU code in the driver's cache - measured 3.5 ms of its time a shader instead of 190 ms),
 * then the object the draws use */
static int create_noted(struct Compiler *me, Shader *s, const void *code, size_t size) {
    me->dxbc = code;
    me->dxbc_size = size;
    me->step = STEP_DRIVER;
    void *warm = NULL;
    if (compile_dev && SUCCEEDED(create_on(compile_dev, s->stage, code, size, &warm))) IUnknown_Release((IUnknown *)warm);
    int ok = SUCCEEDED(create_on(dev, s->stage, code, size, &s->obj));
    me->dxbc = NULL;
    return ok;
}

/* ---- prewarm: the disk cache made ready while the game starts --------------------------
 * Every DXBC file of this SHADER_CACHE_VERSION the game has used before (gpu_shader_cache.h),
 * newest first, is created on both devices by the compiler threads (queue 2, below the game's
 * own shaders) into a table by the cache's key.
 * When the game first uses a program, get_shader_ex still translates it (that gives its
 * interface), then takes the object from here instead of loading it, so the draw is not
 * skipped. SSBU_SHADER_PREWARM=0: no prewarm; under DXVK it is off unless =1 (see init).
 * outputs: the generic attributes between the stages (a vertex program's, from its DXBC; a pixel
 * program's, vs_outputs), for its first draw (warm_prewarmed). */
typedef struct Prewarm { u64 hash, table_hash; u32 vs_outputs, outputs; int stage; LONG state; void *obj; } Prewarm;
enum { PW_EMPTY, PW_QUEUED, PW_LOADING, PW_READY, PW_TAKEN, PW_FAILED };
#define PREWARM_SLOTS 32768
static Prewarm *volatile prewarm_table;
static SRWLOCK prewarm_lock = SRWLOCK_INIT;
static volatile LONG prewarm_total, prewarm_done, prewarm_ready;
static u64 prewarm_t0, prewarm_bytes0;

static u64 private_bytes(void) {
#ifdef _WIN32
    typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, a, b, c, d, pagefile, peak_pagefile, private_usage; } Counters;
    static BOOL (WINAPI *get)(HANDLE, Counters *, DWORD);
    if (!get) get = (BOOL (WINAPI *)(HANDLE, Counters *, DWORD))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters c = {sizeof c};
    return get && get(GetCurrentProcess(), &c, sizeof c) ? c.private_usage : 0;
#else   /* (Linux: the process's anonymous memory in RAM) */
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    u64 kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "RssAnon:", 8)) { kb = strtoull(line + 8, NULL, 10); break; }
    if (f) fclose(f);
    return kb << 10;
#endif
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

/* The game asks for a program: its object, if the prewarm made it; else NULL, and the prewarm
 * leaves it alone from now on (the game's own load takes over). */
static void *prewarm_take(const Shader *s) {
    if (!prewarm_table) return NULL;
    void *obj = NULL;
    AcquireSRWLockExclusive(&prewarm_lock);
    Prewarm *p = prewarm_slot(s->stage, s->hash, s->vs_outputs, s->table_hash, 0);
    if (p && p->state == PW_READY) { obj = p->obj; p->obj = NULL; }
    if (p && p->state != PW_FAILED) p->state = PW_TAKEN;
    ReleaseSRWLockExclusive(&prewarm_lock);
    return obj;
}

static u32 dxbc_outputs(const u8 *code, size_t size);   /* (see "first draws") */

/* (on a compiler thread) one prewarm job: the file, then both devices' objects */
static void prewarm_job(struct Compiler *me, Shader *s) {
    Prewarm *p = s->prewarm;
    AcquireSRWLockExclusive(&prewarm_lock);
    int go = p->state == PW_QUEUED;
    if (go) p->state = PW_LOADING;
    ReleaseSRWLockExclusive(&prewarm_lock);
    void *obj = NULL;
    u32 outputs = s->stage == SHADER_PIXEL ? s->vs_outputs : ~0u;
    if (go) {
        char path[1200];
        cache_path(s, path, sizeof path);
        me->step = STEP_CACHE;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *code = n > 0 ? malloc(n) : NULL;
            if (code && fread(code, 1, n, f) == (size_t)n && create_noted(me, s, code, n)) obj = s->obj;
            if (obj && s->stage == SHADER_VERTEX) outputs = dxbc_outputs(code, n);
            fclose(f);
            free(code);
        }
    }
    AcquireSRWLockExclusive(&prewarm_lock);
    if (go && p->state == PW_LOADING) {
        p->outputs = outputs;
        p->obj = obj;
        p->state = obj ? PW_READY : PW_FAILED;
        if (obj) prewarm_ready++;
        obj = NULL;
    }
    ReleaseSRWLockExclusive(&prewarm_lock);
    if (obj) IUnknown_Release((IUnknown *)obj);   /* the game loaded its own meanwhile */
    if (InterlockedIncrement(&prewarm_done) == prewarm_total)
        fprintf(stderr, "[gfx] prewarm: %ld of %ld shaders ready in %.1f s; private memory %.0f -> %.0f MB\n", (long)prewarm_ready, (long)prewarm_total,
                (GetTickCount64() - prewarm_t0) / 1000.0, prewarm_bytes0 / 1048576.0, private_bytes() / 1048576.0);
}

/* (on a compiler thread) the shader's GPU code: 1 ready, 2 failed */
static LONG make_ready(struct Compiler *me, Shader *s) {
    char path[1200];
    cache_path(s, path, sizeof path);
    me->step = STEP_CACHE;
    FILE *f = s->cached ? fopen(path, "rb") : NULL;
    if (f) {   /* compiled in an earlier run */
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *code = n > 0 ? malloc(n) : NULL;
        int ok = code && fread(code, 1, n, f) == (size_t)n && create_noted(me, s, code, n);
        fclose(f);
        free(code);
        if (ok) { cache_touch(path); return 1; }
    }
    me->step = STEP_FXC;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = shader_cache_compile(s->hlsl, s->name, s->stage, &code, &errors);
    LONG state = 2;
    if (SUCCEEDED(hr) && create_noted(me, s, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code))) {
        f = fopen(path, "wb");
        if (f) { fwrite(ID3D10Blob_GetBufferPointer(code), 1, ID3D10Blob_GetBufferSize(code), f); fclose(f); }
        state = 1;
    } else {
        fprintf(stderr, "[gfx] shader %s failed to compile:\n%.2000s\n", s->name,
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : SUCCEEDED(hr) ? "(the driver refused its bytecode)" : "?");
    }
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return state;
}

static DWORD WINAPI compiler_main(void *arg) {
    struct Compiler *me = arg;
    me->thread = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"shader compiler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ULONG guarantee = 64 << 10;   /* room for the crash report, should a compile overflow even this stack */
    SetThreadStackGuarantee(&guarantee);
    int prewarms = me != &compilers[0];
    for (;;) {
        AcquireSRWLockExclusive(&jobs_lock);
        while (!job_queue[0].head && !job_queue[1].head && !(prewarms && job_queue[2].head))
            SleepConditionVariableSRW(&jobs_cv, &jobs_lock, INFINITE, 0);
        int q = job_queue[0].head ? 0 : job_queue[1].head ? 1 : 2;
        Shader *s = job_queue[q].head;
        if (!(job_queue[q].head = s->next_job)) job_queue[q].tail = NULL;
        ReleaseSRWLockExclusive(&jobs_lock);
        me->since_ms = GetTickCount64();
        me->job = s;
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
        free(s->hlsl);
        s->hlsl = NULL;
        if (state == 1) InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        else InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shader_failures);
        InterlockedExchange(&s->state, state);
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

typedef struct { char name[96]; u64 written; } Found;
static int newest_first(const void *a, const void *b) {
    u64 x = ((const Found *)a)->written, y = ((const Found *)b)->written;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* The disk cache's files of this SHADER_CACHE_VERSION that the game has used (not the ahead-of-time
 * compiler's it never asked for; with all, those too), newest first: the shaders of what was
 * played last, first, and the ahead-of-time compiler's (dated 2001) after every played one. */
static u32 cache_files(const char *dir, Found **out, int all) {
    Found *found = NULL;
    u32 n = 0, cap = 0;
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s/v%u_*.dxbc", dir, SHADER_CACHE_VERSION);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) {
        if (!all && ((u64)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime) < SHADER_CACHE_PLAYED) continue;
        if (n == cap) found = realloc(found, sizeof *found * (cap = cap * 2 + 1024));
        snprintf(found[n].name, sizeof found[n].name, "%s", fd.cFileName);
        found[n++].written = (u64)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    if (n) qsort(found, n, sizeof *found, newest_first);
    *out = found;
    return n;
}

/* A cache file's name: its stage (-1: not one), program hash, pixel program's vs_outputs, tables. */
static int cache_name(const char *name, u64 *hash, u32 *vs_outputs, u64 *tables) {
    unsigned version, vo;
    char stage_name[4];
    unsigned long long h, t;
    if (sscanf(name, "v%u_%2[a-z]_%16llx_%8x_%16llx.dxbc", &version, stage_name, &h, &vo, &t) != 5) return -1;
    *hash = h; *vs_outputs = vo; *tables = t;
    return !strcmp(stage_name, "vs") ? SHADER_VERTEX : !strcmp(stage_name, "ps") ? SHADER_PIXEL : !strcmp(stage_name, "cs") ? SHADER_COMPUTE : -1;
}

static void warm_prewarmed(Prewarm **order, u32 n);   /* (see "first draws") */

/* (a thread of its own, at start) every file of the disk cache into the prewarm queue */
static DWORD WINAPI prewarm_main(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"shader prewarm");
    Found *found;
    u32 n = cache_files(cache_dir(), &found, 0);
    prewarm_t0 = GetTickCount64();
    prewarm_bytes0 = private_bytes();
    Prewarm *table = calloc(PREWARM_SLOTS, sizeof *table);
    prewarm_table = table;
    Prewarm **order = n ? malloc(sizeof *order * n) : NULL;   /* (the entries in queue order) */
    Shader *head = NULL, *tail = NULL;
    u32 queued = 0;
    for (u32 i = 0; i < n && table && order; i++) {
        u64 hash, tables;
        u32 vs_outputs;
        int stage = cache_name(found[i].name, &hash, &vs_outputs, &tables);
        if (stage < 0 || queued >= PREWARM_SLOTS / 2) continue;
        Shader *s = calloc(1, sizeof *s);
        s->hash = hash; s->stage = stage; s->vs_outputs = vs_outputs; s->table_hash = tables;
        snprintf(s->name, sizeof s->name, "%s_%016llx", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)hash);
        AcquireSRWLockExclusive(&prewarm_lock);
        s->prewarm = prewarm_slot(stage, hash, vs_outputs, tables, 1);
        if (s->prewarm && s->prewarm->state == PW_EMPTY) s->prewarm->state = PW_QUEUED;
        else { free(s); s = NULL; }
        ReleaseSRWLockExclusive(&prewarm_lock);
        if (!s) continue;
        if (tail) tail->next_job = s; else head = s;
        tail = s;
        order[queued++] = s->prewarm;
    }
    free(found);
    if (!queued) { free(order); return 0; }
    prewarm_total = (LONG)queued;
    AcquireSRWLockExclusive(&jobs_lock);
    if (job_queue[2].tail) job_queue[2].tail->next_job = head; else job_queue[2].head = head;
    job_queue[2].tail = tail;
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeAllConditionVariable(&jobs_cv);
    fprintf(stderr, "[gfx] prewarm: %u shaders from the disk cache, on %u threads\n", queued, ncompilers - 1);
    if (warm_mode >= 2) warm_prewarmed(order, queued);
    free(order);
    return 0;
}

/* ---- first draws: for a driver that makes a program's GPU code on its own threads ----------
 * AMD's Direct3D 11 driver makes a program's GPU code neither when the program is created nor in
 * the draw that first uses it, but on threads of its own, and the GPU waits for it: from the
 * driver's disk cache when that has the program (kept under the exe's name: about a millisecond,
 * also in a later process), else a whole compile (measured elsewhere: 36 ms for a large one;
 * another render target format, blend or depth state does not compile it again). A player's log
 * (RX 7900 GRE): the first draws of 226 programs each took the renderer under 0.05 ms; 2,696
 * start-up draws, one per program the prewarm made (warm_prewarmed, below), took 1.2 ms each until
 * the GPU had run them (so the driver's cache had all but 3 of them), and a minute later, in the
 * first match, the driver's threads still kept 4.7 cores busy (an earlier build without those
 * draws, at the same point: 0.6). So those draws saved nothing, and most likely left the driver
 * work that ran into the match. What its cache lacks is the programs the game has not drawn yet:
 * Prepare Shaders' files. So:
 *  - when the game closes (gfx_closing), this exe starts again as ssbu --warm-shaders, which draws
 *    every program of the cache once, on devices of its own - the game's played ones first, then
 *    the ones Prepare Shaders made - so that the driver's disk cache has them before a move first
 *    needs one. It notes what it drew (warm.done; all again for another driver or exe), stops as
 *    soon as a game starts and goes on after it, and writes its result to warm.log beside the
 *    cache, which the next start shows in its log;
 *  - SSBU_SHADER_WARM=2 also draws each program the prewarm made once on the rendering device
 *    (warm_prewarmed): into a 1x1 target, with a partner shader made to match its interface,
 *    taking at most a quarter of the time, newest first; after 3 slow ones in a row (over 20 ms:
 *    a compile), or 10 in all, it stops.
 * On (=1) with AMD's driver, not under DXVK; SSBU_SHADER_WARM=0 turns it off, =1 or =2 on with
 * any. The partners declare the stages' interface as the translator does (gpu_shader.c: struct
 * VSOut). */
#define WARM_SLOW_MS 20.0   /* a first draw this long compiled the program */

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

/* A vertex program's generic attributes out (ATTRn of its output signature), from its DXBC;
 * ~0u if it can't be read so. */
static u32 dxbc_outputs(const u8 *code, size_t size) {
    if (!code || size < 32 || memcmp(code, "DXBC", 4)) return ~0u;
    u32 chunks = rd32(code + 28);
    for (u32 c = 0; c < chunks && 32 + 4 * (size_t)c + 4 <= size; c++) {
        u32 at = rd32(code + 32 + 4 * c);
        if ((size_t)at + 8 > size) return ~0u;
        const u8 *chunk = code + at;
        /* (OSG5 and OSG1 elements start with their stream) */
        u32 stride = !memcmp(chunk, "OSGN", 4) ? 24 : !memcmp(chunk, "OSG5", 4) ? 28 : !memcmp(chunk, "OSG1", 4) ? 32 : 0;
        if (!stride) continue;
        u32 len = rd32(chunk + 4), first = stride == 24 ? 0 : 4;
        const u8 *d = chunk + 8;
        if (len < 8 || (size_t)at + 8 + len > size) return ~0u;
        u32 count = rd32(d), mask = 0;
        for (u32 e = 0; e < count; e++) {
            u64 el = 8 + (u64)e * stride + first;
            if (el + 8 > len) return ~0u;
            u32 name = rd32(d + el), index = rd32(d + el + 4);
            if ((u64)name + 5 > len) return ~0u;
            if (!memcmp(d + name, "ATTR", 5) && index < 32) mask |= 1u << index;
        }
        return mask;
    }
    return ~0u;
}

typedef struct Warmer {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11RenderTargetView *rtv;   /* 1x1 */
    ID3D11Query *done;
    SRWLOCK lock;                  /* (the partners) */
    u32 npartners;
    struct { u32 outputs; int stage; void *obj; } partner[256];
} Warmer;

static int warmer_init(Warmer *w, ID3D11Device *d, ID3D11DeviceContext *c) {
    memset(w, 0, sizeof *w);
    InitializeSRWLock(&w->lock);
    w->dev = d;
    w->ctx = c;
    D3D11_TEXTURE2D_DESC td = {1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Texture2D *t = NULL;
    int ok = SUCCEEDED(ID3D11Device_CreateTexture2D(d, &td, NULL, &t)) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource *)t, NULL, &w->rtv)) &&
             SUCCEEDED(ID3D11Device_CreateQuery(d, &qd, &w->done));
    if (t) ID3D11Texture2D_Release(t);
    return ok;
}

static void warmer_free(Warmer *w) {
    for (u32 i = 0; i < w->npartners; i++) IUnknown_Release((IUnknown *)w->partner[i].obj);
    if (w->rtv) ID3D11RenderTargetView_Release(w->rtv);
    if (w->done) ID3D11Query_Release(w->done);
}

/* The other side of a program's interface, on w's device (stage: the partner's): for a pixel
 * program, a vertex shader writing its inputs (a triangle over the target); for a vertex program,
 * a pixel shader reading all its outputs. NULL if it can't be made. */
static void *warm_partner(Warmer *w, int stage, u32 outputs) {
    void *obj = NULL;
    AcquireSRWLockShared(&w->lock);
    for (u32 i = 0; i < w->npartners && !obj; i++)
        if (w->partner[i].stage == stage && w->partner[i].outputs == outputs) obj = w->partner[i].obj;
    ReleaseSRWLockShared(&w->lock);
    if (obj) return obj;
    char src[4096];
    int len = snprintf(src, sizeof src, "struct VSOut {\n  float4 pos : SV_Position;\n  float clipw : CLIPW;\n");
    for (u32 n = 0; n < 32; n++)
        if (outputs >> n & 1) len += snprintf(src + len, sizeof src - len, "  float4 a%u : ATTR%u;\n", n, n);
    if (stage == SHADER_VERTEX)
        snprintf(src + len, sizeof src - len, "};\nVSOut main(uint vid : SV_VertexID) {\n  VSOut o = (VSOut)0;\n"
                                              "  o.pos = float4(vid == 1u ? 3.0f : -1.0f, vid == 2u ? -3.0f : 1.0f, 0.5f, 1.0f);\n  return o;\n}\n");
    else {
        len += snprintf(src + len, sizeof src - len, "};\nfloat4 main(VSOut i) : SV_Target {\n  float4 s = i.pos + i.clipw;\n");
        for (u32 n = 0; n < 32; n++)
            if (outputs >> n & 1) len += snprintf(src + len, sizeof src - len, "  s += i.a%u;\n", n);
        snprintf(src + len, sizeof src - len, "  return s;\n}\n");
    }
    ID3DBlob *code = NULL, *errors = NULL;
    if (SUCCEEDED(D3DCompile(src, strlen(src), "warm", NULL, NULL, "main", stage == SHADER_VERTEX ? "vs_5_0" : "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors)))
        create_on(w->dev, stage, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), &obj);
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    if (!obj) return NULL;
    void *have = NULL;
    AcquireSRWLockExclusive(&w->lock);
    for (u32 i = 0; i < w->npartners && !have; i++)   /* (another thread made it meanwhile) */
        if (w->partner[i].stage == stage && w->partner[i].outputs == outputs) have = w->partner[i].obj;
    if (!have && w->npartners < sizeof w->partner / sizeof *w->partner) {
        w->partner[w->npartners].outputs = outputs;
        w->partner[w->npartners].stage = stage;
        w->partner[w->npartners++].obj = have = obj;
        obj = NULL;
    }
    ReleaseSRWLockExclusive(&w->lock);
    if (obj) IUnknown_Release((IUnknown *)obj);
    return have;
}

/* A draw (or launch) with the program on w's context: its target, the partner, the rest of the
 * state Direct3D's defaults. The partner must be made already (warm_partner). */
static int warm_issue(Warmer *w, int stage, void *obj, u32 outputs) {
    ID3D11DeviceContext *c = w->ctx;
    if (stage == SHADER_COMPUTE) {
        ID3D11DeviceContext_CSSetShader(c, (ID3D11ComputeShader *)obj, NULL, 0);
        ID3D11DeviceContext_Dispatch(c, 1, 1, 1);
        ID3D11DeviceContext_CSSetShader(c, NULL, NULL, 0);
        return 1;
    }
    void *partner = warm_partner(w, stage == SHADER_VERTEX ? SHADER_PIXEL : SHADER_VERTEX, outputs);
    if (!partner) return 0;
    D3D11_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &w->rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_RSSetState(c, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, (ID3D11VertexShader *)(stage == SHADER_VERTEX ? obj : partner), NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, (ID3D11PixelShader *)(stage == SHADER_PIXEL ? obj : partner), NULL, 0);
    ID3D11DeviceContext_Draw(c, 3, 0);
    return 1;
}

static Warmer render_warmer;   /* the rendering device's: its context is the renderer's (under its lock) */
static int render_warmer_ok;

/* (init) the rendering device's warmer (SSBU_SHADER_WARM=2), and what the last run of
 * ssbu --warm-shaders did */
static void warm_start(void) {
    render_warmer_ok = warm_mode >= 2 && warmer_init(&render_warmer, dev, ctx);
    fprintf(stderr, "[gfx] this driver makes a shader's GPU code on its own threads: when the game closes, the shader cache is drawn "
                    "once for the driver's own cache, in the background%s (SSBU_SHADER_WARM=0 turns this off)\n",
            warm_mode >= 2 ? "; the prewarm draws each of its shaders once now too" : "");
    char path[1100], line[512];
    snprintf(path, sizeof path, "%s/warm.log", cache_dir());
    FILE *f = fopen(path, "r");
    if (f && fgets(line, sizeof line, f)) fprintf(stderr, "[gfx] after the last game: %s%s", line, strchr(line, '\n') ? "" : "\n");
    if (f) fclose(f);
}

/* One first draw on the rendering device, between the game's: ms from the draw until the GPU had
 * run it (-1: not drawn). */
static double render_warm(int stage, void *obj, u32 outputs) {
    if (stage == SHADER_COMPUTE || !warm_partner(&render_warmer, stage == SHADER_VERTEX ? SHADER_PIXEL : SHADER_VERTEX, outputs)) return -1;
    if (!enter()) return -1;
    u64 t0 = rt_clock_ns();
    int drew = warm_issue(&render_warmer, stage, obj, outputs);
    if (drew) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)render_warmer.done);
        ID3D11DeviceContext_Flush(ctx);
    }
    forget_draw_state();   /* (the game's next draw sets all of its own again) */
    leave();
    if (!drew) return -1;
    for (;;) {   /* (the lock let go between looks: the game's draws go on meanwhile) */
        if (!enter()) return -1;
        HRESULT hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)render_warmer.done, NULL, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        leave();
        double ms = (double)(rt_clock_ns() - t0) / 1e6;
        if (hr != S_FALSE || ms > 250) return ms;
        if (ms < 20) SwitchToThread(); else Sleep(1);
    }
}

/* (the prewarm's thread) each prewarmed program drawn once on the rendering device, in queue
 * order as its job finishes, unless the game took it first */
static void warm_prewarmed(Prewarm **order, u32 n) {
    if (!render_warmer_ok) return;
    u64 t0 = GetTickCount64();
    u32 drawn = 0, slow = 0, slow_run = 0;
    double total = 0, longest = 0;
    for (u32 i = 0; i < n && slow_run < 3 && slow < 10; i++) {
        Prewarm *p = order[i];
        void *obj = NULL;
        u32 outputs = ~0u;
        for (;;) {   /* (until its job is done: made, failed, or taken by the game first) */
            AcquireSRWLockExclusive(&prewarm_lock);
            LONG state = p->state;
            if (state == PW_READY && p->obj) { obj = p->obj; IUnknown_AddRef((IUnknown *)obj); outputs = p->outputs; }
            ReleaseSRWLockExclusive(&prewarm_lock);
            if (state != PW_QUEUED && state != PW_LOADING) break;
            Sleep(2);
        }
        if (!obj) continue;
        double ms = outputs == ~0u ? -1 : render_warm(p->stage, obj, outputs);
        IUnknown_Release((IUnknown *)obj);
        if (ms < 0) continue;
        drawn++;
        total += ms;
        if (ms > longest) longest = ms;
        if (ms > WARM_SLOW_MS) { slow++; slow_run++; } else slow_run = 0;
        Sleep((DWORD)(ms * 3));   /* (a quarter of the time at most) */
    }
    fprintf(stderr, "[gfx] prewarm: %u shaders drawn once on the rendering device in %.1f s (%.1f ms each on average, longest %.0f ms; %u over %.0f ms)%s\n",
            drawn, (GetTickCount64() - t0) / 1000.0, drawn ? total / drawn : 0.0, longest, slow, WARM_SLOW_MS,
            slow_run >= 3 || slow >= 10 ? ": stopped - the driver had not compiled them yet; the game's draws compile the rest, "
                                          "and they are drawn for its cache when the game closes" : "");
}

#ifdef _WIN32
/* ---- ssbu --warm-shaders <cache dir> <adapter LUID, hex>: started as the game closes. Each
 * thread draws programs once on a device of its own (compiling on the thread itself), then lets
 * the device go, so the driver keeps the GPU code in its disk cache. It runs in Windows'
 * background mode at idle priority (processor, disk and memory below what the user does), and
 * each game holds a mutex while it runs (WARM_GAME): once one does, the threads stop after the
 * program they are drawing. What has been drawn is in warm.done (WarmDone, then the FNV-1a hashes
 * of the files' names, sorted): a later run draws only the others - all again once the driver
 * or this exe has changed, as the driver's cache may not hold their programs then. */
typedef struct WarmDone { char magic[8]; u64 driver, exe; u32 count, unused; } WarmDone;
static const char WARM_DONE_MAGIC[8] = {'s', 's', 'b', 'u', 'w', 'r', 'm', '1'};

typedef struct WarmRun {
    const char *dir;
    Found *files;
    u32 n;
    u8 *done;   /* per file: drawn (or refused) by this run or an earlier one */
    IDXGIAdapter1 *adapter;
    volatile LONG next, drawn, failed, slow, stop;
} WarmRun;

static u64 name_hash(const char *s) {
    u64 h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (u8)*s) * 1099511628211ull;
    return h;
}

static int by_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

static int game_running(void) {
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, WARM_GAME);
    if (h) CloseHandle(h);
    return h != NULL;
}

static DWORD WINAPI warm_worker(void *arg) {
    WarmRun *r = arg;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device *d = NULL;
    ID3D11DeviceContext *c = NULL;
    /* (SSBU_D3D_DEBUG: Direct3D's debug layer, a draw it refused counted as not drawn, its message on stderr) */
    UINT debug = getenv("SSBU_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)r->adapter, r->adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | debug, levels, 2, D3D11_SDK_VERSION, &d, NULL, &c)))
        return 1;
    ID3D11InfoQueue *iq = NULL;
    if (debug && FAILED(ID3D11Device_QueryInterface(d, &IID_ID3D11InfoQueue, (void **)&iq))) iq = NULL;
    Warmer *w = calloc(1, sizeof *w);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (w && warmer_init(w, d, c))
        for (LONG i; !r->stop && (i = InterlockedIncrement(&r->next) - 1) < (LONG)r->n;) {
            if (r->done[i]) continue;   /* (an earlier run drew it) */
            r->done[i] = 1;
            u64 hash, tables;
            u32 vs_outputs;
            int stage = cache_name(r->files[i].name, &hash, &vs_outputs, &tables);
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", r->dir, r->files[i].name);
            FILE *f = stage >= 0 ? fopen(path, "rb") : NULL;
            long size = 0;
            void *code = NULL;
            if (f) {
                fseek(f, 0, SEEK_END);
                size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0 && (code = malloc(size)) && fread(code, 1, size, f) != (size_t)size) { free(code); code = NULL; }
                fclose(f);
            }
            void *obj = NULL;
            u32 outputs = stage == SHADER_PIXEL ? vs_outputs : stage == SHADER_VERTEX ? dxbc_outputs(code, size) : 0;
            if (!code || outputs == ~0u || FAILED(create_on(d, stage, code, size, &obj))) {
                free(code);
                InterlockedIncrement(&r->failed);
                continue;
            }
            free(code);
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            int ok = warm_issue(w, stage, obj, outputs);
            if (ok) {
                ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)w->done);
                ID3D11DeviceContext_Flush(c);
                for (u64 until = GetTickCount64() + 10000; ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)w->done, NULL, 0, 0) == S_FALSE &&
                                                           GetTickCount64() < until;)
                    SwitchToThread();
            }
            QueryPerformanceCounter(&b);
            IUnknown_Release((IUnknown *)obj);
            for (UINT64 m = 0, nm = iq ? ID3D11InfoQueue_GetNumStoredMessages(iq) : 0; m < nm; m++) {
                SIZE_T len = 0;
                D3D11_MESSAGE *msg = SUCCEEDED(ID3D11InfoQueue_GetMessage(iq, m, NULL, &len)) && len ? malloc(len) : NULL;
                if (msg && SUCCEEDED(ID3D11InfoQueue_GetMessage(iq, m, msg, &len)) && msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    fprintf(stderr, "[warm] %s: %.*s\n", r->files[i].name, (int)msg->DescriptionByteLength, msg->pDescription);
                    ok = 0;
                }
                free(msg);
            }
            if (iq) ID3D11InfoQueue_ClearStoredMessages(iq);
            InterlockedIncrement(ok ? &r->drawn : &r->failed);
            if (ok && (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart > WARM_SLOW_MS) InterlockedIncrement(&r->slow);
        }
    if (w) {
        ID3D11DeviceContext_ClearState(c);
        warmer_free(w);
        free(w);
    }
    if (iq) ID3D11InfoQueue_Release(iq);
    ID3D11DeviceContext_Release(c);
    ID3D11Device_Release(d);
    return 0;
}

int gfx_warm_shaders_main(const char *dir, const char *luid) {
    HANDLE one = CreateMutexA(NULL, TRUE, "Local\\ssbu-warm-shaders");   /* (one at a time) */
    if (!one || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    /* disk and memory below what the user does; then the processor too (background mode alone puts
     * the class back to normal) */
    SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    driver_stacks();
    u64 want = strtoull(luid, NULL, 16);
    WarmRun r;
    memset(&r, 0, sizeof r);
    r.dir = dir;
    char name[128] = "the default graphics card";
    WarmDone head;
    memset(&head, 0, sizeof head);
    memcpy(head.magic, WARM_DONE_MAGIC, sizeof head.magic);
    IDXGIFactory1 *f1;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f1))) {
        IDXGIAdapter1 *a;
        for (UINT i = 0; !r.adapter && SUCCEEDED(IDXGIFactory1_EnumAdapters1(f1, i, &a)); i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && ((u64)(u32)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart) == want) {
                r.adapter = a;
                adapter_name(&d, name, sizeof name);
                LARGE_INTEGER umd;
                if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &umd))) head.driver = (u64)umd.QuadPart;
            } else IDXGIAdapter1_Release(a);
        }
        IDXGIFactory1_Release(f1);
    }
    wchar_t exe[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) && GetFileAttributesExW(exe, GetFileExInfoStandard, &fa))
        head.exe = ((u64)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) * 1099511628211ull ^
                   ((u64)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime);
    u64 t0 = GetTickCount64();
    r.n = cache_files(dir, &r.files, 1);
    r.done = calloc(r.n + 1, 1);
    /* what earlier runs drew, with this driver and exe */
    char path[1100], tmp[1110];
    snprintf(path, sizeof path, "%s/warm.done", dir);
    FILE *f = fopen(path, "rb");
    WarmDone had;
    u64 *names = NULL;
    u32 before = 0;
    if (f && fread(&had, sizeof had, 1, f) == 1 && !memcmp(had.magic, head.magic, sizeof had.magic) && had.driver == head.driver &&
        had.exe == head.exe && had.count <= (1u << 24) && (names = malloc(sizeof *names * (had.count + 1))) &&
        fread(names, sizeof *names, had.count, f) == had.count)
        for (u32 i = 0; i < r.n && r.done; i++) {
            u64 k = name_hash(r.files[i].name);
            if (bsearch(&k, names, had.count, sizeof *names, by_u64)) { r.done[i] = 1; before++; }
        }
    if (f) fclose(f);
    free(names);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char *env = getenv("SSBU_WARM_THREADS");
    u32 nthreads = env ? (u32)atoi(env) : si.dwNumberOfProcessors / 2;
    nthreads = nthreads < 1 ? 1 : nthreads > 6 ? 6 : nthreads;
    HANDLE threads[6];
    u32 started = 0;
    if (game_running()) r.stop = 1;
    for (u32 i = 0; i < nthreads && r.done && before + i < r.n && !r.stop; i++)
        if ((threads[started] = CreateThread(NULL, 16 << 20, warm_worker, &r, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL))) started++;
    while (started && WaitForMultipleObjects(started, threads, TRUE, 500) == WAIT_TIMEOUT)
        if (!r.stop && game_running()) InterlockedExchange(&r.stop, 1);
    for (u32 i = 0; i < started; i++) CloseHandle(threads[i]);
    /* what has been drawn, for the next run */
    u32 ndone = 0;
    for (u32 i = 0; i < r.n && r.done; i++) ndone += r.done[i];
    names = malloc(sizeof *names * (ndone + 1));
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (names && ndone > before && (f = fopen(tmp, "wb"))) {
        u32 k = 0;
        for (u32 i = 0; i < r.n; i++)
            if (r.done[i]) names[k++] = name_hash(r.files[i].name);
        qsort(names, k, sizeof *names, by_u64);
        head.count = k;
        int ok = fwrite(&head, sizeof head, 1, f) == 1 && fwrite(names, sizeof *names, k, f) == k;
        ok &= !fclose(f);
        if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
    }
    free(names);
    snprintf(path, sizeof path, "%s/warm.log", dir);
    FILE *out = fopen(path, "w");
    if (out) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        u32 left = r.n - ndone;
        char rest[160];
        if (left) snprintf(rest, sizeof rest, "%u left for after the next game%s", left, r.stop ? " (a game started)" : "");
        else snprintf(rest, sizeof rest, "all %u of the cache drawn", r.n);
        fprintf(out, "%04u-%02u-%02u %02u:%02u: %ld shaders drawn once on %s in %.1f s, %ld of them compiled there (over %.0f ms), %ld not drawn; "
                     "%u drawn before; %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, (long)r.drawn, name, (GetTickCount64() - t0) / 1000.0, (long)r.slow,
                WARM_SLOW_MS, (long)r.failed, before, rest);
        fclose(out);
    }
    free(r.done);
    free(r.files);
    if (r.adapter) IDXGIAdapter1_Release(r.adapter);
    return 0;
}
#endif

/* The game is closing (its window, or the settings menu's Quit): with a driver that makes GPU
 * code on its own threads, the shader cache is drawn once for that driver's own cache, by a
 * process of its own that goes on after this one ends (see "first draws"). */
void gfx_closing(void) {
#ifdef _WIN32
    static LONG once;
    if (!ready || !warm_mode || InterlockedExchange(&once, 1)) return;
    if (game_mutex) CloseHandle(game_mutex);   /* (this game is done) */
    game_mutex = NULL;
    wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH + 96];
    char dir[MAX_PATH];
    DWORD n = GetFullPathNameA(cache_dir(), MAX_PATH, dir, NULL);
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !n || n >= MAX_PATH) return;
    _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" --warm-shaders \"%hs\" %llx", exe, dir,
               (unsigned long long)((u64)(u32)warm_adapter.HighPart << 32 | warm_adapter.LowPart));
    cmd[sizeof cmd / sizeof *cmd - 1] = 0;
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    DWORD how = DETACHED_PROCESS | BELOW_NORMAL_PRIORITY_CLASS;   /* (out of a launcher's job if it allows that: not ended with it) */
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, how | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi) ||
        CreateProcessW(exe, cmd, NULL, NULL, FALSE, how, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fprintf(stderr, "[gfx] the shader cache is drawn once for the driver's own cache, in the background (warm.log in the cache says how far it got)\n");
    }
#endif
}

/* ---- first draws timed (every driver): how long the draw or launch that first used each shader
 * took the renderer - where a driver compiling then would show (a [status] line) */
u64 gfx_stat_first_draws, gfx_stat_first_slow;
static double first_slow_ms, first_longest_ms;
static char first_longest[64];

static void first_draw_done(u64 ns, const Shader *s) {
    double ms = (double)ns / 1e6;
    gfx_stat_first_draws++;
    if (ms > 4) { gfx_stat_first_slow++; first_slow_ms += ms; }
    if (ms > first_longest_ms) { first_longest_ms = ms; snprintf(first_longest, sizeof first_longest, "%s", s->name); }
}

/* The programs that ran for the first time lately, for a lost device's report (check_device):
 * GPU work that hangs is most likely a program's that had never run before. */
#define RECENT_FIRSTS 64
static struct { u64 ns; char name[32]; } recent_firsts[RECENT_FIRSTS];
static u32 recent_first_next;

static void first_used(const Shader *s) {
    u32 k = recent_first_next++ % RECENT_FIRSTS;
    recent_firsts[k].ns = rt_clock_ns();
    snprintf(recent_firsts[k].name, sizeof recent_firsts[k].name, "%s", s->name);
}

#ifdef _WIN32
/* The graphics driver's own threads (those started in a DLL from the system's DriverStore): the
 * processor time they took since the last [status] line - the driver's work besides the draws
 * the game asks for (AMD's makes shaders' GPU code there). */
typedef struct { DWORD tid; u64 cpu; } ThreadCpu;

static void driver_threads_line(void) {
    static LONG (NTAPI *query)(HANDLE, int, void *, ULONG, ULONG *);
    static ThreadCpu seen[512], now_seen[512];   /* (each driver thread's CPU time at the last line, and now) */
    static u32 nseen;
    static u64 last;   /* (FILETIME units) */
    if (!query) query = (LONG (NTAPI *)(HANDLE, int, void *, ULONG, ULONG *))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    FILETIME created, exited, k, u, now;
    GetSystemTimeAsFileTime(&now);
    u64 t = (u64)now.dwHighDateTime << 32 | now.dwLowDateTime, since = last;
    if (!since && GetProcessTimes(GetCurrentProcess(), &created, &exited, &k, &u)) since = (u64)created.dwHighDateTime << 32 | created.dwLowDateTime;
    HANDLE snap = query ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE || !since || t <= since) {
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        return;
    }
    u32 nnow = 0, threads = 0, busy = 0;
    u64 total = 0;
    WCHAR module[64] = L"";
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        void *start = NULL;
        HMODULE m = NULL;
        WCHAR path[MAX_PATH];
        DWORD len;
        if (h && query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) >= 0 && start &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)start, &m) &&
            (len = GetModuleFileNameW(m, path, MAX_PATH)) && len < MAX_PATH && in_driver_store(path, len) &&
            GetThreadTimes(h, &created, &exited, &k, &u)) {
            u64 cpu = ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime), was = 0;
            for (u32 i = 0; i < nseen; i++)
                if (seen[i].tid == te.th32ThreadID) { was = seen[i].cpu <= cpu ? seen[i].cpu : 0; break; }
            threads++;
            total += cpu - was;
            busy += (cpu - was) * 2 > t - since;
            if (nnow < 512) { now_seen[nnow].tid = te.th32ThreadID; now_seen[nnow++].cpu = cpu; }
            if (!module[0]) {
                const WCHAR *base = path + len;
                while (base > path && base[-1] != L'\\') base--;
                _snwprintf(module, 63, L"%ls", base);
            }
        }
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    memcpy(seen, now_seen, sizeof *seen * nnow);
    nseen = nnow;
    last = t;
    if (threads)
        fprintf(stderr, "[status] graphics driver's own threads (%ls): %u, %.1f CPU-s in the last %.0f s, %u of them busy over half of it\n",
                module, threads, total / 1e7, (t - since) / 1e7, busy);
}
#endif

void gfx_status_lines(void) {
    if (!ready) return;
#ifdef _WIN32
    driver_threads_line();
#endif
    if (!gfx_stat_first_draws) return;
    EnterCriticalSection(&lock);
    fprintf(stderr, "[status] first draws with a shader: %llu, %llu of them over 4 ms (%.0f ms in all); longest %.1f ms (%s)\n",
            gfx_stat_first_draws, gfx_stat_first_slow, first_slow_ms, first_longest_ms, first_longest);
    LeaveCriticalSection(&lock);
}

/* Hand a shader to the compiler threads. A compute program's cache load goes to the front:
 * its draw waits for it. */
static void enqueue_shader(Shader *s) {
    InterlockedIncrement(&compiles_pending);
    int q = s->cached ? 0 : 1;
    s->next_job = NULL;
    AcquireSRWLockExclusive(&jobs_lock);
    if (q == 0 && s->stage == SHADER_COMPUTE) {
        if (!(s->next_job = job_queue[0].head)) job_queue[0].tail = s;
        job_queue[0].head = s;
    } else {
        if (job_queue[q].tail) job_queue[q].tail->next_job = s;
        else job_queue[q].head = s;
        job_queue[q].tail = s;
    }
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeConditionVariable(&jobs_cv);
}

/* For a crash or freeze report: what each compiler thread is doing (to line, a line at a time),
 * its program kept next to the disk cache (shadercrash/) so it can be compiled again outside the
 * game. */
void gfx_report_compiles(void (*line)(const char *text)) {
    char text[600];
    static const char *const what[] = {"idle", "reading its DXBC from the disk cache", "fxc compiling its HLSL", "the driver compiling its DXBC"};
    DWORD self = GetCurrentThreadId();
    u64 now = GetTickCount64();
    int busy = 0;
    for (u32 i = 0; i < ncompilers; i++) {
        Shader *s = compilers[i].job;
        LONG step = compilers[i].step;
        if (!s || step == STEP_IDLE) continue;
        if (!busy++) line("shader compiler threads at the time:\n");
        char dir[1100], path[1200];
        snprintf(dir, sizeof dir, "%s/../shadercrash", rt_data_dir);
        CreateDirectoryA(dir, NULL);
        const char *saved = "";
        if (s->hlsl) {
            snprintf(path, sizeof path, "%s/%s_%08x_%016llx.hlsl", dir, s->name, s->vs_outputs, s->table_hash);
            FILE *f = fopen(path, "w");
            if (f) { fputs(s->hlsl, f); fclose(f); saved = "; HLSL saved"; }
        }
        const void *dxbc = compilers[i].dxbc;
        size_t size = compilers[i].dxbc_size;
        if (step == STEP_DRIVER && dxbc && size) {
            snprintf(path, sizeof path, "%s/%s_%08x_%016llx.dxbc", dir, s->name, s->vs_outputs, s->table_hash);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(dxbc, 1, size, f); fclose(f); saved = s->hlsl ? "; HLSL and DXBC saved" : "; DXBC saved"; }
        }
        snprintf(text, sizeof text, "  thread %lu%s: %s - %s, for %.1f s%s\n", (unsigned long)compilers[i].thread, compilers[i].thread == self ? " (this thread)" : "",
                 s->name, what[step], (now - compilers[i].since_ms) / 1000.0, saved);
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

/* The shader for the program at guest address g, ready (state 1) or not. alpha_tested: a pixel
 * shader's variant that does the fixed-function alpha test (its own hash, so its own cache file). */
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
    h = shader_cache_hash(h, p, size, stage);
    /* (the salt changes with the alpha test's code, so cache files of an older form are not used) */
    if (alpha_tested) h = (h ^ 0x616C706861743032ull) * 1099511628211ull;
    CbufSource src = {stage_cb};
    for (u32 i = 0; i < shader_count; i++) {
        Shader *s = shader_cache[i];
        if (s->hash != h || s->stage != stage || (stage == SHADER_PIXEL && s->vs_outputs != vs_outputs)) continue;
        if (s->info.ntables && s->table_hash != table_hash(&s->info, &src)) continue;
        *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
        return s;
    }
    if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
    Shader *s = calloc(1, sizeof *s);
    shader_cache[shader_count++] = s;
    s->hash = h; s->stage = stage; s->vs_outputs = vs_outputs;
    *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
    snprintf(s->name, sizeof s->name, "%s_%016llx%s", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", program_hash,
             alpha_tested ? "_alphatest" : "");
    s->hlsl = stage == SHADER_COMPUTE ? shader_hlsl_cs(p, size, local, shared, &s->info, read_cbuf_word, &src)
            : alpha_tested            ? shader_hlsl_alpha_tested(p, size, vs_outputs, &s->info, read_cbuf_word, &src)
                                      : shader_hlsl_ex(p, size, stage, vs_outputs, &s->info, read_cbuf_word, &src);
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
    if (find && strstr(s->name, find) && size > 0x90) {
        fprintf(stderr, "[find] %s at %#llx, %#x bytes; around it:", s->name, g, size);
        for (u32 i = 0; i < 0x1000 / 4; i++) {
            u32 v = ((const u32 *)((g & ~0xFFFull)))[i];
            if (i % 8 == 0) fprintf(stderr, "\n[find]   %#llx:", (g & ~0xFFFull) + i * 4);
            fprintf(stderr, " %08x", v);
        }
        fprintf(stderr, "\n");
        MEMORY_BASIC_INFORMATION mbi;
        for (u64 a = ALIAS_BASE; a < HEAP_BASE + HEAP_LIMIT && VirtualQuery((void *)a, &mbi, sizeof mbi); a = (u64)mbi.BaseAddress + mbi.RegionSize) {
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            const u8 *r = mbi.BaseAddress;
            for (u64 o = 0; o + 64 <= mbi.RegionSize; o += 16)
                if (!memcmp(r + o, p + 0x50, 64) && (u64)(r + o) != g + 0x50) {
                    fprintf(stderr, "[find]   copy at %#llx; bytes after its end:", (u64)(r + o) - 0x50);
                    const u32 *after = (const u32 *)(r + o - 0x50 + size);
                    for (u32 i = 0; i < 0x180 / 4 && o - 0x50 + size + i * 4 + 4 <= mbi.RegionSize; i++)
                        fprintf(stderr, "%s%08x", i % 8 ? " " : "\n[find]     ", after[i]);
                    fprintf(stderr, "\n");
                }
        }
    }
    static const char *dump;
    static int checked;
    if (!checked++) dump = getenv("SSBU_DUMP_HLSL");
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.hlsl", dump, s->name);
        FILE *f = fopen(path, "w");
        if (f) { fputs(s->hlsl, f); fclose(f); }
    }
    void *made = prewarm_take(s);
    if (made) {   /* the prewarm made it: ready now */
        s->obj = made;
        free(s->hlsl);
        s->hlsl = NULL;
        InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        s->state = 1;
        return s;
    }
    char path[1200];
    cache_path(s, path, sizeof path);
    s->cached = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    enqueue_shader(s);
    /* a compute program that was compiled before is waited for (a few ms): a skipped launch
     * leaves its outputs (skinned vertices) unwritten for a frame */
    if (stage == SHADER_COMPUTE && s->cached)
        for (u64 until = GetTickCount64() + 250; s->state == 0 && GetTickCount64() < until;) SwitchToThread();
    return s;
}

/* The ready shader for the program at guest address g, or NULL while it compiles. */
static Shader *get_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared) {
    Shader *s = find_shader_ex(g, stage, vs_outputs, stage_cb, local, shared, 0);
    return s && s->state == 1 ? s : NULL;
}

/* ---- buffers: guest memory uploaded per draw ----------------------------------------- */
typedef struct Upload { ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u32 cap; } Upload;
static Upload uploads[3][128];   /* per stage (vertex, pixel, compute), per SRV slot */

/* Uploads up to 64 KB go into chunks of rings, one ring per chunk size, each chunk with a view
 * of its own made once: written without overwriting what the GPU may still be reading, so no
 * buffer is renamed per upload (thousands a frame); a ring is discarded (renamed) once it has
 * gone round - and only at the start of a draw, with room left for all its uploads, so no
 * draw's data goes with it. Needs the driver to allow that for buffers read through views. */
#define RING_CLASSES 5
static const u32 ring_chunk[RING_CLASSES] = {256, 1024, 4096, 16384, 65536};
static const u32 ring_count[RING_CLASSES] = {8192, 2048, 1024, 256, 64};
#define RING_ROOM 40   /* chunks of each size one draw may take, at most */
static struct { ID3D11Buffer *buf; ID3D11ShaderResourceView **srv; u32 next, gen; } rings[RING_CLASSES];   /* (gen: discards so far) */
static int rings_ok = -1;

static int rings_init(void) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    memset(&o, 0, sizeof o);
    if (getenv("SSBU_NO_UPLOAD_RINGS") || FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)) ||
        !o.MapNoOverwriteOnDynamicBufferSRV) {
        fprintf(stderr, "[gfx] upload rings off (%s): every upload renames a buffer\n",
                getenv("SSBU_NO_UPLOAD_RINGS") ? "SSBU_NO_UPLOAD_RINGS" : "the driver does not allow writing a buffer the GPU reads through a view");
        return 0;
    }
    for (u32 c = 0; c < RING_CLASSES; c++) {
        D3D11_BUFFER_DESC bd = {ring_chunk[c] * ring_count[c], D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &rings[c].buf))) return 0;
        rings[c].srv = calloc(ring_count[c], sizeof *rings[c].srv);
        for (u32 i = 0; i < ring_count[c]; i++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
            sd.BufferEx.FirstElement = i * ring_chunk[c] / 4;
            sd.BufferEx.NumElements = ring_chunk[c] / 4;
            sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rings[c].buf, &sd, &rings[c].srv[i]))) return 0;
        }
        rings[c].next = ring_count[c];   /* (the first upload discards) */
    }
    fprintf(stderr, "[gfx] upload rings ready\n");
    return 1;
}

/* At the start of a draw or dispatch: a ring without room for all of its uploads starts again. */
static void rings_start_draw(void) {
    if (rings_ok < 0) rings_ok = rings_init();
    if (!rings_ok) return;
    for (u32 c = 0; c < RING_CLASSES; c++)
        if (rings[c].next + RING_ROOM > ring_count[c]) {
            if (rings[c].next < ring_count[c]) rings[c].gen++;   /* (its chunks are as good as gone: not handed out again) */
            rings[c].next = ring_count[c];
        }
}

/* A chunk for `size` bytes, mapped for writing at *dst (unmap with ring_done(*cls)). */
static ID3D11ShaderResourceView *ring_alloc(u32 size, u8 **dst, u32 *cls) {
    u32 c = 0;
    while (c < RING_CLASSES && ring_chunk[c] < size) c++;
    if (c == RING_CLASSES || rings_ok != 1) return NULL;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (rings[c].next >= ring_count[c]) { rings[c].next = 0; rings[c].gen++; how = D3D11_MAP_WRITE_DISCARD; }
    u32 i = rings[c].next++;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rings[c].buf, 0, how, 0, &m))) return NULL;
    *dst = (u8 *)m.pData + (size_t)i * ring_chunk[c];
    *cls = c;
    return rings[c].srv[i];
}

static void ring_done(u32 c) { ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)rings[c].buf, 0); }

static ID3D11ShaderResourceView *ring_upload(const void *data, u32 size) {
    u8 *dst;
    u32 c;
    ID3D11ShaderResourceView *v = ring_alloc(size, &dst, &c);
    if (!v) return NULL;
    if (data) memcpy(dst, data, size);
    else memset(dst, 0, size);
    ring_done(c);
    return v;
}

static void read_gpu(u64 va, u32 size, void *dst);

/* What each stage's slot was given last, with a copy of the bytes: a draw that uploads the same
 * bytes from the same place again - most of a menu's constants, draw after draw - gets the same
 * view, whose data is still there (the ring has not been discarded since, or the slot's own
 * buffer not written since): no map, no copy, and the view stays bound. Drivers that do their
 * work on the calling thread (AMD's, Intel's) pay for every map and bind there, in the game's
 * frame. */
#define REUSE_MAX 65536
/* A slot whose bytes keep changing (most of a match's per-draw constants) is not compared or
 * copied for a while: after a frame of at least REUSE_TRIES uploads with none reused, its next
 * REUSE_PAUSE frames go straight from guest memory, then it is tried again (in a player's
 * 8-fighter match the comparing alone was 3-7.5% of the main thread, and the extra copy came on
 * top, for next to nothing). A slot reused at all in a frame - a menu's - keeps being compared. */
#define REUSE_TRIES 8
#define REUSE_PAUSE 8
typedef struct LastUpload {
    u64 va; u32 size, cls, gen, cap; ID3D11ShaderResourceView *srv; u8 *copy;
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

/* Guest GPU memory [va, va + size) uploaded for a stage's slot: straight from guest memory into
 * a ring chunk when it fits one (else through a copy). */
static ID3D11ShaderResourceView *upload_raw(int stage, u32 slot, const void *data, u32 size);
static ID3D11ShaderResourceView *upload_gpu(int stage, u32 slot, u64 va, u32 size) {
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
    if (keep && lu->srv && lu->va == va && lu->size == size && lu->gen == (lu->cls < RING_CLASSES ? rings[lu->cls].gen : 0)) {
        if (untouched_since(lu->batch, lu->seq, g, size)) {   /* compared already in this submission, and not written since */
            lu->hits++;
            gfx_stat_reused++;
            return lu->srv;
        }
        gfx_stat_reuse_compared += size;
        if (!memcmp(src, lu->copy, size)) {
            lu->batch = gpu_stat_batches;
            lu->seq = wrote_seq;
            lu->hits++;
            gfx_stat_reused++;
            return lu->srv;
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
    u8 *dst;
    u32 c;
    ID3D11ShaderResourceView *v = ring_alloc(padded, &dst, &c);
    if (v) {
        if (keep) memcpy(dst, lu->copy, size);
        else if (src) memcpy(dst, src, size);
        else read_gpu(va, size, dst);
        if (padded > size) memset(dst + size, 0, padded - size);
        ring_done(c);
        gfx_stat_upload_bytes += padded;
        gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += padded;
        *lu = (LastUpload){va, size, c, rings[c].gen, lu->cap, keep ? v : NULL, lu->copy, gpu_stat_batches, wrote_seq,
                           lu->frame, lu->paused_to, lu->tries, lu->hits};
        return v;
    }
    static u8 *tmp;
    static u32 tmp_cap;
    if (padded > tmp_cap) { tmp = realloc(tmp, padded); tmp_cap = padded; }
    if (keep) memcpy(tmp, lu->copy, size);
    else if (src) memcpy(tmp, src, size);
    else read_gpu(va, size, tmp);
    if (padded > size) memset(tmp + size, 0, padded - size);
    v = upload_raw(stage, slot, tmp, size);   /* (the slot's own buffer, unless a ring took it) */
    if (keep && v && v == uploads[stage][slot].srv)
        *lu = (LastUpload){va, size, RING_CLASSES, 0, lu->cap, v, lu->copy, gpu_stat_batches, wrote_seq, lu->frame, lu->paused_to, lu->tries, lu->hits};
    return v;
}

static ID3D11ShaderResourceView *upload_raw(int stage, u32 slot, const void *data, u32 size) {
    Upload *u = &uploads[stage][slot];
    last_upload[stage][slot].srv = NULL;   /* (the slot's buffer may be written below) */
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    gfx_stat_upload_bytes += size;
    gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += size;   /* constant buffers, vertex streams, storage buffers */
    ID3D11ShaderResourceView *r = ring_upload(data, size);
    if (r) return r;
    if (size > u->cap) {
        if (u->srv) ID3D11ShaderResourceView_Release(u->srv);
        if (u->buf) ID3D11Buffer_Release(u->buf);
        u32 cap = 1024;
        while (cap < size) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &u->buf))) { u->cap = 0; u->buf = NULL; return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = cap / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)u->buf, &sd, &u->srv);
        u->cap = cap;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)u->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return NULL;
    if (data) memcpy(m.pData, data, size);
    else memset(m.pData, 0, size);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)u->buf, 0);
    return u->srv;
}

static ID3D11Buffer *params_buffer, *index_buffer;
static u32 index_cap, index_pos, index_first;   /* the index ring: its size, where the next indices go, where this draw's start (in indices) */
static DrawParams params_written;   /* what params_buffer holds (a draw with the same parameters writes nothing) */
static int params_known;

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

/* ---- GPU-resident buffers ---------------------------------------------------------- */
/* What compute programs write stays on the GPU: a buffer per guest range they write (and per
 * copy-engine destination of such data), used directly by later dispatches and by draws whose
 * vertex streams lie inside it; reads that only partly overlap one get the guest's data around
 * it. Guest memory under a region is stale; pages the CPU writes inside one (seen through the
 * write watch) are taken from guest memory. */
typedef struct ResidentView { u64 first; u32 bytes; ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav; } ResidentView;
typedef struct Resident {
    u64 va, guest;
    u32 size;
    ID3D11Buffer *buf;
    ID3D11UnorderedAccessView *uav;
    ResidentView views[64];
    u32 nviews;
    /* Guest memory under the region's first and last partial pages (head and tail bytes), as
     * it was when the GPU copy last took it: whether the bytes inside the region were written
     * (rather than someone's data next to it) is told by comparing with these. */
    u8 *edge;
    u32 head, tail;
    u64 checked;   /* the submission it was last checked against the write watch in */
    u64 seen;      /* (the write watch's clock then: watch_written) */
    /* The GPU's last write into it (a compute program's or a copy's): the write watch's clock
     * then, and the frame. Whole pages stamped after that clock are the CPU's again (the region
     * takes such pages whole), and so are the partial pages' bytes once taken since (head_taken,
     * tail_taken): where all it holds of a range is the CPU's, guest memory there is the same. */
    u64 wrote_seen, wrote_frame;
    u8 head_taken, tail_taken;
} Resident;
static Resident residents[512];
static u32 nresidents;
u64 gfx_stat_residents, gfx_stat_resident_pages;
extern u64 gpu_stat_batches;
static int watched(u64 g, u64 size);

/* The write watch keeps one bit a page of the alias area, and a query that resets it takes the
 * news from everyone else watching that page. Textures, kept constant buffers, compute inputs
 * and resident regions share pages - memory a scene gives up is handed out again for something
 * else - and whoever asked first kept the news for good: the No Contest screen skinned the
 * fighters' arms from inputs never taken again (stretched over the screen). So every question
 * goes through here: pages found written get the next stamp, kept per page, and each user keeps
 * the clock as it was when it last asked ("seen"): its data is current unless one of its pages
 * has a newer stamp. */
#define WATCH_CHUNK 512   /* pages a chunk: 2 MB */
typedef struct WatchChunk { u64 newest, page[WATCH_CHUNK]; } WatchChunk;
static WatchChunk *watch_chunks[ALIAS_SIZE / (WATCH_CHUNK * 0x1000ull)];
static u64 watch_clock;   /* stamps given so far */

/* Take the write watch's news for guest pages [lo, hi) (page aligned, watched): resetting the
 * bits as they are read, so a write after this is news next time. */
static void watch_take(u64 lo, u64 hi) {
    static void *pages[65536];   /* (256 MB: the largest range asked about, in one call) */
    void *first;
    ULONG_PTR count = 1;
    ULONG granularity;
    /* (asking is cheap, resetting is not - even where nothing was written: so ask first) */
    if (GetWriteWatch(0, (void *)lo, hi - lo, &first, &count, &granularity) != 0 || !count) return;
    for (u64 at = (u64)first; at < hi;) {
        count = 65536;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)at, hi - at, pages, &count, &granularity) != 0 || !count) return;
        u64 stamp = ++watch_clock;
        for (ULONG_PTR k = 0; k < count; k++) {
            u64 p = ((u64)pages[k] - ALIAS_BASE) >> 12;
            WatchChunk **c = &watch_chunks[p / WATCH_CHUNK];
            if (!*c && !(*c = calloc(1, sizeof **c))) continue;
            (*c)->page[p % WATCH_CHUNK] = (*c)->newest = stamp;
        }
        if (count < 65536) return;
        at = (u64)pages[count - 1] + 0x1000;
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

/* Has guest memory [g, g + size) been written since *seen? (*seen moves on to now.) Ask before
 * reading the memory, so that a write meanwhile is news next time. */
static int watch_written(u64 g, u64 size, u64 *seen) {
    u64 lo = g & ~0xFFFull, hi = (g + size + 0xFFF) & ~0xFFFull;
    watch_take(lo, hi);
    int written = watch_next(lo, hi, *seen) < hi;
    *seen = watch_clock;
    return written;
}

/* Views and buffers given up during a frame may still be in this frame's bindings (or the
 * arrays they are being collected in): they are released after the present. */
static IUnknown **graveyard;
static u32 ngraves, graves_cap;
static void bury(void *object) {
    if (!object) return;
    if (ngraves == graves_cap) graveyard = realloc(graveyard, sizeof *graveyard * (graves_cap = graves_cap * 2 + 256));
    graveyard[ngraves++] = object;
}
static void release_graves(void) {
    for (u32 i = 0; i < ngraves; i++) graveyard[i]->lpVtbl->Release(graveyard[i]);
    ngraves = 0;
}

static void resident_drop(Resident *r) {
    for (u32 i = 0; i < r->nviews; i++) { bury(r->views[i].srv); bury(r->views[i].uav); }
    bury(r->uav);
    bury(r->buf);
    free(r->edge);
    *r = residents[--nresidents];
    memset(&residents[nresidents], 0, sizeof *residents);
}

static int read_guarded(void *dst, u64 g, u32 n);

static void resident_snapshot(Resident *r) {
    u32 head = (u32)(0x1000 - (r->guest & 0xFFF)) & 0xFFF;   /* up to the first page boundary */
    if (head > r->size) head = r->size;
    u32 tail = (u32)((r->guest + r->size) & 0xFFF);          /* from the last one */
    if (tail > r->size - head) tail = r->size - head;
    r->head = head;
    r->tail = tail;
    r->edge = realloc(r->edge, head + tail + 1);
    /* (zeros where the memory is gone - a region made over older ones may reach memory the
     * guest has unmapped since: resident_sync finds it gone too, and leaves it alone) */
    if (!read_guarded(r->edge, r->guest, head)) memset(r->edge, 0, head);
    if (!read_guarded(r->edge + head, r->guest + r->size - tail, tail)) memset(r->edge + head, 0, tail);
}

/* n bytes of guest memory at g, or 0 if they are not there any more: the guest can unmap the
 * memory under a resident region (svc UnmapPhysicalMemory). The renderer hears of that and drops
 * the region (gfx_guest_unmapped), but only if the memory held an nvmap allocation when it went. */
#ifdef _WIN32
extern __declspec(thread) int rt_probing;   /* host.c: the crash handler leaves a fault to us */
static __declspec(noinline) void copy_guest(void *dst, const void *src, u32 n) { memcpy(dst, src, n); }
static int read_guarded(void *dst, u64 g, u32 n) {
    volatile int ok = 1;
    rt_probing = 1;
    __try {
        copy_guest(dst, P(void, g), n);   /* (a call of its own: a fault inside __try is only caught reliably in a callee) */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    rt_probing = 0;
    return ok;
}
#else   /* (Linux: process_vm_readv fails with EFAULT where a read would fault) */
static int read_guarded(void *dst, u64 g, u32 n) {
    struct iovec to = {dst, n}, from = {P(void, g), n};
    ssize_t got = process_vm_readv(getpid(), &to, 1, &from, 1, 0);
    if (got == (ssize_t)n) return 1;
    if (got >= 0 || errno == EFAULT) return 0;
    memcpy(dst, P(void, g), n);   /* (where the call isn't allowed, as in some sandboxes: a plain read) */
    return 1;
}
#endif

/* A partial page's bytes inside the region (n at g; was: as the GPU copy last took them): taken
 * from guest memory if they changed (and *taken set). 0 if that memory is gone. */
static int resident_take_edge(Resident *r, u64 g, u8 *was, u32 n, u8 *taken) {
    u8 now[0x1000];   /* (an edge lies within one page) */
    if (!n) return 1;
    if (!read_guarded(now, g, n)) return 0;
    if (!memcmp(was, now, n)) return 1;
    memcpy(was, now, n);
    *taken = 1;
    D3D11_BOX box = {(UINT)(g - r->guest), 0, 0, (UINT)(g - r->guest + n), 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->buf, 0, &box, was, 0, 0);
    gfx_stat_resident_pages++;
    return 1;
}

/* The write watch over the region's whole pages starts over (after its GPU copy took guest
 * memory). Not over its partial pages: they are compared by content instead. */
static void resident_watch_reset(Resident *r) {
    u64 lo = (r->guest + 0xFFF) & ~0xFFFull, hi = (r->guest + r->size) & ~0xFFFull;
    if (lo < hi) watch_take(lo, hi);
    r->seen = watch_clock;
}

/* Has the CPU written guest memory inside the region since its GPU copy last took it? Pages
 * wholly inside that it wrote (the write watch says) are taken from guest memory. The partial
 * pages at either end are compared byte for byte, every time, and taken where the bytes inside
 * the region changed - the CPU may have written just its own data next to the region, or
 * (memory reused) right over a region no program writes any more. Not by the write watch:
 * it has one bit a page, and whoever checks a page first resets it for everyone. Small regions
 * pack several to a page in the effect ring (GPU particle output among the CPU's per-draw
 * attributes); the first one checked took the bit and its neighbours kept stale data for good,
 * which drew effect meshes at a stale size over the whole screen (the white-outs). Checked
 * when a draw or dispatch uses the region (once a submission) and once a frame for all.
 * Whole pages go to the GPU copy through a copy of their own too, never straight from guest
 * memory: the driver reads what it is given unguarded. A region is dropped when the guest
 * unmaps the memory under it (gfx_guest_unmapped), but before it was, regions outlived that
 * memory and players' games crashed in the graphics driver on the vsync thread after 20-40
 * minutes; the guarded copy stays for memory taken away without notice. */
static void resident_sync(Resident *r, int every_frame) {
    static int no_pages = -1;   /* debug: leave resident regions alone */
    if (no_pages < 0) no_pages = getenv("SSBU_NO_RESIDENT_PAGES") != NULL;
    if (no_pages || (!every_frame && r->checked == gpu_stat_batches) || !r->edge || !watched(r->guest, r->size)) return;
    r->checked = gpu_stat_batches;
    if (!resident_take_edge(r, r->guest, r->edge, r->head, &r->head_taken) ||
        !resident_take_edge(r, r->guest + r->size - r->tail, r->edge + r->head, r->tail, &r->tail_taken))
        return;   /* (its memory is gone: left alone; once mapped again its bytes differ, and are taken) */
    u64 lo = (r->guest + 0xFFF) & ~0xFFFull, hi = (r->guest + r->size) & ~0xFFFull;   /* the whole pages */
    if (lo >= hi) return;
    u64 seen = r->seen;
    watch_take(lo, hi);   /* (before taking them: what the CPU writes meanwhile is news next time) */
    r->seen = watch_clock;
    for (u64 pg = watch_next(lo, hi, seen); pg < hi; pg = watch_next(pg + 0x1000, hi, seen)) {
        u8 now[0x1000];
        if (!read_guarded(now, pg, 0x1000)) continue;   /* (gone: left as it is) */
        D3D11_BOX box = {(UINT)(pg - r->guest), 0, 0, (UINT)(pg + 0x1000 - r->guest), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->buf, 0, &box, now, 0, 0);
        gfx_stat_resident_pages++;
    }
}

static Resident *resident_covering(u64 lo, u64 hi) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va <= lo && hi <= residents[i].va + residents[i].size) { resident_sync(&residents[i], 0); return &residents[i]; }
    return NULL;
}

/* A compute program or a copy writes into the region. */
static void resident_gpu_wrote(Resident *r) {
    r->wrote_seen = watch_clock;
    r->wrote_frame = gfx_stat_presented;
    r->head_taken = r->tail_taken = 0;
}

/* Is all that r holds of [va, va + bytes) the CPU's again (see wrote_seen)? Then guest memory
 * there is the same, and a read may take it from there. (r synced first, for the stamps.) */
static int resident_cpu_owned(Resident *r, u64 va, u64 bytes) {
    u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
    if (lo >= hi) return 1;
    if (!r->edge || !watched(r->guest, r->size)) return 0;
    u64 glo = r->guest + (lo - r->va), ghi = r->guest + (hi - r->va);
    if (glo < r->guest + r->head && !r->head_taken) return 0;
    if (ghi > r->guest + r->size - r->tail && !r->tail_taken) return 0;
    u64 wlo = (r->guest + 0xFFF) & ~0xFFFull, whi = (r->guest + r->size) & ~0xFFFull;   /* its whole pages */
    u64 p = glo & ~0xFFFull, end = (ghi + 0xFFF) & ~0xFFFull;
    if (p < wlo) p = wlo;
    if (end > whi) end = whi;
    for (; p < end; p += 0x1000) {
        u64 i = (p - ALIAS_BASE) >> 12;
        const WatchChunk *c = watch_chunks[i / WATCH_CHUNK];
        if (!c || c->page[i % WATCH_CHUNK] <= r->wrote_seen) return 0;
    }
    return 1;
}

/* The first resident region holding GPU data in [va, va + size) (synced), or NULL: regions
 * there that hold only what the CPU wrote since (resident_cpu_owned) are the same as guest
 * memory. Memory a scene gives up is handed out again, and a region left behind over it would
 * otherwise send every draw reading its new data through upload_composed. */
static Resident *resident_overlapping_gpu(u64 va, u32 size) {
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va >= va + size || va >= r->va + r->size) continue;
        resident_sync(r, 0);
        if (!resident_cpu_owned(r, va, size)) return r;
    }
    return NULL;
}

/* A raw view of [va, va + bytes) inside r: for shaders to read (srv) or compute to write. Raw
 * views start on 16 bytes (D3D11 refuses others: most of a fighter's mesh parts start 4, 8 or 12
 * bytes into a line of its skinned vertices, and every such draw went through upload_composed),
 * so *skip says how far before va this one starts, for the shader to add to its offsets. */
static ResidentView *resident_viewx(Resident *r, u64 va, u32 bytes, u32 *skip) {
    u64 first = (va - r->va) & ~15ull;
    *skip = (u32)(va - r->va - first);
    bytes = (u32)((va + bytes - r->va - first + 3) & ~3ull);
    if (first + bytes > ((r->size + 15) & ~15u)) bytes = (u32)(((r->size + 15) & ~15u) - first);
    for (u32 i = 0; i < r->nviews; i++)
        if (r->views[i].first == first && r->views[i].bytes == bytes) return &r->views[i];
    if (r->nviews == 64) {
        ResidentView *o = &r->views[0];
        bury(o->srv);
        bury(o->uav);
        *o = r->views[--r->nviews];
    }
    ResidentView *v = &r->views[r->nviews];
    memset(v, 0, sizeof *v);
    v->first = first;
    v->bytes = bytes;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = (UINT)(first / 4);
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = (UINT)(first / 4);
    ud.Buffer.NumElements = bytes / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)r->buf, &sd, &v->srv)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)r->buf, &ud, &v->uav))) {
        if (v->srv) ID3D11ShaderResourceView_Release(v->srv);
        return NULL;
    }
    r->nviews++;
    return v;
}

static ID3D11ShaderResourceView *resident_view(Resident *r, u64 va, u32 bytes, u32 *skip) {
    ResidentView *v = resident_viewx(r, va, bytes, skip);
    return v ? v->srv : NULL;
}

static Resident *resident_create(u64 va, u32 size) {
    if (nresidents == 512) resident_drop(&residents[0]);
    Resident *r = &residents[nresidents];
    memset(r, 0, sizeof *r);
    u32 cap = (size + 15) & ~15u;
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                            D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    read_gpu(va, size, init);   /* what the CPU put there */
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &r->buf);
    free(init);
    if (FAILED(hr)) return NULL;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = cap / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)r->buf, &ud, &r->uav))) { ID3D11Buffer_Release(r->buf); return NULL; }
    r->va = va;
    r->size = size;
    r->guest = gpu_va_to_guest(va);
    if (watched(r->guest, r->size)) resident_snapshot(r);
    nresidents++;
    gfx_stat_residents++;
    return r;
}

/* The resident region holding [va, va + size), made (or widened) as needed, for the GPU to
 * write into. Regions never overlap: a range that overlaps some becomes one region over all of
 * them, with the CPU's data underneath and each old region's GPU data copied over it. */
static Resident *resident_acquire(u64 va, u32 size) {
    u64 lo = va, hi = va + size;
    int overlaps = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va <= va && va + size <= r->va + r->size) { resident_gpu_wrote(r); return r; }
        if (r->va < hi && va < r->va + r->size) {
            overlaps = 1;
            if (r->va < lo) lo = r->va;
            if (r->va + r->size > hi) hi = r->va + r->size;
        }
    }
    if (hi - lo > (256u << 20)) return NULL;
    Resident *n = resident_create(lo, (u32)(hi - lo));
    if (!n) return NULL;
    if (overlaps) {
        Resident merged = *n;   /* resident_drop moves entries around */
        n->buf = NULL; n->uav = NULL; n->edge = NULL;
        resident_drop(n);
        for (u32 i = 0; i < nresidents;) {
            Resident *r = &residents[i];
            if (r->va >= merged.va && r->va + r->size <= merged.va + merged.size && r->buf != merged.buf) {
                D3D11_BOX box = {0, 0, 0, (r->size + 3) & ~3u, 1, 1};
                ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)merged.buf, 0, (UINT)(r->va - merged.va), 0, 0,
                                                          (ID3D11Resource *)r->buf, 0, &box);
                resident_drop(r);
            } else i++;
        }
        residents[nresidents++] = merged;
        n = &residents[nresidents - 1];
    }
    if (n->guest >= ALIAS_BASE && n->guest + n->size <= ALIAS_BASE + ALIAS_SIZE) resident_watch_reset(n);
    resident_gpu_wrote(n);
    return n;
}

/* dst holds guest memory's [va, va + size): copy over it what resident regions hold of that. */
static void resident_patch(ID3D11Buffer *dst, u64 va, u64 size) {
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + size < r->va + r->size ? va + size : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 0);
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, (UINT)(lo - va), 0, 0, (ID3D11Resource *)r->buf, 0, &box);
    }
}

/* A range only partly held by resident regions, as the GPU sees it: the guest's data (in data)
 * with the regions' parts copied over it, in a GPU buffer per slot. Regions are never widened
 * for a read: what lies around them is the CPU's, and it keeps changing in guest memory. */
typedef struct Composed { ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u32 cap; } Composed;
static Composed composed[3][128];

static ID3D11ShaderResourceView *upload_composed(int stage, u32 slot, u64 va, const void *data, u32 size) {
    Composed *c = &composed[stage][slot];
    u32 need = (size + 15) & ~15u;
    gfx_stat_upload_bytes += size;
    gfx_stat_composed++;
    if (!need) return upload_raw(stage, slot, data, size);
    if (need > c->cap) {
        bury(c->srv);   /* may be bound still */
        bury(c->buf);
        c->srv = NULL; c->buf = NULL; c->cap = 0;
        u32 cap = 4096;
        while (cap < need) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &c->buf))) { c->buf = NULL; return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = cap / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)c->buf, &sd, &c->srv))) {
            ID3D11Buffer_Release(c->buf);
            c->buf = NULL; c->srv = NULL;
            return NULL;
        }
        c->cap = cap;
    }
    D3D11_BOX box = {0, 0, 0, size, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)c->buf, 0, &box, data, 0, 0);
    resident_patch(c->buf, va, size);
    return c->srv;
}

/* The copy engine and resident data. */
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) {
    if (!bytes || bytes > (256u << 20) || !enter()) return 0;
    Resident *src = resident_overlapping_gpu(src_va, (u32)bytes);
    if (!src || src_va < src->va || src_va + bytes > src->va + src->size) {
        leave();
        if (src) gfx_resident_flush(src_va, bytes);   /* partly resident: through guest memory */
        return 0;
    }
    Resident *dst = resident_acquire(dst_va, (u32)bytes);   /* may merge regions: look the source up again */
    src = resident_covering(src_va, src_va + bytes);
    if (!dst || !src) { leave(); return 0; }
    D3D11_BOX box = {(UINT)(src_va - src->va), 0, 0, (UINT)(src_va - src->va + bytes), 1, 1};
    if (dst != src)
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst->buf, 0, (UINT)(dst_va - dst->va), 0, 0,
                                                  (ID3D11Resource *)src->buf, 0, &box);
    else {   /* within one buffer: through a scratch one */
        static ID3D11Buffer *tmp;
        static u32 tmp_cap;
        if (bytes > tmp_cap) {
            if (tmp) ID3D11Buffer_Release(tmp);
            tmp_cap = (u32)((bytes + 0xFFFF) & ~0xFFFFull);
            D3D11_BUFFER_DESC bd = {tmp_cap, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
            if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &tmp))) { tmp = NULL; tmp_cap = 0; leave(); return 0; }
        }
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmp, 0, 0, 0, 0, (ID3D11Resource *)src->buf, 0, &box);
        D3D11_BOX all = {0, 0, 0, (UINT)bytes, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst->buf, 0, (UINT)(dst_va - dst->va), 0, 0,
                                                  (ID3D11Resource *)tmp, 0, &all);
    }
    leave();
    return 1;
}

/* Bring the GPU's data for [va, va + bytes) back to guest memory (waits for the GPU). */
void gfx_resident_flush(u64 va, u64 bytes) {
    if (!enter()) return;
    guest_wrote(va, bytes);
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        D3D11_BUFFER_DESC sd = {(UINT)(hi - lo), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Buffer *st;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &sd, NULL, &st))) continue;
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)r->buf, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        gfx_stat_gpu_waits++;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            for (u64 a = lo; a < hi; a += 4) {
                u64 g = gpu_va_to_guest(a);
                if (g) st32(g, ((const u32 *)m.pData)[(a - lo) / 4]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Buffer_Release(st);
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
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->buf, 0, &box, data, 0, 0);
        free(data);
        if (r->guest >= ALIAS_BASE && r->guest + r->size <= ALIAS_BASE + ALIAS_SIZE) { resident_watch_reset(r); resident_snapshot(r); }
    }
    leave();
}

/* ---- textures and samplers ------------------------------------------------------------ */
typedef struct Texture {
    u64 guest, size;
    u32 tic[8];
    u64 sample_hash;
    int watched, stale;    /* in the write-watched alias area; written since it was read */
    u64 seen;              /* (the write watch's clock when it was read: watch_written) */
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *tex;
    u64 used, asked;       /* (asked: the frame the write watch was last asked about it, gfx_stat_presented) */
} Texture;
static Texture textures[4096];
static u32 ntextures;
u64 gfx_stat_textures, gfx_stat_watch_us;
u64 gfx_stat_unpacked;   /* block-compressed textures copied to their exact size (unpack_texture) */

static void watch_time(u64 ns) {   /* (to gfx_stat_watch_us, in microseconds) */
    static u64 rest;
    rest += ns;
    gfx_stat_watch_us += rest / 1000;
    rest %= 1000;
}

/* Which textures has the CPU (or a copy) written since they were read? The alias area is
 * reserved with MEM_WRITE_WATCH, so Windows tracks the pages written (watch_written). A
 * texture's range is asked about the first time a frame draws with it - not every texture
 * every frame: the system's cost goes by the pages asked about, and a match keeps thousands of
 * textures from earlier screens it never draws (a player's 8-fighter match: about 4,000, some
 * 0.7 ms a frame). A write is news to every texture on its pages, each taken again at its
 * next use. */
static int watched(u64 g, u64 size) { return g >= ALIAS_BASE && g + size <= ALIAS_BASE + ALIAS_SIZE; }

static void texture_ask(Texture *t) {
    if (!t->watched || t->stale || t->asked == gfx_stat_presented) return;
    t->asked = gfx_stat_presented;
    u64 t0 = rt_clock_ns();
    if (watch_written(t->guest, t->size, &t->seen)) t->stale = 1;
    watch_time(rt_clock_ns() - t0);
}

/* Once a frame: GPU-resident buffers the CPU has written to (those no draw has checked yet).
 * A region no program has written into for a second that holds only what the CPU wrote since
 * (resident_cpu_owned) is the same as guest memory, and is given up: memory a scene leaves is
 * handed out again, and regions over it otherwise pile up for good (a player's long session
 * had 338). */
#define RESIDENT_IDLE 60
u64 gfx_stat_residents_retired;
static void note_writes(void) {
    u64 t0 = rt_clock_ns();
    for (u32 i = 0; i < nresidents;) {
        Resident *r = &residents[i];
        resident_sync(r, 1);
        if (gfx_stat_presented - r->wrote_frame >= RESIDENT_IDLE && resident_cpu_owned(r, r->va, r->size)) {
            resident_drop(r);   /* (the last one moves here) */
            gfx_stat_residents_retired++;
            continue;
        }
        i++;
    }
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
        if (t->srv && t->guest == g && !memcmp(t->tic, tic, 32)) return t;
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
            if (textures[i].srv && &textures[i] != t) { texture_index_put(&textures[i]); texture_index_adds++; }
    }
    texture_index_put(t);
}

static int depth_sampling = -1;   /* depth buffers as textures (shadow maps, ...); SSBU_NO_DEPTH_SAMPLING or F5: off */

/* Block-compressed textures whose size is not a multiple of the 4x4 block: Direct3D 11 makes
 * level 0 whole blocks, and a texture a few texels bigger is sampled wrongly at its right and
 * bottom edges - coordinates are scaled by size / padded size, and clamping repeats the padding
 * (ui2d's window frames stretch their last column across the middle of a button: the Spirits
 * menu's selected button showed grey bars there). Such a texture is made padded as a source,
 * then copied level by level, texel by texel, into an uncompressed texture of its exact size.
 * Formats: the source's (typeless), how it is read, the copy's, its render target view. */
typedef struct { DXGI_FORMAT bc, read, tex, rtv; } Unpacked;
static int unpacked_formats(DXGI_FORMAT f, Unpacked *u) {
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: *u = (Unpacked){DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM}; break;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: *u = (Unpacked){DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM}; break;
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: *u = (Unpacked){DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM}; break;
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: *u = (Unpacked){DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM}; break;
    case DXGI_FORMAT_BC4_UNORM: *u = (Unpacked){DXGI_FORMAT_BC4_TYPELESS, f, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}; return 1;
    case DXGI_FORMAT_BC4_SNORM: *u = (Unpacked){DXGI_FORMAT_BC4_TYPELESS, f, DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16_SNORM}; return 1;
    case DXGI_FORMAT_BC5_UNORM: *u = (Unpacked){DXGI_FORMAT_BC5_TYPELESS, f, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UNORM}; return 1;
    case DXGI_FORMAT_BC5_SNORM: *u = (Unpacked){DXGI_FORMAT_BC5_TYPELESS, f, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SNORM}; return 1;
    case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        *u = (Unpacked){DXGI_FORMAT_BC6H_TYPELESS, f, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT}; return 1;
    default: return 0;
    }
    u->tex = DXGI_FORMAT_R8G8B8A8_TYPELESS;   /* (an sRGB one read as UNORM and viewed as sRGB: the bytes unchanged) */
    u->rtv = DXGI_FORMAT_R8G8B8A8_UNORM;
    return 1;
}

static ID3D11DeviceContext *unpack_ctx;   /* deferred: the copy runs with the immediate context's state kept (mid-draw) */
static u32 gfx_debug(void);

static ID3D11Texture2D *unpack_texture(ID3D11Texture2D *bc, const TextureDesc *d, u32 levels, u32 layers, UINT misc, const Unpacked *u) {
    if (!decode_ps || (!unpack_ctx && FAILED(ID3D11Device_CreateDeferredContext(dev, 0, &unpack_ctx)))) return NULL;
    D3D11_TEXTURE2D_DESC td = {d->width, d->height, levels, layers, u->tex, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, misc};
    ID3D11Texture2D *out;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &out))) return NULL;
    ID3D11DeviceContext *c = unpack_ctx;
    ID3D11DeviceContext_VSSetShader(c, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, decode_ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    int ok = 1;
    for (u32 layer = 0; layer < layers && ok; layer++)
        for (u32 l = 0; l < levels && ok; l++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {u->read, D3D11_SRV_DIMENSION_TEXTURE2DARRAY};
            sd.Texture2DArray = (D3D11_TEX2D_ARRAY_SRV){l, 1, layer, 1};
            D3D11_RENDER_TARGET_VIEW_DESC rd = {u->rtv, D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
            rd.Texture2DArray = (D3D11_TEX2D_ARRAY_RTV){l, layer, 1};
            ID3D11ShaderResourceView *srv = NULL;
            ID3D11RenderTargetView *rtv = NULL;
            ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)bc, &sd, &srv)) &&
                 SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)out, &rd, &rtv));
            if (ok) {
                D3D11_VIEWPORT vp = {0, 0, (float)(d->width >> l ? d->width >> l : 1), (float)(d->height >> l ? d->height >> l : 1), 0, 1};
                ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);
                ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
                ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &srv);
                ID3D11DeviceContext_Draw(c, 3, 0);
            }
            if (srv) ID3D11ShaderResourceView_Release(srv);
            if (rtv) ID3D11RenderTargetView_Release(rtv);
        }
    ID3D11CommandList *list = NULL;
    if (FAILED(ID3D11DeviceContext_FinishCommandList(c, FALSE, &list))) ok = 0;
    if (ok) ID3D11DeviceContext_ExecuteCommandList(ctx, list, TRUE);
    if (list) ID3D11CommandList_Release(list);
    if (!ok) { ID3D11Texture2D_Release(out); return NULL; }
    return out;
}

static ID3D11ShaderResourceView *get_texture(const u32 *tic, u32 shader_type, float *scale) {
    TextureDesc d;
    *scale = 1.0f;
    int ok = tic_decode(tic, &d);
    u64 g = gpu_va_to_guest(d.address);
    if (!g) return NULL;
    /* A render target at this address: use it directly, read as the texture header says (sRGB
     * or not) - the latest one, as memory is reused. A depth buffer only for a depth format of
     * its size (shadow maps): the same memory may hold a colour image by now. */
    if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
    int depth_tic = d.format == 0x29 || d.format == 0x2A || d.format == 0x2B || d.format == 0x2F || d.format == 0x30 || d.format == 0x3A;
    Surface *rt = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->guest != g || !s->srv) continue;
        if (s->depth && (!depth_sampling || !depth_tic || !ok || s->width != d.width || s->height != d.height)) continue;
        if (!rt || s->used > rt->used) rt = s;
    }
    if (rt && draw_logging) note_sampled(rt);
    if (rt) { *scale = rt->sx; return surface_srv(rt, (int)d.srgb); }
    if (!ok || d.type == TIC_1D_BUFFER || d.bytes == 0) { warn_texture(&d); return NULL; }
    u32 layers = d.type == TIC_CUBE ? 6 : d.type == TIC_CUBE_ARRAY ? 6 * d.depth : d.type == TIC_2D_ARRAY || d.type == TIC_1D_ARRAY ? d.depth : 1;
    if (d.type == TIC_3D) layers = 1;
    u64 size = d.type == TIC_3D ? texture_3d_size(&d) : texture_layer_size(&d) * layers;
    int watch = watched(g, size);
    u64 sh = watch ? 0 : sample_hash(g, size);
    Texture *free_slot = NULL, *lru = NULL, *hit = texture_find(g, tic);
    if (hit) {
        if (watch) texture_ask(hit);
        if (watch ? !hit->stale : hit->sample_hash == sh) { hit->used = ++tick; return hit->srv; }
        free_slot = hit;   /* contents changed: rebuild in place */
    } else
        for (u32 i = 0; i < ntextures; i++) {   /* a new one: a free slot, else the least recently used */
            Texture *t = &textures[i];
            if (!t->srv) { if (!free_slot) free_slot = t; continue; }
            if (!lru || t->used < lru->used) lru = t;
        }
    Texture *t = free_slot;
    if (!t) t = ntextures < 4096 ? &textures[ntextures++] : lru;
    if (t->srv) { ID3D11ShaderResourceView_Release(t->srv); ID3D11Texture2D_Release(t->tex); t->srv = NULL; }
    if (t != hit) { t->guest = g; memcpy(t->tic, tic, 32); texture_index_add(t); }
    if (watch) watch_written(g, size, &t->seen);   /* (before reading: a write meanwhile is news next frame) */
    if (d.type == TIC_3D) {   /* level 0 only, which is all a lookup table has */
        D3D11_TEXTURE3D_DESC td = {d.width, d.height, d.depth, 1, d.dxgi, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        u32 rowb = DIV_UP_(d.width, d.bw) * d.bytes, rows = DIV_UP_(d.height, d.bh);
        u8 *buf = malloc((size_t)rowb * rows * d.depth + 64);
        texture_read_3d(&d, g, buf);
        D3D11_SUBRESOURCE_DATA init = {buf, rowb, rowb * rows};
        ID3D11Texture3D *tex3;
        HRESULT hr = ID3D11Device_CreateTexture3D(dev, &td, &init, &tex3);
        free(buf);
        if (FAILED(hr)) { warn_texture(&d); return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = d.dxgi;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex3, &sd, &t->srv))) {
            ID3D11Texture3D_Release(tex3); t->srv = NULL; return NULL;
        }
        t->tex = (ID3D11Texture2D *)tex3;   /* only ever released, and skipped by captures */
        t->guest = g; t->size = size; t->watched = watch; t->stale = 0; t->asked = gfx_stat_presented;
        memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
        gfx_stat_textures++;
        return t->srv;
    }
    u32 levels = d.levels, max_levels = 1;
    while ((d.width >> max_levels) || (d.height >> max_levels)) max_levels++;
    if (levels > max_levels) levels = max_levels;
    D3D11_TEXTURE2D_DESC td = {d.width, d.height, levels, layers, d.dxgi, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                               layers == 6 || (d.type == TIC_CUBE_ARRAY) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0};
    if (d.type != TIC_CUBE && d.type != TIC_CUBE_ARRAY) td.MiscFlags = 0;
    Unpacked unpacked;
    int unpack = 0;
    if (d.bw > 1 && ((d.width % 4) || (d.height % 4))) {   /* block-compressed sizes must be multiples of 4 at level 0 */
        td.Width = (d.width + 3) & ~3u; td.Height = (d.height + 3) & ~3u;
        if (!(gfx_debug() & 128) && (unpack = unpacked_formats(d.dxgi, &unpacked))) td.Format = unpacked.bc;
    }
    D3D11_SUBRESOURCE_DATA *init = calloc(levels * layers, sizeof *init);
    u8 **mem = calloc(levels * layers, sizeof *mem);
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 w = (d.width >> l) ? d.width >> l : 1, h = (d.height >> l) ? d.height >> l : 1;
            u32 bw = (w + d.bw - 1) / d.bw, bh = (h + d.bh - 1) / d.bh;
            /* a padded texture's level can be a block wider or taller than the image's (9 wide: 12, level 1 6) */
            u32 pw = (td.Width >> l) ? td.Width >> l : 1, ph = (td.Height >> l) ? td.Height >> l : 1;
            u32 pbw = (pw + d.bw - 1) / d.bw, pbh = (ph + d.bh - 1) / d.bh;
            if (pbw < bw) pbw = bw;
            if (pbh < bh) pbh = bh;
            u8 *buf = pbw > bw || pbh > bh ? calloc(1, (size_t)pbw * pbh * d.bytes + 64) : malloc((size_t)bw * bh * d.bytes + 64);
            texture_read(&d, g, layer, l, buf);
            for (u32 y = bh; pbw > bw && y-- > 0;)   /* rows spread out to the wider pitch, from the last */
                memmove(buf + (size_t)y * pbw * d.bytes, buf + (size_t)y * bw * d.bytes, (size_t)bw * d.bytes);
            for (u32 y = 0; pbw > bw && y < bh; y++) memset(buf + ((size_t)y * pbw + bw) * d.bytes, 0, (size_t)(pbw - bw) * d.bytes);
            gfx_stat_upload_bytes += (u64)bw * bh * d.bytes;
            gfx_stat_upload_kind[3] += (u64)bw * bh * d.bytes;
            init[layer * levels + l] = (D3D11_SUBRESOURCE_DATA){buf, pbw * d.bytes, pbw * pbh * d.bytes};
            mem[layer * levels + l] = buf;
        }
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, init, &t->tex);
    for (u32 i = 0; i < levels * layers; i++) free(mem[i]);
    free(mem);
    free(init);
    if (FAILED(hr)) { warn_texture(&d); t->tex = NULL; return NULL; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = d.dxgi == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : d.dxgi;
    ID3D11Texture2D *exact = unpack ? unpack_texture(t->tex, &d, levels, layers, td.MiscFlags, &unpacked) : NULL;
    if (exact) {   /* (else the padded one, as before) */
        ID3D11Texture2D_Release(t->tex);
        t->tex = exact;
        sd.Format = unpacked.tex != DXGI_FORMAT_R8G8B8A8_TYPELESS ? unpacked.tex
                  : unpacked.read != d.dxgi ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        gfx_stat_unpacked++;
    }
    switch (shader_type) {
    case TEX_ARRAY_2D: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = levels; sd.Texture2DArray.ArraySize = layers; break;
    case TEX_CUBE: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = levels; break;
    case TEX_ARRAY_CUBE: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY; sd.TextureCubeArray.MipLevels = levels; sd.TextureCubeArray.NumCubes = layers / 6; break;
    default:
        if (layers > 1) { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = levels; sd.Texture2DArray.ArraySize = layers; }
        else { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = levels; }
    }
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->tex, &sd, &t->srv))) {
        ID3D11Texture2D_Release(t->tex); t->tex = NULL; t->srv = NULL; return NULL;
    }
    t->guest = g; t->size = size; t->watched = watch; t->stale = 0; t->asked = gfx_stat_presented;
    memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
    gfx_stat_textures++;
    return t->srv;
}

/* Samplers by their 32-byte descriptors (TSC), and state objects by their descriptions (below):
 * in hash tables, as draws look them up thousands of times a frame among hundreds kept. */
#define STATE_SLOTS 4096   /* (Direct3D's own limit of objects of a kind) */
static u32 desc_hash(const void *d, size_t size) {
    const u8 *p = d;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i + 4 <= size; i += 4) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 0x100000001B3ull; }
    return (u32)(h >> 32 ^ h);
}

typedef struct Sampler { u32 tsc[8]; ID3D11SamplerState *state; } Sampler;
static Sampler samplers[STATE_SLOTS];
static u32 nsamplers;

static D3D11_TEXTURE_ADDRESS_MODE wrap_mode(u32 m) {
    switch (m) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    }
    return D3D11_TEXTURE_ADDRESS_CLAMP;
}

static ID3D11SamplerState *get_sampler(const u32 *tsc) {
    u32 k = desc_hash(tsc, 32) & (STATE_SLOTS - 1);
    for (u32 n = 0; n < STATE_SLOTS && samplers[k].state; n++, k = (k + 1) & (STATE_SLOTS - 1))
        if (!memcmp(samplers[k].tsc, tsc, 32)) return samplers[k].state;
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3, aniso = (tsc[0] >> 20) & 7, cmp = (tsc[0] >> 9) & 1;
    int lin_mag = mag == 2, lin_min = min == 2, lin_mip = mip == 3;
    sd.Filter = (D3D11_FILTER)((lin_mip ? 1 : 0) | (lin_mag ? 4 : 0) | (lin_min ? 0x10 : 0));
    if (aniso) { sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 1u << aniso; }
    if (cmp) sd.Filter = (D3D11_FILTER)(sd.Filter | 0x80);
    sd.AddressU = wrap_mode(tsc[0] & 7); sd.AddressV = wrap_mode((tsc[0] >> 3) & 7); sd.AddressW = wrap_mode((tsc[0] >> 6) & 7);
    sd.ComparisonFunc = (D3D11_COMPARISON_FUNC)(((tsc[0] >> 10) & 7) + 1);
    i32 bias = (i32)(tsc[1] << 7) >> 19;   /* 13-bit signed, 8 fractional bits */
    sd.MipLODBias = bias / 256.0f;
    sd.MinLOD = (tsc[2] & 0xFFF) / 256.0f;
    sd.MaxLOD = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    memcpy(sd.BorderColor, &tsc[4], 16);
    ID3D11SamplerState *state = NULL;
    ID3D11Device_CreateSamplerState(dev, &sd, &state);
    if (state && nsamplers < STATE_SLOTS * 3 / 4) {   /* (k: the free slot the search ended at) */
        memcpy(samplers[k].tsc, tsc, 32);
        samplers[k].state = state;
        nsamplers++;
    }
    return state;
}

/* ---- fixed-function state ---------------------------------------------------------- */
static D3D11_COMPARISON_FUNC compare_func(u32 f) {
    if (f >= 0x200 && f <= 0x207) return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);   /* GL-style values */
    if (f >= 1 && f <= 8) return (D3D11_COMPARISON_FUNC)f;                          /* D3D-style */
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(u32 op) {
    switch (op) {
    case 1: case 0x1E00: return D3D11_STENCIL_OP_KEEP;
    case 2: case 0: return D3D11_STENCIL_OP_ZERO;
    case 3: case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 4: case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 7: case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 8: case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static D3D11_BLEND blend_factor(u32 f) {
    switch (f) {
    case 0x1: case 0x4000: return D3D11_BLEND_ZERO;
    case 0x2: case 0x4001: return D3D11_BLEND_ONE;
    case 0x3: case 0x4300: return D3D11_BLEND_SRC_COLOR;
    case 0x4: case 0x4301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x5: case 0x4302: return D3D11_BLEND_SRC_ALPHA;
    case 0x6: case 0x4303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x7: case 0x4304: return D3D11_BLEND_DEST_ALPHA;
    case 0x8: case 0x4305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x9: case 0x4306: return D3D11_BLEND_DEST_COLOR;
    case 0xA: case 0x4307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0xB: case 0x4308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0xE: case 0xC001: case 0xC003: return D3D11_BLEND_BLEND_FACTOR;
    case 0xF: case 0xC002: case 0xC004: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x10: case 0xC900: return D3D11_BLEND_SRC1_COLOR;
    case 0x11: case 0xC901: return D3D11_BLEND_INV_SRC1_COLOR;
    case 0x12: case 0xC902: return D3D11_BLEND_SRC1_ALPHA;
    case 0x13: case 0xC903: return D3D11_BLEND_INV_SRC1_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND alpha_factor(D3D11_BLEND b) {   /* colour factors are not allowed for alpha */
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return b;
    }
}

static D3D11_BLEND_OP blend_op(u32 op) {
    switch (op) {
    case 2: case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: case 0x8007: return D3D11_BLEND_OP_MIN;
    case 5: case 0x8008: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

/* State objects are cached by their descriptions (in hash tables, as the samplers above): a
 * session makes hundreds of each kind, a match many more than the menus, and every draw looks up
 * three - searched one by one, the results screen after a match spent most of its frame
 * comparing descriptions. */
#define STATE_CACHE(Name, Desc, Iface, Create)                                                 \
    static struct { Desc desc; Iface *obj; } Name##_cache[STATE_SLOTS];                            \
    static u32 Name##_count;                                                                      \
    static Iface *Name(const Desc *d) {                                                           \
        u32 k = desc_hash(d, sizeof *d) & (STATE_SLOTS - 1);                                       \
        for (u32 n = 0; n < STATE_SLOTS && Name##_cache[k].obj; n++, k = (k + 1) & (STATE_SLOTS - 1)) \
            if (!memcmp(&Name##_cache[k].desc, d, sizeof *d)) return Name##_cache[k].obj;          \
        Iface *o = NULL;                                                                          \
        Create(dev, d, &o);                                                                       \
        if (o && Name##_count < STATE_SLOTS * 3 / 4) {   /* (k: the free slot the search ended at) */ \
            Name##_cache[k].desc = *d;                                                            \
            Name##_cache[k].obj = o;                                                              \
            Name##_count++;                                                                       \
        }                                                                                         \
        return o;                                                                                 \
    }
STATE_CACHE(raster_state, D3D11_RASTERIZER_DESC, ID3D11RasterizerState, ID3D11Device_CreateRasterizerState)
STATE_CACHE(depth_state, D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState, ID3D11Device_CreateDepthStencilState)
STATE_CACHE(blend_state, D3D11_BLEND_DESC, ID3D11BlendState, ID3D11Device_CreateBlendState)

/* ---- draws ------------------------------------------------------------------------- */
u64 gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target;
/* SSBU_GFX_DEBUG bits: 1 all colour channels written, 2 no depth/stencil test, 4 no culling,
 * 8 no blending, 16 log every draw of one frame (after SSBU_GFX_LOG_AT seconds), 64 no alpha test,
 * 128 block-compressed textures left padded (no exact-size copies) */
static u32 gfx_debug(void) {
    static int checked;
    static u32 v;
    if (!checked++ && getenv("SSBU_GFX_DEBUG")) v = (u32)strtoul(getenv("SSBU_GFX_DEBUG"), NULL, 0);
    return v;
}
static int draw_logging;
static u32 instance_counter;

static D3D11_PRIMITIVE_TOPOLOGY topology(u32 t) {
    switch (t) {
    case 0: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 1: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 4: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 5: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case 10: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case 11: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    case 12: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case 13: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

/* Constant buffers of 4 KB or more stay on the GPU between draws: NVN binds 64 KB for each
 * program's own constants, and sending that again for every draw took most of the frame. They
 * are kept as the GPU would see memory: writes in the command stream (constant loads, inline
 * and copy-engine writes - the HUD's positions change between draws that way) are applied to
 * them in order, and what the CPU wrote is found by the write watch, asked at a buffer's first
 * use in each submission; memory outside the watched area is never kept. */
typedef struct KeptCb { u64 va, guest, used, checked, seen; u32 size, mapped; int stale; ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u8 *shadow; } KeptCb;
extern u64 gpu_stat_batches;   /* (gpu.c: submissions so far) */
#define KEPT_SETS 256
#define KEPT_WAYS 4
static KeptCb kept[KEPT_SETS * KEPT_WAYS];
static u64 kept_clock;
static u16 kept_blocks[1 << 16];   /* how many kept buffers touch each (hashed) 64 KB block of GPU VA */
#define KEPT_BLOCK(block) ((u32)((u64)(block) * 0x9E3779B97F4A7C15ull >> 48))
/* Command stream writes not yet applied to kept buffers: per thread (a command list runs on
 * the thread that submits it, its draws too), so extending the run needs no lock. */
static __declspec(thread) u64 pending_lo, pending_hi;
static int kept_verify = -1;   /* SSBU_KEPT_VERIFY (debugging): keep a copy of what each holds, and report draws that find memory different */
u64 gfx_stat_kept_mismatches, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_reloaded;

static void kept_blocks_add(const KeptCb *e, int d) {
    for (u64 b = e->va >> 16; b <= (e->va + e->size - 1) >> 16; b++) kept_blocks[KEPT_BLOCK(b)] += (u16)d;
}

static void kept_drop(KeptCb *e) {
    if (e->srv) kept_blocks_add(e, -1);
    bury(e->srv);   /* (may be bound still) */
    bury(e->buf);
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
        if (!e->srv || e->stale || hi <= e->va || lo >= e->va + e->mapped) continue;
        u64 a = (lo > e->va ? lo : e->va) & ~3ull, z = (hi < e->va + e->mapped ? hi : e->va + e->mapped) + 3 & ~3ull;
        /* Only from the memory it was made from, still mapped there: the guest may have mapped
         * other memory at these addresses since (and unmapped the old: the driver would fault
         * reading it). Otherwise it is taken again, or made again, at its next use. */
        u64 g = gpu_va_to_guest(a);
        if (g != e->guest + (a - e->va) || gpu_va_to_guest(z - 1) != g + (z - 1 - a)) { e->stale = 1; continue; }
        D3D11_BOX box = {(UINT)(a - e->va), 0, 0, (UINT)(z - e->va), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)e->buf, 0, &box, P(void, e->guest + (a - e->va)), 0, 0);
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

static ID3D11ShaderResourceView *kept_cb(u64 va, u32 size) {
    KeptCb *set = &kept[((va >> 4) * 0x9E3779B97F4A7C15ull >> 56) * KEPT_WAYS], *e = NULL, *lru = &set[0];
    for (u32 w = 0; w < KEPT_WAYS && !e; w++) {
        if (set[w].srv && set[w].va == va && set[w].size == size) e = &set[w];
        else if (set[w].used < lru->used) lru = &set[w];
    }
    u64 g = gpu_va_to_guest(va);
    /* (still mapped as when it was made, asked at its first use in a submission and before it
     * is taken again: a 64 KB binding reaches past the program's constants, into memory the
     * guest may have unmapped since - the driver would fault reading it) */
    if (e && e->guest == g && ((e->checked == gpu_stat_batches && !e->stale) || kept_mapped(va, g, size) == e->mapped)) {
        if (e->checked != gpu_stat_batches) {   /* first use in this submission: has the CPU written it since? */
            if (watch_written(g, e->mapped, &e->seen)) e->stale = 1;
            e->checked = gpu_stat_batches;
        }
        if (e->stale) {   /* take it all again */
            D3D11_BOX box = {0, 0, 0, e->mapped, 1, 1};
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)e->buf, 0, &box, P(void, g), 0, 0);
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
            if (gfx_stat_kept_mismatches++ < 200)
                fprintf(stderr, "[kept] va %#llx (%#x bytes) +%#x: kept %08x, memory %08x (submission %llu)\n", va, e->mapped, o,
                        ld32((u64)(e->shadow + o)), ld32(g + o), gpu_stat_batches);
        }
        return e->srv;
    }
    if (!g || size < 0x1000) return NULL;
    u32 mapped = kept_mapped(va, g, size);
    if (!watched(g, mapped)) return NULL;
    if (!e) e = lru;
    kept_drop(e);
    if (kept_verify < 0) kept_verify = getenv("SSBU_KEPT_VERIFY") != NULL;
    u32 cap = (size + 15) & ~15u;
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    watch_written(g, mapped, &e->seen);   /* (before reading: a write meanwhile is news next submission) */
    memcpy(init, P(void, g), mapped);
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &e->buf);
    free(init);
    if (FAILED(hr)) { e->buf = NULL; return NULL; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_R32_TYPELESS;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    vd.BufferEx.NumElements = cap / 4;
    vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->buf, &vd, &e->srv))) { ID3D11Buffer_Release(e->buf); e->buf = NULL; return NULL; }
    e->va = va; e->guest = g; e->size = size; e->mapped = mapped; e->used = ++kept_clock; e->checked = gpu_stat_batches;
    if (kept_verify) { e->shadow = malloc(mapped); memcpy(e->shadow, P(void, g), mapped); }
    gfx_stat_kept_made++;
    kept_blocks_add(e, 1);
    gfx_stat_upload_bytes += size;
    gfx_stat_upload_kind[0] += size;
    return e->srv;
}

/* ---- draw state, set only when it changes ---------------------------------------------
 * A draw sets the whole pipeline; most of it is what the draw before set. What the context has
 * is mirrored here, and anything else that uses the context (clears, blits, compute, captures,
 * presenting) makes the mirror unknown, so the next draw sets everything again. Binding render
 * targets makes D3D drop shader resource bindings of the same textures, so a change of render
 * targets makes the resource bindings unknown too. */
#define UNKNOWN_PTR ((void *)1)
static struct {
    ID3D11ShaderResourceView *srv[2][112];
    ID3D11SamplerState *smp[2][16];
    ID3D11RenderTargetView *rtv[8];
    ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp;
    D3D11_RECT scissor;
    ID3D11RasterizerState *rs;
    ID3D11DepthStencilState *ds;
    UINT stencil_ref;
    ID3D11BlendState *bs;
    float blend_factor[4];
    void *vs, *ps;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib;
    DXGI_FORMAT ib_format;
    int fixed_set;   /* input layout and the per-draw constant buffer bound */
} cur;

static void forget_resource_bindings(void) {
    for (u32 s = 0; s < 2; s++) {
        for (u32 i = 0; i < 112; i++) cur.srv[s][i] = UNKNOWN_PTR;
        for (u32 i = 0; i < 16; i++) cur.smp[s][i] = UNKNOWN_PTR;
    }
}

static void forget_draw_state(void) {
    forget_resource_bindings();
    for (u32 i = 0; i < 8; i++) cur.rtv[i] = UNKNOWN_PTR;
    cur.dsv = UNKNOWN_PTR;
    cur.vp.Width = -1;
    cur.scissor.right = -1;
    cur.rs = UNKNOWN_PTR; cur.ds = UNKNOWN_PTR; cur.bs = UNKNOWN_PTR;
    cur.vs = cur.ps = UNKNOWN_PTR;
    cur.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    cur.ib = UNKNOWN_PTR;
    cur.fixed_set = 0;
}

/* Bind views to a stage's slots [start, start + count): just the span that differs. */
static void set_srvs(int stage, u32 start, u32 count, ID3D11ShaderResourceView *const *v) {
    ID3D11ShaderResourceView **have = &cur.srv[stage == SHADER_VERTEX ? 0 : 1][start];
    u32 lo = count, hi = 0;
    for (u32 i = 0; i < count; i++)
        if (have[i] != v[i]) { if (lo == count) lo = i; hi = i + 1; }
    if (lo == count) return;
    memcpy(have + lo, v + lo, (hi - lo) * sizeof *v);
    if (stage == SHADER_VERTEX) ID3D11DeviceContext_VSSetShaderResources(ctx, start + lo, hi - lo, v + lo);
    else ID3D11DeviceContext_PSSetShaderResources(ctx, start + lo, hi - lo, v + lo);
}

static void set_samplers(int stage, u32 count, ID3D11SamplerState *const *v) {
    ID3D11SamplerState **have = cur.smp[stage == SHADER_VERTEX ? 0 : 1];
    u32 lo = count, hi = 0;
    for (u32 i = 0; i < count; i++)
        if (have[i] != v[i]) { if (lo == count) lo = i; hi = i + 1; }
    if (lo == count) return;
    memcpy(have + lo, v + lo, (hi - lo) * sizeof *v);
    if (stage == SHADER_VERTEX) ID3D11DeviceContext_VSSetSamplers(ctx, lo, hi - lo, v + lo);
    else ID3D11DeviceContext_PSSetSamplers(ctx, lo, hi - lo, v + lo);
}

/* Bind a stage's constant buffers and textures. */
static void bind_resources(const u32 *regs, const CbBinding cb[STAGES][18], int stage, const Shader *s, DrawParams *params) {
    int mstage = stage == SHADER_VERTEX ? STAGE_VERTEX : STAGE_FRAGMENT;
    ID3D11ShaderResourceView *srv[18] = {0};
    static u8 data[0x10000];
    (void)regs;
    for (u32 b = 0; b < 18; b++) {
        if (!(s->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[mstage][b].size;
        if (size > 0x10000) size = 0x10000;
        if (size > s->info.cbuf_extent[b]) size = s->info.cbuf_extent[b];   /* only what the program can read */
        if (size >= 0x1000 && cb[mstage][b].address && !draw_logging && (srv[b] = kept_cb(cb[mstage][b].address, size)))
            continue;   /* big ones stay on the GPU */
        if (size > gfx_stat_biggest_cb.size) {   /* (for the statistics: the largest one lately) */
            gfx_stat_biggest_cb.size = size;
            gfx_stat_biggest_cb.bank = b;
            gfx_stat_biggest_cb.extent = s->info.cbuf_extent[b];
            snprintf(gfx_stat_biggest_cb.shader, sizeof gfx_stat_biggest_cb.shader, "%s", s->name);
        }
        if (cb[mstage][b].address && size && !draw_logging) { srv[b] = upload_gpu(stage, b, cb[mstage][b].address, size); continue; }
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
        srv[b] = upload_raw(stage, b, data, size);
    }
    set_srvs(stage, 0, 18, srv);

    /* storage buffers, for global memory loads: their descriptors are in constant buffer 0 */
    if (s->info.sb_count) {
        ID3D11ShaderResourceView *gm[16] = {0};
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
            Resident *res = (va & 3) == 0 ? resident_covering(va, va + size) : NULL;   /* written by compute: the GPU's copy */
            u32 skip;
            if (res && (gm[k] = resident_view(res, va, size, &skip))) {   /* (the view may start a little before va) */
                params->sb[stage][k][0] = (u32)(va - skip);
                params->sb[stage][k][1] = (u32)((va - skip) >> 32);
                params->sb[stage][k][2] = size + skip;
                continue;
            }
            int partly = resident_overlapping_gpu(va, size) != NULL;
            if (!partly && !draw_logging) { gm[k] = upload_gpu(stage, 96 + k, va, size); continue; }
            if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
            read_gpu(va, size, gdata);
            if (partly) {   /* partly: the guest's data around the GPU's */
                if ((gm[k] = upload_composed(stage, 96 + k, va, gdata, size))) continue;
            }
            if (draw_logging) {
                const u32 *u = (const u32 *)gdata;
                for (u32 i = 0; i < 32 && i * 4 < size; i += 8)
                    fprintf(stderr, "[draw]     +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, u[i], u[i + 1], u[i + 2], u[i + 3],
                            u[i + 4], u[i + 5], u[i + 6], u[i + 7]);
            }
            gm[k] = upload_raw(stage, 96 + k, gdata, size);
        }
        set_srvs(stage, 96, 16, gm);
    }

    ID3D11ShaderResourceView *tex[32] = {0};
    ID3D11SamplerState *smp[16] = {0};
    u32 tex_cb = regs[M_BINDLESS_TEXTURE] & 0x1F;
    u64 tic_pool = (u64)regs[M_TEX_HEADER_POOL] << 32 | regs[M_TEX_HEADER_POOL + 1];
    u64 tsc_pool = (u64)regs[M_TEX_SAMPLER_POOL] << 32 | regs[M_TEX_SAMPLER_POOL + 1];
    for (u32 k = 0; k < s->info.ntex; k++) {
        u32 handle = 0, tic[8], tsc[8];
        if (tex_cb < 18 && cb[mstage][tex_cb].address) read_gpu(cb[mstage][tex_cb].address + s->info.tex[k].handle_offset, 4, &handle);
        u32 tic_index = handle & 0xFFFFF, tsc_index = regs[M_SAMPLER_BINDING] & 1 ? tic_index : (handle >> 20) & 0xFFF;
        read_gpu(tic_pool + tic_index * 32ull, 32, tic);
        read_gpu(tsc_pool + tsc_index * 32ull, 32, tsc);
        float scale;
        tex[k] = get_texture(tic, s->info.tex[k].type, &scale);
        if (k < 16) smp[k] = get_sampler(tsc);
        params->swz[stage][k] = (tic[0] >> 19) & 0xFFF;
        /* a render target's render scale, 4.12 fixed point in the top half (0: 1), for the few
         * programs that step in texels (gpu_shader.c: tsc_) */
        if (scale != 1.0f) params->swz[stage][k] |= (u32)(scale * 4096.0f + 0.5f) << 16;
        if (draw_logging) {
            TextureDesc d;
            int ok = tic_decode(tic, &d);
            int rt = 0;
            for (u32 i = 0; i < nsurfaces && !rt; i++) rt = surfaces[i].tex && surfaces[i].guest == gpu_va_to_guest(d.address);
            fprintf(stderr, "[draw]   %s tex%u: cb%u+%#x handle %#x -> tic %u tsc %u: fmt %#x %ux%u type %u layout %u va %#llx swz %#x srgb %u "
                            "wrap %u/%u filter %#x %s%s%s\n",
                    stage == SHADER_VERTEX ? "vs" : "ps", k, tex_cb, s->info.tex[k].handle_offset, handle, tic_index, tsc_index,
                    d.format, d.width, d.height, d.type, d.header_version, d.address, (tic[0] >> 19) & 0xFFF, d.srgb, tsc[0] & 7,
                    (tsc[0] >> 3) & 7, tsc[1] & 0xFF, ok ? "" : "(format unknown) ", tex[k] ? "bound" : "NOT BOUND", rt ? " (render target)" : "");
        }
    }
    if (s->info.ntex) {
        set_srvs(stage, 64, 32, tex);
        set_samplers(stage, 16, smp);
    }
}

/* SSBU_GPU_TIMES=<second>: the draws of one frame from then on timed on the GPU (a timestamp
 * before and after each), and the costliest shader pairs written to the log - what a frame asks
 * of the graphics card, draw by draw. */
#define GPU_TIMES_MAX 8192
static int gpu_times_state;   /* 0: not asked for, 1: waiting for the second, 2: timing a frame, 3: done */
static ID3D11Query *gpu_times_q[GPU_TIMES_MAX + 2], *gpu_times_disjoint;
static u32 gpu_times_n;
static struct { char vs[24], ps[24]; u32 w, h; } gpu_times_what[GPU_TIMES_MAX / 2];

static void gpu_times_before(void) {
    if (gpu_times_state != 2 || gpu_times_n + 2 > GPU_TIMES_MAX) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gpu_times_q[gpu_times_n]);
}
static void gpu_times_after(const char *vs, const char *ps, u32 w, u32 h) {
    if (gpu_times_state != 2 || gpu_times_n + 2 > GPU_TIMES_MAX) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gpu_times_q[gpu_times_n + 1]);
    u32 k = gpu_times_n / 2;
    snprintf(gpu_times_what[k].vs, sizeof gpu_times_what[k].vs, "%s", vs);
    snprintf(gpu_times_what[k].ps, sizeof gpu_times_what[k].ps, "%s", ps);
    gpu_times_what[k].w = w; gpu_times_what[k].h = h;
    gpu_times_n += 2;
}
/* (at each present) */
static void gpu_times_frame(void) {
    if (gpu_times_state == 0) {
        gpu_times_state = getenv("SSBU_GPU_TIMES") ? 1 : 3;
        if (gpu_times_state == 1) {
            D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP, 0}, dd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            for (u32 i = 0; i < GPU_TIMES_MAX + 2; i++)
                if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &gpu_times_q[i]))) gpu_times_state = 3;
            if (FAILED(ID3D11Device_CreateQuery(dev, &dd, &gpu_times_disjoint))) gpu_times_state = 3;
        }
    }
    if (gpu_times_state == 1 && rt_now_ns() > (u64)(atof(getenv("SSBU_GPU_TIMES")) * 1e9)) {
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)gpu_times_disjoint);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gpu_times_q[GPU_TIMES_MAX]);
        gpu_times_n = 0;
        gpu_times_state = 2;
        return;
    }
    if (gpu_times_state != 2) return;
    gpu_times_state = 3;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gpu_times_q[GPU_TIMES_MAX + 1]);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gpu_times_disjoint);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gpu_times_disjoint, &dj, sizeof dj, 0) == S_FALSE) Sleep(1);
    if (dj.Disjoint || !dj.Frequency) { fprintf(stderr, "[gpu times] the GPU clock changed during the frame: not timed\n"); return; }
    UINT64 *t = calloc(GPU_TIMES_MAX + 2, sizeof *t);
    for (u32 i = 0; i < gpu_times_n; i++) ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gpu_times_q[i], &t[i], sizeof t[i], 0);
    for (u32 i = GPU_TIMES_MAX; i < GPU_TIMES_MAX + 2; i++) ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gpu_times_q[i], &t[i], sizeof t[i], 0);
    double us = 1e6 / (double)dj.Frequency, total = (double)(t[GPU_TIMES_MAX + 1] - t[GPU_TIMES_MAX]) * us, drawn = 0;
    /* by shader pair and target size: count, time */
    struct { u32 first, count; double us; } *g = calloc(GPU_TIMES_MAX / 2, sizeof *g);
    u32 ng = 0;
    for (u32 k = 0; k < gpu_times_n / 2; k++) {
        double d = t[2 * k + 1] > t[2 * k] ? (double)(t[2 * k + 1] - t[2 * k]) * us : 0;
        drawn += d;
        u32 j = 0;
        for (; j < ng; j++) {
            u32 f = g[j].first;
            if (!strcmp(gpu_times_what[f].vs, gpu_times_what[k].vs) && !strcmp(gpu_times_what[f].ps, gpu_times_what[k].ps) &&
                gpu_times_what[f].w == gpu_times_what[k].w && gpu_times_what[f].h == gpu_times_what[k].h) break;
        }
        if (j == ng) g[ng++].first = k;
        g[j].count++;
        g[j].us += d;
    }
    fprintf(stderr, "[gpu times] one frame: %.0f us on the GPU from start to present, %u draws taking %.0f us; costliest:\n", total, gpu_times_n / 2, drawn);
    for (u32 shown = 0; shown < 25 && shown < ng; shown++) {
        u32 best = 0;
        for (u32 j = 1; j < ng; j++) if (g[j].us > g[best].us) best = j;
        u32 f = g[best].first;
        fprintf(stderr, "[gpu times]   %8.0f us  %4u draws  %s %s  %ux%u\n", g[best].us, g[best].count, gpu_times_what[f].vs, gpu_times_what[f].ps,
                gpu_times_what[f].w, gpu_times_what[f].h);
        g[best].us = -1;
    }
    free(g);
    free(t);
}

void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    if (!enter()) return;
    kept_flush();
    if (draw_logging) save_sampled();
    static int state_known;
    if (!state_known) { forget_draw_state(); state_known = 1; }
    rings_start_draw();
    dump_shaders(regs);
    u32 begin = regs[M_BEGIN];
    if (begin & (1u << 26)) instance_counter++;
    else if (!(begin & (1u << 27))) instance_counter = 0;

    D3D11_PRIMITIVE_TOPOLOGY topo = topology(begin & 0xFFFF);
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 vs_g = gpu_va_to_guest(region + regs[M_PIPELINE(1) + 1]);
    u64 ps_g = regs[M_PIPELINE(5)] & 1 ? gpu_va_to_guest(region + regs[M_PIPELINE(5) + 1]) : 0;
    /* the pixel shader is asked for once the vertex shader is translated (its outputs are the
     * pixel shader's inputs), not once it is compiled: both compile at the same time */
    Shader *vs_e = vs_g ? find_shader_ex(vs_g, SHADER_VERTEX, 0, cb[STAGE_VERTEX], NULL, 0, 0) : NULL;
    Shader *ps_e = vs_e && vs_e->state != 2 && ps_g ? find_shader_ex(ps_g, SHADER_PIXEL, vs_e->info.outputs, cb[STAGE_FRAGMENT], NULL, 0, 0) : NULL;
    /* The fixed-function alpha test (ui2d's alpha compare: the main menu's stencil masks are cut
     * from a texture's alpha this way) needs the pixel shader's variant that discards; while that
     * compiles, the plain one draws as before. */
    D3D11_COMPARISON_FUNC alpha_func = regs[M_ALPHA_TEST] & 1 && !(gfx_debug() & 64) ? compare_func(regs[M_ALPHA_FUNC]) : D3D11_COMPARISON_ALWAYS;
    Shader *pa_e = alpha_func != D3D11_COMPARISON_ALWAYS && ps_e && ps_e->state != 2 && (ps_e->info.color_mask & 1)
                 ? find_shader_ex(ps_g, SHADER_PIXEL, vs_e->info.outputs, cb[STAGE_FRAGMENT], NULL, 0, 1) : NULL;
    Shader *vs = vs_e && vs_e->state == 1 ? vs_e : NULL;
    Shader *ps = pa_e && pa_e->state == 1 ? pa_e : ps_e && ps_e->state == 1 ? ps_e : NULL;
    /* no fragment stage (depth only: shadow maps, depth pre-passes) draws with no pixel shader */
    if (!vs || (ps_g && !ps) || topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
        gfx_stat_skipped++;
        if (topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) gfx_skip_topology++; else gfx_skip_shader++;
        leave();
        return;
    }

    /* render targets */
    ID3D11RenderTargetView *rtv[8] = {0};
    ID3D11DepthStencilView *dsv = NULL;
    u32 sel = regs[M_CT_SELECT], count = sel & 15, rt_w = 0, rt_h = 0;
    float rt_sx = 1.0f, rt_sy = 1.0f;   /* the targets' render scale: host pixels per game pixel */
    for (u32 i = 0; i < count && i < 8; i++) {
        u32 ct = (sel >> (4 + 3 * i)) & 7;
        u64 va = (u64)regs[M_CT(ct)] << 32 | regs[M_CT(ct) + 1];
        if (!va || !regs[M_CT(ct) + 4]) continue;
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_CT(ct) + 2], regs[M_CT(ct) + 3], regs[M_CT(ct) + 4], 0);
        if (sf) { rtv[i] = sf->rtv; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = sf->sx; rt_sy = sf->sy; } }
    }
    if (regs[M_ZT_SELECT] & 1) {
        u64 va = (u64)regs[M_ZT_A] << 32 | regs[M_ZT_A + 1];
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_ZT_SIZE], regs[M_ZT_SIZE + 1], regs[M_ZT_A + 2], 1);
        if (sf) { dsv = sf->dsv; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; rt_sx = sf->sx; rt_sy = sf->sy; } }
    }
    if (!rt_w) { gfx_stat_skipped++; gfx_skip_target++; leave(); return; }
    /* (a render target bound as a texture of the same draw is unbound by D3D: fine) */
    if (memcmp(cur.rtv, rtv, sizeof rtv) || cur.dsv != dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 8, rtv, dsv);
        memcpy(cur.rtv, rtv, sizeof rtv);
        cur.dsv = dsv;
        forget_resource_bindings();
    }

    /* viewport and the clip-space fix-up */
    DrawParams params;
    memset(&params, 0, sizeof params);
    float sx = f32_of(regs[M_VIEWPORT_SCALE(0)]), sy = f32_of(regs[M_VIEWPORT_SCALE(0) + 1]), sz = f32_of(regs[M_VIEWPORT_SCALE(0) + 2]);
    float ox = f32_of(regs[M_VIEWPORT_SCALE(0) + 3]), oy = f32_of(regs[M_VIEWPORT_SCALE(0) + 4]), oz = f32_of(regs[M_VIEWPORT_SCALE(0) + 5]);
    int lower_left = regs[M_WINDOW_ORIGIN] & 1;
    if (lower_left) { oy = (float)rt_h - oy; sy = -sy; }
    /* Depth: the hardware clips z to [-w, w] or [0, w] (VIEWPORT_Z_CLIP), then maps it with
     * the viewport's z scale and offset. D3D clips to [0, w], so a [-w, w] range is halved
     * into it, and the mapping becomes the viewport's depth range. */
    int minus_one = !(regs[M_VIEWPORT_Z_CLIP] & 1);
    float zmin = minus_one ? oz - sz : oz, zmax = oz + sz;
    if (zmin < 0) zmin = 0;
    if (zmin > 1) zmin = 1;
    if (zmax < 0) zmax = 0;
    if (zmax > 1) zmax = 1;
    D3D11_VIEWPORT vp = {ox - fabsf(sx), oy - fabsf(sy), 2 * fabsf(sx), 2 * fabsf(sy), zmin, zmax};
    if (vp.Width < 1) { vp.TopLeftX = 0; vp.Width = (float)rt_w; }
    if (vp.Height < 1) { vp.TopLeftY = 0; vp.Height = (float)rt_h; }
    /* (worked out in the game's pixels, then in the targets' own) */
    vp.TopLeftX *= rt_sx; vp.Width *= rt_sx;
    vp.TopLeftY *= rt_sy; vp.Height *= rt_sy;
    if (memcmp(&cur.vp, &vp, sizeof vp)) { ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); cur.vp = vp; }
    params.fix[0] = sx < 0 ? -1.0f : 1.0f;
    params.fix[1] = sy < 0 ? 1.0f : -1.0f;
    params.fix[2] = minus_one ? 0.5f : 1.0f;
    params.fix[3] = minus_one ? 0.5f : 0.0f;
    params.frag[0] = lower_left ? 1.0f : 0.0f;
    params.frag[1] = (float)rt_h;
    /* the fragment position in the game's pixels: SV_Position times these (gpu_shader.c ps_input) */
    params.base[2] = f32_bits(1.0f / rt_sx);
    params.base[3] = f32_bits(1.0f / rt_sy);
    if (pa_e && ps == pa_e) { params.frag[2] = f32_of(alpha_test_mask(alpha_func)); params.frag[3] = f32_of(regs[M_ALPHA_REF]); }
    /* Facing: the hardware takes the signed area in window coordinates (x, y_w = scale * ndc + offset)
     * as GL does, counter-clockwise positive, and WINDOW_ORIGIN.FLIP_Y (bit 4) swaps the front face.
     * D3D judges the winding as seen on the target, which after the fix-up above is the window
     * area itself for a lower-left origin and its mirror image for an upper-left one. */
    int flipped = (!lower_left) != ((regs[M_WINDOW_ORIGIN] >> 4) & 1);

    {
        D3D11_RECT r = {0, 0, 16384, 16384};
        if (regs[M_SCISSOR(0)] & 1) {
            u32 h = regs[M_SCISSOR(0) + 1], v = regs[M_SCISSOR(0) + 2];
            r = (D3D11_RECT){(LONG)(h & 0xFFFF), (LONG)(v & 0xFFFF), (LONG)(h >> 16), (LONG)(v >> 16)};
            if (lower_left) { LONG t = (LONG)rt_h - r.bottom; r.bottom = (LONG)rt_h - r.top; r.top = t; }
            /* scaled outwards: at a fractional scale neighbouring rectangles overlap by a pixel
             * rather than leaving a gap between them */
            r = (D3D11_RECT){(LONG)floorf((float)r.left * rt_sx), (LONG)floorf((float)r.top * rt_sy),
                             (LONG)ceilf((float)r.right * rt_sx), (LONG)ceilf((float)r.bottom * rt_sy)};
        }
        if (memcmp(&cur.scissor, &r, sizeof r)) { ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r); cur.scissor = r; }
    }

    /* rasterizer, depth/stencil, blending */
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = regs[M_FILL_MODE] == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (regs[M_CULL_ENABLE] & 1) {
        u32 face = regs[M_CULL_FACE];
        rd.CullMode = face == 0x404 ? D3D11_CULL_FRONT : face == 0x405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    }
    rd.FrontCounterClockwise = (regs[M_FRONT_FACE] == 0x901) != flipped;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (gfx_debug() & 4) rd.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState *rs = raster_state(&rd);
    if (cur.rs != rs) { ID3D11DeviceContext_RSSetState(ctx, rs); cur.rs = rs; }

    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (regs[M_DEPTH_TEST] & 1) && dsv;
    dd.DepthWriteMask = regs[M_DEPTH_WRITE] & 1 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = compare_func(regs[M_DEPTH_FUNC]);
    dd.StencilEnable = (regs[M_STENCIL_TEST] & 1) && dsv;
    dd.StencilReadMask = (UINT8)regs[M_STENCIL_FUNC_MASK];
    dd.StencilWriteMask = (UINT8)regs[M_STENCIL_MASK];
    dd.FrontFace = (D3D11_DEPTH_STENCILOP_DESC){stencil_op(regs[M_STENCIL_OP_FAIL]), stencil_op(regs[M_STENCIL_OP_ZFAIL]),
                                                stencil_op(regs[M_STENCIL_OP_ZPASS]), compare_func(regs[M_STENCIL_FUNC])};
    dd.BackFace = dd.FrontFace;
    if (regs[M_TWO_SIDED_STENCIL] & 1)
        dd.BackFace = (D3D11_DEPTH_STENCILOP_DESC){stencil_op(regs[M_BACK_STENCIL_OP_FAIL]), stencil_op(regs[M_BACK_STENCIL_OP_ZFAIL]),
                                                   stencil_op(regs[M_BACK_STENCIL_OP_ZPASS]), compare_func(regs[M_BACK_STENCIL_FUNC])};
    if (gfx_debug() & 2) { dd.DepthEnable = FALSE; dd.StencilEnable = FALSE; }
    ID3D11DepthStencilState *ds = depth_state(&dd);
    if (cur.ds != ds || cur.stencil_ref != (regs[M_STENCIL_REF] & 0xFF)) {
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds, regs[M_STENCIL_REF] & 0xFF);
        cur.ds = ds;
        cur.stencil_ref = regs[M_STENCIL_REF] & 0xFF;
    }

    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < 8; i++) {
        D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[i];
        u32 mask = regs[M_CT_WRITE(i)];
        t->RenderTargetWriteMask = (UINT8)((mask & 1 ? 1 : 0) | (mask & 0x10 ? 2 : 0) | (mask & 0x100 ? 4 : 0) | (mask & 0x1000 ? 8 : 0));
        t->BlendEnable = regs[M_BLEND_ENABLE(i)] & 1;
        if (!t->BlendEnable) continue;
        /* The alpha op and factors apply as written, whatever the "separate for alpha" flag says (as in yuzu/Eden):
         * the game never sets that flag. Copying the colour ones into alpha wrote the shader's alpha where the game
         * keeps the target's - e.g. the results cards (alpha ONE/ONE) lost the alpha under Mega Man's eyes, whose
         * clap animation gives them alpha 0, and showed the card behind through them. */
        u32 base = regs[M_BLEND_STATE_PER_TARGET] & 1 ? M_BLEND_PER_TARGET(i) + 1 : 0;
        u32 cop, csrc, cdst, aop, asrc, adst;
        if (base) {
            cop = regs[base]; csrc = regs[base + 1]; cdst = regs[base + 2]; aop = regs[base + 3]; asrc = regs[base + 4]; adst = regs[base + 5];
        }
        else {
            cop = regs[M_BLEND_COLOR_OP]; csrc = regs[M_BLEND_COLOR_SRC]; cdst = regs[M_BLEND_COLOR_DST];
            aop = regs[M_BLEND_ALPHA_OP]; asrc = regs[M_BLEND_ALPHA_SRC]; adst = regs[M_BLEND_ALPHA_DST];
        }
        t->BlendOp = blend_op(cop); t->SrcBlend = blend_factor(csrc); t->DestBlend = blend_factor(cdst);
        t->BlendOpAlpha = blend_op(aop); t->SrcBlendAlpha = alpha_factor(blend_factor(asrc)); t->DestBlendAlpha = alpha_factor(blend_factor(adst));
    }
    float blend_const[4] = {f32_of(regs[M_BLEND_CONST(0)]), f32_of(regs[M_BLEND_CONST(1)]), f32_of(regs[M_BLEND_CONST(2)]), f32_of(regs[M_BLEND_CONST(3)])};
    for (u32 i = 0; i < 8; i++) {
        if (gfx_debug() & 1) bd.RenderTarget[i].RenderTargetWriteMask = 15;
        if (gfx_debug() & 8) bd.RenderTarget[i].BlendEnable = FALSE;
    }
    ID3D11BlendState *bs = blend_state(&bd);
    if (cur.bs != bs || memcmp(cur.blend_factor, blend_const, sizeof blend_const)) {
        ID3D11DeviceContext_OMSetBlendState(ctx, bs, blend_const, 0xFFFFFFFF);
        cur.bs = bs;
        memcpy(cur.blend_factor, blend_const, sizeof blend_const);
    }
    if (draw_logging)
        fprintf(stderr, "[draw] programs: region %#llx vs +%#x ps +%#x\n", region, regs[M_PIPELINE(1) + 1], regs[M_PIPELINE(5) + 1]);
    if (draw_logging)
        fprintf(stderr, "[draw] %s %s rt0 %#llx %ux%u fmt %#x (ct sel %#x) zt %d | vp %.0f,%.0f %.0fx%.0f fix %.2f %.2f %.2f %.2f | "
                "%s %u (first %u) topo %u | mask %#x blend %d | depth %d/%d func %#x | cull %d (en %#x face %#x front %#x origin %#x scale %g %g)\n",
                vs->name, ps ? ps->name : "(no ps)", gpu_va_to_guest((u64)regs[M_CT(sel >> 4 & 7)] << 32 | regs[M_CT(sel >> 4 & 7) + 1]), rt_w, rt_h, regs[M_CT(sel >> 4 & 7) + 4], sel, dsv != NULL,
                vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, params.fix[0], params.fix[1], params.fix[2], params.fix[3],
                indexed ? "indexed" : "arrays", indexed ? regs[M_DRAW_INDEX_COUNT] : regs[M_VERTEX_ARRAY_COUNT],
                indexed ? regs[M_INDEX_BUFFER + 5] : regs[M_VERTEX_ARRAY_START], begin & 0xFFFF, regs[M_CT_WRITE(0)],
                bd.RenderTarget[0].BlendEnable, dd.DepthEnable, regs[M_DEPTH_WRITE] & 1, regs[M_DEPTH_FUNC], rd.CullMode,
                regs[M_CULL_ENABLE], regs[M_CULL_FACE], regs[M_FRONT_FACE], regs[M_WINDOW_ORIGIN],
                f32_of(regs[M_VIEWPORT_SCALE(0)]), f32_of(regs[M_VIEWPORT_SCALE(0) + 1]));
    if (draw_logging) {
        const D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
        u32 sh = regs[M_SCISSOR(0) + 1], sv = regs[M_SCISSOR(0) + 2];
        fprintf(stderr, "[draw]   scissor %s x %u-%u y %u-%u | stencil %d func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "two-sided %u: back func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "blend src %d dst %d op %d, alpha src %d dst %d op %d | alpha test %u func %#x ref %g\n",
                regs[M_SCISSOR(0)] & 1 ? "on" : "off", sh & 0xFFFF, sh >> 16, sv & 0xFFFF, sv >> 16, dd.StencilEnable, regs[M_STENCIL_FUNC],
                regs[M_STENCIL_REF] & 0xFF, regs[M_STENCIL_FUNC_MASK] & 0xFF, regs[M_STENCIL_MASK] & 0xFF, regs[M_STENCIL_OP_FAIL],
                regs[M_STENCIL_OP_ZFAIL], regs[M_STENCIL_OP_ZPASS], regs[M_TWO_SIDED_STENCIL] & 1, regs[M_BACK_STENCIL_FUNC],
                regs[M_BACK_STENCIL_REF] & 0xFF, regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF, regs[M_BACK_STENCIL_MASK] & 0xFF,
                regs[M_BACK_STENCIL_OP_FAIL], regs[M_BACK_STENCIL_OP_ZFAIL], regs[M_BACK_STENCIL_OP_ZPASS], t->SrcBlend, t->DestBlend, t->BlendOp,
                t->SrcBlendAlpha, t->DestBlendAlpha, t->BlendOpAlpha, regs[M_ALPHA_TEST] & 1, regs[M_ALPHA_FUNC], f32_of(regs[M_ALPHA_REF]));
    }

    /* vertex streams: each attribute the shader reads gets its stream's data at t18 + n */
    ID3D11ShaderResourceView *vsrv[32] = {0}, *stream_srv[32] = {0};
    u32 stream_skip[32];   /* (how far before the stream its view starts: added to its attributes' offsets) */
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
        u32 off = (fmt >> 7) & 0x3FFF;
        if (stream_srv[j] && off + stream_skip[j] <= 0x3FFF) {   /* another attribute of the same stream */
            vsrv[n] = stream_srv[j];
            params.attr[n][0] = fmt + (stream_skip[j] << 7);
            continue;
        }
        /* A stream inside data the GPU keeps (compute results) is read from there; one only
         * partly inside gets the guest's data around the GPU's. */
        Resident *res = (start & 3) == 0 ? resident_covering(start, start + size) : NULL;
        if (res) {
            u32 skip;
            ID3D11ShaderResourceView *v = resident_view(res, start, (u32)size, &skip);
            if (draw_logging)
                fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) from resident %#llx + %#x\n", n, j, start, size, res->va, res->size);
            if (v && off + skip <= 0x3FFF) {   /* (a view starting before the stream: so do the offsets) */
                vsrv[n] = stream_srv[j] = v;
                stream_skip[j] = skip;
                params.attr[n][0] = fmt + (skip << 7);
                continue;
            }
        }
        stream_skip[j] = 0;
        Resident *part = resident_overlapping_gpu(start, (u32)size);
        if (!part && !draw_logging) { vsrv[n] = stream_srv[j] = upload_gpu(SHADER_VERTEX, 18 + n, start, (u32)size); continue; }
        if (size > stream_cap) { stream_data = realloc(stream_data, size + 16); stream_cap = (u32)size; }
        read_gpu(start, (u32)size, stream_data);
        vsrv[n] = stream_srv[j] = part ? upload_composed(SHADER_VERTEX, 18 + n, start, stream_data, (u32)size) : NULL;
        if (part && draw_logging)
            fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) partly in resident %#llx + %#x\n", n, j, start, size, part->va, part->size);
        if (!vsrv[n]) vsrv[n] = stream_srv[j] = upload_raw(SHADER_VERTEX, 18 + n, stream_data, (u32)size);
        if (draw_logging) {
            u32 off = (fmt >> 7) & 0x3FFF, stride = st & 0xFFF;
            float f[6] = {0};
            for (u32 v = 0; v < 3; v++)
                if (off + stride * v + 8 <= size) memcpy(&f[2 * v], stream_data + off + stride * v, 8);
            fprintf(stderr, "[draw]   attr%u: fmt %#x (stream %u, offset %u, size %#x, type %u) stride %u, %llu bytes at %#llx: v0 %g %g v1 %g %g v2 %g %g\n",
                    n, fmt, j, off, (fmt >> 21) & 0x3F, (fmt >> 27) & 7, stride, size, start,
                    f[0], f[1], f[2], f[3], f[4], f[5]);
        }
    }
    set_srvs(SHADER_VERTEX, 18, 32, vsrv);
    params.base[0] = regs[M_GLOBAL_BASE_VERTEX];
    params.base[1] = regs[M_GLOBAL_BASE_INSTANCE] + instance_counter;

    bind_resources(regs, cb, SHADER_VERTEX, vs, &params);
    if (ps) bind_resources(regs, cb, SHADER_PIXEL, ps, &params);

    /* per-draw parameters */
    if (!params_buffer) {
        D3D11_BUFFER_DESC pd = {(sizeof(DrawParams) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &pd, NULL, &params_buffer);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (!params_known || memcmp(&params_written, &params, sizeof params)) {
        params_known = 0;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)params_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &params, sizeof params);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)params_buffer, 0);
            params_written = params;
            params_known = 1;
        }
    } else gfx_stat_reused++;
    if (!cur.fixed_set) {
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &params_buffer);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &params_buffer);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        cur.fixed_set = 1;
    }
    if (cur.vs != vs->obj) { ID3D11DeviceContext_VSSetShader(ctx, (ID3D11VertexShader *)vs->obj, NULL, 0); cur.vs = vs->obj; }
    void *ps_obj = ps ? ps->obj : NULL;
    if (cur.ps != ps_obj) { ID3D11DeviceContext_PSSetShader(ctx, (ID3D11PixelShader *)ps_obj, NULL, 0); cur.ps = ps_obj; }
    /* debug: SSBU_GFX_PS_FILE (HLSL, entry main) replaces the pixel shader named by SSBU_GFX_PS_FOR */
    static const char *ps_for = (const char *)1;
    static ID3D11PixelShader *ps_override;
    if (ps_for == (const char *)1) {
        ps_for = getenv("SSBU_GFX_PS_FOR");
        FILE *f = ps_for && getenv("SSBU_GFX_PS_FILE") ? fopen(getenv("SSBU_GFX_PS_FILE"), "rb") : NULL;
        if (f) {
            static char src[65536];
            size_t n = fread(src, 1, sizeof src - 1, f);
            fclose(f);
            src[n] = 0;
            ID3DBlob *b = compile_hlsl(src, "main", "ps_5_0");
            if (b) {
                ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_override);
                ID3D10Blob_Release(b);
            }
        }
    }
    if (ps_override && ps && strstr(ps->name, ps_for)) { ID3D11DeviceContext_PSSetShader(ctx, ps_override, NULL, 0); cur.ps = ps_override; }
    static const char *magenta_only = (const char *)1;
    if (magenta_only == (const char *)1) magenta_only = getenv("SSBU_GFX_MAGENTA");   /* only draws with this pixel shader */
    if ((gfx_debug() & 32) && ps && (!magenta_only || strstr(ps->name, magenta_only))) {   /* debug: every pixel magenta, to see what geometry lands where */
        static ID3D11PixelShader *magenta;
        if (!magenta) {
            static const char src[] = "float4 main() : SV_Target { return float4(1, 0, 1, 1); }";
            ID3DBlob *b = compile_hlsl(src, "main", "ps_5_0");
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &magenta);
            ID3D10Blob_Release(b);
        }
        ID3D11DeviceContext_PSSetShader(ctx, magenta, NULL, 0);
        cur.ps = magenta;
    }
    if (cur.topo != topo) { ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo); cur.topo = topo; }

    if (indexed) {
        u32 fmt = regs[M_INDEX_BUFFER + 4], isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4, first = regs[M_INDEX_BUFFER + 5];
        u32 n = regs[M_DRAW_INDEX_COUNT];
        u64 va = ((u64)regs[M_INDEX_BUFFER] << 32 | regs[M_INDEX_BUFFER + 1]) + (u64)first * isz;
        if (!n || n > (16u << 20)) { gfx_stat_skipped++; leave(); return; }
        /* indices go one after another into a ring (no renaming per draw): the draw starts at
         * its place in it, a new pass round discards */
        u32 bytes = n * (isz == 1 ? 2 : isz);
        /* the same indices from the same place as the last indexed draw's, which the ring still
         * holds (not discarded since): used where they are */
        static struct { u64 va; u32 bytes, isz, first, gen; int ok; u64 batch, seq; u8 copy[65536]; } last_ix;
        static u32 index_gen;
        u64 ig = isz != 1 && bytes <= sizeof last_ix.copy ? gpu_va_to_guest(va) : 0;
        const u8 *isrc = ig && gpu_va_to_guest(va + bytes - 1) == ig + bytes - 1 ? P(const u8, ig) : NULL;
        int again = isrc && last_ix.ok && last_ix.va == va && last_ix.bytes == bytes && last_ix.isz == isz && last_ix.gen == index_gen &&
                    (untouched_since(last_ix.batch, last_ix.seq, ig, bytes) || !memcmp(isrc, last_ix.copy, bytes));
        if (again) { last_ix.batch = gpu_stat_batches; last_ix.seq = wrote_seq; }
        if (again) { index_first = last_ix.first; gfx_stat_reused++; }
        if (!again && bytes > index_cap) {
            if (index_buffer) bury(index_buffer);
            index_cap = 4u << 20;
            while (index_cap < bytes) index_cap *= 2;
            D3D11_BUFFER_DESC ib = {index_cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            ID3D11Device_CreateBuffer(dev, &ib, NULL, &index_buffer);
            index_pos = index_cap;   /* (discard first) */
        }
        u32 unit = isz == 4 ? 4 : 2;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (!again) {
            index_pos = (index_pos + 3) & ~3u;
            if (index_pos + bytes > index_cap) { index_pos = 0; index_gen++; how = D3D11_MAP_WRITE_DISCARD; }
            index_first = index_pos / unit;
            gfx_stat_upload_bytes += bytes;
            last_ix.ok = 0;
        }
        if (!again && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)index_buffer, 0, how, 0, &m))) {
            m.pData = (u8 *)m.pData + index_pos;
            index_pos += bytes;
            if (isrc) {   /* (kept, to compare the next draw's with) */
                memcpy(last_ix.copy, isrc, bytes);
                memcpy(m.pData, last_ix.copy, bytes);
                last_ix.va = va; last_ix.bytes = bytes; last_ix.isz = isz; last_ix.first = index_first; last_ix.gen = index_gen; last_ix.ok = 1;
                last_ix.batch = gpu_stat_batches; last_ix.seq = wrote_seq;
            } else if (isz == 1) {   /* 8-bit indices widen to 16 */
                static u8 tmp8[1 << 16];
                u32 left = n, done = 0;
                while (left) {
                    u32 k = left > sizeof tmp8 ? (u32)sizeof tmp8 : left;
                    read_gpu(va + done, k, tmp8);
                    for (u32 i = 0; i < k; i++) ((u16 *)m.pData)[done + i] = tmp8[i];
                    done += k; left -= k;
                }
            } else read_gpu(va, n * isz, m.pData);
            if (draw_logging) {
                u32 ix[6] = {0};
                for (u32 i = 0; i < 6 && i < n; i++) ix[i] = isz == 4 ? ((u32 *)m.pData)[i] : ((u16 *)m.pData)[i];
                fprintf(stderr, "[draw]   indices (%u-byte): %u %u %u %u %u %u\n", isz, ix[0], ix[1], ix[2], ix[3], ix[4], ix[5]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)index_buffer, 0);
        }
        DXGI_FORMAT ifmt = isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        if (cur.ib != index_buffer || cur.ib_format != ifmt) {
            ID3D11DeviceContext_IASetIndexBuffer(ctx, index_buffer, ifmt, 0);
            cur.ib = index_buffer;
            cur.ib_format = ifmt;
        }
    }
    /* Two-sided stencil whose back faces have their own reference or masks: D3D has one set for
     * both, so the front faces and the back faces are drawn apart, each with its own. (The UI
     * draws its masks unculled and relies on one face's write mask.) Which face is which differs
     * from culling: WINDOW_ORIGIN.FLIP_Y swaps the front face for culling (see the facing above)
     * but not for the stencil state - with it set, the hardware's "back" state is what D3D's
     * front faces get (seen on the main menu's tiles, whose fills need it). */
    u32 back_ref = regs[M_BACK_STENCIL_REF] & 0xFF, back_write = regs[M_BACK_STENCIL_MASK] & 0xFF, back_read = regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF;
    int split = dd.StencilEnable && (regs[M_TWO_SIDED_STENCIL] & 1) &&
                (back_ref != (regs[M_STENCIL_REF] & 0xFF) || back_write != dd.StencilWriteMask || back_read != dd.StencilReadMask);
    static int force_swap = -2;   /* debug: SSBU_STENCIL_SWAP=0/1 overrides */
    if (force_swap == -2) force_swap = getenv("SSBU_STENCIL_SWAP") ? atoi(getenv("SSBU_STENCIL_SWAP")) != 0 : -1;
    int swap_faces = force_swap >= 0 ? force_swap : (int)((regs[M_WINDOW_ORIGIN] >> 4) & 1);
    const Shader *first = !vs->drawn ? vs : ps && !ps->drawn ? ps : NULL;   /* (its first draw: timed) */
    u64 first_t0 = first ? rt_clock_ns() : 0;
    for (int pass = 0; pass < (split ? 2 : 1); pass++) {
        if (split) {
            int back_faces = pass == 1, back = back_faces != swap_faces;
            if (rd.CullMode == (back_faces ? D3D11_CULL_BACK : D3D11_CULL_FRONT)) continue;   /* those faces are culled anyway */
            D3D11_RASTERIZER_DESC r = rd;
            D3D11_DEPTH_STENCIL_DESC d = dd;
            r.CullMode = back_faces ? D3D11_CULL_FRONT : D3D11_CULL_BACK;   /* just this pass's faces */
            if (back) { d.StencilReadMask = (UINT8)back_read; d.StencilWriteMask = (UINT8)back_write; }
            cur.rs = raster_state(&r);
            cur.ds = depth_state(&d);
            cur.stencil_ref = back ? back_ref : regs[M_STENCIL_REF] & 0xFF;
            ID3D11DeviceContext_RSSetState(ctx, cur.rs);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, cur.ds, cur.stencil_ref);
        }
        gpu_times_before();
        if (indexed) ID3D11DeviceContext_DrawIndexed(ctx, regs[M_DRAW_INDEX_COUNT], index_first, (INT)regs[M_GLOBAL_BASE_VERTEX]);
        else ID3D11DeviceContext_Draw(ctx, regs[M_VERTEX_ARRAY_COUNT], regs[M_VERTEX_ARRAY_START]);
        gpu_times_after(vs->name, ps ? ps->name : "-", rt_w, rt_h);
    }
    if (first) {
        first_draw_done(rt_clock_ns() - first_t0, first);
        if (!vs->drawn) first_used(vs);
        if (ps && !ps->drawn) first_used(ps);
        vs->drawn = 1;
        if (ps) ps->drawn = 1;
    }
    gfx_stat_drawn++;
    leave();
}

static void save_bmp(ID3D11Texture2D *src, const char *path);

/* With SSBU_CAPTURE_SURFACES as well: every colour render target, drawn into an RGBA8
 * image first (so any format can be saved), as surf_<frame>_<guest address>.bmp. */
typedef struct Texture Texture;
static void capture_textures(const char *dir, int frame);

/* A surface drawn into an RGBA8 image (so any format can be saved) with the given blit pixel
 * shader, saved as a BMP at its host size (render scale included). Leaves the pipeline state changed. */
static void save_surface_bmp(Surface *s, ID3D11PixelShader *ps, const char *path) {
    D3D11_TEXTURE2D_DESC td = {s->hw, s->hh, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *tmp;
    ID3D11RenderTargetView *rtv;
    if (!ps || FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tmp))) return;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tmp, NULL, &rtv);
    D3D11_VIEWPORT vp = {0, 0, (float)s->hw, (float)s->hh, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11ShaderResourceView *raw = s->depth ? s->srv : surface_srv(s, 0);   /* the bytes as stored */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &raw);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    save_bmp(tmp, path);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(tmp);
}

static void capture_surfaces(const char *dir, int frame) {
    capture_textures(dir, frame);
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->depth || !s->srv) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/surf_%03d_%llx_fmt%x.bmp", dir, frame, s->guest, s->format);
        save_surface_bmp(s, blit_ps, path);
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
    if (nsampled_pending) forget_draw_state();
    for (u32 i = 0; i < nsampled_pending; i++) {
        Surface *s = sampled_pending[i];
        if (!s->tex || !s->srv) continue;
        char path[300];
        snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
        save_surface_bmp(s, blit_ps, path);
        if (!s->depth) {
            snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u_alpha.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
            save_surface_bmp(s, blit_alpha_ps, path);
        }
    }
    nsampled_pending = 0;
}

/* SSBU_CAPTURE=<dir>: save the game's picture there every few seconds (frame_N.bmp); with
 * SSBU_CAPTURE_WINDOW too, the window's back buffer as well, as fitted into it (window_N.bmp). */
static void save_bmp(ID3D11Texture2D *back, const char *path) {
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D *staging;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            u32 w = td.Width, h = td.Height, row = w * 3, pad = (4 - row % 4) % 4, data = (row + pad) * h;
            u8 hdr[54] = {'B', 'M'};
            *(u32 *)(hdr + 2) = 54 + data; *(u32 *)(hdr + 10) = 54; *(u32 *)(hdr + 14) = 40;
            *(i32 *)(hdr + 18) = (i32)w; *(i32 *)(hdr + 22) = -(i32)h; *(u16 *)(hdr + 26) = 1; *(u16 *)(hdr + 28) = 24;
            *(u32 *)(hdr + 34) = data;
            fwrite(hdr, 1, 54, f);
            u8 *line = malloc(row + pad + 4);
            for (u32 y = 0; y < h; y++) {
                const u8 *p = (const u8 *)m.pData + (size_t)y * m.RowPitch;
                for (u32 x = 0; x < w; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
                memset(line + row, 0, pad);
                fwrite(line, 1, row + pad, f);
            }
            free(line);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

static void capture(ID3D11Texture2D *back, ID3D11Texture2D *window) {
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
    save_bmp(back, path);
    if (window && getenv("SSBU_CAPTURE_WINDOW")) {
        snprintf(path, sizeof path, "%s/window_%03d.bmp", dir, count);
        save_bmp(window, path);
    }
    if (getenv("SSBU_CAPTURE_SURFACES")) capture_surfaces(dir, count);
    count++;
}

/* 2D engine: source and destination surfaces (format, layout, block size, depth, layer,
 * pitch, width, height, address) at methods 0x80 and 0x8C; the blit rectangle and the
 * 32.32 fixed-point source position and step at 0x22C-0x237. */
/* ---- compute --------------------------------------------------------------------- */
/* A launch runs its program over the grid. Global memory is the storage buffers described in
 * constant buffer 0, uploaded as writable buffers; afterwards whatever the program changed is
 * written back to guest memory - word by word against what was uploaded, so that nothing the
 * CPU wrote meanwhile elsewhere in those buffers is overwritten. (The game skins its fighters
 * this way: the draws that follow read the results as vertex data.) */
u64 gfx_stat_dispatched;
static u64 first_dispatch_ns;
typedef struct RwBuf { ID3D11Buffer *buf, *staging; ID3D11UnorderedAccessView *uav; u32 cap; u8 *before; } RwBuf;
static RwBuf rwbufs[16];

static RwBuf *rw_upload(u32 k, const u8 *data, u32 size) {
    RwBuf *b = &rwbufs[k];
    u32 need = (size + 15) & ~15u;
    gfx_stat_upload_bytes += size;
    if (need > b->cap) {
        if (b->uav) ID3D11UnorderedAccessView_Release(b->uav);
        if (b->buf) ID3D11Buffer_Release(b->buf);
        if (b->staging) ID3D11Buffer_Release(b->staging);
        b->uav = NULL; b->buf = b->staging = NULL; b->cap = 0;
        u32 cap = 4096;
        while (cap < need) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_BUFFER_DESC sd = {cap, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &b->buf)) || FAILED(ID3D11Device_CreateBuffer(dev, &sd, NULL, &b->staging)))
            return NULL;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = cap / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b->buf, &ud, &b->uav))) return NULL;
        b->before = realloc(b->before, cap);
        b->cap = cap;
    }
    memset(b->before, 0, need);
    memcpy(b->before, data, size);
    D3D11_BOX box = {0, 0, 0, need, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b->buf, 0, &box, b->before, 0, 0);
    return b;
}

/* Storage buffers a compute program only reads (the fighters' bind pose and skin weights,
 * the whole model's for every part skinned): kept on the GPU for as long as guest memory under
 * them goes unwritten (the write watch says), instead of uploaded in full for every dispatch. */
typedef struct InputBuf { u64 va, guest; u32 size; ID3D11Buffer *buf; ID3D11UnorderedAccessView *uav; u64 used, seen; u8 *shadow; } InputBuf;
static InputBuf inputs[128];
static u64 input_clock;

static void input_drop(InputBuf *b) {
    bury(b->uav);   /* (may be bound still) */
    bury(b->buf);
    free(b->shadow);
    memset(b, 0, sizeof *b);
}

static ID3D11UnorderedAccessView *input_uav(u64 va, u32 size) {
    static int verify = -1;   /* SSBU_INPUT_VERIFY (debugging): keep a copy of what each holds, and report uses that find memory different */
    if (verify < 0) verify = getenv("SSBU_INPUT_VERIFY") != NULL;
    u64 g = gpu_va_to_guest(va);
    if (!g || !size || !watched(g, size) || gpu_va_to_guest(va + size - 1) != g + size - 1) return NULL;   /* one watched piece only */
    InputBuf *b = NULL, *lru = &inputs[0];
    for (u32 i = 0; i < 128 && !b; i++) {
        if (inputs[i].buf && inputs[i].va == va && inputs[i].size == size && inputs[i].guest == g) b = &inputs[i];
        else if (inputs[i].used < lru->used) lru = &inputs[i];
    }
    u32 cap = (size + 15) & ~15u;
    if (b) {
        if (watch_written(g, size, &b->seen)) {   /* written since: take it again */
            D3D11_BOX box = {0, 0, 0, size, 1, 1};
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b->buf, 0, &box, P(void, g), 0, 0);
            gfx_stat_upload_bytes += size;
            if (b->shadow) memcpy(b->shadow, P(void, g), size);
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
        return b->uav;
    }
    b = lru;
    input_drop(b);
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    watch_written(g, size, &b->seen);   /* (before reading: a write meanwhile is news next time) */
    read_gpu(va, size, init);
    if (verify && (b->shadow = malloc(size))) memcpy(b->shadow, init, size);
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &b->buf);
    free(init);
    if (FAILED(hr)) { b->buf = NULL; return NULL; }
    gfx_stat_upload_bytes += size;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = cap / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b->buf, &ud, &b->uav))) { ID3D11Buffer_Release(b->buf); b->buf = NULL; return NULL; }
    b->va = va; b->guest = g; b->size = size; b->used = ++input_clock;
    return b->uav;
}

/* ---- memory taken away --------------------------------------------------------------------
 * Resident regions, kept constant buffers and compute inputs are made from the guest memory at
 * a GPU address, and outlive the draws that made them. When the game unmaps that address, or
 * the memory under it, the device layer says so (hle_nv.c, once the GPU thread's queued work
 * has run) and they are dropped: their next use makes them again from what is there then.
 * Before, nothing told them. A region went on checking memory that was gone - the graphics
 * driver crashed reading it on the vsync thread, in players' games after 20-40 minutes - and a
 * buffer the game mapped at the same address later read the old region's GPU data instead of
 * its own. Dropped under the lock, as the outermost entry point leaves (for these calls, their
 * own): no entry point keeps a pointer to a region or buffer across leave(), and resident_drop
 * moves regions around. What is dropped may still be bound: its views are buried. */
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
        if (e->srv && (spans(va, e->va, e->mapped) || spans(g, e->guest, e->mapped))) { kept_drop(e); gfx_stat_unmap_dropped++; }
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
    forget_draw_state();   /* (binding outputs for compute drops their bindings as inputs) */
    rings_start_draw();
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 g = gpu_va_to_guest(region + q[8]);
    u32 local[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    u32 grid[3] = {q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16};
    u32 shared = q[17] & 0x3FFFF;
    CbBinding cb[18];
    memset(cb, 0, sizeof cb);
    for (u32 i = 0; i < 8; i++)
        if (q[20] >> i & 1) { cb[i].address = (u64)(q[30 + 2 * i] & 0xFF) << 32 | q[29 + 2 * i]; cb[i].size = q[30 + 2 * i] >> 15; }
    if (!g || !grid[0] || !grid[1] || !grid[2] || !local[0]) { leave(); return; }
    Shader *cs = get_shader_ex(g, SHADER_COMPUTE, 0, cb, local, shared);
    if (!cs) { gfx_skip_shader++; leave(); return; }

    /* constant buffers */
    ID3D11ShaderResourceView *srv[18] = {0};
    static u8 data[0x10000];
    for (u32 b = 0; b < 18; b++) {
        if (!(cs->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[b].size > 0x10000 ? 0x10000 : cb[b].size;
        if (size > cs->info.cbuf_extent[b]) size = cs->info.cbuf_extent[b];
        if (cb[b].address && size) read_gpu(cb[b].address, size, data);
        else { size = 16; memset(data, 0, 16); }
        srv[b] = upload_raw(SHADER_COMPUTE, b, data, size);
    }
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 18, srv);

    /* storage buffers */
    DrawParams params;
    memset(&params, 0, sizeof params);
    if (!params_buffer) {
        D3D11_BUFFER_DESC pd = {(sizeof(DrawParams) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &pd, NULL, &params_buffer);
    }
    ID3D11UnorderedAccessView *uav[16] = {0};
    RwBuf *bound[16] = {0};
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
        if (!va || !size) continue;
        Resident *over = resident_overlapping_gpu(va, size);
        if (learned) {
            /* what it writes stays on the GPU; what it only reads is the GPU's data where there is
             * some, the guest's elsewhere */
            Resident *res = cs->written_mask >> k & 1 ? resident_acquire(va, size) : over ? resident_covering(va, va + size) : NULL;
            u32 skip;
            ResidentView *v = res ? resident_viewx(res, va, size, &skip) : NULL;
            if (v) {   /* (the view may start a little before va) */
                params.sb[0][k][0] = (u32)(va - skip);
                params.sb[0][k][1] = (u32)((va - skip) >> 32);
                params.sb[0][k][2] = size + skip;
                uav[k] = v->uav;
                continue;
            }
            if (!(cs->written_mask >> k & 1) && !over && (uav[k] = input_uav(va, size))) continue;   /* only read: cached */
        } else if (over)
            gfx_resident_flush(va, size);   /* learning diffs it all against guest memory, which needs the GPU's data first */
        if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
        read_gpu(va, size, gdata);
        bound[k] = rw_upload(k, gdata, size);
        if (bound[k] && learned && over) resident_patch(bound[k]->buf, va, size);
        if (bound[k]) { uav[k] = bound[k]->uav; va_of[k] = va; size_of[k] = size; }
    }
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 16, uav, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    params_known = 0;   /* (the next draw writes its own) */
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)params_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &params, sizeof params);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)params_buffer, 0);
    }
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &params_buffer);
    ID3D11DeviceContext_CSSetShader(ctx, (ID3D11ComputeShader *)cs->obj, NULL, 0);
    u64 first_t0 = cs->drawn ? 0 : rt_clock_ns();
    ID3D11DeviceContext_Dispatch(ctx, grid[0], grid[1], grid[2]);
    if (!cs->drawn) { first_draw_done(rt_clock_ns() - first_t0, cs); first_used(cs); cs->drawn = 1; }
    ID3D11UnorderedAccessView *none[16] = {0};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 16, none, NULL);
    gfx_stat_dispatched++;
    if (!first_dispatch_ns) first_dispatch_ns = rt_now_ns();
    if (draw_logging)
        fprintf(stderr, "[draw] dispatch %s grid %ux%ux%u, buffers %#llx+%#x %#llx+%#x %#llx+%#x (written %#x)\n", cs->name, grid[0], grid[1],
                grid[2], (u64)params.sb[0][0][1] << 32 | params.sb[0][0][0], params.sb[0][0][2], (u64)params.sb[0][1][1] << 32 | params.sb[0][1][0],
                params.sb[0][1][2], (u64)params.sb[0][2][1] << 32 | params.sb[0][2][0], params.sb[0][2][2], cs->written_mask);

    /* while learning, what it wrote goes back to guest memory; the buffers written are noted */
    if (cs->info.stores && !learned) cs->learn_runs++;
    if (cs->info.stores && !learned)
        for (u32 k = 0; k < 16; k++) {
            RwBuf *b = bound[k];
            if (!b) continue;
            u32 n = (size_of[k] + 3) & ~3u;
            D3D11_BOX box = {0, 0, 0, n, 1, 1};
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)b->staging, 0, 0, 0, 0, (ID3D11Resource *)b->buf, 0, &box);
            gfx_stat_gpu_waits++;
            if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b->staging, 0, D3D11_MAP_READ, 0, &m))) continue;
            const u32 *now = m.pData, *was = (const u32 *)b->before;
            int changed = 0;
            for (u32 i = 0; i < size_of[k] / 4; i++) {
                if (now[i] == was[i]) continue;
                changed = 1;
                u64 gg = gpu_va_to_guest(va_of[k] + 4ull * i);
                if (gg) st32(gg, now[i]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b->staging, 0);
            if (changed) {
                cs->written_mask |= 1u << k;
                gfx_resident_written(va_of[k], size_of[k]);   /* a region over it takes the new data too */
            }
        }
    leave();
}

/* A clear of only some colour channels (mask: R 1, G 2, B 4, A 8): a full-target triangle of the
 * clear colour, written through that channel mask. */
static void clear_channels(Surface *s, const float color[4], u32 mask) {
    static ID3D11PixelShader *ps;
    forget_draw_state();
    static ID3D11Buffer *cb;
    if (!ps) {
        ID3DBlob *b = compile_hlsl(BLIT_HLSL, "psclear", "ps_5_0");
        if (b) { ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps); ID3D10Blob_Release(b); }
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    }
    if (!ps || !cb || !s->rtv) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, color, 16);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
    ID3D11ShaderResourceView *none[96] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 96, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)s->hw, (float)s->hh, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RECT all = {0, 0, 16384, 16384};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &all);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, TRUE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    ID3D11DeviceContext_OMSetBlendState(ctx, blend_state(&bd), NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 2, 1, &cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

u64 gfx_stat_blits;
static ID3D11SamplerState *point_sampler;

void gfx_blit2d(const u32 *regs) {
    if (!enter()) return;
    if (draw_logging) save_sampled();
    forget_draw_state();
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
    double du = regs[0x231] + regs[0x230] / 4294967296.0, dv = regs[0x233] + regs[0x232] / 4294967296.0;
    double sx = regs[0x235] + regs[0x234] / 4294967296.0, sy = (i32)regs[0x237] + regs[0x236] / 4294967296.0;
    u32 dx = regs[0x22C], dy = regs[0x22D], dw = regs[0x22E], dh = regs[0x22F];
    if (!point_sampler) {
        D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
        ID3D11Device_CreateSamplerState(dev, &sd, &point_sampler);
    }
    /* draw the source rectangle into the destination rectangle */
    ID3D11ShaderResourceView *none[96] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 96, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst->rtv, NULL);
    /* (the rectangles are in the game's pixels; the destination's own are dst->sx, sy times those) */
    D3D11_VIEWPORT vp = {(float)dx * dst->sx, (float)dy * dst->sy, (float)dw * dst->sx, (float)dh * dst->sy, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RECT all = {0, 0, 16384, 16384};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &all);
    /* each destination pixel samples the source at src0 + (pixel - dst0) * step, in source pixels:
     * the game's, whatever either surface's render scale (the shader works in the destination's
     * host pixels, so dst0 and step are given in those, and the source is read normalized) */
    static ID3D11Buffer *rect_cb;
    if (!rect_cb) {
        D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &rect_cb);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (rect_cb && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rect_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float r[8] = {(float)sx, (float)sy, (float)(du / dst->sx), (float)(dv / dst->sy), (float)dx * dst->sx, (float)dy * dst->sy,
                      1.0f / (float)src->width, 1.0f / (float)src->height};
        memcpy(m.pData, r, sizeof r);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)rect_cb, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &rect_cb);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit2d_ps ? blit2d_ps : blit_ps, NULL, 0);
    ID3D11ShaderResourceView *in = surface_srv(src, is_srgb(dst->view));   /* read as the destination writes: bytes unchanged */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
    ID3D11SamplerState *smp = (regs[0x223] >> 4) & 1 ? linear_sampler : point_sampler;
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, none);
    leave();
}

/* How far behind the GPU is: at each present, how many earlier frames it has yet to finish
 * (event queries, polled, never waited for). Around 2 or more means the GPU is the limit;
 * near 0 means the CPU side is. */
#define FRAME_EVENTS 8
static ID3D11Query *frame_events[FRAME_EVENTS];
static int frame_event_live[FRAME_EVENTS];
static u32 frame_event_cur;
static u64 gfx_stat_inflight, gfx_stat_inflight_frames, gfx_stat_present_ticks;

static void gpu_timer_frame(void) {
    if (!frame_events[0]) {
        for (u32 i = 0; i < FRAME_EVENTS; i++) {
            D3D11_QUERY_DESC q = {D3D11_QUERY_EVENT, 0};
            ID3D11Device_CreateQuery(dev, &q, &frame_events[i]);
        }
        if (!frame_events[FRAME_EVENTS - 1]) return;
    }
    u32 behind = 0;
    for (u32 i = 0; i < FRAME_EVENTS; i++) {
        if (!frame_event_live[i]) continue;
        BOOL done = FALSE;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)frame_events[i], &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done)
            frame_event_live[i] = 0;
        else
            behind++;
    }
    gfx_stat_inflight += behind;
    gfx_stat_inflight_frames++;
    if (!frame_event_live[frame_event_cur]) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)frame_events[frame_event_cur]);
        frame_event_live[frame_event_cur] = 1;
    }
    frame_event_cur = (frame_event_cur + 1) % FRAME_EVENTS;
}

/* ---- a lost device: Windows reset the GPU (some GPU work ran past its 2-second limit), or the
 * driver failed, restarted or was updated. Every object of the device is dead from then on and
 * nothing can be drawn again, so the game stops, saying why. (Before, a player's game stopped 98
 * minutes in with "reading address 0" on the vsync thread: the back buffer's view came back NULL
 * and its Release faulted.) */
static HRESULT device_removed_reason(void) {
    static double lose_at = -1;   /* SSBU_GFX_LOSE_DEVICE_AT: the second to act as if the GPU hung (tests) */
    if (lose_at == -1) lose_at = getenv("SSBU_GFX_LOSE_DEVICE_AT") ? atof(getenv("SSBU_GFX_LOSE_DEVICE_AT")) : -2;
    if (lose_at >= 0 && rt_now_ns() > (u64)(lose_at * 1e9)) return DXGI_ERROR_DEVICE_HUNG;
    return ID3D11Device_GetDeviceRemovedReason(dev);
}

static const char *removed_why(HRESULT why) {
    switch (why) {
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG - the GPU took too long over some work, or stopped, and Windows reset it";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED - the graphics driver stopped, restarted or was updated, or the GPU went away";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET - the GPU was reset over bad work";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR - the graphics driver failed";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL - the driver refused the work it was given";
    default: return "an unknown reason";
    }
}

/* After a presenting call failed with hr: if the device is lost, the game stops here, naming the
 * programs first run in the 10 s before (newest first, with how long before). */
static void check_device(HRESULT hr, const char *doing) {
    HRESULT why = device_removed_reason();
    if (why == S_OK) return;
    char recent[400] = "none";
    u64 now = rt_clock_ns();
    size_t n = 0;
    for (u32 i = 0; i < RECENT_FIRSTS && i < recent_first_next && n < sizeof recent - 32; i++) {
        u32 k = (recent_first_next - 1 - i) % RECENT_FIRSTS;
        if (now - recent_firsts[k].ns > 10000000000ull) break;
        n += (size_t)snprintf(recent + n, sizeof recent - n, "%s%s (%.1f s)", i ? ", " : "", recent_firsts[k].name, (double)(now - recent_firsts[k].ns) / 1e9);
    }
    fprintf(stderr, "[gfx] the graphics device was lost while %s (%#lx): %s (%#lx); programs first run in the 10 s before: %s\n", doing,
            (unsigned long)hr, removed_why(why), (unsigned long)why, recent);
    rt_fatal("the graphics device was lost (%s, %#lx). Nothing can be drawn after that: start the game again.\n"
             "Programs first run in the 10 s before (GPU work that hangs is most likely theirs): %s",
             removed_why(why), (unsigned long)why, recent);
}

void watchdog_present(void);   /* debug_watchdog.c */
void hitch_present(void);      /* debug_profile.c */

void gfx_present(u64 guest, u32 width, u32 height) {
    (void)width; (void)height;
    if (!enter()) return;
    watchdog_present();
    hitch_present();
    if (draw_logging) save_sampled();
    forget_draw_state();
    gfx_stat_presented++;
    release_graves();
    note_writes();
    debug_messages();
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
    /* the window's new size (gpu_display.c), while nothing holds the back buffer */
    unsigned bw, bh;
    if (display_take_size(&bw, &bh)) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11DeviceContext_Flush(ctx);
        HRESULT hr = IDXGISwapChain1_ResizeBuffers(swapchain, 0, bw, bh, DXGI_FORMAT_UNKNOWN, swapchain_flags);
        if (FAILED(hr)) fprintf(stderr, "[gfx] cannot resize the swap chain to %ux%u (%#lx)\n", bw, bh, (unsigned long)hr);
    }
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    HRESULT hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    if (FAILED(hr) || !rtv || device_removed_reason() != S_OK) {   /* nothing to draw the window into */
        if (rtv) ID3D11RenderTargetView_Release(rtv);
        if (back) ID3D11Texture2D_Release(back);
        check_device(hr, "drawing the window");
        static int told;
        if (!told++) fprintf(stderr, "[gfx] cannot draw into the window (%#lx): frame not shown\n", (unsigned long)hr);
        leave();
        return;
    }
    D3D11_TEXTURE2D_DESC back_desc;
    ID3D11Texture2D_GetDesc(back, &back_desc);
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11ShaderResourceView *none = NULL;
    if (src) {   /* the game's picture at its own size (render scale included): pixel for pixel */
        if (src->hw != final_w || src->hh != final_h) make_final(src->hw, src->hh);
        D3D11_VIEWPORT vp = {0, 0, (float)final_w, (float)final_h, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &final_rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        /* the bytes as they are: the display shows them as sRGB, which an sRGB framebuffer's already are */
        ID3D11ShaderResourceView *raw = surface_srv(src, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &raw);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    } else {
        static const float grey[4] = {0.1f, 0.1f, 0.12f, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, final_rtv, grey);
    }
    /* then into the window: 16:9, centred, black around it */
    static const float black[4] = {0, 0, 0, 1};
    unsigned fit_x, fit_y, fit_w, fit_h;
    display_fit(back_desc.Width, back_desc.Height, &fit_x, &fit_y, &fit_w, &fit_h);
    D3D11_VIEWPORT wvp = {(float)fit_x, (float)fit_y, (float)fit_w, (float)fit_h, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11RenderTargetView *rtv_srgb = NULL;
    if ((final_w > fit_w || final_h > fit_h) && down_ps && down_cb) {
        /* a picture bigger than its place (render scale): shrunk, each window pixel the average
         * of the picture's under it, in linear light (an sRGB view of each side) */
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, &rd, &rtv_srgb);
    }
    D3D11_MAPPED_SUBRESOURCE dm;
    if (rtv_srgb && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)down_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dm))) {
        float rx = (float)final_w / (float)fit_w, ry = (float)final_h / (float)fit_h;
        float k[8] = {(float)final_w, (float)final_h, 1.0f / (float)final_w, 1.0f / (float)final_h,
                      ceilf(rx), ceilf(ry), rx, ry};   /* taps per axis; the footprint in picture pixels */
        memcpy(dm.pData, k, sizeof k);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)down_cb, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv_srgb, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &wvp);
        ID3D11DeviceContext_PSSetShader(ctx, down_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 3, 1, &down_cb);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &final_srv_srgb);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    } else {   /* as big as its place or smaller: drawn bilinear */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &wvp);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &final_srv);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    }
    if (rtv_srgb) ID3D11RenderTargetView_Release(rtv_srgb);
    capture(final_tex, back);   /* (before the overlay) */

    /* debug keys and the overlay */
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
    int latency = display_take_latency();   /* (changed in the menu) */
    if (latency) {
        IDXGIDevice1 *dxgi1;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&dxgi1))) {
            IDXGIDevice1_SetMaximumFrameLatency(dxgi1, (UINT)latency);
            IDXGIDevice1_Release(dxgi1);
        }
    }
    if (keys & 4) {   /* F3: the frame as the game drew it (without the overlay) */
        CreateDirectoryA("screenshots", NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[128];
        snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        save_bmp(final_tex, path);
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
        CreateDirectoryA("screenshots", NULL);
        snprintf(f4_dir, sizeof f4_dir, "screenshots/frame_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA(f4_dir, NULL);
        snprintf(sampled_dir, sizeof sampled_dir, "%s", f4_dir);
        nsampled_saved = nsampled_pending = 0;
        sampled_seq = 0;
        char screen[200];
        snprintf(screen, sizeof screen, "%s/screen.bmp", f4_dir);
        save_bmp(final_tex, screen);
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
                 (gfx_stat_skipped - last_skipped) / secs, gfx_stat_shaders, (long)compiles_pending, (long)prewarm_ready, (long)prewarm_total, ntextures, nsurfaces, nresidents,
                 (gfx_stat_watch_us - last_watch) / 1000.0 / secs, depth_sampling != 0 ? "on" : "off",
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
            for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) nk += kept[i].srv != NULL;
            fprintf(stderr, "[stats] kept constant buffers: %u; so far %llu made, %llu updated by the command stream, %llu taken again after CPU writes, %llu mismatches\n",
                    nk, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_reloaded, gfx_stat_kept_mismatches);
            fprintf(stderr, "[stats] memory taken away: %llu unmaps heard, %llu resident regions, kept buffers and compute inputs dropped\n",
                    gfx_stat_unmapped, gfx_stat_unmap_dropped);
            static u64 last_compared, compared_frames0;
            fprintf(stderr, "[stats] state objects: %u rasterizer, %u depth/stencil, %u blend, %u samplers; reuse checks compared %.2f MB/frame\n",
                    raster_state_count, depth_state_count, blend_state_count, nsamplers,
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
    overlay_present(dev, ctx, rtv, fit_x, fit_y, fit_w, fit_h, overlay, details ? detail : hint);
    if (menu_shot) {   /* F3 with the settings menu open: the window as it is, menu and all */
        CreateDirectoryA("screenshots", NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[128];
        snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u_menu.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        save_bmp(back, path);
        fprintf(stderr, "[gfx] screenshot: %s\n", path);
    }

    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    gpu_timer_frame();
    gpu_times_frame();
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    hr = IDXGISwapChain1_Present(swapchain, 0, display_present_flags());   /* (blocks while the GPU is frames behind) */
    QueryPerformanceCounter(&p1);
    gfx_stat_present_ticks += (u64)(p1.QuadPart - p0.QuadPart);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) check_device(hr, "showing a frame");
    leave();
}

int gfx_window_focused(void) { return window && GetForegroundWindow() == window; }
void *gfx_window_handle(void) { return window; }

/* The output (monitor) the swap chain is on, referenced; NULL before there is one (display pacing). */
void *gfx_display_output(void) {
    if (!ready || !enter()) return NULL;
    IDXGIOutput *output = NULL;
    if (FAILED(IDXGISwapChain1_GetContainingOutput(swapchain, &output))) output = NULL;
    leave();
    return output;
}

/* The plain 2D textures in the cache, drawn into RGBA8 images like the surfaces
 * (tex_<frame>_<guest address>_fmt<format>.bmp). */
static void capture_textures(const char *dir, int frame) {
    u32 saved = 0;
    const char *only = getenv("SSBU_CAPTURE_TEX_FMT");   /* just this texture format */
    for (u32 i = 0; i < ntextures && saved < 40; i++) {
        Texture *t = &textures[i];
        if (!t->srv) continue;
        if (only && (t->tic[0] & 0x7F) != strtoul(only, NULL, 0)) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd0;
        ID3D11ShaderResourceView_GetDesc(t->srv, &vd0);
        if (vd0.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D) continue;
        D3D11_TEXTURE2D_DESC src;
        ID3D11Texture2D_GetDesc(t->tex, &src);
        if (src.ArraySize != 1 || (src.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) continue;
        /* a plain 2D view of level 0, whatever view the shaders use */
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11ShaderResourceView_GetDesc(t->srv, &vd);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MostDetailedMip = 0;
        vd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView *view;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->tex, &vd, &view))) continue;
        D3D11_TEXTURE2D_DESC td = {src.Width, src.Height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *tmp;
        ID3D11RenderTargetView *rtv;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tmp))) { ID3D11ShaderResourceView_Release(view); continue; }
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tmp, NULL, &rtv);
        D3D11_VIEWPORT vp = {0, 0, (float)src.Width, (float)src.Height, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &view);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        char path[1024];
        snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, src.Width, src.Height);
        save_bmp(tmp, path);
        if (blit_alpha_ps) {   /* and its alpha */
            ID3D11DeviceContext_PSSetShader(ctx, blit_alpha_ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u_alpha.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, src.Width, src.Height);
            save_bmp(tmp, path);
        }
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        saved++;
        ID3D11ShaderResourceView_Release(view);
        ID3D11RenderTargetView_Release(rtv);
        ID3D11Texture2D_Release(tmp);
    }
}
