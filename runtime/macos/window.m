/* The game's window on macOS: SDL3, on the process's main thread, drawn into with Metal.
 *
 * As on Linux (linux/window.c), the main thread makes the window at start-up and then runs its
 * events for good, keeping what the rest of the runtime asks for up to date: the keyboard
 * (behind GetAsyncKeyState), focus, the debug keys, the settings menu's keys and typed letters
 * (gpu_menu.c) and the window's close button (and Cmd+Q).
 * The renderer draws into the window's CAMetalLayer, made here (rt_metal_layer): BGRA8 in sRGB,
 * at the window's size in pixels - 2 x 2 a point on a Retina display - which this file keeps up
 * to date as the window changes size or display, and reports (display_note_size).
 *
 * SDL starts here with video, events and audio only. Game controllers are hle_pad.c's: its
 * input thread starts SDL's gamepads and makes every call to them (IOKit's pads are tied to the
 * thread that started them). The pads of GameController.framework (Xbox and MFi pads over
 * Bluetooth) come and go through notifications on the main thread, which this loop runs.
 *
 * display.ini (gpu_display.c) sizes the window, in points, and picks its mode: windowed (kept
 * 16:9 while resized), or macOS's own full screen - a space of its own; the display keeps its
 * mode, and Game Mode turns on - for borderless and fullscreen alike. F11 or Option+Return
 * switches between the two, as F11 and Alt+Enter do on Windows; so do the Window menu's Toggle
 * Full Screen (Ctrl+Cmd+F, fn+F) and the window's green button, which are macOS's. The debug keys
 * are F1-F6 (with fn on most Mac keyboards) and Cmd+1 - Cmd+6. The pointer hides after 2 s
 * without moving (hide_cursor). App Nap is kept off while the game runs (it would slow the
 * game's timers down whenever the window is hidden). Two more things for the rest of the
 * runtime: waiting for the display's refreshes (display pacing: rt_window_wait_vblank) and
 * asking for a line of text over the window (the software keyboard: rt_window_ask_text). */
#import <AppKit/AppKit.h>
#import <CoreVideo/CVDisplayLink.h>
#import <Metal/MTLPixelFormat.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "../display.h"
#include "../gpu_menu.h"

/* (the Win32 shim's, runtime/linux/include/windows.h, whose BOOL is not Objective-C's) */
__attribute__((noreturn)) void ExitProcess(unsigned code);
enum { VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
       VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
       VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_F1 = 0x70 };

void gfx_hotkey(int n);   /* (the renderer) F1 + n, or Cmd+1 + n, was pressed */
void rt_upnp_remove(void);   /* (linux/upnp.c: the netplay relay's port mapping, if it made one) */

static SDL_Window *window;
static NSWindow *nswindow;          /* (SDL's, for the display it is on) */
static SDL_MetalView view;
static CAMetalLayer *layer;
static volatile int focused;
static const bool *keys;
static int nkeys;
static int full;                    /* full screen, or on the way there */
static int menu_full_screen;        /* Ctrl+Cmd+F is a menu's (SDL's Window menu): see key_down */
static int cursor_hidden;
static Uint64 mouse_moved_at;
static id activity;                 /* (App Nap stays off while this is held: for good) */
static volatile CGDirectDisplayID display_id;   /* the window's display, for display pacing */

/* ---- display.ini's window ----------------------------------------------------------------- */
/* The window's inside in pixels: the layer's drawables are made that size, and the renderer's
 * swap chain follows it (display.h). */
static void note_size(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    float density = SDL_GetWindowPixelDensity(window);   /* (pixels a point: the backing scale) */
    if (w <= 0 || h <= 0) return;
    if (density > 0) layer.contentsScale = density;
    layer.drawableSize = CGSizeMake(w, h);
    display_note_size((unsigned)w, (unsigned)h);
}

