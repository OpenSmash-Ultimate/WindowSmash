#include "module.h"

GuestFn f_7100000100;
GuestFn f_7100000200;
GuestFn f_7100000300;
GuestFn f_7100001000;
GuestFn f_7100001100;
GuestFn f_7100001200;
GuestFn f_7100001300;
GuestFn f_7100001400;
GuestFn f_7100007f00;
GuestFn f_7100007f10;

static const RtFunc funcs[] = {
    {0x7100000100ull, f_7100000100},
    {0x7100000200ull, f_7100000200},
    {0x7100000300ull, f_7100000300},
    {0x7100001000ull, f_7100001000},
    {0x7100001100ull, f_7100001100},
    {0x7100001200ull, f_7100001200},
    {0x7100001300ull, f_7100001300},
    {0x7100001400ull, f_7100001400},
    {0x7100007F00ull, f_7100007f00},
    {0x7100007F10ull, f_7100007f10},
};
static const RtSym exports[] = {
    {"nninitStartup", 0x7100000200ull},
    {"nnMain", 0x7100000300ull},
};
static const char *const imports[] = {
    "longjmp",
    "boottest_sum",
    "boottest_init_log",
    "boottest_absent",
    0
};
static const u8 import_weak[] = {0,0,0,1,0};
static const RtReloc relocs[] = {
    {0xA000ull, 0, 0},
    {0xA008ull, 0, 1},
    {0xA010ull, 0, 2},
    {0xA018ull, 0, 3},
    {0, 0, 0}
};

/* (13.0.5's build id: the runtime starts no other main module - runtime/host.c game_version_ok) */
const RtModule rt_module_main = {
    "main", 0x7100000000ull, 0x20000ull, 0x8000ull, 0x7100000100ull,
    funcs, 10, exports, 2, imports, import_weak, 4, relocs, 4, "21450C647B8C5940ED5B68218EA128FD3C5D597D",
};
