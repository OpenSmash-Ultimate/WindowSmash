/* Display options: display.ini (SSBU_DISPLAY=<file>, else in the current directory: the port
 * root, where every launcher starts the game), written with the defaults and their documentation
 * when it does not exist.
 *
 * The window: windowed (resizable, kept 16:9 while dragged), borderless over its monitor, or
 * "fullscreen" (borderless and topmost: no display mode change). F11 or Alt+Enter switches
 * between the window and the full-screen form and back. The game's picture is drawn 16:9,
 * centred, with black bars when the window or monitor has another shape.
 *
 * Pacing: the game sees one display refresh per frame at 60 a second (ipc_vi.c's vsync thread
 * waits in display_wait_tick).
 *   clock    a precise 60.000 Hz clock; a frame is shown at the monitor's next refresh.
 *   vrr      the same clock, and each frame goes to the screen the moment it is ready
 *            (DXGI_PRESENT_ALLOW_TEARING): with G-Sync / FreeSync it is shown for exactly
 *            16.7 ms; without, it tears.
 *   display  the monitor's own refreshes (every 1st/2nd/4th at 60/120/240 Hz): every frame is
 *            on screen equally long, but the game then runs at the monitor's real rate (59.94
 *            frames a second on a 59.94 Hz monitor). Falls back to the clock on other rates.
 * SSBU_WINDOW, SSBU_WINDOW_SIZE (WxH) and SSBU_PACING override the file (test runs).
 *
 * Linux: the window is SDL's, made and run by linux/window.c with this file's settings
 * (display_window_mode ...); borderless and fullscreen are both SDL's desktop full screen.
 * DXVK presents at once at sync interval 0 (Vulkan's immediate mode) whatever the pacing, so
 * clock and vrr differ only in the flag; display falls back to the clock (DXVK's WaitForVBlank
 * only sleeps a refresh).
 *
 * macOS: the window is SDL's too, made and run by macos/window.m; its size is in points (a
 * Retina display has 2 x 2 pixels a point), and borderless and fullscreen are both macOS's own
 * full screen, a space of its own. vrr asks the renderer to show each frame at once rather than
 * at the display's next refresh (the flag), and display waits for the display's refreshes with
 * a display link (rt_window_wait_vblank). The clock sleeps on the shim's waitable timer, as on
 * Linux: a kqueue timer marked critical (macos/platform.c's rt_sleep_until_ns), which the system
 * does not put off to wake with others (mach_wait_until's sleeps come a millisecond or more late). */
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "display.h"
#include "host.h"
#ifndef __APPLE__
#include <dxgi.h>
#endif

enum { MODE_WINDOWED = DISPLAY_WINDOWED, MODE_BORDERLESS = DISPLAY_BORDERLESS, MODE_FULLSCREEN = DISPLAY_FULLSCREEN };

static struct {
    int mode, width, height, latency, hide_cursor, overlay;
    Pacing pacing;
    int loaded;
    char gpu[64];
    float render_scale;   /* 0: auto */
} cfg = {MODE_WINDOWED, 1280, 720, 3, 1, 1, PACING_CLOCK, 0, "auto", 1.0f};

static const char DEFAULT_INI[] =
    "; display.ini: the game's window and the pacing of its frames. Read when the game starts; the\n"
    "; settings menu (Esc in the game) changes it too.\n"
    "; Delete this file to get it written again with the defaults. A line starting with ';' is a comment.\n"
    "\n"
    "; window = windowed | borderless | fullscreen\n"
#ifdef _WIN32
    ";   windowed:   a normal window, kept 16:9 while you resize it.\n"
    ";   borderless: no frame, covering the monitor the window is on.\n"
    ";   fullscreen: borderless and always on top (the monitor keeps its resolution).\n"
#elif defined __APPLE__
    ";   windowed:   a normal window, kept 16:9 while you resize it.\n"
    ";   borderless, fullscreen: macOS's full screen, a space of its own on the window's display (it keeps\n"
    ";               its resolution, and Game Mode turns on): what the window's green button does.\n"
    "; Option+Return or Ctrl+Cmd+F (also fn+F, and F11 if macOS leaves it to the game) switches between\n"
    "; the window and full screen while playing.\n"
