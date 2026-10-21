/* GameCube controllers read straight from USB on Windows (hle_padusb.c): Nintendo's GameCube
 * adapter (and other brands' adapters in their Wii U mode) and the Switch 2's GameCube controller
 * on a cable. The input thread (hle_pad.c) makes players of their ports like any other pad.
 *
 * A port's state is in SDL's terms, as hle_pad.c's other pads: sticks -32767..32767 with +y down,
 * triggers 0..32767. Elsewhere than on Windows there is nothing here: SDL reads them. */
#pragma once
#include "host.h"

enum { PADUSB_PORTS = 12 };   /* (three adapters, or adapters and controllers) */
enum {   /* PadUsbState.buttons */
    PU_A = 1 << 0, PU_B = 1 << 1, PU_X = 1 << 2, PU_Y = 1 << 3, PU_START = 1 << 4, PU_Z = 1 << 5, PU_ZL = 1 << 6,
    PU_L = 1 << 7, PU_R = 1 << 8,   /* the clicks at the bottom of the triggers */
    PU_UP = 1 << 9, PU_DOWN = 1 << 10, PU_LEFT = 1 << 11, PU_RIGHT = 1 << 12,
    PU_HOME = 1 << 13, PU_CAPTURE = 1 << 14, PU_C = 1 << 15,
};

typedef struct PadUsbState {
    int connected;
    u32 buttons;
    i32 lx, ly, rx, ry;       /* the control stick and the C stick */
    i32 lt, rt;
    char name[48];            /* "Nintendo GameCube Controller" */
    char id[24];              /* for controllers.ini: "gcadapter-<port 1-4>", "gcswitch2" */
    char path[192];           /* the device, and the adapter's port */
} PadUsbState;

/* On the input thread, once, before SDL starts: the devices SDL is to leave alone, as
 * SDL_HIDAPI_IGNORE_DEVICES lists them ("0x057e/0x0337,..."), or NULL when this file reads none
 * (not Windows, or SSBU_GC_USB=0). */
const char *padusb_start(void);
void padusb_poll(PadUsbState out[PADUSB_PORTS]);   /* every port, as last read */
void padusb_rumble(int port, int on);
