/* The settings menu (gpu_menu.h): display.ini, controllers.ini and the online_*.txt files,
 * changed from inside the game. Every change is saved at once (hle_settings.c keeps the files'
 * comments); the ones the game can take while running take effect at once, the others at its
 * next start, which the menu can do (Game > Restart).
 *
 * It runs on the vsync thread, inside the renderer's present: menu_frame reads the pads (their
 * console buttons, as the game would get them) and the keys the window queued, menu_draw
 * draws it over the picture with the overlay's quads. Pages: Display, Controllers (with a page
 * of a pad's buttons), Online, Game.
 *
 * Tests: SSBU_MENU_AT=<seconds> opens it then; SSBU_MENU_KEYS="<seconds>:<key>,..." presses
 * UP DOWN LEFT RIGHT ENTER ESC TAB BKSP DEL, SHOT (a screenshot with the menu), or types
 * =<text>. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "display.h"
#include "gpu_menu.h"
#include "gpu_overlay.h"
#include "net.h"
#include "pad.h"
#include "settings.h"

int gfx_window_focused(void);
#ifdef _WIN32
int gfx_adapter_names(char names[][64], int max);   /* (gpu_d3d11.c) the graphics cards, fastest first */
#endif

#define PUBLIC_LOBBY "smash.recomps.wtf:39421"   /* (the online_server.txt the port comes with) */

static volatile int open_, releasing;
int menu_is_open(void) { return open_; }
int menu_blocks_input(void) { return open_ || releasing; }

/* ---- messages ------------------------------------------------------------------------- */
static SRWLOCK toast_lock = SRWLOCK_INIT;
static char toast[160];
static ULONGLONG toast_until;   /* 1: not shown yet (its time starts at the first draw) */

void menu_toast(const char *text) {
    AcquireSRWLockExclusive(&toast_lock);
    snprintf(toast, sizeof toast, "%s", text);
    toast_until = 1;
    ReleaseSRWLockExclusive(&toast_lock);
}

int menu_visible(void) { return open_ || (toast_until && (toast_until == 1 || GetTickCount64() < toast_until)); }

/* ---- keys from the window's thread ---------------------------------------------------- */
static volatile LONG q[64], q_head, q_tail;

static void push(LONG v) {
    LONG h = q_head;
    if (h - q_tail >= 64) return;
    q[h & 63] = v;
    InterlockedExchange(&q_head, h + 1);
}

void menu_key(int key) { push(key); }
void menu_char(unsigned ch) { if (ch >= 32 && ch < 127) push((LONG)(0x10000 | ch)); }

/* ---- what can be pressed ---------------------------------------------------------------- */
enum {   /* (from keys, pads or the test script) */
    EV_UP = 1, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_A, EV_BKSP, EV_B, EV_DEL, EV_NEXT, EV_PREV, EV_DONE, EV_ESC, EV_CHAR
};

/* ---- the rows of a page ------------------------------------------------------------------ */
enum { P_DISPLAY, P_PADS, P_ONLINE, P_GAME, P_REMAP };
static const char *const PAGE_NAMES[4] = {"Display", "Controllers", "Online", "Game"};
enum { K_INFO, K_CHOICE, K_ACTION, K_TEXT };
enum { W_NONE, W_NOW, W_RESTART };
enum {
    R_NONE, R_WINDOW, R_PACING, R_LATENCY, R_CURSOR, R_OVERLAY, R_SIZE, R_SCALE, R_GPU,
    R_PAD, R_PAD_PLAYER, R_PAD_LAYOUT, R_PAD_DEADZONE, R_PAD_RUMBLE, R_PAD_TRIGGER, R_PAD_REMAP, R_PAD_TEST, R_PAD_RESET,
    R_KEYBOARD, R_BACKGROUND, R_PADS_FILE,
    R_NET_STATUS, R_NET_MODE, R_NET_CODE, R_NET_SERVER, R_NET_NAME, R_NET_DIRECT,
    R_RESTART, R_QUIT, R_REMAP_BACK, R_REMAP_FIRST = 100   /* + console button */
};

typedef struct Row { int id, kind, when; char label[48], value[112], note[64]; const char *help; } Row;
static Row rows[40];
static int nrows;

static Row *add(int id, int kind, int when, const char *label, const char *help) {
    Row *r = &rows[nrows < 39 ? nrows++ : 39];
    memset(r, 0, sizeof *r);
    r->id = id; r->kind = kind; r->when = when; r->help = help;
    snprintf(r->label, sizeof r->label, "%s", label);
    return r;
}

/* ---- state ---------------------------------------------------------------------------- */
static int page, sel[5], top[5];
static int pad_slot = -1;                /* the pad the Controllers page sets up */
static int confirm;                      /* a row asking yes or no */
static int editing, grid_r, grid_c;      /* a text row being typed */
static char edit_buf[64];
static int capture_button = -1;          /* the console button a new control is wanted for */
static u32 capture_held;
static ULONGLONG capture_until;
static int restart_pending;
static int *overlay_on;                  /* the frame-rate overlay's switch (gpu_d3d11.c) */
static char pins[PAD_SLOTS][128];        /* controllers.ini's [players], read at the menu's opening and changes */
static int rumble_slot = -1;
static ULONGLONG rumble_stop;
static char version[64];
static char gpus[8][64];
static int ngpus;

static struct { int mode; char custom[128], code[40], name[40]; int direct; } net;
enum { NET_PUBLIC, NET_PRIVATE, NET_CUSTOM, NET_OFFLINE };
static const char *const NET_MODES[4] = {"public lobby", "private lobby (a code)", "your own server", "offline"};

static void net_load(void) {
    char line[256] = "", host[128] = "", code[40] = "";
    settings_line_read("online_server.txt", line, sizeof line);
    sscanf(line, "%127s %39s", host, code);
    memset(&net, 0, sizeof net);
    snprintf(net.code, sizeof net.code, "%s", code);
    if (!host[0] || !_stricmp(host, "off")) net.mode = NET_OFFLINE;
    else if (!_stricmp(host, PUBLIC_LOBBY)) net.mode = code[0] ? NET_PRIVATE : NET_PUBLIC;
    else { net.mode = NET_CUSTOM; snprintf(net.custom, sizeof net.custom, "%s", host); }
    settings_line_read("online_name.txt", net.name, sizeof net.name);
    char direct[16] = "";
    net.direct = !(settings_line_read("online_direct.txt", direct, sizeof direct) && !_strnicmp(direct, "off", 3));
}

static void net_save_server(void) {
    char line[256];
    if (net.mode == NET_OFFLINE) snprintf(line, sizeof line, "off");
    else if (net.mode == NET_PUBLIC) snprintf(line, sizeof line, "%s", PUBLIC_LOBBY);
    else if (net.mode == NET_PRIVATE) {
        if (!net.code[0]) snprintf(net.code, sizeof net.code, "friends");
        snprintf(line, sizeof line, "%s %s", PUBLIC_LOBBY, net.code);
    }
    else snprintf(line, sizeof line, "%s%s%s", net.custom[0] ? net.custom : "127.0.0.1:39420", net.code[0] ? " " : "", net.code);
    settings_line_write("online_server.txt", line);
}

