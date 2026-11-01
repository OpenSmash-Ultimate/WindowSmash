/* Controllers for nn::hid: every pad SDL3 can see, on eight player slots (pad.h).
 *
 * SDL3.dll is loaded at run time from the exe's own folder; without it (or with
 * SSBU_PAD_BACKEND=xinput) XInput pads 0-3 are players 1-4. An input thread owns the devices:
 * every SDL call is made on it (it is SDL's "main thread": SDL's video is never started), and
 * every millisecond it reads each pad into its slot's copy. Guest threads only read the copies.
 *
 * Slots: a new pad takes the lowest free player; a pad that comes back gets its player back
 * (same GUID and serial number, else the same device path, else the first free player such a
 * pad had); [players] in controllers.ini keeps a player for one pad.
 *
 * controllers.ini (SSBU_CONTROLLERS=<file>, else in the current directory: the port root, where
 * every launcher starts the game) says how pads become players; it is written with the defaults
 * and their documentation when it does not exist.
 *
 * GameCube controllers (on Nintendo's adapter or one in its "Wii U" mode, which SDL3 reads with its
 * own driver, or any pad SDL names a GameCube controller) play as the console's GameCube controllers:
 * the game then uses its GameCube controls, as with a GameCube adapter on a Switch. Their buttons go
 * by their letters; Z is the console's ZR, and L and R are the analog triggers, which the game reads
 * itself (a GameCube npad's state carries both). style = gamecube makes any pad play as one.
 *
 * Linux: the program links SDL2 (SSBU_HAVE_SDL2), started by the window (linux/window.c), and
 * the input thread reads its game controllers the same way; there is no XInput.
 *
 * macOS: the program links SDL3 (SDL3.framework, in the app's Frameworks). The window starts
 * SDL's video on the main thread (macos/window.m); the input thread here starts its gamepads and
 * makes every call to them, as on Windows - it must: SDL's IOKit pads hang on the run loop of the
 * thread that started them, which only that thread runs (in SDL_UpdateGamepads). The pads of
 * GameController.framework (Xbox and MFi pads over Bluetooth) come and go through notifications
 * on the main thread, whose event loop runs for good. No XInput either. */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>

#include "pad.h"
#include "padusb.h"
#ifdef SSBU_HAVE_SDL3
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_guid.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_version.h>
#elif defined SSBU_HAVE_SDL2
#include <SDL.h>
#include <dlfcn.h>
#endif

/* The pad's own controls, numbered as SDL3 numbers its gamepad buttons (a stable ABI). */
enum {
    RB_SOUTH, RB_EAST, RB_WEST, RB_NORTH, RB_BACK, RB_GUIDE, RB_START, RB_LEFT_STICK, RB_RIGHT_STICK,
    RB_LEFT_SHOULDER, RB_RIGHT_SHOULDER, RB_DPAD_UP, RB_DPAD_DOWN, RB_DPAD_LEFT, RB_DPAD_RIGHT, RB_MISC1,
    RB_RIGHT_PADDLE1, RB_LEFT_PADDLE1, RB_RIGHT_PADDLE2, RB_LEFT_PADDLE2, RB_TOUCHPAD, RB_MISC2, RB_MISC3, RB_MISC4, RB_COUNT = 26
};
enum { RA_LEFTX, RA_LEFTY, RA_RIGHTX, RA_RIGHTY, RA_LEFT_TRIGGER, RA_RIGHT_TRIGGER, RA_COUNT };
#ifdef SSBU_HAVE_SDL3
_Static_assert(RB_SOUTH == SDL_GAMEPAD_BUTTON_SOUTH && RB_TOUCHPAD == SDL_GAMEPAD_BUTTON_TOUCHPAD &&
               RB_MISC3 == SDL_GAMEPAD_BUTTON_MISC3 && RB_MISC4 == SDL_GAMEPAD_BUTTON_MISC4 &&
               RB_COUNT == SDL_GAMEPAD_BUTTON_COUNT && RA_RIGHT_TRIGGER == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, "SDL numbering");
#elif defined SSBU_HAVE_SDL2   /* (SDL2's numbering is SDL3's up to the touchpad; A is the bottom button) */
_Static_assert(RB_SOUTH == SDL_CONTROLLER_BUTTON_A && RB_RIGHT_PADDLE1 == SDL_CONTROLLER_BUTTON_PADDLE1 &&
               RB_TOUCHPAD == SDL_CONTROLLER_BUTTON_TOUCHPAD && RA_RIGHT_TRIGGER == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "SDL numbering");