/* The refresh rate of the window's display (and which display it is: display pacing). */
static void note_refresh(void) {
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    int hz = dm && dm->refresh_rate > 0 ? (int)lroundf(dm->refresh_rate) : 0;
    NSScreen *screen = nswindow.screen;
    if (!hz && screen) hz = (int)screen.maximumFramesPerSecond;
    display_note_refresh(hz);
    NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
    if (number) display_id = number.unsignedIntValue;
}

/* (SDL's full screen is macOS's, a space of its own: it takes a moment, and the window's events
 * say when it is done - also when the menu or the green button switched) */
static void set_full(int on) {
    if (on == full) return;
    if (!SDL_SetWindowFullscreen(window, on)) fprintf(stderr, "[display] cannot switch to %s: %s\n", on ? "full screen" : "the window", SDL_GetError());
    else full = on;
}

static void show_pointer(void) {
    if (cursor_hidden) { SDL_ShowCursor(); cursor_hidden = 0; }
}

/* (every turn of the event loop) the pointer hides over the focused window after 2 s still */
static void idle_pointer(Uint64 now) {
    if (display_hide_cursor() && !cursor_hidden && focused && SDL_GetMouseFocus() == window && now - mouse_moved_at > 2000) {
        SDL_HideCursor();
        cursor_hidden = 1;
    }
}

/* Whether a menu has Toggle Full Screen on Ctrl+Cmd+F, as SDL's Window menu does: AppKit then
 * takes that key itself, besides passing it on to SDL. */
static int menu_toggles_full_screen(NSMenu *m) {
    const NSEventModifierFlags both = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    for (NSMenuItem *it in m.itemArray) {
        if (it.action == @selector(toggleFullScreen:) && [it.keyEquivalent isEqualToString:@"f"] && (it.keyEquivalentModifierMask & both) == both) return 1;
        if (it.submenu && menu_toggles_full_screen(it.submenu)) return 1;
    }
    return 0;
}

/* ---- the keyboard --------------------------------------------------------------------------- */
static SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F1 + 11) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_RETURN: return SDL_SCANCODE_RETURN;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;   /* (the Mac's delete key) */
    case VK_DELETE: return SDL_SCANCODE_DELETE;    /* (fn+delete) */
    case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;
    case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_UP: return SDL_SCANCODE_UP;
    case VK_DOWN: return SDL_SCANCODE_DOWN;
    case VK_SHIFT: return SDL_SCANCODE_LSHIFT;
    case VK_CONTROL: return SDL_SCANCODE_LCTRL;
    case VK_MENU: return SDL_SCANCODE_LALT;        /* (Option) */
    case VK_LWIN: return SDL_SCANCODE_LGUI;        /* (Command) */
    case VK_RWIN: return SDL_SCANCODE_RGUI;
    }
    return SDL_SCANCODE_UNKNOWN;
}

short GetAsyncKeyState(int vk) {
    if (!keys) return 0;
    SDL_Scancode s = scancode_of(vk);
    int down = s != SDL_SCANCODE_UNKNOWN && (int)s < nkeys && keys[s];
    if (vk == VK_RETURN) down |= keys[SDL_SCANCODE_KP_ENTER];   /* (as on Windows: Enter on a keypad too) */
    if (vk == VK_SHIFT) down |= keys[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) down |= keys[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU) down |= keys[SDL_SCANCODE_RALT];
    return down ? (short)0x8000 : 0;
}

void *GetForegroundWindow(void) { return focused ? (void *)window : NULL; }
void *rt_window(void) { return window; }
void *rt_metal_layer(void) { return (__bridge void *)layer; }

/* The debug keys and the full-screen switch. Ctrl+Cmd+F is left to the Window menu when it has
 * it (AppKit switches through the menu whatever SDL is told: here too, it would switch back). */
