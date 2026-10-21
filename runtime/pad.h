/* Controllers for nn::hid (hle_pad.c): up to eight players' pads, read on an input thread.
 *
 * Slot n is npad id n (player n + 1). A slot's state is in the console's terms: NpadButton bits
 * A..DOWN (0-15) and sticks with +y up, inside a circle of radius 32767. A GameCube-style slot
 * (gc) also has its analog L and R triggers, 0..32767; the game makes L and R out of those. */
#pragma once
#include "host.h"

enum { PAD_SLOTS = 8 };

typedef struct PadState {
    u32 connected;            /* a device drives this slot */
    u32 gc;                   /* plays as a GameCube controller, not a Pro Controller */
    u64 buttons;
    i32 lx, ly, rx, ry;
    i32 lt, rt;               /* (gc) the analog L and R triggers */
} PadState;

void pad_start(void);                                     /* once; the first hid call does it */
int pad_get(u32 slot, PadState *out);                     /* returns out->connected */
int pad_keyboard_slot(void);                              /* the keyboard's slot, or -1 */
int pad_background(void);                                 /* pads work while the window is not focused */
int pad_rumble_on(void);                                  /* 0: controllers.ini turns every pad's rumble off */
/* Rumble on one side (1 left, 2 right) of a slot's pad, amplitude 0..1. */
void pad_rumble(u32 slot, int side, float amplitude);
float pad_rumble_value(u32 slot, int side);               /* what was last sent */
int pad_describe(char *buf, int size);                    /* "1 DualSense, 2 Xbox" for the F2 panel */

/* The settings menu (gpu_menu.c). A pad as it is set up now: its settings in effect (from the
 * [device ...] named in `section`, "" = [device default] only), each console button's controls
 * as controllers.ini names them, and which of its controls are pressed now (bit n = SDL's
 * gamepad button n, and the triggers past half way). */
enum { PAD_RAW_LEFT_TRIGGER_BIT = 30, PAD_RAW_RIGHT_TRIGGER_BIT = 31, PAD_RAW_GUIDE_BIT = 5 };
#define PAD_RAW_LEFT_TRIGGER (1u << PAD_RAW_LEFT_TRIGGER_BIT)
#define PAD_RAW_RIGHT_TRIGGER (1u << PAD_RAW_RIGHT_TRIGGER_BIT)
typedef struct PadInfo {
    int connected, slot, xinput;
    char name[96], type[24], guid[40], section[96];
    int layout, deadzone, rumble, trigger;    /* layout 0 positional, 1 labels */
    char map[16][40];
    u32 raw;
} PadInfo;
int pad_info(u32 slot, PadInfo *out);                     /* returns out->connected */
const char *pad_raw_name(int bit);                        /* "south", ..., "lefttrigger" */
const char *pad_button_name(int npad);                    /* "A", ..., "DOWN" */
const char *pad_ini_path(void);
void pad_reload(void);                                    /* controllers.ini read again, pads set up again (soon) */
int menu_blocks_input(void);                              /* (gpu_menu.c) the menu is open: the game sees no input */