#endif
static const char *const RAW_BUTTON_NAMES[RB_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2",
    "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
/* The console's buttons, as nn::hid::NpadButton bits 0-15. */
static const char *const NPAD_BUTTON_NAMES[16] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR",
                                                   "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
enum { N_A, N_B, N_X, N_Y, N_LSTICK, N_RSTICK, N_L, N_R, N_ZL, N_ZR, N_PLUS, N_MINUS, N_LEFT, N_UP, N_RIGHT, N_DOWN };

/* What one pad reports: buttons by RB_* bit; axes as SDL gives them (sticks -32768..32767 with +y
 * DOWN, triggers 0..32767). */
typedef struct Raw { u32 buttons; i32 axis[RA_COUNT]; } Raw;

enum { SRC_NONE, SRC_BUTTON, SRC_TRIGGER };
typedef struct Source { u8 kind, index; } Source;
enum { MAXSRC = 4 };
typedef struct Mapping { Source src[16][MAXSRC]; u8 n[16]; } Mapping;

/* ---- configuration ----------------------------------------------------------------- */
typedef struct DevCfg {
    char match[96];               /* GUID, name or type of the pad ("" = [device default]) */
    int layout;                   /* -1 unset, 0 positional, 1 labels */
    int style;                    /* -1 unset, 0 auto, 1 pro, 2 gamecube */
    int deadzone, rumble, trigger;   /* percent; -1 unset */
    Source map[16][MAXSRC]; u8 nmap[16], has_map[16];
} DevCfg;

static struct {
    int keyboard_slot;            /* -1: no keyboard */
    int background;
    int rumble;                   /* 0: no pad rumbles ([general] rumble) */
    char pin[PAD_SLOTS][96];
    DevCfg def, dev[32];
    int ndev;
    char path[MAX_PATH];
} cfg = {.keyboard_slot = 0, .background = 1, .rumble = 1, .def = {"", -1, -1, -1, -1, -1}};

static void config_defaults(void) {
    char path[MAX_PATH];
    memcpy(path, cfg.path, sizeof path);
    memset(&cfg, 0, sizeof cfg);
    cfg.keyboard_slot = 0;
    cfg.background = 1;
    cfg.rumble = 1;
    cfg.def.layout = cfg.def.style = cfg.def.deadzone = cfg.def.rumble = cfg.def.trigger = -1;
    memcpy(cfg.path, path, sizeof path);
}

static const char DEFAULT_INI[] =
    "; controllers.ini: how pads become players. Read when the game starts; the settings menu (Esc,\n"
    "; or the pad's Home / PS / Xbox button in the game) changes it too.\n"
    "; Delete this file to get it written again with the defaults. A line starting with ';' is a comment.\n"
    ";\n"
    "; Any controller SDL3 knows works: Xbox, PlayStation, Switch Pro, GameCube (on Nintendo's adapter), and\n"
    "; most others. Each new pad takes the lowest free player; a pad unplugged and plugged back in gets its\n"
    "; player back. The game's log names every pad as it connects, e.g.\n"
    ";   [input] player 2: \"DualSense Wireless Controller\" (ps5, GUID 050000004c050000e60c000000810000)\n"
    "\n"
    "[general]\n"
    "; keyboard_player = 1-8, or 0 for no keyboard. The keyboard adds to that player's pad.\n"
    "keyboard_player = 1\n"
    "; background = 1: pads work while the game's window is not focused. 0: only while it is.\n"
    "background = 1\n"
    "; rumble = 1: controllers rumble (each as strong as its own rumble below). 0: no controller rumbles.\n"
    "rumble = 1\n"
    "\n"
    "[players]\n"
    "; Keep a player for one pad: <player> = <GUID, name or type of the pad>. Other pads leave it free.\n"
    "; 2 = DualSense Wireless Controller\n"
    "\n"
    "[device default]\n"
    "; layout = positional: buttons by position, as on a Switch pad (the bottom face button is B).\n"
    ";          labels: by the letter printed on the pad (an Xbox pad's A is A). PlayStation pads stay positional.\n"
    "layout = positional\n"
    "; style = auto: GameCube controllers play as GameCube controllers (the game's GameCube controls; buttons\n"
    ";        by their letters, analog L and R), every other pad as a Pro Controller.\n"
    ";        gamecube: play as a GameCube controller (the triggers are L and R, the bumpers Z). pro: Pro Controller.\n"
    "style = auto\n"
    "; deadzone = 0-50, percent of the stick's travel ignored around the centre. 0: only the game's own.\n"
    "deadzone = 0\n"
    "; rumble = 0-100, percent strength. 0: off.\n"
    "rumble = 100\n"
    "; trigger_threshold = 1-99, percent: how far a trigger is pulled to press ZL / ZR. (As a GameCube controller,\n"
    ";                     the game itself decides how far L and R are pulled.)\n"
    "trigger_threshold = 25\n"
    "; Remap: <console button> = <pad control>[, <pad control> ...]\n"
    ";   console buttons: A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK UP DOWN LEFT RIGHT\n"
    ";   pad controls:    south east west north leftshoulder rightshoulder lefttrigger righttrigger start back\n"
    ";                    guide leftstick rightstick dpup dpdown dpleft dpright misc1 leftpaddle1 rightpaddle1\n"
    ";                    leftpaddle2 rightpaddle2 touchpad misc3 misc4 none\n"
    ";   (a GameCube controller's Z is rightshoulder; misc3 and misc4 are the clicks at the bottom of L and R)\n"
    "; ZR = rightshoulder\n"
    "; R = righttrigger\n"
    "\n"
    "; Settings for one kind of pad, or one pad: [device <type, name or GUID>], with the keys above.\n"
    "; Types: xbox360 xboxone ps3 ps4 ps5 switchpro joyconpair gamecube ...\n"
    "; [device ps5]\n"
    "; rumble = 60\n";

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int parse_source(const char *name, Source *out) {
    if (!_stricmp(name, "none")) { out->kind = SRC_NONE; return 1; }
    if (!_stricmp(name, "lefttrigger")) { out->kind = SRC_TRIGGER; out->index = RA_LEFT_TRIGGER; return 1; }
    if (!_stricmp(name, "righttrigger")) { out->kind = SRC_TRIGGER; out->index = RA_RIGHT_TRIGGER; return 1; }
    for (int b = 0; b < RB_COUNT; b++)
        if (!_stricmp(name, RAW_BUTTON_NAMES[b])) { out->kind = SRC_BUTTON; out->index = (u8)b; return 1; }
    return 0;
}

static int npad_button(const char *name) {
    for (int b = 0; b < 16; b++) if (!_stricmp(name, NPAD_BUTTON_NAMES[b])) return b;
    return -1;
}

static void config_line(DevCfg *dc, const char *key, char *value, int line) {
    int b;
    if (!_stricmp(key, "layout")) {
        if (!_stricmp(value, "positional")) dc->layout = 0;
        else if (!_stricmp(value, "labels")) dc->layout = 1;
        else fprintf(stderr, "[input] controllers.ini line %d: layout is positional or labels, not '%s'\n", line, value);
    } else if (!_stricmp(key, "style")) {
        if (!_stricmp(value, "auto")) dc->style = 0;
        else if (!_stricmp(value, "pro")) dc->style = 1;
        else if (!_stricmp(value, "gamecube")) dc->style = 2;
        else fprintf(stderr, "[input] controllers.ini line %d: style is auto, pro or gamecube, not '%s'\n", line, value);
    } else if (!_stricmp(key, "deadzone")) dc->deadzone = atoi(value) < 0 ? 0 : atoi(value) > 50 ? 50 : atoi(value);
    else if (!_stricmp(key, "rumble")) dc->rumble = atoi(value) < 0 ? 0 : atoi(value) > 100 ? 100 : atoi(value);
    else if (!_stricmp(key, "trigger_threshold")) dc->trigger = atoi(value) < 1 ? 1 : atoi(value) > 99 ? 99 : atoi(value);
    else if ((b = npad_button(key)) >= 0) {
        dc->has_map[b] = 1;
        dc->nmap[b] = 0;
        for (char *t = strtok(value, ", \t"); t; t = strtok(NULL, ", \t")) {
            Source s = {0};
            if (!parse_source(t, &s)) fprintf(stderr, "[input] controllers.ini line %d: no pad control '%s'\n", line, t);
            else if (s.kind != SRC_NONE && dc->nmap[b] < MAXSRC) dc->map[b][dc->nmap[b]++] = s;
        }
    } else fprintf(stderr, "[input] controllers.ini line %d: unknown setting '%s'\n", line, key);
}

static void config_parse(FILE *f) {
    char buf[512];
    int section = 0, line = 0;   /* 0 none, 1 general, 2 players, 3 device */
    DevCfg *dc = NULL;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        char *s = trim(buf);
        if (*s == ';' || *s == '#' || !*s) continue;
        for (char *p = s; *p; p++)   /* a comment after a value */
            if ((*p == ';' || *p == '#') && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; s = trim(s); break; }
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) *end = 0;
            char *name = trim(s + 1);
            section = 0;
            if (!_stricmp(name, "general")) section = 1;
            else if (!_stricmp(name, "players")) section = 2;
            else if (!_strnicmp(name, "device", 6) && (name[6] == ' ' || name[6] == '\t' || !name[6])) {
                char *what = trim(name + 6);
                section = 3;
                if (!*what || !_stricmp(what, "default")) dc = &cfg.def;
                else if (cfg.ndev < (int)(sizeof cfg.dev / sizeof *cfg.dev)) {
                    dc = &cfg.dev[cfg.ndev++];
                    memset(dc, 0, sizeof *dc);
                    dc->layout = dc->style = dc->deadzone = dc->rumble = dc->trigger = -1;
                    snprintf(dc->match, sizeof dc->match, "%s", what);
                } else dc = NULL;
            } else fprintf(stderr, "[input] controllers.ini line %d: unknown section [%s]\n", line, name);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (section == 1 && !_stricmp(key, "keyboard_player")) {
            int n = atoi(value);
            cfg.keyboard_slot = n >= 1 && n <= PAD_SLOTS ? n - 1 : -1;
        } else if (section == 1 && !_stricmp(key, "background")) cfg.background = atoi(value) != 0;
        else if (section == 1 && !_stricmp(key, "rumble")) cfg.rumble = atoi(value) != 0;
        else if (section == 2) {
            int n = atoi(key);
            if (n >= 1 && n <= PAD_SLOTS) snprintf(cfg.pin[n - 1], sizeof cfg.pin[n - 1], "%s", value);
            else fprintf(stderr, "[input] controllers.ini line %d: players are 1-8, not '%s'\n", line, key);
        } else if (section == 3 && dc) config_line(dc, key, value, line);
        else if (section) fprintf(stderr, "[input] controllers.ini line %d: unknown setting '%s'\n", line, key);
    }
}