static void key_down(const SDL_KeyboardEvent *k) {
    SDL_Scancode s = k->scancode;
    int cmd = (k->mod & SDL_KMOD_GUI) != 0, ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F6) gfx_hotkey(s - SDL_SCANCODE_F1);
    else if (cmd && !ctrl && s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_6) gfx_hotkey(s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_F11 || ((s == SDL_SCANCODE_RETURN || s == SDL_SCANCODE_KP_ENTER) && (k->mod & SDL_KMOD_ALT)) ||
        (s == SDL_SCANCODE_F && cmd && ctrl && !menu_full_screen))
        set_full(!full);
}

/* The settings menu's keys (gpu_menu.c): Esc always, the others while it is open. (Delete, the
 * Mac's backspace, is SDL's backspace; fn+Delete its delete.) */
static void menu_key_of(const SDL_KeyboardEvent *k) {
    int key = 0;
    switch (k->scancode) {
    case SDL_SCANCODE_ESCAPE: if (!k->repeat) key = MENU_KEY_ESC; break;
    case SDL_SCANCODE_UP: key = MENU_KEY_UP; break;
    case SDL_SCANCODE_DOWN: key = MENU_KEY_DOWN; break;
    case SDL_SCANCODE_LEFT: key = MENU_KEY_LEFT; break;
    case SDL_SCANCODE_RIGHT: key = MENU_KEY_RIGHT; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: if (!k->repeat && !(k->mod & SDL_KMOD_ALT)) key = MENU_KEY_ENTER; break;
    case SDL_SCANCODE_BACKSPACE: key = MENU_KEY_BACKSPACE; break;
    case SDL_SCANCODE_DELETE: key = MENU_KEY_DELETE; break;
    case SDL_SCANCODE_TAB: if (!k->repeat) key = k->mod & SDL_KMOD_SHIFT ? MENU_KEY_SHIFT_TAB : MENU_KEY_TAB; break;
    default: break;
    }
    if (key && (key == MENU_KEY_ESC || menu_is_open())) menu_key(key);
}

/* Typed letters for the menu: SDL3 sends text only while text input is on, so it is on while
 * the menu is open (not while playing: the input method would take keys such as a dead key). */
static void menu_text_input(void) {
    static int on;
    int want = menu_is_open();
    if (want == on || !window) return;
    if (want ? SDL_StartTextInput(window) : SDL_StopTextInput(window)) on = want;
}

/* ---- start-up and the event loop ------------------------------------------------------------ */
int rt_window_init(void) {
    if (!pthread_main_np()) { fprintf(stderr, "[gfx] the window must be made on the main thread\n"); return 0; }
    activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityLatencyCritical | NSActivityUserInitiated
                                                              reason:@"Super Smash Bros. Ultimate runs in real time"];
    SDL_SetHint(SDL_HINT_APP_NAME, "Super Smash Bros. Ultimate");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* (controllers.ini's background = ... decides) */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");              /* hle_pad.c's input thread starts and reads the pads */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");        /* full screen in a space of its own: macOS's */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[gfx] SDL could not start the video system: %s\n", SDL_GetError());
        return 0;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) fprintf(stderr, "[audio] SDL could not start audio: %s\n", SDL_GetError());
    int width, height;
    display_window_size(&width, &height);
    SDL_Rect usable;   /* (the whole window on the screen it opens on, title bar and all) */
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 640 && usable.h > 400) {
        double fit = fmin((usable.w - 16.0) / width, (usable.h - 48.0) / height);
        if (fit < 1) {
            fprintf(stderr, "[display] a %dx%d window is too big for the screen: %dx%d\n", width, height, (int)(width * fit), (int)(height * fit));
            width = (int)(width * fit);
            height = (int)(height * fit);
        }
    }
    window = SDL_CreateWindow("Super Smash Bros. Ultimate (native)", width, height,
                              SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "[gfx] cannot create the window: %s\n", SDL_GetError());
        return 0;
    }
    view = SDL_Metal_CreateView(window);
    layer = view ? (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view) : nil;
    if (!layer) {
        fprintf(stderr, "[gfx] cannot give the window a Metal layer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        return 0;
    }
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer.colorspace = srgb;   /* (the picture is sRGB: macOS converts it for the display) */
    CGColorSpaceRelease(srgb);
    layer.framebufferOnly = NO;
    layer.opaque = YES;
    nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    SDL_SetWindowMinimumSize(window, 640, 360);
    SDL_SetWindowAspectRatio(window, 16.0f / 9, 16.0f / 9);   /* (a window keeps the picture's shape while resized) */
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (display_window_mode() != DISPLAY_WINDOWED) set_full(1);
    SDL_ShowWindow(window);
    [NSApp activate];   /* (started from a terminal, the game comes to the front all the same) */
    mouse_moved_at = SDL_GetTicks();
    note_size();
    note_refresh();
    keys = SDL_GetKeyboardState(&nkeys);
    focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    menu_full_screen = menu_toggles_full_screen(NSApp.mainMenu);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    fprintf(stderr, "[gfx] window ready (SDL %s video driver, %dx%d points, %dx%d pixels)\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?", width, height, pw, ph);
    return 1;
}