static void load_version(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/../../version.txt", rt_data_dir ? rt_data_dir : ".");
    version[0] = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(version, sizeof version, f)) version[strcspn(version, "\r\n")] = 0;
        fclose(f);
    }
}

static void load_pins(void);

static void open_menu(void) {
    if (open_) return;
    net_load();
    load_pins();
    load_version();
#ifdef _WIN32
    ngpus = gfx_adapter_names(gpus, 8);
#endif
    confirm = editing = 0;
    capture_button = -1;
    if (page == P_REMAP) page = P_PADS;
    open_ = 1;
    fprintf(stderr, "[menu] open\n");
}

static void close_menu(void) {
    if (!open_) return;
    confirm = editing = 0;
    capture_button = -1;
    releasing = 1;   /* (the game gets input back once nothing is held) */
    open_ = 0;
    fprintf(stderr, "[menu] closed\n");
}

/* ---- saving --------------------------------------------------------------------------- */
static void changed(int when) {
    if (when == W_RESTART && !restart_pending) {
        restart_pending = 1;
        menu_toast("Saved: takes effect when the game starts again (Game > Restart)");
    }
}

static void set_display(const char *key, const char *value) { settings_ini_set(display_ini_path(), NULL, key, value); }

/* The [device ...] a pad's settings go in: the one it takes them from now, else its name's. */
static void pad_section(const PadInfo *p, char *out, int size) { snprintf(out, (size_t)size, "device %s", p->section[0] ? p->section : p->name); }

static void set_pad(const PadInfo *p, const char *key, const char *value) {
    char section[128];
    pad_section(p, section, sizeof section);
    settings_ini_set(pad_ini_path(), section, key, value);
    pad_reload();
}

static int pad_matches(const PadInfo *p, const char *what) {
    return *what && (!_stricmp(what, p->guid) || !_stricmp(what, p->name) || !_stricmp(what, p->type));
}

static void load_pins(void) {
    for (int k = 1; k <= PAD_SLOTS; k++) {
        char key[4];
        snprintf(key, sizeof key, "%d", k);
        if (!settings_ini_get(pad_ini_path(), "players", key, pins[k - 1], sizeof pins[k - 1])) pins[k - 1][0] = 0;
    }
}

/* The player controllers.ini keeps for this pad ([players]), 0 none. */
static int pad_pin(const PadInfo *p) {
    for (int k = 1; k <= PAD_SLOTS; k++)
        if (pad_matches(p, pins[k - 1])) return k;
    return 0;
}

/* ---- the pages -------------------------------------------------------------------------- */
static PadInfo pads[PAD_SLOTS];
static int npads_connected;

static const char *on_off(int v) { return v ? "on" : "off"; }

static void note_env(Row *r, const char *var) {
    const char *v = getenv(var);
    if (v && *v) snprintf(r->note, sizeof r->note, "%s set", var);
}