#else
    ";   windowed:   a normal window; the picture stays 16:9 inside it, with black bars.\n"
    ";   borderless, fullscreen: the desktop's full screen on the window's monitor (it keeps its resolution).\n"
#endif
#ifndef __APPLE__
    "; F11 or Alt+Enter switches between the window and full screen while playing.\n"
#endif
    "window = windowed\n"
#ifdef _WIN32
    "; width, height: the window's inside, at 100% display scaling (scaled with Windows' setting).\n"
#elif defined __APPLE__
    "; width, height: the window's inside in points (macOS's screen units: 2 x 2 pixels on a Retina display).\n"
#else
    "; width, height: the window's inside, in the desktop's units (scaled ones on a scaled Wayland desktop).\n"
#endif
    "width = 1280\n"
    "height = 720\n"
    "\n"
    "; pacing = clock | vrr | display\n"
#ifdef _WIN32
    ";   clock:   a steady 60 frames a second; each is shown at the monitor's next refresh.\n"
    ";   vrr:     for G-Sync / FreeSync monitors: each frame is shown the moment it is ready, so every\n"
    ";            frame stays on screen exactly 16.7 ms. Best with window = borderless. Tears without VRR.\n"
    ";   display: follow the monitor's refreshes (60, 120 or 240 Hz only; else as clock). The game then\n"
    ";            runs at the monitor's exact rate, e.g. 59.94 frames a second.\n"
#elif defined __APPLE__
    ";   clock:   a steady 60 frames a second; each is shown at the display's next refresh.\n"
    ";   vrr:     the same clock, and each frame is shown the moment it is ready: in full screen on a ProMotion\n"
    ";            or Adaptive-Sync display every frame stays on screen exactly 16.7 ms (without one it may\n"
    ";            tear). A window is still shown at the desktop's refreshes.\n"
    ";   display: follow the display's refreshes (60, 120 or 240 Hz only; else as clock). The game then\n"
    ";            runs at the display's exact rate, e.g. 59.94 frames a second.\n"
#else
    ";   clock:   a steady 60 frames a second, each presented the moment it is ready. A compositing\n"
    ";            desktop shows it at its next refresh; in full screen without one it may tear\n"
    ";            (DXVK_CONFIG=\"dxgi.tearFree = True\" in the environment prevents that).\n"
    ";   vrr:     the same clock, for FreeSync / G-Sync monitors with VRR on in the desktop or driver:\n"
    ";            every frame stays on screen exactly 16.7 ms. Best with window = fullscreen.\n"
    ";   display: as clock (Linux has no wait for the monitor's refresh here).\n"
#endif
    "pacing = clock\n"
    "; latency = 1-3: how many frames the graphics card may work ahead. Lower is more responsive.\n"
    "latency = 3\n"
    "; hide_cursor = 1: hide the mouse pointer over the game after 2 seconds without moving it.\n"
    "hide_cursor = 1\n"
    "; overlay = 1: show the frame rate in the corner from the start (F1 shows or hides it while playing).\n"
    "overlay = 1\n"
    "\n"
    "; render_scale = 1 | auto | a number from 0.5 to 4\n"
    ";   The game draws its pictures at 1920x1080. render_scale draws them this many times as wide and\n"
    ";   as tall: sharper on a bigger screen, more work for the graphics card (2 = four times the pixels).\n"
#ifdef __APPLE__
    ";   auto: as many pixels as the picture has in the window when the game starts, at least 1: 1.33 for\n"
    ";   a 1280x720 window on a Retina display (2 x 2 pixels a point); in full screen, the screen's. A\n"
    ";   bigger picture than the window is shrunk to fit.\n"
#else
    ";   auto: as many pixels as the picture has in the window when the game starts (1.33 on a 2560x1440\n"
    ";   monitor with window = borderless), at least 1. A bigger picture than the window is shrunk to fit.\n"
#endif
    "render_scale = 1\n"