static void config_load(void) {
    const char *env = getenv("SSBU_CONTROLLERS");
    snprintf(cfg.path, sizeof cfg.path, "%s", env && *env ? env : "controllers.ini");
    FILE *f = fopen(cfg.path, "r");
    if (!f && !(env && *env)) {
        FILE *w = fopen(cfg.path, "w");
        if (w) { fputs(DEFAULT_INI, w); fclose(w); fprintf(stderr, "[input] wrote the default %s\n", cfg.path); }
        f = fopen(cfg.path, "r");
    }
    if (f) { config_parse(f); fclose(f); }
}

/* ---- devices ------------------------------------------------------------------------- */
typedef struct Dev {
    int used, slot;               /* slot -1: no free player */
    int xinput;                   /* XInput user index, or -1 for an SDL pad */
    int usb;                      /* hle_padusb.c's port + 1, or 0 */
#ifdef SSBU_HAVE_SDL3
    SDL_JoystickID id;
    SDL_Gamepad *gp;
#elif defined SSBU_HAVE_SDL2
    SDL_JoystickID id;
    SDL_GameController *gp;
#endif
    char guid[40], name[96], type[24], serial[64], path[192];
    int deadzone, rumble, trigger, layout;
    int gc;                       /* plays as a GameCube controller */
    int labels[4], has_labels;    /* the letters on its face buttons (dev_configure) */
    char section[96];             /* the [device ...] it takes settings from ("" = only default) */
    Mapping map;
    u16 sent_low, sent_high;      /* rumble last sent, and when */
    ULONGLONG sent_at;
} Dev;

static Dev devs[16];
static struct Known { char guid[40], serial[64], path[192]; int slot; } known[32];
static int nknown;

static int dev_matches(const Dev *d, const char *what) {
    return *what && (!_stricmp(what, d->guid) || !_stricmp(what, d->name) || !_stricmp(what, d->type));
}

/* A GameCube controller: SDL's type for one, or (SDL2, adapters in PC mode) a name that says so. */
static int dev_is_gamecube(const Dev *d) {
    if (!_stricmp(d->type, "gamecube")) return 1;
    for (const char *s = d->name; *s; s++)
        if (!_strnicmp(s, "gamecube", 8)) return 1;
    return 0;
}

/* The settings of one pad: built-in defaults, then [device default], then the [device ...] that
 * names it most exactly (GUID, then name, then type). `labels` gives the console button each face
 * button is printed with (N_A..N_Y), or -1 each when the pad has no such letters. */
static void dev_configure(Dev *d, const int labels[4]) {
    if (labels && labels != d->labels) { memcpy(d->labels, labels, sizeof d->labels); d->has_labels = 1; }
    else if (!labels) d->has_labels = 0;
    const DevCfg *best = NULL;
    for (int rank = 0; rank < 3 && !best; rank++)
        for (int i = 0; i < cfg.ndev && !best; i++) {
            const char *m = cfg.dev[i].match;
            if ((rank == 0 && !_stricmp(m, d->guid)) || (rank == 1 && !_stricmp(m, d->name)) || (rank == 2 && !_stricmp(m, d->type)))
                best = &cfg.dev[i];
        }
    const DevCfg *layers[2] = {&cfg.def, best};
    snprintf(d->section, sizeof d->section, "%s", best ? best->match : "");
    int layout = 0, style = 0, gamecube = dev_is_gamecube(d);
    d->deadzone = 0; d->rumble = 100; d->trigger = 25;
    for (int l = 0; l < 2; l++) {
        if (!layers[l]) continue;
        if (layers[l]->layout >= 0) layout = layers[l]->layout;
        if (layers[l]->style >= 0) style = layers[l]->style;
        if (layers[l]->deadzone >= 0) d->deadzone = layers[l]->deadzone;
        if (layers[l]->rumble >= 0) d->rumble = layers[l]->rumble;
        if (layers[l]->trigger >= 0) d->trigger = layers[l]->trigger;
    }
    /* positional: the console's A is the right face button, B the bottom, X the top, Y the left */
    static const struct { int npad; Source src; } POSITIONAL[16] = {
        {N_A, {SRC_BUTTON, RB_EAST}}, {N_B, {SRC_BUTTON, RB_SOUTH}}, {N_X, {SRC_BUTTON, RB_NORTH}}, {N_Y, {SRC_BUTTON, RB_WEST}},
        {N_LSTICK, {SRC_BUTTON, RB_LEFT_STICK}}, {N_RSTICK, {SRC_BUTTON, RB_RIGHT_STICK}},
        {N_L, {SRC_BUTTON, RB_LEFT_SHOULDER}}, {N_R, {SRC_BUTTON, RB_RIGHT_SHOULDER}},
        {N_ZL, {SRC_TRIGGER, RA_LEFT_TRIGGER}}, {N_ZR, {SRC_TRIGGER, RA_RIGHT_TRIGGER}},
        {N_PLUS, {SRC_BUTTON, RB_START}}, {N_MINUS, {SRC_BUTTON, RB_BACK}},
        {N_LEFT, {SRC_BUTTON, RB_DPAD_LEFT}}, {N_UP, {SRC_BUTTON, RB_DPAD_UP}},
        {N_RIGHT, {SRC_BUTTON, RB_DPAD_RIGHT}}, {N_DOWN, {SRC_BUTTON, RB_DPAD_DOWN}},
    };
    d->layout = layout;
    memset(&d->map, 0, sizeof d->map);
    for (int i = 0; i < 16; i++) { d->map.src[POSITIONAL[i].npad][0] = POSITIONAL[i].src; d->map.n[POSITIONAL[i].npad] = 1; }
    /* A GameCube controller goes by its letters: SDL puts A at the bottom, X right, B left, Y on top. */
    static const int GAMECUBE_LABELS[4] = {N_A, N_X, N_B, N_Y};
    int lettered = labels && labels[0] >= 0 && labels[1] >= 0 && labels[2] >= 0 && labels[3] >= 0;
    if (gamecube && !lettered) { labels = GAMECUBE_LABELS; lettered = 1; }
    if ((layout == 1 || gamecube) && lettered)
        for (int f = 0; f < 4; f++) {   /* faces in RB_SOUTH..RB_NORTH order */
            d->map.src[labels[f]][0] = (Source){SRC_BUTTON, (u8)(RB_SOUTH + f)};
            d->map.n[labels[f]] = 1;
        }
    /* As a GameCube controller, L and R are the triggers (with the clicks at their bottoms), and Z
     * is the console's ZR, as a GameCube npad reports it: the right bumper (a GameCube controller's
     * only one), or the left one on other pads. There is no ZL. */
    d->gc = style == 2 || (style == 0 && gamecube);
    if (d->gc) {
        d->map.src[N_L][0] = (Source){SRC_TRIGGER, RA_LEFT_TRIGGER}; d->map.src[N_L][1] = (Source){SRC_BUTTON, RB_MISC3};
        d->map.src[N_R][0] = (Source){SRC_TRIGGER, RA_RIGHT_TRIGGER}; d->map.src[N_R][1] = (Source){SRC_BUTTON, RB_MISC4};
        d->map.src[N_ZR][0] = (Source){SRC_BUTTON, RB_RIGHT_SHOULDER}; d->map.src[N_ZR][1] = (Source){SRC_BUTTON, RB_LEFT_SHOULDER};
        d->map.n[N_L] = d->map.n[N_R] = d->map.n[N_ZR] = 2;
        d->map.n[N_ZL] = 0;
    }
    for (int l = 0; l < 2; l++)
        for (int b = 0; layers[l] && b < 16; b++)
            if (layers[l]->has_map[b]) {
                memcpy(d->map.src[b], layers[l]->map[b], sizeof d->map.src[b]);
                d->map.n[b] = layers[l]->nmap[b];
            }
}