static const int SIZES[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int REMAP_ORDER[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5, 13, 15, 12, 14};   /* A B X Y L R ZL ZR + - sticks, d-pad */

static void build(void) {
    nrows = 0;
    Row *r;
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (page) {
    case P_DISPLAY:
#ifdef __APPLE__
        r = add(R_WINDOW, K_CHOICE, W_NOW, "Window", "Windowed: a normal window. Borderless and full screen: macOS's full screen, in a space of its own. Option+Return switches too.");
#else
        r = add(R_WINDOW, K_CHOICE, W_NOW, "Window", "Windowed: a normal window. Borderless: covering the monitor. Full screen: borderless and always on top. F11 switches too.");
#endif
        snprintf(r->value, sizeof r->value, "%s", ds.mode == DISPLAY_BORDERLESS ? "borderless" : ds.mode == DISPLAY_FULLSCREEN ? "full screen" : "windowed");
        note_env(r, "SSBU_WINDOW");
        r = add(R_PACING, K_CHOICE, W_NOW, "Frame pacing",
                "Clock: a steady 60 frames a second. VRR: for G-Sync / FreeSync monitors, smoothest with borderless (tears without VRR). Display: follow a 60/120/240 Hz monitor's own refresh.");
        snprintf(r->value, sizeof r->value, "%s", ds.pacing == PACING_VRR ? "vrr" : ds.pacing == PACING_DISPLAY ? "display" : "clock");
        note_env(r, "SSBU_PACING");
        r = add(R_LATENCY, K_CHOICE, W_NOW, "Frames queued ahead", "How many frames the graphics card may work ahead: 1 is the most responsive, 3 the smoothest on a busy PC.");
        snprintf(r->value, sizeof r->value, "%d", ds.latency);
        r = add(R_CURSOR, K_CHOICE, W_NOW, "Hide the mouse pointer", "Hides the pointer over the game after 2 seconds without moving it.");
        snprintf(r->value, sizeof r->value, "%s", on_off(ds.hide_cursor));
        r = add(R_OVERLAY, K_CHOICE, W_NOW, "Frame rate in the corner", "The frame rate and a graph of frame times (F1 shows or hides it while playing; F2 adds details).");
        snprintf(r->value, sizeof r->value, "%s", on_off(overlay_on ? *overlay_on : ds.overlay));
        note_env(r, "SSBU_OVERLAY");
#ifdef __APPLE__
        r = add(R_SIZE, K_CHOICE, W_RESTART, "Window size", "The size of the game's window when it is not full screen, in points (on a Retina display, a point is 2 x 2 pixels).");
#else
        r = add(R_SIZE, K_CHOICE, W_RESTART, "Window size", "The size of the game's window when it is not full screen (at 100% Windows scaling).");
#endif
        snprintf(r->value, sizeof r->value, "%d x %d", ds.width, ds.height);
        note_env(r, "SSBU_WINDOW_SIZE");
#ifndef __APPLE__   /* (the Metal renderer draws at the game's own 1920x1080) */
        r = add(R_SCALE, K_CHOICE, W_RESTART, "Render scale",
                "How many times 1920x1080 the game draws at: sharper on a big screen, more work for the graphics card. Auto: as many pixels as the window has.");
        if (ds.render_scale <= 0) snprintf(r->value, sizeof r->value, "auto");
        else snprintf(r->value, sizeof r->value, "%g", ds.render_scale);
        note_env(r, "SSBU_RENDER_SCALE");
#endif
#ifdef _WIN32
        r = add(R_GPU, K_CHOICE, W_RESTART, "Graphics card", "Auto picks the fastest card (on a laptop, the NVIDIA or AMD one). Pick another to use it instead.");
        snprintf(r->value, sizeof r->value, "%s", ds.gpu);
        note_env(r, "SSBU_GPU");
#endif
        break;
    case P_PADS:
        if (!npads_connected) {
            r = add(R_NONE, K_INFO, W_NONE, "Controllers", "Plug in or pair a controller: it shows here as it connects. The keyboard works too (see below).");
            snprintf(r->value, sizeof r->value, "none connected");
        } else {
            r = add(R_PAD, K_CHOICE, W_NONE, "Controller", "Which controller to set up (left / right to pick another).");
            if (pad && pad->slot >= 0) snprintf(r->value, sizeof r->value, "player %d: %.80s", pad->slot + 1, pad->name);
            else if (pad) snprintf(r->value, sizeof r->value, "no player: %.80s", pad->name);
            if (pad) {
                int pin = pad_pin(pad);
                r = add(R_PAD_PLAYER, K_CHOICE, W_NOW, "Player", "Auto: the lowest free player. A number keeps that player for this controller, every time it connects.");
                if (pin) snprintf(r->value, sizeof r->value, "%d (kept)", pin);
                else snprintf(r->value, sizeof r->value, "auto (now %d)", pad->slot + 1);
                r = add(R_PAD_LAYOUT, K_CHOICE, W_NOW, "Face buttons",
                        "By position: as on a Switch pad, the right button is A. By label: the letter printed on the pad (an Xbox pad's A is A; PlayStation pads stay by position).");
                snprintf(r->value, sizeof r->value, "%s", pad->layout ? "by label" : "by position");
                r = add(R_PAD_DEADZONE, K_CHOICE, W_NOW, "Stick dead zone", "How much of the sticks' travel around the centre is ignored: raise it if a worn stick drifts.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->deadzone);
                r = add(R_PAD_RUMBLE, K_CHOICE, W_NOW, "Rumble", "The strength of the rumble; 0 is off.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->rumble);
                r = add(R_PAD_TRIGGER, K_CHOICE, W_NOW, "Trigger press point", "How far a trigger is pulled before it counts as ZL / ZR.");
                snprintf(r->value, sizeof r->value, "%d%%", pad->trigger);
                add(R_PAD_REMAP, K_ACTION, W_NOW, "Buttons...", "Which control on this pad is each of the game's buttons.");
                add(R_PAD_TEST, K_ACTION, W_NONE, "Test rumble", "Rumbles this pad for half a second.");
                add(R_PAD_RESET, K_ACTION, W_NOW, "Back to the defaults", "This pad's buttons, dead zone, rumble and trigger as they were.");
                char section[128];
                pad_section(pad, section, sizeof section);
                for (int i = 0; i < nrows; i++)
                    if (rows[i].id >= R_PAD_LAYOUT && rows[i].id <= R_PAD_RESET && rows[i].id != R_PAD_TEST)
                        snprintf(rows[i].note, sizeof rows[i].note, "[%.40s]", section);
            }
        }
        r = add(R_KEYBOARD, K_CHOICE, W_NOW, "Keyboard plays as",
                "The keyboard adds to this player: arrows move, Z X C V are A B X Y, A S are L R, Q W are ZL ZR, Enter is +, I J K L the right stick.");
        if (pad_keyboard_slot() < 0) snprintf(r->value, sizeof r->value, "nobody");
        else snprintf(r->value, sizeof r->value, "player %d", pad_keyboard_slot() + 1);
        r = add(R_BACKGROUND, K_CHOICE, W_NOW, "Pads when not focused", "On: controllers work while another window is in front. Off: only while the game's window is.");
        snprintf(r->value, sizeof r->value, "%s", on_off(pad_background()));
        r = add(R_PADS_FILE, K_ACTION, W_NONE, "Open controllers.ini", "Everything else, by hand: settings per kind of pad, more remaps. Changes there count at the next start.");
        snprintf(r->value, sizeof r->value, "%s", pad_ini_path());
        break;
    case P_REMAP:
        r = add(R_REMAP_BACK, K_ACTION, W_NONE, "< Back", "Back to the controller's settings.");
        if (pad) snprintf(r->value, sizeof r->value, "%.80s", pad->name);
        for (int i = 0; pad && i < 16; i++) {
            int b = REMAP_ORDER[i];
            char label[24];
            snprintf(label, sizeof label, "%s", pad_button_name(b));
            r = add(R_REMAP_FIRST + b, K_ACTION, W_NOW, label, "A or Enter: then press the control for this button. Y or Delete: its default again.");
            snprintf(r->value, sizeof r->value, "%s", pad->map[b]);
        }
        break;
    case P_ONLINE: {
        const char *launcher = getenv("SSBU_NET_RELAY") && *getenv("SSBU_NET_RELAY") ? "SSBU_NET_RELAY set" :
                               getenv("SSBU_NET_HOST") && *getenv("SSBU_NET_HOST") ? "hosting (launcher)" : NULL;
        r = add(R_NET_STATUS, K_INFO, W_NONE, "Now", "What the game is connected to.");
        char status[512];
        net_status(status, sizeof status);
        char *nl = strchr(status, '\n');
        if (nl) *nl = 0;
        snprintf(r->value, sizeof r->value, "%.110s", status[0] ? status : net_configured() ? "connecting..." : "offline");
        r = add(R_NET_MODE, K_CHOICE, W_RESTART, "Play online in",
                "Public lobby: everyone's rooms in Local Wireless. Private: only players with the same code. Your own server: host:port. Offline: no netplay.");
        snprintf(r->value, sizeof r->value, "%s", NET_MODES[net.mode]);
        if (net.mode == NET_PRIVATE || net.mode == NET_CUSTOM) {
            r = add(R_NET_CODE, K_TEXT, W_RESTART, "Lobby code", "Players with the same code see each other's rooms (letters and digits, no spaces).");
            snprintf(r->value, sizeof r->value, "%s", net.code[0] ? net.code : (net.mode == NET_PRIVATE ? "friends" : "(none)"));
        }
        if (net.mode == NET_CUSTOM) {
            r = add(R_NET_SERVER, K_TEXT, W_RESTART, "Server", "The lobby server's address and port, e.g. 192.168.1.20:39420.");
            snprintf(r->value, sizeof r->value, "%s", net.custom[0] ? net.custom : "(none)");
        }
        r = add(R_NET_NAME, K_TEXT, W_RESTART, "Your name", "What other players see (up to 32 letters).");
        snprintf(r->value, sizeof r->value, "%s", net.name[0] ? net.name : "(picked at the first online start)");
        r = add(R_NET_DIRECT, K_CHOICE, W_RESTART, "Direct connections",
                "On: rooms connect players directly when they can (less lag). Off: everything goes through the lobby, so other players never learn your internet address.");
        snprintf(r->value, sizeof r->value, "%s", on_off(net.direct));
        for (int i = 1; i < nrows; i++) {
            if (launcher) snprintf(rows[i].note, sizeof rows[i].note, "%s", launcher);
            else if (!settings_online()) snprintf(rows[i].note, sizeof rows[i].note, "for Play SSBU");
        }
        if (restart_pending) add(R_RESTART, K_ACTION, W_NONE, "Restart now", "Starts the game again, with the changes. A match in progress is lost.");
        break;
    }
    case P_GAME:
        add(R_RESTART, K_ACTION, W_NONE, "Restart the game", "Starts the game again (for the settings marked 'next start'). A match in progress is lost.");
        add(R_QUIT, K_ACTION, W_NONE, "Quit the game", "Closes the game. A match in progress is lost.");
#ifdef __APPLE__
        static const char *const KEYS[][2] = {{"Esc / Home button", "this menu (or hold - and + for a second)"}, {"F1, Cmd+1", "frame rate on / off"},
                                               {"F2, Cmd+2", "details"}, {"F3, Cmd+3", "screenshot (screenshots folder)"},
                                               {"Option+Return", "full screen (or Ctrl+Cmd+F)"}};
#else
        static const char *const KEYS[][2] = {{"Esc / Home button", "this menu (or hold - and + for a second)"}, {"F1", "frame rate on / off"},
                                               {"F2", "details"}, {"F3", "screenshot (screenshots folder)"}, {"F11, Alt+Enter", "full screen"}};
#endif
        for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
            r = add(R_NONE, K_INFO, W_NONE, KEYS[i][0], "Keys that work while playing.");
            snprintf(r->value, sizeof r->value, "%s", KEYS[i][1]);
        }
        r = add(R_NONE, K_INFO, W_NONE, "Version", "The game version this folder has (version.txt).");
        snprintf(r->value, sizeof r->value, "%s", version[0] ? version : "unknown (no version.txt)");
        break;
    }
}

static int selectable(const Row *r) { return r->kind != K_INFO; }

/* ---- changing a row ----------------------------------------------------------------------- */
static int cycle(int v, int n, int dir) { return ((v + dir) % n + n) % n; }

static void start_edit(int id) {
    editing = id;
    grid_r = grid_c = 0;
    const char *v = id == R_NET_CODE ? net.code : id == R_NET_SERVER ? net.custom : net.name;
    snprintf(edit_buf, sizeof edit_buf, "%s", v);
}

static void finish_edit(void) {
    char *s = edit_buf;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    if (editing == R_NET_NAME) {
        if (*s) { snprintf(net.name, sizeof net.name, "%.32s", s); settings_line_write("online_name.txt", net.name); changed(W_RESTART); }
    } else if (!strchr(s, ' ')) {
        if (editing == R_NET_CODE) snprintf(net.code, sizeof net.code, "%s", s);
        else if (editing == R_NET_SERVER && *s) snprintf(net.custom, sizeof net.custom, "%s", s);
        net_save_server();
        changed(W_RESTART);
    } else menu_toast("No spaces in a lobby code or a server's address");
    editing = 0;
}

static void change(const Row *row, int dir) {
    DisplaySettings ds;
    display_settings(&ds);
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    char v[64];
    switch (row->id) {
    case R_WINDOW: {
        int m = cycle(ds.mode, 3, dir);
        display_set_mode(m);
        set_display("window", m == DISPLAY_BORDERLESS ? "borderless" : m == DISPLAY_FULLSCREEN ? "fullscreen" : "windowed");
        break;
    }
    case R_PACING: {
        Pacing p = (Pacing)cycle(ds.pacing, 3, dir);
        display_set_pacing(p);
        set_display("pacing", p == PACING_VRR ? "vrr" : p == PACING_DISPLAY ? "display" : "clock");
        break;
    }
    case R_LATENCY:
        snprintf(v, sizeof v, "%d", cycle(ds.latency - 1, 3, dir) + 1);
        display_set_latency(atoi(v));
        set_display("latency", v);
        break;
    case R_CURSOR:
        display_set_hide_cursor(!ds.hide_cursor);
        set_display("hide_cursor", ds.hide_cursor ? "0" : "1");
        break;
    case R_OVERLAY:
        if (overlay_on) *overlay_on = !*overlay_on;
        set_display("overlay", overlay_on && *overlay_on ? "1" : "0");
        break;
    case R_SIZE: {
        int n = sizeof SIZES / sizeof *SIZES, i = -1;
        for (int k = 0; k < n; k++) if (SIZES[k][0] == ds.width && SIZES[k][1] == ds.height) i = k;
        if (i >= 0) i = cycle(i, n, dir);
        else {   /* (a size typed by hand: the next one up or down) */
            i = 0;
            while (i < n && SIZES[i][0] * SIZES[i][1] < ds.width * ds.height) i++;
            i = cycle(dir < 0 ? i - 1 : i, n, 0);
        }
        snprintf(v, sizeof v, "%d", SIZES[i][0]);
        set_display("width", v);
        snprintf(v, sizeof v, "%d", SIZES[i][1]);
        set_display("height", v);
        break;
    }
    case R_SCALE: {
        static const char *const SCALES[] = {"1", "auto", "1.5", "2", "3"};
        static const float VALUES[] = {1, 0, 1.5f, 2, 3};
        int n = sizeof SCALES / sizeof *SCALES, i = -1;
        for (int k = 0; k < n; k++) if (VALUES[k] == ds.render_scale) i = k;
        i = i < 0 ? (dir > 0 ? 0 : n - 1) : cycle(i, n, dir);
        set_display("render_scale", SCALES[i]);
        display_set_render_scale(VALUES[i]);   /* (shown now; the renderer takes it at the next start) */
        break;
    }
#ifdef _WIN32
    case R_GPU: {
        int i = -1;   /* auto, then the cards */
        for (int k = 0; k < ngpus; k++) if (!_stricmp(ds.gpu, gpus[k])) i = k;
        if (i < 0 && _stricmp(ds.gpu, "auto")) i = ngpus;   /* (something typed by hand: next is auto) */
        i = cycle(i + 1, ngpus + 1, dir) - 1;
        set_display("gpu", i < 0 ? "auto" : gpus[i]);
        break;
    }
#endif
    case R_PAD: {
        int start = pad_slot < 0 ? 0 : pad_slot;
        for (int k = 1; k <= PAD_SLOTS; k++) {
            int s = cycle(start, PAD_SLOTS, dir * k);
            if (pads[s].connected) { pad_slot = s; break; }
        }
        return;
    }
    case R_PAD_PLAYER: {
        if (!pad) return;
        int pin = cycle(pad_pin(pad), PAD_SLOTS + 1, dir);
        for (int k = 1; k <= PAD_SLOTS; k++) {   /* any old pin of this pad goes */
            snprintf(v, sizeof v, "%d", k);
            if (pad_matches(pad, pins[k - 1])) settings_ini_set(pad_ini_path(), "players", v, NULL);
        }
        if (pin) {
            snprintf(v, sizeof v, "%d", pin);
            settings_ini_set(pad_ini_path(), "players", v, pad->guid);
        }
        load_pins();
        pad_reload();
        break;
    }
    case R_PAD_LAYOUT: if (pad) set_pad(pad, "layout", pad->layout ? "positional" : "labels"); break;
    case R_PAD_DEADZONE:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->deadzone / 5, 11, dir) * 5); set_pad(pad, "deadzone", v); }
        break;
    case R_PAD_RUMBLE:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->rumble / 10, 11, dir) * 10); set_pad(pad, "rumble", v); }
        break;
    case R_PAD_TRIGGER:
        if (pad) { snprintf(v, sizeof v, "%d", cycle(pad->trigger / 5 - 1, 19, dir) * 5 + 5); set_pad(pad, "trigger_threshold", v); }
        break;
    case R_KEYBOARD:
        snprintf(v, sizeof v, "%d", cycle(pad_keyboard_slot() + 1, PAD_SLOTS + 1, dir));
        settings_ini_set(pad_ini_path(), "general", "keyboard_player", v);
        pad_reload();
        break;
    case R_BACKGROUND:
        settings_ini_set(pad_ini_path(), "general", "background", pad_background() ? "0" : "1");
        pad_reload();
        break;
    case R_NET_MODE:
        net.mode = cycle(net.mode, 4, dir);
        net_save_server();
        break;
    case R_NET_DIRECT:
        net.direct = !net.direct;
        settings_line_write("online_direct.txt", net.direct ? "on" : "off");
        break;
    default:
        return;
    }
    changed(row->when);
}

