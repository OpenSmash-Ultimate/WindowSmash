/* First end-to-end check: run recompiled game code natively and verify its result.
 *
 * Target: main+0x25ee6c0 (13.0.5; 11.0.0's main+0x256c220), an animation lookup. Given (this, const char *name) it computes
 * hash40(lowercase(name) + ".nuanmb") and returns that hash if the table at this+0x10..0x18
 * (16-byte entries, hash at +8) contains it, else 0. The expected hashes below come from an
 * independent bitwise CRC-32, not from anything in the game image. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp.h"

#define BASE 0x7100000000ull

extern const struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[];
extern const u32 rt_func_count;

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    (void)c;
    fprintf(stderr, "guest trap at %#llx: %s\n", pc, why);
    exit(3);
}

GuestFn *rt_lookup(u64 addr) {
    u32 lo = 0, hi = rt_func_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (rt_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo == rt_func_count || rt_funcs[lo].addr != addr) rt_trap(NULL, addr, "no recompiled function at this address");
    return rt_funcs[lo].fn;
}
GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) { (void)cache; return rt_lookup(addr); }

static u64 ref_hash40(const char *s) {
    u32 crc = 0xFFFFFFFFu;
    u64 len = 0;
    for (; *s; s++, len++) {
        crc ^= (u8)*s;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ((len & 0xFF) << 32) | (u32)~crc;
}

struct Entry { u64 unused; u64 hash; };
struct Object { u64 pad[2]; struct Entry *begin, *end; };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: smoke <relocated image>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    _fseeki64(f, 0, SEEK_END);
    size_t size = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    void *image = VirtualAlloc((void *)BASE, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (image != (void *)BASE) { fprintf(stderr, "cannot map guest image at %#llx\n", BASE); return 2; }
    if (fread(image, 1, size, f) != size) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);
    printf("mapped %zu MiB of game image at %#llx\n", size >> 20, BASE);

    struct Entry table[] = {
        {0, ref_hash40("a00wait1.nuanmb")}, {0, ref_hash40("c05attack11.nuanmb")}, {0, ref_hash40("f00jumpf.nuanmb")},
    };
    struct Object obj = {{0, 0}, table, table + 3};

    static const struct { const char *name, *canonical; } cases[] = {
        {"A00Wait1", "a00wait1.nuanmb"}, {"c05attack11", "c05attack11.nuanmb"}, {"F00JUMPF", "f00jumpf.nuanmb"},
        {"e02catch", NULL}, {"", NULL}, {NULL, NULL},
    };

    size_t stack_size = 1 << 20;
    u8 *stack = malloc(stack_size);
    GuestFn *lookup_anim = rt_lookup(BASE + 0x25ee6c0);
    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Cpu cpu = {0};
        cpu.sp = (u64)(stack + stack_size);
        cpu.x[0] = (u64)&obj;
        cpu.x[1] = (u64)cases[i].name;
        lookup_anim(&cpu);
        u64 want = cases[i].canonical ? ref_hash40(cases[i].canonical) : 0;
        int ok = cpu.x[0] == want;
        failures += !ok;
        printf("  %-4s name=%-14s -> %#012llx  (expected %#012llx)\n", ok ? "ok" : "FAIL",
               cases[i].name ? cases[i].name : "(null)", cpu.x[0], want);
    }
    printf(failures ? "%d FAILED\n" : "all cases match the reference\n", failures);
    return failures != 0;
}