/* Which player a newly connected pad becomes; -1 when all are taken. */
static int dev_assign(const Dev *d) {
    int taken[PAD_SLOTS] = {0};
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++)
        if (devs[i].used && &devs[i] != d && devs[i].slot >= 0) taken[devs[i].slot] = 1;
    for (int s = 0; s < PAD_SLOTS; s++)
        if (!taken[s] && dev_matches(d, cfg.pin[s])) return s;
    for (int pass = 0; pass < 3; pass++)
        for (int k = 0; k < nknown; k++) {
            const struct Known *e = &known[k];
            if (strcmp(e->guid, d->guid) || taken[e->slot]) continue;
            if ((pass == 0 && *d->serial && !strcmp(e->serial, d->serial)) || (pass == 1 && *d->path && !strcmp(e->path, d->path)) ||
                (pass == 2 && !*cfg.pin[e->slot]))
                return e->slot;
        }
    for (int s = 0; s < PAD_SLOTS; s++)
        if (!taken[s] && !*cfg.pin[s]) return s;
    return -1;
}

static void dev_remember(const Dev *d) {
    for (int k = 0; k < nknown; k++)
        if (!strcmp(known[k].guid, d->guid) && known[k].slot == d->slot) {
            snprintf(known[k].serial, sizeof known[k].serial, "%s", d->serial);
            snprintf(known[k].path, sizeof known[k].path, "%s", d->path);
            return;
        }
    struct Known *e = &known[nknown < (int)(sizeof known / sizeof *known) ? nknown++ : (int)(sizeof known / sizeof *known) - 1];
    snprintf(e->guid, sizeof e->guid, "%s", d->guid);
    snprintf(e->serial, sizeof e->serial, "%s", d->serial);
    snprintf(e->path, sizeof e->path, "%s", d->path);
    e->slot = d->slot;
}

/* A pad's raw report in the console's terms. */
static void stick(int deadzone, i32 x, i32 y_down, i32 *ox, i32 *oy) {
    double fx = x, fy = -(double)y_down, r = sqrt(fx * fx + fy * fy), max = 32767.0, dz = deadzone / 100.0 * max;
    if (r <= dz || r == 0) { *ox = *oy = 0; return; }
    double scale = dz > 0 ? (r - dz) / (max - dz) * max / r : 1.0;
    if (r * scale > max) scale = max / r;   /* inside the circle, as the console reports */
    *ox = (i32)lround(fx * scale);
    *oy = (i32)lround(fy * scale);
}

/* As a GameCube controller, L and R are analog: the furthest-pulled of their controls (a button
 * counts as fully pulled). The game makes the buttons out of them itself. */
static i32 dev_trigger(const Dev *d, const Raw *raw, int b) {
    i32 v = 0;
    for (int j = 0; j < d->map.n[b]; j++) {
        Source s = d->map.src[b][j];
        i32 t = s.kind == SRC_TRIGGER ? raw->axis[s.index] : s.kind == SRC_BUTTON && raw->buttons >> s.index & 1 ? 32767 : 0;
        if (t > v) v = t > 32767 ? 32767 : t;
    }
    return v;
}

static void dev_state(const Dev *d, const Raw *raw, PadState *o) {
    i32 threshold = d->trigger * 32767 / 100;
    o->connected = 1;
    o->gc = (u32)d->gc;
    o->buttons = 0;
    o->lt = d->gc ? dev_trigger(d, raw, N_L) : 0;
    o->rt = d->gc ? dev_trigger(d, raw, N_R) : 0;
    for (int b = 0; b < 16; b++)
        for (int j = 0; j < d->map.n[b] && !(d->gc && (b == N_L || b == N_R)); j++) {
            Source s = d->map.src[b][j];
            if ((s.kind == SRC_BUTTON && raw->buttons >> s.index & 1) || (s.kind == SRC_TRIGGER && raw->axis[s.index] >= threshold)) {
                o->buttons |= 1ull << b;
                break;
            }
        }
    stick(d->deadzone, raw->axis[RA_LEFTX], raw->axis[RA_LEFTY], &o->lx, &o->ly);
    stick(d->deadzone, raw->axis[RA_RIGHTX], raw->axis[RA_RIGHTY], &o->rx, &o->ry);
}

/* ---- the slot table, shared with guest threads ------------------------------------- */
static SRWLOCK lock = SRWLOCK_INIT;
static PadState slots[PAD_SLOTS];
static char slot_names[PAD_SLOTS][48];
static PadInfo slot_info[PAD_SLOTS];
static float rumble_amp[PAD_SLOTS][2];
static ULONGLONG rumble_at[PAD_SLOTS];

/* What the settings menu shows of a pad, and its controls as pressed now (PAD_RAW_*). */
static void dev_info(const Dev *d, const Raw *raw, PadInfo *o) {
    o->connected = 1;
    o->slot = d->slot;
    snprintf(o->name, sizeof o->name, "%s", d->name);
    snprintf(o->type, sizeof o->type, "%s", d->type);
    snprintf(o->guid, sizeof o->guid, "%s", d->guid);
    snprintf(o->section, sizeof o->section, "%s", d->section);
    o->layout = d->layout; o->deadzone = d->deadzone; o->rumble = d->rumble; o->trigger = d->trigger;
    o->xinput = d->xinput >= 0;
    for (int b = 0; b < 16; b++) {
        int n = 0;
        o->map[b][0] = 0;
        for (int j = 0; j < d->map.n[b]; j++) {
            Source src = d->map.src[b][j];
            const char *name = src.kind == SRC_TRIGGER ? (src.index == RA_LEFT_TRIGGER ? "lefttrigger" : "righttrigger") : RAW_BUTTON_NAMES[src.index];
            n += snprintf(o->map[b] + n, sizeof o->map[b] - (size_t)n, "%s%s", n ? ", " : "", name);
            if (n >= (int)sizeof o->map[b]) break;
        }
        if (!o->map[b][0]) snprintf(o->map[b], sizeof o->map[b], "none");
    }
    o->raw = raw->buttons & ((1u << RB_COUNT) - 1);
    if (raw->axis[RA_LEFT_TRIGGER] > 16384) o->raw |= PAD_RAW_LEFT_TRIGGER;
    if (raw->axis[RA_RIGHT_TRIGGER] > 16384) o->raw |= PAD_RAW_RIGHT_TRIGGER;
}

static void publish(const PadState fresh[PAD_SLOTS], const char names[PAD_SLOTS][48], const PadInfo info[PAD_SLOTS]) {
    AcquireSRWLockExclusive(&lock);
    memcpy(slots, fresh, sizeof slots);
    memcpy(slot_names, names, sizeof slot_names);
    memcpy(slot_info, info, sizeof slot_info);
    ReleaseSRWLockExclusive(&lock);
}