#ifdef _WIN32
    "\n"
    "; gpu = auto, or part of a graphics card's name (e.g. nvidia, radeon, intel)\n"
    ";   auto: the fastest card. On a laptop with two, the NVIDIA or AMD one, not the processor's own.\n"
    ";   The game's log (build\\play.log) lists the cards it found at its start.\n"
    "gpu = auto\n"
#endif
    ;

static void parse_window(const char *v) {
    if (!_stricmp(v, "windowed")) cfg.mode = MODE_WINDOWED;
    else if (!_stricmp(v, "borderless")) cfg.mode = MODE_BORDERLESS;
    else if (!_stricmp(v, "fullscreen")) cfg.mode = MODE_FULLSCREEN;
    else fprintf(stderr, "[display] window is windowed, borderless or fullscreen, not '%s'\n", v);
}

static void parse_pacing(const char *v) {
    if (!_stricmp(v, "clock")) cfg.pacing = PACING_CLOCK;
    else if (!_stricmp(v, "vrr")) cfg.pacing = PACING_VRR;
    else if (!_stricmp(v, "display")) cfg.pacing = PACING_DISPLAY;
    else fprintf(stderr, "[display] pacing is clock, vrr or display, not '%s'\n", v);
}

/* render_scale = auto, or a number from 0.5 to 4 (1: the game's own 1920x1080). */
static void parse_render_scale(const char *v) {
    char *end;
    double x = strtod(v, &end);
    if (!_stricmp(v, "auto")) cfg.render_scale = 0;
    else if (end != v && x >= 0.5 && x <= 4) cfg.render_scale = (float)x;
    else { fprintf(stderr, "[display] render_scale is auto or a number from 0.5 to 4, not '%s': 1 it is\n", v); cfg.render_scale = 1; }
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

const char *display_ini_path(void) {
    const char *env = getenv("SSBU_DISPLAY");
    return env && *env ? env : "display.ini";
}

static BOOL CALLBACK load_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    const char *env = getenv("SSBU_DISPLAY"), *path = display_ini_path();
    FILE *f = fopen(path, "r");
    if (!f && !(env && *env)) {
        FILE *w = fopen(path, "w");
        if (w) { fputs(DEFAULT_INI, w); fclose(w); fprintf(stderr, "[display] wrote the default %s\n", path); }
        f = fopen(path, "r");
    }
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq;
        if (*s == ';' || *s == '#' || *s == '[' || !(eq = strchr(s, '='))) continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        for (char *p = value; *p; p++) if ((*p == ';' || *p == '#') && p > value && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; value = trim(value); break; }
        if (!_stricmp(key, "window")) parse_window(value);
        else if (!_stricmp(key, "width")) cfg.width = atoi(value);
        else if (!_stricmp(key, "height")) cfg.height = atoi(value);
        else if (!_stricmp(key, "pacing")) parse_pacing(value);
        else if (!_stricmp(key, "latency")) cfg.latency = atoi(value);
        else if (!_stricmp(key, "hide_cursor")) cfg.hide_cursor = atoi(value) != 0;
        else if (!_stricmp(key, "overlay")) cfg.overlay = atoi(value) != 0;
        else if (!_stricmp(key, "gpu")) snprintf(cfg.gpu, sizeof cfg.gpu, "%s", value);
        else if (!_stricmp(key, "render_scale")) parse_render_scale(value);
        else fprintf(stderr, "[display] %s: unknown setting '%s'\n", path, key);
    }
    if (f) fclose(f);
    if ((env = getenv("SSBU_GPU")) && *env) snprintf(cfg.gpu, sizeof cfg.gpu, "%s", env);
    if ((env = getenv("SSBU_WINDOW")) && *env) parse_window(env);
    if ((env = getenv("SSBU_PACING")) && *env) parse_pacing(env);
    if ((env = getenv("SSBU_WINDOW_SIZE")) && *env) sscanf(env, "%dx%d", &cfg.width, &cfg.height);
    if ((env = getenv("SSBU_RENDER_SCALE")) && *env) parse_render_scale(env);
    if (cfg.width < 320) cfg.width = 320;
    if (cfg.height < 180) cfg.height = 180;
    if (cfg.latency < 1) cfg.latency = 1;
    if (cfg.latency > 3) cfg.latency = 3;
    cfg.loaded = 1;
    return TRUE;
}