static void action(const Row *row) {
    PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
    switch (row->kind) {
    case K_CHOICE: change(row, 1); return;
    case K_TEXT: start_edit(row->id); return;
    }
    if (row->id >= R_REMAP_FIRST && pad) {
        capture_button = row->id - R_REMAP_FIRST;
        capture_held = ~0u;   /* (whatever is held now must go first) */
        capture_until = GetTickCount64() + 6000;
        return;
    }
    switch (row->id) {
    case R_PAD_REMAP: if (pad) { page = P_REMAP; sel[P_REMAP] = 1; top[P_REMAP] = 0; } break;
    case R_REMAP_BACK: page = P_PADS; break;
    case R_PAD_TEST:
        if (pad && pad->slot >= 0) {
            pad_rumble((u32)pad->slot, 1, 0.8f);
            pad_rumble((u32)pad->slot, 2, 0.8f);
            rumble_slot = pad->slot;
            rumble_stop = GetTickCount64() + 500;
        }
        break;
    case R_PAD_RESET:
        if (pad) {
            static const char *const KEYS[] = {"layout", "deadzone", "rumble", "trigger_threshold"};
            char section[128];
            pad_section(pad, section, sizeof section);
            for (u32 i = 0; i < sizeof KEYS / sizeof *KEYS; i++) settings_ini_set(pad_ini_path(), section, KEYS[i], NULL);
            for (int b = 0; b < 16; b++) settings_ini_set(pad_ini_path(), section, pad_button_name(b), NULL);
            pad_reload();
            menu_toast("This controller is back to the defaults");
        }
        break;
    case R_PADS_FILE: {
#ifdef _WIN32
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof cmd, "notepad.exe \"%s\"", pad_ini_path());
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
#else
        char cmd[MAX_PATH + 64];
#ifdef __APPLE__
        snprintf(cmd, sizeof cmd, "open -t '%s' >/dev/null 2>&1 &", pad_ini_path());   /* (the default text editor) */
#else
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", pad_ini_path());
#endif
        if (system(cmd)) {}
#endif
        menu_toast("controllers.ini opened in another window (changes there count at the next start)");
        break;
    }
    case R_RESTART:
    case R_QUIT:
        confirm = row->id;
        break;
    }
}