/* What a pad's motors should do now: the game's amplitude per side (left = the low-frequency,
 * heavy motor of an Xbox-style pad), scaled by the pad's strength; nothing when the game has not
 * sent a value for 3 s. Returns whether it should be sent now: on a change (at most every 10 ms)
 * and every 100 ms while running, as each send lasts 200 ms. */
static int rumble_due(Dev *d, ULONGLONG now, u16 *low, u16 *high) {
    float l, r;
    ULONGLONG at;
    AcquireSRWLockShared(&lock);
    l = rumble_amp[d->slot][0]; r = rumble_amp[d->slot][1]; at = rumble_at[d->slot];
    ReleaseSRWLockShared(&lock);
    if (now - at > 3000 || !cfg.rumble) l = r = 0;
    l = l < 0 ? 0 : l > 1 ? 1 : l;
    r = r < 0 ? 0 : r > 1 ? 1 : r;
    *low = (u16)(l * 65535.0f * d->rumble / 100);
    *high = (u16)(r * 65535.0f * d->rumble / 100);
    int changed = *low != d->sent_low || *high != d->sent_high;
    if ((changed && now - d->sent_at >= 10) || ((*low || *high) && now - d->sent_at >= 100)) {
        d->sent_low = *low; d->sent_high = *high; d->sent_at = now;
        return 1;
    }
    return 0;
}

static void log_connect(const Dev *d) {
    if (d->slot >= 0)
        fprintf(stderr, "[input] player %d: \"%s\" (%s, GUID %s)%s\n", d->slot + 1, d->name, d->type, d->guid,
                d->gc ? ", as a GameCube controller" : "");
    else fprintf(stderr, "[input] \"%s\" (%s) connected, but all 8 players are taken\n", d->name, d->type);
}

/* ---- XInput, when SDL3 is missing ------------------------------------------------------ */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef struct { WORD left, right; } XVibration;
static DWORD (WINAPI *xinput_get_state)(DWORD, XState *);
static DWORD (WINAPI *xinput_set_state)(DWORD, XVibration *);

static void xinput_raw(const XPad *p, Raw *raw) {
    static const struct { WORD x; int rb; } MAP[] = {
        {0x1000, RB_SOUTH}, {0x2000, RB_EAST}, {0x4000, RB_WEST}, {0x8000, RB_NORTH}, {0x0020, RB_BACK}, {0x0010, RB_START},
        {0x0040, RB_LEFT_STICK}, {0x0080, RB_RIGHT_STICK}, {0x0100, RB_LEFT_SHOULDER}, {0x0200, RB_RIGHT_SHOULDER},
        {0x0001, RB_DPAD_UP}, {0x0002, RB_DPAD_DOWN}, {0x0004, RB_DPAD_LEFT}, {0x0008, RB_DPAD_RIGHT},
    };
    memset(raw, 0, sizeof *raw);
    for (u32 i = 0; i < sizeof MAP / sizeof *MAP; i++)
        if (p->buttons & MAP[i].x) raw->buttons |= 1u << MAP[i].rb;
    raw->axis[RA_LEFTX] = p->lx;
    raw->axis[RA_LEFTY] = p->ly == -32768 ? 32767 : -p->ly;   /* XInput's +y is up */
    raw->axis[RA_RIGHTX] = p->rx;
    raw->axis[RA_RIGHTY] = p->ry == -32768 ? 32767 : -p->ry;
    raw->axis[RA_LEFT_TRIGGER] = p->lt * 32767 / 255;
    raw->axis[RA_RIGHT_TRIGGER] = p->rt * 32767 / 255;
}

static int xinput_start(void) {
#ifdef _WIN32
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) {
        xinput_get_state = (DWORD (WINAPI *)(DWORD, XState *))(void *)GetProcAddress(m, "XInputGetState");
        xinput_set_state = (DWORD (WINAPI *)(DWORD, XVibration *))(void *)GetProcAddress(m, "XInputSetState");
    }
#endif
    return xinput_get_state != NULL;
}

/* Pads 0-3 are players 1-4. A missing pad is asked for again every half second only: XInput is
 * slow to answer for an empty user index. */
static void xinput_poll(PadState fresh[PAD_SLOTS], char names[PAD_SLOTS][48], PadInfo info[PAD_SLOTS]) {
    static ULONGLONG retry[4];
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 4; i++) {
        Dev *d = &devs[i];
        XState s;
        if (!d->used && now < retry[i]) continue;
        if (xinput_get_state((DWORD)i, &s)) {
            if (d->used) { fprintf(stderr, "[input] player %d: \"%s\" disconnected\n", d->slot + 1, d->name); d->used = 0; }
            retry[i] = now + 500;
            continue;
        }
        if (!d->used) {
            memset(d, 0, sizeof *d);
            d->used = 1; d->xinput = i; d->slot = i;
            snprintf(d->name, sizeof d->name, "XInput pad %d", i + 1);
            snprintf(d->type, sizeof d->type, "xinput");
            snprintf(d->guid, sizeof d->guid, "xinput%d", i);
            dev_configure(d, NULL);
            log_connect(d);
        }
        Raw raw;
        xinput_raw(&s.pad, &raw);
        dev_state(d, &raw, &fresh[i]);
        dev_info(d, &raw, &info[i]);
        snprintf(names[i], sizeof names[i], "%s", d->name);
        u16 low, high;
        if (xinput_set_state && rumble_due(d, now, &low, &high)) { XVibration v = {low, high}; xinput_set_state((DWORD)i, &v); }
    }
}

/* ---- GameCube controllers straight from USB (hle_padusb.c) ---------------------------------
 * Its ports become pads as SDL's do; they go by their letters (A at the bottom, X right, B left,
 * Y on top, as SDL puts a GameCube controller's) and play as GameCube controllers (style = auto). */
static const char *usb_ignore;   /* what SDL leaves to it (padusb_start), or NULL */

static void usb_raw(const PadUsbState *s, Raw *raw) {
    static const struct { u32 pu; int rb; } MAP[] = {
        {PU_A, RB_SOUTH}, {PU_B, RB_WEST}, {PU_X, RB_EAST}, {PU_Y, RB_NORTH}, {PU_START, RB_START}, {PU_Z, RB_RIGHT_SHOULDER},
        {PU_ZL, RB_LEFT_SHOULDER}, {PU_L, RB_MISC3}, {PU_R, RB_MISC4}, {PU_UP, RB_DPAD_UP}, {PU_DOWN, RB_DPAD_DOWN},
        {PU_LEFT, RB_DPAD_LEFT}, {PU_RIGHT, RB_DPAD_RIGHT}, {PU_HOME, RB_GUIDE}, {PU_CAPTURE, RB_MISC1}, {PU_C, RB_MISC2},
    };
    memset(raw, 0, sizeof *raw);
    for (u32 i = 0; i < sizeof MAP / sizeof *MAP; i++)
        if (s->buttons & MAP[i].pu) raw->buttons |= 1u << MAP[i].rb;
    raw->axis[RA_LEFTX] = s->lx; raw->axis[RA_LEFTY] = s->ly;
    raw->axis[RA_RIGHTX] = s->rx; raw->axis[RA_RIGHTY] = s->ry;
    raw->axis[RA_LEFT_TRIGGER] = s->lt; raw->axis[RA_RIGHT_TRIGGER] = s->rt;
}