static void load(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, load_once, NULL, NULL);
}

int display_latency(void) { load(); return cfg.latency; }
Pacing display_pacing(void) { load(); return cfg.pacing; }
const char *display_gpu(void) { load(); return cfg.gpu; }
float display_render_scale(void) { load(); return cfg.render_scale; }

/* (the settings menu) the render scale it saved: shown from now on, the renderer's own counts
 * only at the next start */
static float saved_render_scale;
static int render_scale_saved;
void display_set_render_scale(float scale) { saved_render_scale = scale; render_scale_saved = 1; }
int display_overlay(void) { load(); return cfg.overlay; }

/* ---- the settings menu's changes ------------------------------------------------------- */
static volatile LONG asked_mode = -1, asked_latency;

void display_settings(DisplaySettings *out) {
    load();
    *out = (DisplaySettings){cfg.mode, cfg.width, cfg.height, cfg.latency, cfg.hide_cursor, cfg.overlay, cfg.pacing, "", render_scale_saved ? saved_render_scale : cfg.render_scale};
    snprintf(out->gpu, sizeof out->gpu, "%s", cfg.gpu);
}

void display_set_pacing(Pacing pacing) { load(); cfg.pacing = pacing; }
void display_set_hide_cursor(int on) { load(); cfg.hide_cursor = on != 0; }
void display_set_latency(int frames) {
    load();
    cfg.latency = frames < 1 ? 1 : frames > 3 ? 3 : frames;
    InterlockedExchange(&asked_latency, cfg.latency);
}
int display_take_latency(void) { return (int)InterlockedExchange(&asked_latency, 0); }
int display_take_mode(void) { return (int)InterlockedExchange(&asked_mode, -1); }
void *gfx_window_handle(void);