/* ---- pads and keys -------------------------------------------------------------------- */
static u64 nav_held;
static int nav_wait_release;             /* after a control was taken for a button: until everything is let go */
static ULONGLONG nav_next;
static ULONGLONG combo_since;
static int combo_armed = 1;
static u32 guide_held;

enum { NB_UP = 1, NB_DOWN = 2, NB_LEFT = 4, NB_RIGHT = 8, NB_A = 16, NB_B = 32, NB_L = 64, NB_R = 128, NB_Y = 256, NB_PLUS = 512 };

/* Every pad's buttons as the menu's: the d-pad or left stick, A, B, L/ZL, R/ZR, Y, +. */
static u64 pad_nav(int *combo, u32 *guide) {
    u64 nb = 0;
    *combo = 0;
    *guide = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++) {
        PadState p;
        if (!pad_get(s, &p)) continue;
        u64 b = p.buttons;
        if (b & 1ull << 13 || p.ly > 16384) nb |= NB_UP;
        if (b & 1ull << 15 || p.ly < -16384) nb |= NB_DOWN;
        if (b & 1ull << 12 || p.lx < -16384) nb |= NB_LEFT;
        if (b & 1ull << 14 || p.lx > 16384) nb |= NB_RIGHT;
        if (b & 1ull << 0) nb |= NB_A;
        if (b & 1ull << 1) nb |= NB_B;
        if (b & (1ull << 6 | 1ull << 8)) nb |= NB_L;
        if (b & (1ull << 7 | 1ull << 9)) nb |= NB_R;
        if (b & 1ull << 3) nb |= NB_Y;
        if (b & 1ull << 10) nb |= NB_PLUS;
        if ((b & 1ull << 10) && (b & 1ull << 11)) *combo = 1;
        if (pads[s].connected && pads[s].raw & 1u << PAD_RAW_GUIDE_BIT) *guide = 1;
    }
    return nb;
}

static int keys_held(void) {
    static const int VK[] = {VK_RETURN, VK_ESCAPE, VK_BACK, VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, 'Z', 'X', 'C', 'V'};
    for (u32 i = 0; i < sizeof VK / sizeof *VK; i++)
        if (GetAsyncKeyState(VK[i]) & 0x8000) return 1;
    return 0;
}

/* ---- the text grid (for pads) ------------------------------------------------------------- */
static const char *const GRID[] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789-_.", ": "};
enum { GRID_ROWS = 6, GRID_COLS = 13 };
static const char *const GRID_KEYS[] = {"space", "delete", "done", "cancel"};   /* the last row, after ':' */

static int grid_cols(int r) { return r == GRID_ROWS - 1 ? 1 + 4 : GRID_COLS; }