/* The router's port mapping goes as the game quits, on a thread of its own: waited for 3 s at
 * most (a router slow to answer takes that again after connecting), so the window closes. */
static void upnp_remove_bounded(void) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        rt_upnp_remove();
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * (int64_t)NSEC_PER_SEC));
}

/* Runs on the main thread for good. `status` is called every `every` seconds (0: never). */
_Noreturn void rt_window_loop(int every, void (*status)(void)) {
    Uint64 last = SDL_GetTicks();
    for (;;) {
        SDL_Event e;
        int got = 0;
        if (window) got = SDL_WaitEventTimeout(&e, 8);
        else CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);   /* (no window: the main queue's work still runs) */
        while (got) {
            switch (e.type) {
            case SDL_EVENT_QUIT:   /* (the close button, Cmd+Q) */
                fprintf(stderr, "[gfx] window closed\n");
                upnp_remove_bounded();   /* (ExitProcess runs no atexit handlers: as svcExitProcess does) */
                ExitProcess(0);
            case SDL_EVENT_WINDOW_FOCUS_GAINED: focused = 1; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: focused = 0; show_pointer(); break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: note_size(); break;
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: note_refresh(); break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: full = 1; fprintf(stderr, "[display] full screen\n"); break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: full = 0; fprintf(stderr, "[display] windowed\n"); break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: show_pointer(); break;
            case SDL_EVENT_MOUSE_MOTION:
                mouse_moved_at = SDL_GetTicks();
                show_pointer();
                break;
            case SDL_EVENT_TEXT_INPUT:   /* (typed letters, for the settings menu) */
                if (menu_is_open())
                    for (const char *c = e.text.text; c && *c; c++) menu_char((unsigned char)*c);
                break;
            case SDL_EVENT_KEY_DOWN:
                menu_key_of(&e.key);
                if (!e.key.repeat) key_down(&e.key);
                break;
            }
            got = SDL_PollEvent(&e);
        }
        Uint64 now = SDL_GetTicks();
        int asked = display_take_mode();   /* (the settings menu: borderless and fullscreen are both macOS's full screen) */
        if (window && asked >= 0) set_full(asked != DISPLAY_WINDOWED);
        if (window) {
            menu_text_input();
            idle_pointer(now);
        }
        if (every > 0 && status && now - last >= (Uint64)every * 1000) {
            last = now;
            status();
        }
    }
}

/* ---- the display's refreshes (display.ini's pacing = display) ------------------------------- */
static pthread_mutex_t vblank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vblank_cond = PTHREAD_COND_INITIALIZER;
static unsigned long long vblanks;

static CVReturn on_vblank(CVDisplayLinkRef link, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags in, CVOptionFlags *flags, void *unused) {
    (void)link; (void)now; (void)out; (void)in; (void)flags; (void)unused;
    pthread_mutex_lock(&vblank_lock);
    vblanks++;
    pthread_cond_broadcast(&vblank_cond);
    pthread_mutex_unlock(&vblank_lock);
    return kCVReturnSuccess;
}