static void usb_poll(PadState fresh[PAD_SLOTS], char names[PAD_SLOTS][48], PadInfo info[PAD_SLOTS]) {
    static PadUsbState ports[PADUSB_PORTS];
    static const int LETTERS[4] = {N_A, N_X, N_B, N_Y};
    ULONGLONG now = GetTickCount64();
    padusb_poll(ports);
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {   /* gone */
        Dev *d = &devs[i];
        if (!d->used || !d->usb || ports[d->usb - 1].connected) continue;
        if (d->slot >= 0) fprintf(stderr, "[input] player %d: \"%s\" disconnected\n", d->slot + 1, d->name);
        d->used = 0;
    }
    for (int p = 0; p < PADUSB_PORTS; p++) {   /* came (devs 0-3 are XInput's when there is no SDL) */
        int open = !ports[p].connected;
        for (int i = 0; i < (int)(sizeof devs / sizeof *devs) && !open; i++) open = devs[i].used && devs[i].usb == p + 1;
        Dev *d = NULL;
        for (int i = 4; i < (int)(sizeof devs / sizeof *devs) && !open && !d; i++) if (!devs[i].used) d = &devs[i];
        if (!d) continue;
        memset(d, 0, sizeof *d);
        d->used = 1; d->xinput = -1; d->usb = p + 1;
        snprintf(d->name, sizeof d->name, "%s", ports[p].name);
        snprintf(d->type, sizeof d->type, "gamecube");
        snprintf(d->guid, sizeof d->guid, "%s", ports[p].id);
        snprintf(d->path, sizeof d->path, "%s", ports[p].path);
        dev_configure(d, LETTERS);
        d->slot = dev_assign(d);
        if (d->slot >= 0) dev_remember(d);
        log_connect(d);
    }
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
        Dev *d = &devs[i];
        if (!d->used || !d->usb) continue;
        if (d->slot < 0 && (d->slot = dev_assign(d)) >= 0) { dev_remember(d); log_connect(d); }   /* a player became free */
        if (d->slot < 0) continue;
        Raw raw;
        usb_raw(&ports[d->usb - 1], &raw);
        dev_state(d, &raw, &fresh[d->slot]);
        dev_info(d, &raw, &info[d->slot]);
        snprintf(names[d->slot], sizeof names[d->slot], "%s", d->name);
        u16 low, high;
        if (rumble_due(d, now, &low, &high)) padusb_rumble(d->usb - 1, low || high);
    }
}

/* ---- SDL3 ---------------------------------------------------------------------------- */
#ifdef SSBU_HAVE_SDL3
#define SDL_FUNCS(X) X(SDL_SetHint) X(SDL_Init) X(SDL_GetError) X(SDL_free) X(SDL_GetVersion) \
    X(SDL_AddGamepadMappingsFromFile) X(SDL_SetGamepadEventsEnabled) X(SDL_SetJoystickEventsEnabled) X(SDL_UpdateGamepads) \
    X(SDL_GetGamepads) X(SDL_OpenGamepad) X(SDL_CloseGamepad) X(SDL_GamepadConnected) X(SDL_GetGamepadGUIDForID) \
    X(SDL_GUIDToString) X(SDL_GetGamepadSerial) X(SDL_GetGamepadPath) X(SDL_GetGamepadName) X(SDL_GetGamepadType) \
    X(SDL_GetGamepadStringForType) X(SDL_GetGamepadButton) X(SDL_GetGamepadAxis) X(SDL_GetGamepadButtonLabel) \
    X(SDL_RumbleGamepad) X(SDL_SetGamepadPlayerIndex)
#define SDL_DECLARE(f) static __typeof__(f) *p_##f;
SDL_FUNCS(SDL_DECLARE)

static int sdl_start(void) {
    const char *backend = getenv("SSBU_PAD_BACKEND");
    if (backend && !_stricmp(backend, "xinput")) return 0;
#ifdef __APPLE__
#define SDL_LINK(f) p_##f = f;   /* (linked: the app's own SDL3.framework) */
    SDL_FUNCS(SDL_LINK)
#else
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    if (!n || n + 9 >= MAX_PATH) return 0;
    wcscpy(path + n, L"SDL3.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) { fprintf(stderr, "[input] no SDL3.dll beside the exe: XInput pads only\n"); return 0; }
    const char *missing = NULL;
#define SDL_LOAD(f) if (!(p_##f = (__typeof__(f) *)(void *)GetProcAddress(m, #f)) && !missing) missing = #f;
    SDL_FUNCS(SDL_LOAD)
    if (missing) { fprintf(stderr, "[input] SDL3.dll has no %s: XInput pads only\n", missing); return 0; }
#endif
    p_SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (usb_ignore) p_SDL_SetHint(SDL_HINT_HIDAPI_IGNORE_DEVICES, usb_ignore);       /* hle_padusb.c reads those */
    if (usb_ignore) p_SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, usb_ignore);
    p_SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* we decide (background = ...) */
    p_SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "auto");       /* PlayStation pads change mode only for rumble */
    if (!p_SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[input] SDL_Init failed (%s): XInput pads only\n", p_SDL_GetError());
        return 0;
    }
    p_SDL_SetGamepadEventsEnabled(false);   /* nothing reads SDL's event queue: read the state instead */
    p_SDL_SetJoystickEventsEnabled(false);
    int mapped = p_SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
    int v = p_SDL_GetVersion();
    fprintf(stderr, "[input] SDL %d.%d.%d%s\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
            mapped > 0 ? ", with gamecontrollerdb.txt" : "");
    return 1;
}

static void sdl_open(SDL_JoystickID id) {
    Dev *d = NULL;
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs) && !d; i++) if (!devs[i].used) d = &devs[i];
    if (!d) return;
    SDL_Gamepad *gp = p_SDL_OpenGamepad(id);
    if (!gp) return;
    memset(d, 0, sizeof *d);
    d->used = 1; d->xinput = -1; d->id = id; d->gp = gp;
    p_SDL_GUIDToString(p_SDL_GetGamepadGUIDForID(id), d->guid, sizeof d->guid);
    const char *s;
    snprintf(d->name, sizeof d->name, "%s", (s = p_SDL_GetGamepadName(gp)) ? s : "gamepad");
    snprintf(d->type, sizeof d->type, "%s", (s = p_SDL_GetGamepadStringForType(p_SDL_GetGamepadType(gp))) ? s : "gamepad");
    snprintf(d->serial, sizeof d->serial, "%s", (s = p_SDL_GetGamepadSerial(gp)) ? s : "");
    snprintf(d->path, sizeof d->path, "%s", (s = p_SDL_GetGamepadPath(gp)) ? s : "");
    int labels[4];
    for (int f = 0; f < 4; f++) {
        switch (p_SDL_GetGamepadButtonLabel(gp, (SDL_GamepadButton)(SDL_GAMEPAD_BUTTON_SOUTH + f))) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: labels[f] = N_A; break;
        case SDL_GAMEPAD_BUTTON_LABEL_B: labels[f] = N_B; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: labels[f] = N_X; break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y: labels[f] = N_Y; break;
        default: labels[f] = -1;
        }
    }
    dev_configure(d, labels);
    d->slot = dev_assign(d);
    if (d->slot >= 0) { dev_remember(d); p_SDL_SetGamepadPlayerIndex(gp, d->slot); }
    log_connect(d);
}