static void edit_event(int ev, unsigned ch) {
    size_t n = strlen(edit_buf);
    size_t max = editing == R_NET_NAME ? 32 : editing == R_NET_SERVER ? 60 : 32;
    switch (ev) {
    case EV_CHAR: if (n < max) { edit_buf[n] = (char)ch; edit_buf[n + 1] = 0; } break;
    case EV_BKSP: case EV_B: case EV_DEL: if (n) edit_buf[n - 1] = 0; break;
    case EV_ENTER: case EV_DONE: finish_edit(); break;
    case EV_ESC: case EV_PREV: editing = 0; break;
    case EV_UP: grid_r = cycle(grid_r, GRID_ROWS, -1); if (grid_c >= grid_cols(grid_r)) grid_c = grid_cols(grid_r) - 1; break;
    case EV_DOWN: grid_r = cycle(grid_r, GRID_ROWS, 1); if (grid_c >= grid_cols(grid_r)) grid_c = grid_cols(grid_r) - 1; break;
    case EV_LEFT: grid_c = cycle(grid_c, grid_cols(grid_r), -1); break;
    case EV_RIGHT: grid_c = cycle(grid_c, grid_cols(grid_r), 1); break;
    case EV_A:
        if (grid_r < GRID_ROWS - 1) edit_event(EV_CHAR, (unsigned char)GRID[grid_r][grid_c]);
        else if (grid_c == 0) edit_event(EV_CHAR, ':');
        else if (grid_c == 1) edit_event(EV_CHAR, ' ');
        else if (grid_c == 2) edit_event(EV_DEL, 0);
        else if (grid_c == 3) finish_edit();
        else editing = 0;
        break;
    }
}

static void move_sel(int dir) {
    int *s = &sel[page];
    for (int k = 0; k < nrows; k++) {
        int i = cycle(*s, nrows, dir * (k + 1));
        if (selectable(&rows[i])) { *s = i; return; }
    }
}

static void event(int ev, unsigned ch) {
    if (confirm) {
        if (ev == EV_A || ev == EV_ENTER) {
            int what = confirm;
            confirm = 0;
            if (what == R_RESTART) settings_restart();
            else { fprintf(stderr, "[menu] quit\n"); fflush(stderr); ExitProcess(0); }
        } else if (ev == EV_B || ev == EV_BKSP || ev == EV_ESC) confirm = 0;
        return;
    }
    if (editing) { edit_event(ev, ch); return; }
    if (capture_button >= 0) { if (ev == EV_ESC) capture_button = -1; return; }
    Row *row = sel[page] < nrows ? &rows[sel[page]] : NULL;
    switch (ev) {
    case EV_UP: move_sel(-1); break;
    case EV_DOWN: move_sel(1); break;
    case EV_LEFT: case EV_RIGHT: if (row && row->kind == K_CHOICE) change(row, ev == EV_RIGHT ? 1 : -1); break;
    case EV_A: case EV_ENTER: if (row && selectable(row)) action(row); break;
    case EV_DEL:
        if (page == P_REMAP && row && row->id >= R_REMAP_FIRST && pad_slot >= 0 && pads[pad_slot].connected) {
            char section[128];
            pad_section(&pads[pad_slot], section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(row->id - R_REMAP_FIRST), NULL);
            pad_reload();
        }
        break;
    case EV_B: case EV_BKSP:
        if (page == P_REMAP) page = P_PADS;
        else close_menu();
        break;
    case EV_ESC: close_menu(); break;
    case EV_NEXT: case EV_PREV:
        page = page == P_REMAP ? P_PADS : cycle(page, 4, ev == EV_NEXT ? 1 : -1);
        break;
    case EV_CHAR:
        if (ch >= '1' && ch <= '4') page = (int)(ch - '1');
        else if (ch == ' ' && row && selectable(row)) action(row);
        break;
    }
}