#ifdef _WIN32
void display_dpi_aware(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *set)(DPI_AWARENESS_CONTEXT) =
        user32 ? (BOOL (WINAPI *)(DPI_AWARENESS_CONTEXT))(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
    if (set) set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
#else
void display_dpi_aware(void) {}   /* (SDL gives the window's pixels: linux/window.c) */
#endif

/* ---- the window --------------------------------------------------------------------- */
static volatile LONG pending_size;          /* client w << 16 | h, 0 = none */
#ifdef _WIN32
static volatile LONG monitor_changed = 1;
static int current_mode, hidden_cursor;
static WINDOWPLACEMENT windowed_place = {sizeof(WINDOWPLACEMENT)};
static int have_place;
static POINT last_mouse;
static ULONGLONG last_mouse_at;

static const DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW | WS_VISIBLE, FULL_STYLE = WS_POPUP | WS_VISIBLE;

/* The frame's thickness around a client area at this DPI. */
static RECT frame_of(DWORD style, UINT dpi) {
    RECT r = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    return r;
}

static RECT monitor_rect(HWND h) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

/* The configured windowed size at this window's DPI, centred on its monitor's work area. */
static RECT windowed_rect(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    int cw = MulDiv(cfg.width, (int)dpi, 96), ch = MulDiv(cfg.height, (int)dpi, 96);
    RECT f = frame_of(WINDOWED_STYLE, dpi);
    int ww = cw + f.right - f.left, wh = ch + f.bottom - f.top;
    RECT wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    return (RECT){x, y, x + ww, y + wh};
}

static void set_mode(HWND h, int mode) {
    if (mode == current_mode) return;
    if (current_mode == MODE_WINDOWED) have_place = GetWindowPlacement(h, &windowed_place);
    current_mode = mode;
    if (mode == MODE_WINDOWED) {
        SetWindowLongPtrW(h, GWL_STYLE, WINDOWED_STYLE);
        if (have_place) {
            SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            SetWindowPlacement(h, &windowed_place);
        } else {
            RECT r = windowed_rect(h);
            SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    } else {
        RECT m = monitor_rect(h);
        SetWindowLongPtrW(h, GWL_STYLE, FULL_STYLE);
        SetWindowPos(h, mode == MODE_FULLSCREEN ? HWND_TOPMOST : HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    fprintf(stderr, "[display] %s\n", mode == MODE_WINDOWED ? "windowed" : mode == MODE_BORDERLESS ? "borderless" : "fullscreen");
}

/* (the window thread) the menu's choice: the window's mode now, and F11's full-screen form */
#define WM_DISPLAY_MODE (WM_APP + 1)

void display_set_mode(int mode) {
    load();
    if (mode != MODE_WINDOWED) cfg.mode = mode;
    HWND h = (HWND)gfx_window_handle();
    if (h) PostMessageW(h, WM_DISPLAY_MODE, (WPARAM)mode, 0);
}

HWND display_create_window(HINSTANCE inst, const wchar_t *cls, const wchar_t *title) {
    load();
    /* made at the system DPI first, then sized for its own monitor's */
    HWND h = CreateWindowExW(0, cls, title, WINDOWED_STYLE & ~WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, NULL, NULL, inst, NULL);
    if (!h) return NULL;
    current_mode = MODE_WINDOWED;
    RECT r = windowed_rect(h);
    SetWindowPos(h, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (cfg.mode != MODE_WINDOWED) set_mode(h, cfg.mode);
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 500, NULL);   /* the idle pointer */
    return h;
}
#endif

int display_take_size(unsigned *w, unsigned *h) {
    LONG v = InterlockedExchange(&pending_size, 0);
    if (!v) return 0;
    *w = (unsigned)v >> 16;
    *h = (unsigned)v & 0xFFFF;
    return 1;
}

void display_fit(unsigned w, unsigned h, unsigned *x, unsigned *y, unsigned *fw, unsigned *fh) {
    if ((unsigned long long)w * 9 > (unsigned long long)h * 16) { *fh = h; *fw = h * 16 / 9; }   /* wider: bars left and right */
    else { *fw = w; *fh = w * 9 / 16; }
    if (!*fw) *fw = 1;
    if (!*fh) *fh = 1;
    *x = (w - *fw) / 2;
    *y = (h - *fh) / 2;
}

#ifdef _WIN32
int display_message(HWND h, UINT msg, WPARAM w, LPARAM l, LRESULT *result) {
    switch (msg) {
    case WM_KEYDOWN:
        if (w != VK_F11 || (l & (1 << 30))) return 0;
        set_mode(h, current_mode == MODE_WINDOWED ? (cfg.mode == MODE_FULLSCREEN ? MODE_FULLSCREEN : MODE_BORDERLESS) : MODE_WINDOWED);
        *result = 0;
        return 1;
    case WM_SYSKEYDOWN:   /* Alt+Enter */
        if (w != VK_RETURN || !(l & (1 << 29))) return 0;
        if (!(l & (1 << 30)))
            set_mode(h, current_mode == MODE_WINDOWED ? (cfg.mode == MODE_FULLSCREEN ? MODE_FULLSCREEN : MODE_BORDERLESS) : MODE_WINDOWED);
        *result = 0;
        return 1;
    case WM_DISPLAY_MODE:
        set_mode(h, (int)w);
        *result = 0;
        return 1;
    case WM_MENUCHAR:     /* (no "ding" for Alt+key) */
        *result = MAKELRESULT(0, MNC_CLOSE);
        return 1;
    case WM_SIZING: {     /* a windowed frame keeps the picture's shape */
        if (current_mode != MODE_WINDOWED) return 0;
        RECT *r = (RECT *)l, f = frame_of((DWORD)GetWindowLongPtrW(h, GWL_STYLE), GetDpiForWindow(h));
        int fw = f.right - f.left, fh = f.bottom - f.top;
        int cw = r->right - r->left - fw, ch = r->bottom - r->top - fh;
        if (w == WMSZ_TOP || w == WMSZ_BOTTOM) cw = ch * 16 / 9;
        else ch = cw * 9 / 16;
        if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) r->top = r->bottom - ch - fh;
        else r->bottom = r->top + ch + fh;
        if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT) r->left = r->right - cw - fw;
        else r->right = r->left + cw + fw;
        *result = TRUE;
        return 1;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        UINT dpi = GetDpiForWindow(h);
        RECT f = frame_of(WINDOWED_STYLE, dpi ? dpi : 96);
        mm->ptMinTrackSize.x = MulDiv(640, (int)(dpi ? dpi : 96), 96) + f.right - f.left;
        mm->ptMinTrackSize.y = MulDiv(360, (int)(dpi ? dpi : 96), 96) + f.bottom - f.top;
        *result = 0;
        return 1;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)l;
        if (current_mode == MODE_WINDOWED)
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        *result = 0;
        return 1;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)) InterlockedExchange(&pending_size, (LONG)((u32)LOWORD(l) << 16 | HIWORD(l)));
        return 0;
    case WM_MOVE:
    case WM_DISPLAYCHANGE:
        InterlockedExchange(&monitor_changed, 1);
        if (msg == WM_DISPLAYCHANGE && current_mode != MODE_WINDOWED) {   /* the monitor's resolution changed */
            RECT m = monitor_rect(h);
            SetWindowPos(h, NULL, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        if (p.x != last_mouse.x || p.y != last_mouse.y) {
            last_mouse = p;
            last_mouse_at = GetTickCount64();
            if (hidden_cursor) { hidden_cursor = 0; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && hidden_cursor) { SetCursor(NULL); *result = TRUE; return 1; }
        return 0;
    case WM_TIMER:
        if (w != 1) return 0;
        if (cfg.hide_cursor && !hidden_cursor && GetTickCount64() - last_mouse_at > 2000) {
            POINT p;
            GetCursorPos(&p);
            if (WindowFromPoint(p) == h && GetForegroundWindow() == h) {
                RECT c;
                GetClientRect(h, &c);
                ScreenToClient(h, &p);
                if (PtInRect(&c, p)) { hidden_cursor = 1; SetCursor(NULL); }
            }
        }
        *result = 0;
        return 1;
    }
    return 0;
}
#else
/* Linux and macOS: linux/window.c or macos/window.m makes the window with SDL, on the main
 * thread, and asks here. */
static volatile LONG refresh_hz;

int display_window_mode(void) { load(); return cfg.mode; }
void display_set_mode(int mode) {   /* (the window's loop takes it: display_take_mode) */
    load();
    if (mode != MODE_WINDOWED) cfg.mode = mode;
    InterlockedExchange(&asked_mode, mode);
}
void display_window_size(int *width, int *height) { load(); *width = cfg.width; *height = cfg.height; }
int display_hide_cursor(void) { load(); return cfg.hide_cursor; }
void display_note_size(unsigned w, unsigned h) {
    if (w && h && w < 0x8000 && h < 0x10000) InterlockedExchange(&pending_size, (LONG)(w << 16 | h));
}
void display_note_refresh(int hz) { InterlockedExchange(&refresh_hz, hz); }
#endif

/* ---- pacing ----------------------------------------------------------------------------- */
static int tearing;

void display_set_tearing(int supported) { tearing = supported; }

unsigned display_present_flags(void) {
    load();
    return cfg.pacing == PACING_VRR && tearing ? 0x200u /* DXGI_PRESENT_ALLOW_TEARING */ : 0;
}

void *gfx_display_output(void);   /* gpu_d3d11.c: the swap chain's output (IDXGIOutput, referenced), or NULL */

/* The refresh rate of the window's monitor, in Hz (0 unknown). */
#ifdef _WIN32
static int monitor_hz(void) {
    HWND h = (HWND)gfx_window_handle();
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!h || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) ||
        !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return 0;
    return (int)dm.dmDisplayFrequency;
}

/* `display` pacing: the next vblank the game should see, or 0 to use the clock this time. */
static int vblank_tick(void) {
    static IDXGIOutput *output;
    static int divisor, failed;
    if (failed) return 0;
    if (InterlockedExchange(&monitor_changed, 0) || !output) {
        if (output) IDXGIOutput_Release(output);
        output = (IDXGIOutput *)gfx_display_output();
        int hz = monitor_hz();
        divisor = hz >= 58 && hz <= 61 ? 1 : hz >= 118 && hz <= 121 ? 2 : hz >= 238 && hz <= 241 ? 4 : 0;
        static int logged_hz = -1;
        if (hz != logged_hz && output) {
            logged_hz = hz;
            fprintf(stderr, "[display] %d Hz monitor: %s\n", hz, divisor ? "display pacing" : "clock pacing (display pacing needs 60, 120 or 240 Hz)");
        }
    }
    if (!output || !divisor) return 0;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < divisor; i++)
        if (FAILED(IDXGIOutput_WaitForVBlank(output))) { failed = 1; break; }
    QueryPerformanceCounter(&t1);
    if (!failed && (t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart > 50 * divisor) failed = 1;
    if (failed) fprintf(stderr, "[display] waiting for the monitor's refresh failed: clock pacing from now on\n");
    return !failed;
}
#elif defined __APPLE__
int rt_window_wait_vblank(int n, int timeout_ms);   /* (macos/window.m) */

static int monitor_hz(void) { return (int)refresh_hz; }

/* `display` pacing: the next refresh the game should see - every 1st/2nd/4th of a 60/120/240 Hz
 * display's (a display link's) - or 0 to use the clock this time: at other rates, and for a
 * second after the display stopped refreshing (asleep). */
static int vblank_tick(void) {
    static int logged_hz = -1, quiet;
    static ULONGLONG retry_at;
    int hz = monitor_hz(), divisor = hz >= 58 && hz <= 61 ? 1 : hz >= 118 && hz <= 121 ? 2 : hz >= 238 && hz <= 241 ? 4 : 0;
    if (hz != logged_hz) {
        logged_hz = hz;
        fprintf(stderr, "[display] %d Hz display: %s\n", hz, divisor ? "display pacing" : "clock pacing (display pacing needs 60, 120 or 240 Hz)");
    }
    if (!divisor || GetTickCount64() < retry_at) return 0;
    if (rt_window_wait_vblank(divisor, 3000 * divisor / hz + 5)) return 1;   /* (3 refreshes' time) */
    if (!quiet++) fprintf(stderr, "[display] the display did not refresh: clock pacing until it does\n");
    retry_at = GetTickCount64() + 1000;
    return 0;
}
#else
static int monitor_hz(void) { return (int)refresh_hz; }

static int vblank_tick(void) {
    static int logged;
    if (!logged++) fprintf(stderr, "[display] display pacing: no wait for the monitor's refresh on Linux, clock pacing instead\n");
    return 0;
}
#endif

void display_wait_tick(void) {
    static LARGE_INTEGER freq, base;
    static long long ticks;
    static HANDLE timer;
    static int logged;
    load();
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&base);
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) { timeBeginPeriod(1); timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); }
    }
    if (!logged && gfx_window_handle()) {
        logged = 1;
        int hz = monitor_hz();
        fprintf(stderr, "[display] %d Hz monitor, %s pacing%s\n", hz,
                cfg.pacing == PACING_VRR ? "vrr" : cfg.pacing == PACING_DISPLAY ? "display" : "clock",
                cfg.pacing == PACING_VRR && !tearing ? " (no tearing support: as clock)" : "");
    }
    if (cfg.pacing == PACING_DISPLAY && vblank_tick()) {   /* the clock restarts from here if it is needed later */
        QueryPerformanceCounter(&base);
        ticks = 0;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long period = freq.QuadPart / 60;
    if (now.QuadPart - (base.QuadPart + ticks * freq.QuadPart / 60) > 2 * period) {   /* late by two refreshes: skip, don't burst */
        base = now;
        ticks = 0;
    }
    ticks++;
    long long due = base.QuadPart + ticks * freq.QuadPart / 60;
    for (;;) {
        QueryPerformanceCounter(&now);
        long long left = due - now.QuadPart;
        if (left <= 0) break;
        long long us = left * 1000000 / freq.QuadPart;
        if (us > 700 && timer) {
            LARGE_INTEGER wait = {.QuadPart = -(us - 500) * 10};   /* to half a millisecond before */
            if (SetWaitableTimer(timer, &wait, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, 100); continue; }
        }
        YieldProcessor();
    }
}