/* (gpu_display.c's vsync thread) Waits until the window's display has refreshed n times, for at
 * most timeout_ms: 1, or 0 if it did not (no display link; the display asleep). A CoreVideo
 * display link, started at the first call, ticks at every refresh of the display the window is
 * on, on a thread of its own. */
int rt_window_wait_vblank(int n, int timeout_ms) {
    static CVDisplayLinkRef link;
    static CGDirectDisplayID linked;
    static int failed;
    CGDirectDisplayID d = display_id;
    if (failed || !d || n <= 0) return 0;
    if (!link) {
        if (CVDisplayLinkCreateWithCGDisplay(d, &link) != kCVReturnSuccess || CVDisplayLinkSetOutputCallback(link, on_vblank, NULL) != kCVReturnSuccess ||
            CVDisplayLinkStart(link) != kCVReturnSuccess) {
            fprintf(stderr, "[display] no display link for display %u: clock pacing\n", (unsigned)d);
            failed = 1;
            return 0;
        }
        linked = d;
    } else if (d != linked) {   /* (the window went to another display) */
        CVDisplayLinkSetCurrentCGDisplay(link, d);
        linked = d;
    }
    unsigned long long end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (unsigned long long)timeout_ms * 1000000;
    int ok = 0;
    pthread_mutex_lock(&vblank_lock);
    for (unsigned long long until = vblanks + (unsigned)n;;) {
        if (vblanks >= until) { ok = 1; break; }
        unsigned long long t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (t >= end) break;
        struct timespec left = {(time_t)((end - t) / 1000000000), (long)((end - t) % 1000000000)};
        pthread_cond_timedwait_relative_np(&vblank_cond, &vblank_lock, &left);
    }
    pthread_mutex_unlock(&vblank_lock);
    return ok;
}

/* ---- the software keyboard (hle_applet.c) ---------------------------------------------------- */
static void SDLCALL run_block(void *block) { ((__bridge void (^)(void))block)(); }

/* Asks for a line of text in an alert over the window: 1 OK (the answer in text), 0 cancelled,
 * -1 when there is no window. From any thread: the alert runs on the main thread, called from its
 * event loop (SDL_RunOnMainThread) - not as a block of the main queue's, which would hold the
 * queue's other work up meanwhile (pads coming and going) - and the caller waits for it. */
int rt_window_ask_text(const char *prompt, const char *initial, char *text, size_t cap) {
    if (!window) return -1;
    NSString *question = [NSString stringWithUTF8String:prompt] ?: @"Enter text", *start = [NSString stringWithUTF8String:initial] ?: @"";
    __block int ok = 0;
    __block NSString *answer = nil;
    void (^ask)(void) = ^{
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = question;
        [a addButtonWithTitle:@"OK"];       /* (Return) */
        [a addButtonWithTitle:@"Cancel"];   /* (Esc) */
        NSTextField *field = [NSTextField textFieldWithString:start];
        field.frame = NSMakeRect(0, 0, 380, field.fittingSize.height);
        a.accessoryView = field;
        [a layout];
        a.window.initialFirstResponder = field;
        show_pointer();
        [NSApp activate];
        ok = [a runModal] == NSAlertFirstButtonReturn;
        if (ok) answer = field.stringValue;
        [nswindow makeKeyAndOrderFront:nil];   /* (the keyboard back to the game: AppKit leaves no window key) */
    };
    if (!SDL_RunOnMainThread(run_block, (__bridge void *)ask, true)) {
        fprintf(stderr, "[keyboard] cannot ask on the main thread: %s\n", SDL_GetError());
        return -1;
    }
    if (ok) snprintf(text, cap, "%s", answer.UTF8String ? answer.UTF8String : "");
    return ok;
}