/* SSBU_MENU_AT / SSBU_MENU_KEYS (tests) */
static int script_events(void) {
    static struct { double at; int ev; char text[48]; int done; } steps[64];
    static int nsteps = -1, opened;
    static double open_at = -1;
    int shot = 0;
    if (nsteps < 0) {
        nsteps = 0;
        const char *a = getenv("SSBU_MENU_AT");
        open_at = a && *a ? atof(a) : -1;
        const char *s = getenv("SSBU_MENU_KEYS");
        static const struct { const char *name; int ev; } NAMES[] = {{"UP", EV_UP}, {"DOWN", EV_DOWN}, {"LEFT", EV_LEFT}, {"RIGHT", EV_RIGHT},
            {"ENTER", EV_ENTER}, {"ESC", EV_ESC}, {"TAB", EV_NEXT}, {"BKSP", EV_BKSP}, {"DEL", EV_DEL}, {"SHOT", -1}};
        while (s && *s && nsteps < 64) {
            char tok[64];
            int n = 0;
            if (sscanf(s, "%lf:%63[^,]%n", &steps[nsteps].at, tok, &n) < 2) break;
            s += n;
            if (*s == ',') s++;
            if (tok[0] == '=') { steps[nsteps].ev = EV_CHAR; snprintf(steps[nsteps].text, sizeof steps[nsteps].text, "%s", tok + 1); nsteps++; continue; }
            for (u32 i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
                if (!strcmp(tok, NAMES[i].name)) { steps[nsteps++].ev = NAMES[i].ev; break; }
        }
    }
    double now = rt_now_ns() / 1e9;
    if (open_at >= 0 && !opened && now >= open_at) { opened = 1; open_menu(); }
    for (int i = 0; i < nsteps; i++) {
        if (steps[i].done || now < steps[i].at) continue;
        steps[i].done = 1;
        if (steps[i].ev < 0) shot = 1;
        else if (steps[i].ev == EV_CHAR) for (const char *c = steps[i].text; *c; c++) event(EV_CHAR, (unsigned char)*c);
        else if (steps[i].ev == EV_ESC && !open_) open_menu();
        else event(steps[i].ev, 0);
    }
    return shot;
}

int menu_frame(int *overlay, int f3) {
    ULONGLONG now = GetTickCount64();
    overlay_on = overlay;
    for (u32 s = 0; s < PAD_SLOTS; s++) pad_info(s, &pads[s]);
    npads_connected = 0;
    for (u32 s = 0; s < PAD_SLOTS; s++) npads_connected += pads[s].connected;
    if ((pad_slot < 0 || !pads[pad_slot].connected) && npads_connected)
        for (int s = 0; s < PAD_SLOTS; s++) if (pads[s].connected) { pad_slot = s; break; }
    if (rumble_slot >= 0 && now >= rumble_stop) {
        pad_rumble((u32)rumble_slot, 1, 0);
        pad_rumble((u32)rumble_slot, 2, 0);
        rumble_slot = -1;
    }
    /* opening and closing: Esc (the queue), the Home button, or - and + held for a second */
    int combo;
    u32 guide;
    u64 nb = pad_nav(&combo, &guide);
    int focused = gfx_window_focused();
    int toggle = guide && !guide_held && (focused || open_);
    guide_held = guide;
    if (combo && combo_armed && (focused || open_)) {
        if (!combo_since) combo_since = now;
        if (now - combo_since >= 1000) { toggle = 1; combo_armed = 0; }
    } else if (!combo) { combo_since = 0; combo_armed = 1; }
    int shot = script_events() || (f3 && open_);
    if (open_) build();
    while (q_tail != q_head) {
        LONG v = q[q_tail & 63];
        InterlockedIncrement(&q_tail);
        if (v & 0x10000) { if (open_) event(EV_CHAR, (unsigned)(v & 0xFF)); continue; }
        static const int EV_OF[] = {0, EV_ESC, EV_UP, EV_DOWN, EV_LEFT, EV_RIGHT, EV_ENTER, EV_BKSP, EV_NEXT, EV_PREV, EV_DEL};
        if (v == MENU_KEY_ESC && !open_) { open_menu(); continue; }
        if (open_ && v > 0 && v < (LONG)(sizeof EV_OF / sizeof *EV_OF)) event(EV_OF[v], 0);
        if (open_) build();
    }
    if (toggle && !open_) open_menu();
    else if (toggle) close_menu();
    if (toggle) {
        nav_held = nb;   /* (what opened it is not also a press in it) */
        if (open_) build();
    }
    if (open_ && capture_button >= 0) {   /* a new control for a button: the first one pressed after all are let go */
        PadInfo *pad = pad_slot >= 0 && pads[pad_slot].connected ? &pads[pad_slot] : NULL;
        u32 raw = pad ? pad->raw : 0;
        if (!pad || now > capture_until) capture_button = -1;
        else if (capture_held == ~0u) { if (!raw) capture_held = 0; }
        else if (raw) {
            int bit = 0;
            while (!(raw >> bit & 1)) bit++;
            char section[128];
            pad_section(pad, section, sizeof section);
            settings_ini_set(pad_ini_path(), section, pad_button_name(capture_button), pad_raw_name(bit));
            pad_reload();
            capture_button = -1;
            nav_wait_release = 1;
        }
        nav_held = nb;
    } else if (open_ && nav_wait_release) {
        if (!nb) nav_wait_release = 0;
        nav_held = nb;
    } else if (open_) {
        const u64 DIRS = NB_UP | NB_DOWN | NB_LEFT | NB_RIGHT;
        u64 pressed = nb & ~nav_held, dirs = nb & DIRS;
        if (pressed & DIRS) nav_next = now + 400;   /* held: repeats after 0.4 s, every 80 ms */
        else if (dirs && now >= nav_next) { pressed |= dirs; nav_next = now + 80; }
        nav_held = nb;
        static const struct { u64 bit; int ev; } MAP[] = {{NB_UP, EV_UP}, {NB_DOWN, EV_DOWN}, {NB_LEFT, EV_LEFT}, {NB_RIGHT, EV_RIGHT}, {NB_A, EV_A},
                                                         {NB_B, EV_B}, {NB_L, EV_PREV}, {NB_R, EV_NEXT}, {NB_Y, EV_DEL}, {NB_PLUS, EV_DONE}};
        for (u32 i = 0; i < sizeof MAP / sizeof *MAP && open_; i++)
            if (pressed & MAP[i].bit) { event(MAP[i].ev, 0); build(); }
    }
    if (!open_) nav_held = nb;
    if (releasing && !open_ && !nb && !keys_held()) releasing = 0;
    if (open_) {
        build();
        if (sel[page] >= nrows) sel[page] = nrows - 1;
        if (sel[page] < 0) sel[page] = 0;
        if (nrows && !selectable(&rows[sel[page]])) move_sel(1);
    }
    return shot ? MENU_SHOT : 0;
}

/* ---- drawing ------------------------------------------------------------------------- */
static float text_w(const char *s, float size) { return (float)strlen(s) * size / 2; }

/* s wrapped to lines of at most `cols` characters, into out. */
static void wrap(const char *s, int cols, char *out, size_t size) {
    size_t o = 0;
    while (*s && o + 2 < size) {
        int n = (int)strlen(s);
        if (n <= cols) { o += (size_t)snprintf(out + o, size - o, "%s", s); break; }
        int cut = cols;
        while (cut > 0 && s[cut] != ' ') cut--;
        if (!cut) cut = cols;
        o += (size_t)snprintf(out + o, size - o, "%.*s\n", cut, s);
        s += cut;
        while (*s == ' ') s++;
    }
    out[o < size ? o : size - 1] = 0;
}

static void box(float x, float y, float w, float h, float s) {
    ovl_rect(x, y, w, h, OVL_RGBA(14, 16, 24, 245));
    ovl_rect(x, y, w, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

/* The message showing now (its time starting at the first look), or 0. */
static int toast_now(char *t, size_t size) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&toast_lock);
    if (toast_until == 1) toast_until = now + 4000;
    int show = toast_until && now < toast_until;
    snprintf(t, size, "%s", toast);
    ReleaseSRWLockExclusive(&toast_lock);
    return show;
}

/* A question or an editor over the menu: the menu dimmed, the box lighter and solid. */
static void modal_box(float w, float h, float x, float y, float bw, float bh, float s) {
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 120));
    ovl_rect(x, y, bw, bh, OVL_RGBA(30, 34, 48, 255));
    ovl_rect(x, y, bw, 3 * s, OVL_RGBA(225, 60, 60, 255));
}

static void draw_toast(float w, float h, float s) {
    char t[160];
    if (!toast_now(t, sizeof t)) return;
    float size = 28 * s, tw = text_w(t, size) + 48 * s, th = size + 28 * s;
    float x = (w - tw) / 2, y = h - th - 40 * s;
    box(x, y, tw, th, s);
    ovl_text(x + 24 * s, y + 14 * s, size, OVL_RGBA(240, 240, 240, 255), t);
}