/* Pads that came and went (every 100 ms), then every pad's state. */
static void sdl_poll(PadState fresh[PAD_SLOTS], char names[PAD_SLOTS][48], PadInfo info[PAD_SLOTS]) {
    static ULONGLONG scanned;
    ULONGLONG now = GetTickCount64();
    p_SDL_UpdateGamepads();
    if (now - scanned >= 100) {
        scanned = now;
        int n = 0;
        SDL_JoystickID *ids = p_SDL_GetGamepads(&n);
        for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
            Dev *d = &devs[i];
            if (!d->used || d->usb) continue;
            int present = 0;
            for (int k = 0; k < n && !present; k++) present = ids[k] == d->id;
            if (present && p_SDL_GamepadConnected(d->gp)) continue;
            if (d->slot >= 0) fprintf(stderr, "[input] player %d: \"%s\" disconnected\n", d->slot + 1, d->name);
            p_SDL_CloseGamepad(d->gp);
            d->used = 0;
        }
        for (int k = 0; k < n; k++) {
            int open = 0;
            for (int i = 0; i < (int)(sizeof devs / sizeof *devs) && !open; i++) open = devs[i].used && devs[i].id == ids[k];
            if (!open) sdl_open(ids[k]);
        }
        for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++)   /* a player became free */
            if (devs[i].used && !devs[i].usb && devs[i].slot < 0 && (devs[i].slot = dev_assign(&devs[i])) >= 0) {
                dev_remember(&devs[i]);
                p_SDL_SetGamepadPlayerIndex(devs[i].gp, devs[i].slot);
                log_connect(&devs[i]);
            }
        if (ids) p_SDL_free(ids);
    }
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
        Dev *d = &devs[i];
        if (!d->used || d->usb || d->slot < 0) continue;
        Raw raw = {0};
        for (int b = 0; b < RB_COUNT; b++)
            if (p_SDL_GetGamepadButton(d->gp, (SDL_GamepadButton)b)) raw.buttons |= 1u << b;
        for (int a = 0; a < RA_COUNT; a++) raw.axis[a] = p_SDL_GetGamepadAxis(d->gp, (SDL_GamepadAxis)a);
        dev_state(d, &raw, &fresh[d->slot]);
        dev_info(d, &raw, &info[d->slot]);
        snprintf(names[d->slot], sizeof names[d->slot], "%s", d->name);
        u16 low, high;
        if (rumble_due(d, now, &low, &high)) p_SDL_RumbleGamepad(d->gp, low, high, low || high ? 200 : 0);
    }
}
#elif defined SSBU_HAVE_SDL2
/* ---- SDL2 (Linux) ----------------------------------------------------------------------
 * The main thread starts SDL with the window and runs its event loop (linux/window.c), with
 * controller and joystick events off and SDL_AUTO_UPDATE_JOYSTICKS=0: that loop leaves the pads
 * alone, and SDL_GameControllerUpdate here reads them and finds the ones that came and went.
 * SDL2's joystick calls take its joystick lock, so this thread may make them. Face buttons are
 * positional (SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0): A is the bottom one, as SDL3's SOUTH. */
static const char *(*p_SDL_GameControllerPath)(SDL_GameController *);   /* (SDL 2.24 and later) */

static int sdl_start(void) {
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER) && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "[input] SDL could not start its game controllers (%s): keyboard only\n", SDL_GetError());
        return 0;
    }
    SDL_GameControllerEventState(SDL_IGNORE);   /* (window.c says so too; this is in case it has no window) */
    SDL_JoystickEventState(SDL_IGNORE);
    p_SDL_GameControllerPath = (const char *(*)(SDL_GameController *))dlsym(RTLD_DEFAULT, "SDL_GameControllerPath");
    int mapped = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    SDL_version v;
    SDL_GetVersion(&v);
    fprintf(stderr, "[input] SDL %d.%d.%d%s\n", v.major, v.minor, v.patch, mapped > 0 ? ", with gamecontrollerdb.txt" : "");
    return 1;
}

/* A pad type as SDL3 names it (controllers.ini's [device <type>]). */
static const char *sdl2_type(int type) {
    switch (type) {
    case SDL_CONTROLLER_TYPE_XBOX360: return "xbox360";
    case SDL_CONTROLLER_TYPE_XBOXONE: return "xboxone";
    case SDL_CONTROLLER_TYPE_PS3: return "ps3";
    case SDL_CONTROLLER_TYPE_PS4: return "ps4";
    case SDL_CONTROLLER_TYPE_PS5: return "ps5";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "switchpro";
    case 11: return "joyconleft";   /* (SDL 2.24's NINTENDO_SWITCH_JOYCON_LEFT, _RIGHT, _PAIR) */
    case 12: return "joyconright";
    case 13: return "joyconpair";
    default: return "standard";
    }
}

static void sdl_open(int index) {
    Dev *d = NULL;
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs) && !d; i++) if (!devs[i].used) d = &devs[i];
    if (!d) return;
    SDL_GameController *gp = SDL_GameControllerOpen(index);
    if (!gp) return;
    memset(d, 0, sizeof *d);
    d->used = 1; d->xinput = -1; d->gp = gp;
    d->id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gp));
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), d->guid, sizeof d->guid);
    const char *s;
    int type = SDL_GameControllerGetType(gp);
    snprintf(d->name, sizeof d->name, "%s", (s = SDL_GameControllerName(gp)) ? s : "gamepad");
    snprintf(d->type, sizeof d->type, "%s", sdl2_type(type));
    snprintf(d->serial, sizeof d->serial, "%s", (s = SDL_GameControllerGetSerial(gp)) ? s : "");
    snprintf(d->path, sizeof d->path, "%s", p_SDL_GameControllerPath && (s = p_SDL_GameControllerPath(gp)) ? s : "");
    /* the letters on the face buttons (south, east, west, north), as SDL3 gives them by type (SDL2's
     * GameCube driver, positional, puts X right and B left of A) */
    static const int XBOX[4] = {N_A, N_B, N_X, N_Y}, NINTENDO[4] = {N_B, N_A, N_Y, N_X}, NONE[4] = {-1, -1, -1, -1},
                     GAMECUBE[4] = {N_A, N_X, N_B, N_Y};
    const int *labels = dev_is_gamecube(d) ? GAMECUBE :
                        type == SDL_CONTROLLER_TYPE_PS3 || type == SDL_CONTROLLER_TYPE_PS4 || type == SDL_CONTROLLER_TYPE_PS5 ? NONE :
                        type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO || (type >= 11 && type <= 13) ? NINTENDO : XBOX;
    dev_configure(d, labels);
    d->slot = dev_assign(d);
    if (d->slot >= 0) { dev_remember(d); SDL_GameControllerSetPlayerIndex(gp, d->slot); }
    log_connect(d);
}