void menu_draw(float w, float h) {
    float s = h / 1080.0f;
    if (!open_) { draw_toast(w, h, s); return; }
    const unsigned WHITE = OVL_RGBA(240, 240, 245, 255), GREY = OVL_RGBA(150, 155, 170, 255), DIM = OVL_RGBA(110, 115, 130, 255),
                   GREEN = OVL_RGBA(120, 220, 140, 255), AMBER = OVL_RGBA(245, 190, 90, 255), ACCENT = OVL_RGBA(225, 60, 60, 255);
    ovl_rect(0, 0, w, h, OVL_RGBA(0, 0, 0, 150));
    float px = w * 0.06f, py = h * 0.06f, pw = w * 0.88f, ph = h * 0.88f;
    box(px, py, pw, ph, s);
    ovl_text(px + 32 * s, py + 22 * s, 44 * s, WHITE, "Settings");
    char banner[200] = "";
    char status[512];
    net_status(status, sizeof status);
    if (strstr(status, "Room:")) snprintf(banner, sizeof banner, "You are in a netplay room: the game keeps running while this menu is open.");
    else if (restart_pending) snprintf(banner, sizeof banner, "Some changes take effect when the game starts again: Game > Restart the game.");
    char t[160];
    if (toast_now(t, sizeof t)) ovl_text(px + 300 * s, py + 34 * s, 24 * s, WHITE, t);   /* (a message, in the menu: here) */
    else if (banner[0]) ovl_text(px + 300 * s, py + 34 * s, 24 * s, strstr(status, "Room:") ? ACCENT : AMBER, banner);
    /* the pages */
    float ty = py + 110 * s;
    for (int i = 0; i < 4; i++) {
        int on = page == i || (page == P_REMAP && i == P_PADS);
        if (on) ovl_rect(px + 20 * s, ty - 6 * s, 250 * s, 50 * s, OVL_RGBA(55, 62, 90, 255));
        char t[32];
        snprintf(t, sizeof t, "%d %s", i + 1, PAGE_NAMES[i]);
        ovl_text(px + 36 * s, ty, 32 * s, on ? WHITE : GREY, t);
        ty += 60 * s;
    }
    /* the rows */
    float rx = px + 300 * s, ry = py + 110 * s, rw = px + pw - 28 * s - rx, rh = 50 * s, size = 29 * s;
    float help_y = py + ph - 150 * s;
    int fit = (int)((help_y - 16 * s - ry) / rh);
    if (fit < 1) fit = 1;
    int *t0 = &top[page];
    if (sel[page] < *t0) *t0 = sel[page];
    if (sel[page] >= *t0 + fit) *t0 = sel[page] - fit + 1;
    if (*t0 > nrows - fit) *t0 = nrows - fit > 0 ? nrows - fit : 0;
    float vx = rx + rw * 0.40f;
    for (int i = *t0; i < nrows && i < *t0 + fit; i++) {
        const Row *r = &rows[i];
        float y = ry + (float)(i - *t0) * rh;
        int on = i == sel[page];
        if (on) ovl_rect(rx, y - 6 * s, rw, rh - 4 * s, OVL_RGBA(55, 62, 90, 255));
        ovl_text(rx + 16 * s, y, size, r->kind == K_INFO ? GREY : WHITE, r->label);
        char v[160];
        if (r->kind == K_CHOICE && on) snprintf(v, sizeof v, "< %s >", r->value);
        else snprintf(v, sizeof v, "%s", r->value);
        const char *tag = r->note[0] ? r->note : r->when == W_NOW ? "now" : r->when == W_RESTART ? "next start" : "";
        float tag_size = 22 * s, tag_w = text_w(tag, tag_size);
        int room = (int)((rx + rw - 24 * s - tag_w - vx) / (size / 2)) - 1;
        if (room > 0 && (int)strlen(v) > room) { v[room > 3 ? room - 3 : 0] = 0; strcat(v, "..."); }
        ovl_text(vx, y, size, r->kind == K_INFO ? WHITE : on ? WHITE : OVL_RGBA(200, 205, 220, 255), v);
        ovl_text(rx + rw - 16 * s - tag_w, y + 5 * s, tag_size, r->note[0] ? DIM : r->when == W_NOW ? GREEN : AMBER, tag);
    }
    if (*t0 > 0) ovl_text(rx + rw - 40 * s, ry - 40 * s, 26 * s, GREY, "^");
    if (*t0 + fit < nrows) ovl_text(rx + rw - 40 * s, help_y - 34 * s, 26 * s, GREY, "v");
    /* the selected row's help, and the keys */
    ovl_rect(px + 20 * s, help_y - 8 * s, pw - 40 * s, 2 * s, OVL_RGBA(60, 66, 90, 255));
    if (sel[page] < nrows && rows[sel[page]].help) {
        char wrapped[600];
        wrap(rows[sel[page]].help, (int)((pw - 80 * s) / (13 * s)), wrapped, sizeof wrapped);
        ovl_text(px + 36 * s, help_y + 4 * s, 26 * s, OVL_RGBA(200, 210, 230, 255), wrapped);
    }
    ovl_text(px + 36 * s, py + ph - 44 * s, 22 * s, DIM,
             page == P_REMAP ? "Up/Down: choose   A/Enter: press a new control   Y/Delete: default   B/Backspace: back   Esc: close"
                             : "Up/Down: choose   Left/Right: change   A/Enter: select   L/R or Tab, 1-4: page   B/Esc: close");
    /* on top: a question, the text being typed, or a control being waited for */
    if (confirm) {
        float bw = 820 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, confirm == R_RESTART ? "Start the game again?" : "Quit the game?");
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, "A match in progress is lost.");
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, "A / Enter: yes      B / Esc: no");
    } else if (editing) {
        float bw = 900 * s, bh = 600 * s, bx = (w - bw) / 2, by = (h - bh) / 2, cs = 56 * s;
        modal_box(w, h, bx, by, bw, bh, s);
        ovl_text(bx + 36 * s, by + 28 * s, 30 * s, GREY, editing == R_NET_NAME ? "Your name" : editing == R_NET_CODE ? "Lobby code" : "Server (host:port)");
        char t[80];
        snprintf(t, sizeof t, "%s_", edit_buf);
        ovl_rect(bx + 30 * s, by + 72 * s, bw - 60 * s, 56 * s, OVL_RGBA(16, 18, 26, 255));
        ovl_text(bx + 44 * s, by + 82 * s, 34 * s, WHITE, t);
        float gx = bx + (bw - GRID_COLS * cs) / 2, gy = by + 160 * s;
        for (int r = 0; r < GRID_ROWS; r++)
            for (int c = 0; c < grid_cols(r); c++) {
                char cell[16];
                float cx = gx + c * cs, cw = cs - 6 * s;
                if (r < GRID_ROWS - 1) snprintf(cell, sizeof cell, "%c", GRID[r][c]);
                else if (c == 0) snprintf(cell, sizeof cell, ":");
                else { snprintf(cell, sizeof cell, "%s", GRID_KEYS[c - 1]); cx = gx + cs + (c - 1) * 2.75f * cs; cw = 2.75f * cs - 6 * s; }
                int on = r == grid_r && c == grid_c;
                ovl_rect(cx, gy + r * cs, cw, cs - 6 * s, on ? OVL_RGBA(225, 60, 60, 255) : OVL_RGBA(40, 45, 62, 255));
                ovl_text(cx + (cw - text_w(cell, 30 * s)) / 2, gy + r * cs + 10 * s, 30 * s, WHITE, cell);
            }
        ovl_text(bx + 36 * s, by + bh - 52 * s, 22 * s, DIM, "Type, then Enter (Esc: cancel). Pad: A types, B deletes, + done, L cancels.");
    } else if (capture_button >= 0) {
        float bw = 900 * s, bh = 200 * s, bx = (w - bw) / 2, by = (h - bh) / 2;
        modal_box(w, h, bx, by, bw, bh, s);
        char t[160];
        PadInfo *pad = pad_slot >= 0 ? &pads[pad_slot] : NULL;
        snprintf(t, sizeof t, "Press the control for %s", pad_button_name(capture_button));
        ovl_text(bx + 36 * s, by + 34 * s, 34 * s, WHITE, t);
        snprintf(t, sizeof t, "on %.60s", pad ? pad->name : "the controller");
        ovl_text(bx + 36 * s, by + 84 * s, 26 * s, GREY, t);
        ULONGLONG now = GetTickCount64();
        snprintf(t, sizeof t, "Esc: cancel   (%d s)", capture_until > now ? (int)((capture_until - now + 999) / 1000) : 0);
        ovl_text(bx + 36 * s, by + 140 * s, 26 * s, AMBER, t);
    }
}