/* Pads that came and went (every 100 ms), then every pad's state. */
static void sdl_poll(PadState fresh[PAD_SLOTS], char names[PAD_SLOTS][48], PadInfo info[PAD_SLOTS]) {
    static ULONGLONG scanned;
    ULONGLONG now = GetTickCount64();
    SDL_GameControllerUpdate();
    if (now - scanned >= 100) {
        scanned = now;
        int n = SDL_NumJoysticks();
        for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
            Dev *d = &devs[i];
            if (!d->used || d->usb || SDL_GameControllerGetAttached(d->gp)) continue;
            if (d->slot >= 0) fprintf(stderr, "[input] player %d: \"%s\" disconnected\n", d->slot + 1, d->name);
            SDL_GameControllerClose(d->gp);
            d->used = 0;
        }
        for (int k = 0; k < n; k++) {
            if (!SDL_IsGameController(k)) continue;
            SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(k);
            int open = 0;
            for (int i = 0; i < (int)(sizeof devs / sizeof *devs) && !open; i++) open = devs[i].used && devs[i].id == id;
            if (!open) sdl_open(k);
        }
        for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++)   /* a player became free */
            if (devs[i].used && !devs[i].usb && devs[i].slot < 0 && (devs[i].slot = dev_assign(&devs[i])) >= 0) {
                dev_remember(&devs[i]);
                SDL_GameControllerSetPlayerIndex(devs[i].gp, devs[i].slot);
                log_connect(&devs[i]);
            }
    }
    for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
        Dev *d = &devs[i];
        if (!d->used || d->usb || d->slot < 0) continue;
        Raw raw = {0};
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < RB_COUNT; b++)
            if (SDL_GameControllerGetButton(d->gp, (SDL_GameControllerButton)b)) raw.buttons |= 1u << b;
        for (int a = 0; a < RA_COUNT; a++) raw.axis[a] = SDL_GameControllerGetAxis(d->gp, (SDL_GameControllerAxis)a);
        dev_state(d, &raw, &fresh[d->slot]);
        dev_info(d, &raw, &info[d->slot]);
        snprintf(names[d->slot], sizeof names[d->slot], "%s", d->name);
        u16 low, high;
        if (rumble_due(d, now, &low, &high)) SDL_GameControllerRumble(d->gp, low, high, low || high ? 200 : 0);
    }
}
#else
static int sdl_start(void) { return 0; }
static void sdl_poll(PadState fresh[PAD_SLOTS], char names[PAD_SLOTS][48], PadInfo info[PAD_SLOTS]) { (void)fresh; (void)names; (void)info; }
#endif

/* ---- controllers.ini again (the settings menu) ---------------------------------------- */
static volatile LONG reload_asked;

/* On the input thread: the file read again, every pad set up again and given its player again
 * (pinned pads first, then the others in their players' order, so nobody moves without a reason). */
static void config_reload(int sdl) {
    config_defaults();
    config_load();
    Dev *order[sizeof devs / sizeof *devs];
    int n = 0;
    for (int pinned = 1; pinned >= 0; pinned--)
        for (int s = -1; s < PAD_SLOTS; s++)
            for (int i = 0; i < (int)(sizeof devs / sizeof *devs); i++) {
                Dev *d = &devs[i];
                if (!d->used || d->slot != s) continue;
                int is_pinned = 0;
                for (int k = 0; k < PAD_SLOTS && !is_pinned; k++) is_pinned = dev_matches(d, cfg.pin[k]);
                if (is_pinned == pinned) order[n++] = d;
            }
    int was[sizeof devs / sizeof *devs];
    for (int i = 0; i < n; i++) {
        was[i] = order[i]->slot;
        dev_configure(order[i], order[i]->has_labels ? order[i]->labels : NULL);
        if (order[i]->xinput < 0) order[i]->slot = -1;   /* (XInput pads keep theirs: pad n is player n) */
    }
    for (int i = 0; i < n; i++) {
        Dev *d = order[i];
        if (d->xinput >= 0) continue;
        d->slot = dev_assign(d);
        if (d->slot >= 0) dev_remember(d);
#ifdef SSBU_HAVE_SDL3
        if (sdl && d->gp && d->slot >= 0) p_SDL_SetGamepadPlayerIndex(d->gp, d->slot);
#elif defined SSBU_HAVE_SDL2
        if (sdl && d->gp && d->slot >= 0) SDL_GameControllerSetPlayerIndex(d->gp, d->slot);
#endif
        if (d->slot != was[i]) log_connect(d);
    }
    (void)sdl;
    fprintf(stderr, "[input] %s read again: %d pad%s set up again\n", cfg.path, n, n == 1 ? "" : "s");
}

void pad_reload(void) { pad_start(); InterlockedExchange(&reload_asked, 1); }
const char *pad_ini_path(void) { pad_start(); return cfg.path; }

int pad_info(u32 slot, PadInfo *out) {
    memset(out, 0, sizeof *out);
    if (slot >= PAD_SLOTS) return 0;
    pad_start();
    AcquireSRWLockShared(&lock);
    *out = slot_info[slot];
    ReleaseSRWLockShared(&lock);
    return out->connected;
}

const char *pad_raw_name(int bit) {
    if (bit == PAD_RAW_LEFT_TRIGGER_BIT) return "lefttrigger";
    if (bit == PAD_RAW_RIGHT_TRIGGER_BIT) return "righttrigger";
    return bit >= 0 && bit < RB_COUNT ? RAW_BUTTON_NAMES[bit] : "none";
}

const char *pad_button_name(int npad) { return npad >= 0 && npad < 16 ? NPAD_BUTTON_NAMES[npad] : "?"; }

/* ---- the input thread ---------------------------------------------------------------- */
static DWORD WINAPI input_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"input");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    usb_ignore = padusb_start();
    int sdl = sdl_start(), xinput = !sdl && xinput_start();
    if (!sdl && !xinput && !usb_ignore) { fprintf(stderr, "[input] no SDL and no XInput: keyboard only\n"); return 0; }
    for (;;) {
        PadState fresh[PAD_SLOTS] = {0};
        char names[PAD_SLOTS][48] = {{0}};
        static PadInfo info[PAD_SLOTS];
        memset(info, 0, sizeof info);
        if (InterlockedExchange(&reload_asked, 0)) config_reload(sdl);
        if (sdl) sdl_poll(fresh, names, info);
        else if (xinput) xinput_poll(fresh, names, info);
        if (usb_ignore) usb_poll(fresh, names, info);
        publish(fresh, names, info);
        LARGE_INTEGER due = {.QuadPart = -10000};   /* 1 ms */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 50);
        else Sleep(1);
    }
}

static BOOL CALLBACK start_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    config_load();
    CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
    return TRUE;
}

void pad_start(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, start_once, NULL, NULL);
}

int pad_get(u32 slot, PadState *out) {
    memset(out, 0, sizeof *out);
    if (slot >= PAD_SLOTS) return 0;
    pad_start();
    AcquireSRWLockShared(&lock);
    *out = slots[slot];
    ReleaseSRWLockShared(&lock);
    return (int)out->connected;
}

int pad_keyboard_slot(void) { pad_start(); return cfg.keyboard_slot; }
int pad_background(void) { pad_start(); return cfg.background; }
int pad_rumble_on(void) { pad_start(); return cfg.rumble; }

void pad_rumble(u32 slot, int side, float amplitude) {
    if (slot >= PAD_SLOTS || side < 1 || side > 2) return;
    AcquireSRWLockExclusive(&lock);
    rumble_amp[slot][side - 1] = amplitude;
    rumble_at[slot] = GetTickCount64();
    ReleaseSRWLockExclusive(&lock);
}

float pad_rumble_value(u32 slot, int side) {
    if (slot >= PAD_SLOTS || side < 1 || side > 2) return 0;
    AcquireSRWLockShared(&lock);
    float v = rumble_amp[slot][side - 1];
    ReleaseSRWLockShared(&lock);
    return v;
}

int pad_describe(char *buf, int size) {
    int n = 0;
    buf[0] = 0;
    AcquireSRWLockShared(&lock);
    for (int s = 0; s < PAD_SLOTS && n < size; s++)
        if (slots[s].connected) n += snprintf(buf + n, (size_t)(size - n), "%s%d %s", n ? ", " : "", s + 1, slot_names[s]);
    ReleaseSRWLockShared(&lock);
    return n < size ? n : size - 1;
}
