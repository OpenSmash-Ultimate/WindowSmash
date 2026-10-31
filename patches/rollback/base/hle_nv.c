/* The GPU driver's device layer, natively.
 *
 * The SDK's graphics driver (NVN) runs recompiled; it reaches the hardware only through the
 * NvOsDrv* calls - open a device node, ioctl it, close it. This file is that set of device
 * nodes:
 *
 *   /dev/nvmap            memory handles over guest memory
 *   /dev/nvhost-as-gpu    the GPU virtual address space (what is mapped where)
 *   /dev/nvhost-ctrl-gpu  chip characteristics (Tegra X1, GM20B)
 *   /dev/nvhost-ctrl      sync points: fence waits and events
 *   /dev/nvhost-gpu       channels: command submission (GPFIFO)
 *   /dev/nvhost-nvdec, -vic, -nvjpg
 *                         the multimedia engines behind host1x (video decode, colour
 *                         conversion, JPEG): command buffers go to gpu_host1x.c, which so far
 *                         records the jobs; they complete at once, their output left as it
 *                         was - the game plays its movies as blank frames
 *
 * Command execution is pluggable (gpu_submit). Until a GPU back end exists, submitted work
 * is accepted and completes at once, so fences signal and the driver never stalls.
 * Structure layouts follow the ioctl argument blocks the driver builds; unknown requests
 * are logged once and succeed. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"
#include "gpu.h"
#include "gpu_backend.h"   /* (gfx_gpu_unmapped, gfx_guest_unmapped) */

enum { NV_OK = 0, NV_NOT_SUPPORTED = 2, NV_BAD_PARAMETER = 4, NV_TIMEOUT = 5, NV_CONFIG_NOT_FOUND = 0x30006 };
enum { DEV_NONE, DEV_CTRL, DEV_CTRL_GPU, DEV_AS, DEV_NVMAP, DEV_CHANNEL, DEV_HOST1X };

static const struct { const char *path; int kind; u32 cls; } NODES[] = {
    {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRL_GPU}, {"/dev/nvhost-as-gpu", DEV_AS},
    {"/dev/nvmap", DEV_NVMAP}, {"/dev/nvhost-gpu", DEV_CHANNEL}, {"/dev/nvhost-nvdec", DEV_HOST1X, 0xF0},
    {"/dev/nvhost-vic", DEV_HOST1X, 0x5D}, {"/dev/nvhost-nvjpg", DEV_HOST1X, 0xC0},
};

static struct { int kind; u32 syncpoint, cls; } files[256];
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static volatile LONG lock_made;
DWORD nv_lock_owner(void) { return __atomic_load_n(&lock_made, __ATOMIC_ACQUIRE) ? (DWORD)(uintptr_t)lock.OwningThread : 0; }   /* (freeze reports) */

/* ---- shared GPU state ------------------------------------------------------------ */
NvMap gpu_maps[NVMAP_MAX];
u32 gpu_syncpt_min[SYNCPT_MAX], gpu_syncpt_max[SYNCPT_MAX];
/* GPU virtual memory: a two-level page table of 4 KB pages over the 40-bit address space,
 * each entry the guest address the page maps to (0: unmapped). Mappings are remembered by
 * their start so that an unmap knows its size. */
#define PAGE_BITS 12
#define LEAF_BITS 10
#define TOP_COUNT (1u << (40 - PAGE_BITS - LEAF_BITS))
static u64 *page_dir[TOP_COUNT];
static struct { u64 va, size; } *map_sizes;
static u32 map_count, map_cap, next_handle = 1, next_syncpoint = 20;
static u64 next_va = 0x100000000ull;   /* bump allocator for VA requests without a fixed address */

static BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&lock);
    __atomic_store_n(&lock_made, 1, __ATOMIC_RELEASE);
    return TRUE;
}

/* Does guest memory [addr, addr + size) hold any nvmap allocation - memory the GPU (thread) may
 * read? (The game frees ordinary heap pages mid-match too.) */
static int gpu_memory_overlaps(u64 addr, u64 size) {
    u32 n = next_handle < NVMAP_MAX ? next_handle : NVMAP_MAX;
    for (u32 h = 1; h < n; h++)
        if (gpu_maps[h].addr && gpu_maps[h].size && gpu_maps[h].addr < addr + size && addr < gpu_maps[h].addr + gpu_maps[h].size) return 1;
    return 0;
}

/* The kernel is about to decommit guest memory [addr, addr + size). If the GPU may read it,
 * queued GPU work runs first, and the renderer lets go of what it made from it: nothing may
 * read that memory once it is gone (the graphics driver crashed doing so). */
void gpu_memory_unmapping(u64 addr, u64 size) {
    if (!gpu_memory_overlaps(addr, size)) return;
    gpu_queue_drain();
    gfx_guest_unmapped(addr, size);
}

u64 gpu_va_to_guest(u64 va) {   /* 0 when unmapped */
    u64 page = va >> PAGE_BITS;
    if (page >> LEAF_BITS >= TOP_COUNT) return 0;
    u64 *leaf = page_dir[page >> LEAF_BITS];
    u64 g = leaf ? leaf[page & ((1u << LEAF_BITS) - 1)] : 0;
    return g ? g + (va & ((1u << PAGE_BITS) - 1)) : 0;
}

u64 gpu_stat_drains;   /* (the GPU thread's queue drained for an unmapping) */

/* Point the pages of [va, va + size) at guest memory from `guest` on (0: unmap them). Where a
 * page mapped other memory before, queued GPU work runs first (it may still read the old), and
 * the renderer then lets go of what it made from the old mapping. */
static void set_pages(u64 va, u64 size, u64 guest) {
    u64 lo = 0, hi = 0;   /* (the pages that mapped other memory) */
    for (u64 off = 0; off < size; off += 1u << PAGE_BITS) {
        u64 page = (va + off) >> PAGE_BITS;
        if (page >> LEAF_BITS >= TOP_COUNT) break;
        u64 **leaf = &page_dir[page >> LEAF_BITS];
        if (!*leaf) {
            if (!guest) continue;
            *leaf = calloc(1u << LEAF_BITS, sizeof **leaf);
        }
        u64 *entry = &(*leaf)[page & ((1u << LEAF_BITS) - 1)], now = guest ? guest + off : 0;
        if (*entry && *entry != now) {
            if (!hi && gpu_threaded()) { gpu_queue_drain(); gpu_stat_drains++; }
            if (!hi) lo = va + off;
            hi = va + off + (1u << PAGE_BITS);
        }
        *entry = now;
    }
    if (hi) gfx_gpu_unmapped(lo, hi - lo);
}

static void unmap_range(u64 va) {
    for (u32 i = 0; i < map_count; i++)
        if (map_sizes[i].va == va) {
            set_pages(va, map_sizes[i].size, 0);
            map_sizes[i] = map_sizes[--map_count];
            return;
        }
}

/* Is [va, va + size) one mapping of guest memory from `guest` on already? */
static int mapped_as(u64 va, u64 size, u64 guest) {
    u32 i = 0;
    while (i < map_count && map_sizes[i].va != va) i++;
    if (i == map_count || map_sizes[i].size != size) return 0;
    for (u64 off = 0; off < size; off += 1u << PAGE_BITS)
        if (gpu_va_to_guest(va + off) != guest + off) return 0;
    return 1;
}

static void map_range(u64 va, u64 size, u64 guest) {
    if (mapped_as(va, size, guest)) return;   /* (the same again: what the renderer made of it stays) */
    unmap_range(va);
    if (map_count == map_cap) map_sizes = realloc(map_sizes, sizeof *map_sizes * (map_cap = map_cap * 2 + 1024));
    map_sizes[map_count].va = va;
    map_sizes[map_count++].size = size;
    set_pages(va, size, guest);
}

static u64 alloc_va(u64 size, u64 align) {
    if (align < 0x10000) align = 0x10000;
    u64 va = (next_va + align - 1) & ~(align - 1);
    next_va = va + ((size + 0xFFFF) & ~0xFFFFull);
    return va;
}

static void warn_once(const char *dev, u32 request) {
    static u32 seen[256], count;
    for (u32 i = 0; i < count; i++) if (seen[i] == request) return;
    if (count < 256) seen[count++] = request;
    fprintf(stderr, "[nvdrv] %s: ioctl %#010x not handled (group %#x, nr %#x, size %#x) - succeeding\n",
            dev, request, (request >> 8) & 0xFF, request & 0xFF, (request >> 16) & 0x3FFF);
}

/* ---- devices --------------------------------------------------------------------- */
static u32 ctrl(u32 nr, u64 a, u32 request) {
    switch (nr) {
    case 0x1B: return NV_CONFIG_NOT_FOUND;                       /* GetConfig */
    case 0x1D: case 0x1E: {                                      /* EventWait / EventWaitAsync */
        u32 id = ld32(a), value = ld32(a + 4), timeout = ld32(a + 8);
        if (id >= SYNCPT_MAX) return NV_BAD_PARAMETER;
        /* A value submitted work will reach: wait for the GPU thread to get there (the async
         * form too - it then has nothing to wait for). A timeout past a few seconds is taken
         * as none being meant: whatever is wrong, the game goes on rather than hanging. */
        if (gpu_threaded() && (i32)(gpu_syncpt_max[id] - value) >= 0 && (i32)(gpu_syncpt_min[id] - value) < 0) {
            if (!gpu_syncpt_wait(id, value, timeout && timeout < 5000 ? timeout : 5000)) {
                if (timeout && timeout < 5000) return NV_TIMEOUT;
                static int warned;
                if (warned++ < 4) fprintf(stderr, "[nvdrv] sync point %u: still short of %u after 5 s - going on\n", id, value);
            }
            st32(a + 12, gpu_syncpt_min[id]);
            return NV_OK;
        }
        /* The driver also advances sync points from inside its command lists, and values no
         * submission will reach are taken as reached: the GPU is modelled as finishing
         * everything instantly. */
        if ((i32)(gpu_syncpt_max[id] - value) < 0) gpu_syncpt_max[id] = value;
        if ((i32)(gpu_syncpt_min[id] - value) < 0) gpu_syncpt_min[id] = value;
        st32(a + 12, gpu_syncpt_min[id]);
        return NV_OK;
    }
    case 0x14: st32(a + 4, gpu_syncpt_min[ld32(a) % SYNCPT_MAX]); return NV_OK;   /* SyncptRead */
    case 0x1F: case 0x20: case 0x21: case 0x1C: return NV_OK;   /* event register / unregister / kill / clear */
    }
    warn_once("nvhost-ctrl", request);
    return NV_OK;
}

static void characteristics(u64 gc) {
    static const u32 words[] = {
        0x120, 0xB, 0xA1, 1,             /* arch (GM200), implementation (GM20B), revision, GPCs */
        0x40000, 0, 0,                   /* L2 cache size (u64 split), on-board memory (u64 low) */
    };
    memset(P(void, gc), 0, 0xA0);
    st32(gc + 0x00, 0x120); st32(gc + 0x04, 0xB); st32(gc + 0x08, 0xA1); st32(gc + 0x0C, 1);
    st64(gc + 0x10, 0x40000); st64(gc + 0x18, 0);
    st32(gc + 0x20, 2);        /* TPCs per GPC */
    st32(gc + 0x24, 0x20);     /* bus type */
    st32(gc + 0x28, 0x20000);  /* big page size */
    st32(gc + 0x2C, 0x20000);  /* compression page size */
    st32(gc + 0x30, 0x1B);     /* PDE coverage bits */
    st32(gc + 0x34, 0x30000);  /* available big page sizes */
    st32(gc + 0x38, 1);        /* GPC mask */
    st32(gc + 0x3C, 0x503);    /* SM version */
    st32(gc + 0x40, 0x503);    /* SPA version */
    st32(gc + 0x44, 0x80);     /* warps per SM */
    st32(gc + 0x48, 0x28);     /* GPU VA bits */
    st32(gc + 0x4C, 0);
    st64(gc + 0x50, 0x55);     /* flags */
    st32(gc + 0x58, 0x902D);   /* 2D class */
    st32(gc + 0x5C, 0xB197);   /* 3D class */
    st32(gc + 0x60, 0xB1C0);   /* compute class */
    st32(gc + 0x64, 0xB06F);   /* GPFIFO class */
    st32(gc + 0x68, 0xA140);   /* inline-to-memory class */
    st32(gc + 0x6C, 0xB0B5);   /* DMA copy class */
    st32(gc + 0x70, 1);        /* max FBPs */
    st32(gc + 0x74, 0);
    st32(gc + 0x78, 2);        /* LTCs per FBP */
    st32(gc + 0x7C, 1);        /* LTS per LTC */
    st32(gc + 0x80, 0);
    st32(gc + 0x84, 1);        /* max GPCs */
    st32(gc + 0x88, 0x21D70);  /* ROP/L2 enable mask */
    st32(gc + 0x8C, 0);
    memcpy(P(void, gc + 0x90), "gm20b", 6);
    (void)words;
}

static u32 ctrl_gpu(u32 nr, u64 a, u32 request, u64 out, u64 out_size) {
    switch (nr) {
    case 0x01: st32(a, 1); return NV_OK;                                  /* ZcullGetCtxSize */
    case 0x02: {                                                          /* ZcullGetInfo */
        static const u32 info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
        memcpy(P(void, a), info, sizeof info);
        return NV_OK;
    }
    case 0x03: case 0x04: case 0x07: return NV_OK;                        /* ZBC set / query, flush L2 */
    case 0x05:                                                            /* GetCharacteristics */
        st64(a, 0xA0);
        characteristics(a + 0x10);
        if (ld64(a + 8)) memcpy(P(void, ld64(a + 8)), P(void, a + 0x10), 0xA0);
        if (out && out_size >= 0xA0) memcpy(P(void, out), P(void, a + 0x10), 0xA0);
        return NV_OK;
    case 0x06:                                                            /* GetTpcMasks */
        if (ld32(a) && ld64(a + 0x10)) st32(ld64(a + 0x10), 3);
        if (out && out_size >= 4) st32(out, 3);
        if (((request >> 16) & 0x3FFF) >= 0x1C) st32(a + 0x18, 3);
        return NV_OK;
    case 0x14: st32(a, 7); st32(a + 4, 1); return NV_OK;                 /* GetActiveSlotMask */
    case 0x1C: st64(a, rt_now_ns()); return NV_OK;                       /* GetGpuTime */
    }
    warn_once("nvhost-ctrl-gpu", request);
    return NV_OK;
}

static u32 address_space(u32 nr, u64 a, u32 request) {
    switch (nr) {
    case 0x01: return NV_OK;                                              /* BindChannel */
    case 0x02: {                                                          /* AllocSpace */
        u64 size = (u64)ld32(a) * ld32(a + 4);
        if (!(ld32(a + 8) & 1)) st64(a + 0x10, alloc_va(size, ld64(a + 0x10)));
        return NV_OK;
    }
    case 0x03: return NV_OK;                                              /* FreeSpace */
    case 0x05: unmap_range(ld64(a)); return NV_OK;                        /* UnmapBuffer */
    case 0x06: {                                                          /* MapBufferEx */
        u32 flags = ld32(a), handle = ld32(a + 8);
        u64 offset = ld64(a + 0x10), size = ld64(a + 0x18), va = ld64(a + 0x20);
        if (!handle || handle >= NVMAP_MAX || !gpu_maps[handle].size) return NV_BAD_PARAMETER;
        if (!size) size = gpu_maps[handle].size - offset;
        if (!(flags & 1)) va = alloc_va(size, ld32(a + 0xC));
        map_range(va, size, gpu_maps[handle].addr + offset);
        st64(a + 0x20, va);
        return NV_OK;
    }
    case 0x08: {                                                          /* GetVaRegions */
        u64 r = a + 0x10;
        st64(r, 0x4000000); st32(r + 8, 0x1000); st32(r + 12, 0); st64(r + 16, (0x100000000ull - 0x4000000) / 0x1000);
        st64(r + 24, 0x100000000ull); st32(r + 32, 0x20000); st32(r + 36, 0); st64(r + 40, (0x10000000000ull - 0x100000000ull) / 0x20000);
        st32(a + 8, 0x30);
        return NV_OK;
    }
    case 0x09: return NV_OK;                                              /* AllocAsEx / InitializeEx */
    case 0x14: {                                                          /* Remap: entries of 0x14 bytes */
        u32 n = ((request >> 16) & 0x3FFF) / 0x14;
        if (gpu_threaded()) { gpu_queue_drain(); gpu_stat_drains++; }      /* (queued work may still read what moves) */
        for (u32 i = 0; i < n; i++) {
            u64 e = a + 0x14ull * i;
            u32 handle = ld32(e + 4);
            u64 va = (u64)ld32(e + 12) << 16, size = (u64)ld32(e + 16) << 16;
            if (handle && handle < NVMAP_MAX) map_range(va, size, gpu_maps[handle].addr + ((u64)ld32(e + 8) << 16));
            else unmap_range(va);
        }
        return NV_OK;
    }
    }
    warn_once("nvhost-as-gpu", request);
    return NV_OK;
}

static u32 nvmap(u32 nr, u64 a, u32 request) {
    switch (nr) {
    case 0x01: {                                                          /* Create */
        u32 h = next_handle++;
        if (h >= NVMAP_MAX) rt_fatal("nvdrv: out of nvmap handles");
        gpu_maps[h] = (NvMap){ld32(a), 0, 0x1000, 0};
        st32(a + 4, h);
        return NV_OK;
    }
    case 0x03: st32(a + 4, ld32(a)); return NV_OK;                        /* FromId: ids are handles */
    case 0x04: {                                                          /* Alloc */
        u32 h = ld32(a);
        if (!h || h >= NVMAP_MAX) return NV_BAD_PARAMETER;
        gpu_maps[h].align = ld32(a + 12);
        gpu_maps[h].kind = ld8(a + 16);
        gpu_maps[h].addr = ld64(a + 24);
        return NV_OK;
    }
    case 0x05: {                                                          /* Free */
        u32 h = ld32(a);
        if (!h || h >= NVMAP_MAX) return NV_BAD_PARAMETER;
        st64(a + 8, gpu_maps[h].addr);
        st32(a + 16, (u32)gpu_maps[h].size);
        st32(a + 20, 0);
        return NV_OK;
    }
    case 0x09: {                                                          /* Param */
        u32 h = ld32(a), p = ld32(a + 4), v = 0;
        if (!h || h >= NVMAP_MAX) return NV_BAD_PARAMETER;
        v = p == 1 ? (u32)gpu_maps[h].size : p == 2 ? gpu_maps[h].align : p == 3 ? (u32)gpu_maps[h].addr :
            p == 4 ? 0x40000000 : p == 5 ? gpu_maps[h].kind : 0;
        st32(a + 8, v);
        return NV_OK;
    }
    case 0x0E: st32(a, ld32(a + 4)); return NV_OK;                        /* GetId */
    }
    warn_once("nvmap", request);
    return NV_OK;
}

static u32 submit(u32 fd, u64 a, u64 entries, u32 count) {
    u32 flags = ld32(a + 12), id = files[fd].syncpoint;
    u32 incr = 0;
    if (flags & ((1u << 1) | (1u << 8)))   /* increment requested: two for the GPU, plus any extra */
        incr = 2 + (flags & (1u << 8) ? ld32(a + 20) : 0);
    if (gpu_threaded()) {   /* run later, on the GPU thread: the sync point reaches the fence then */
        gpu_syncpt_max[id] += incr;
        gpu_queue_submit(fd, P(u64, entries), count, id, gpu_syncpt_max[id]);
    } else {
        gpu_submit(fd, P(u64, entries), count);
        gpu_syncpt_max[id] += incr;
        gpu_syncpt_min[id] = gpu_syncpt_max[id];   /* everything completes now */
    }
    st32(a + 16, id);
    st32(a + 20, gpu_syncpt_max[id]);
    return NV_OK;
}

static u32 channel(u32 fd, u32 group, u32 nr, u64 a, u32 request, u64 in, u64 in_size) {
    if (group == 0x48) switch (nr) {
    case 0x01: case 0x03: case 0x0B: case 0x0C: case 0x0D: case 0x1D: case 0x09:
        return NV_OK;   /* fd, timeout, zcull, notifier, priority, timeslice, object */
    case 0x08: {   /* SubmitGpfifo: entries follow the 0x18-byte header */
        u32 count = ld32(a + 8);
        return submit(fd, a, in && in_size ? in : a + 0x18, count);
    }
    case 0x1B: return submit(fd, a, in, ld32(a + 8));                     /* SubmitGpfifo, entries inline */
    case 0x1A:                                                            /* AllocGpfifoEx2 */
        st32(a + 24, files[fd].syncpoint);
        st32(a + 28, gpu_syncpt_max[files[fd].syncpoint]);
        return NV_OK;
    }
    if (group == 0x47 && (nr == 0x14 || nr == 0x15)) return NV_OK;           /* client data */
    warn_once("nvhost-gpu", request);
    return NV_OK;
}

/* A host1x client engine. Submit: {command buffers, relocations, sync point increments,
 * fences} counts, then the arrays in that order (12, 16 + 4 for the shift, 20 bytes each),
 * then one u32 fence threshold per fence, written back. The command buffers go to the
 * engine (gpu_host1x.c); the job counts as done at once. */
static u32 host1x(u32 fd, u32 group, u32 nr, u64 a, u32 request) {
    if (group == 0x48 && nr == 0x01) return NV_OK;                         /* SetNvmapFd */
    if (group == 0) switch (nr) {
    case 0x01: {                                                          /* Submit */
        u32 cmdbufs = ld32(a), relocs = ld32(a + 4), incrs = ld32(a + 8), fences = ld32(a + 12);
        u64 incr = a + 16 + 12ull * cmdbufs + 20ull * relocs, fence = incr + 20ull * incrs;
        for (u32 i = 0; i < cmdbufs; i++) {   /* {nvmap handle, byte offset, word count} */
            u64 cb = a + 16 + 12ull * i;
            host1x_submit(files[fd].cls, ld32(cb), ld32(cb + 4), ld32(cb + 8));
        }
        for (u32 i = 0; i < incrs; i++) {
            u32 id = ld32(incr + 20ull * i) % SYNCPT_MAX;
            gpu_syncpt_max[id] += ld32(incr + 20ull * i + 4);
            gpu_syncpt_min[id] = gpu_syncpt_max[id];
            if (i < fences) st32(fence + 4ull * i, gpu_syncpt_max[id]);
        }
        return NV_OK;
    }
    case 0x02: st32(a + 4, files[fd].syncpoint); return NV_OK;            /* GetSyncpoint */
    case 0x03: st32(a + 4, 0); return NV_OK;                              /* GetWaitbase */
    case 0x07: return NV_OK;                                              /* SetSubmitTimeout */
    case 0x09: case 0x0A: {                                               /* MapBuffer / UnmapBuffer */
        u32 n = ld32(a);
        for (u32 i = 0; i < n && nr == 0x09; i++) {                       /* the engines' address of each handle */
            u32 handle = ld32(a + 12 + 8ull * i);
            st32(a + 12 + 8ull * i + 4, host1x_map(handle));
        }
        return NV_OK;
    }
    }
    warn_once("nvhost (host1x)", request);
    return NV_OK;
}

static u32 ioctl(u32 fd, u32 request, u64 a, u64 in, u64 in_size, u64 out, u64 out_size) {
    u32 group = (request >> 8) & 0xFF, nr = request & 0xFF, r;
    if (fd >= 256 || !files[fd].kind) return NV_BAD_PARAMETER;
    EnterCriticalSection(&lock);
    switch (files[fd].kind) {
    case DEV_CTRL: LeaveCriticalSection(&lock); return ctrl(nr, a, request);   /* may wait */
    case DEV_CTRL_GPU: r = ctrl_gpu(nr, a, request, out, out_size); break;
    case DEV_AS: r = address_space(nr, a, request); break;
    case DEV_NVMAP: r = nvmap(nr, a, request); break;
    case DEV_CHANNEL: r = channel(fd, group, nr, a, request, in, in_size); break;
    case DEV_HOST1X: r = host1x(fd, group, nr, a, request); break;
    default: warn_once("unknown device", request); r = NV_OK;
    }
    LeaveCriticalSection(&lock);
    if (rt_trace) fprintf(stderr, "[nvdrv] fd %u ioctl %#010x -> %u\n", fd, request, r);
    return r;
}

/* ---- the driver's OS layer ------------------------------------------------------- */
HLE(_ZN2nv18InitializeGraphicsEPvm) { InitOnceExecuteOnce(&once, init, NULL, NULL); (void)c; }

HLE(NvOsDrvOpen) {
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    const char *path = P(char, c->x[0]);
    int kind = DEV_NONE;
    u32 cls = 0;
    for (u32 i = 0; i < sizeof NODES / sizeof *NODES; i++)
        if (!strcmp(path, NODES[i].path)) { kind = NODES[i].kind; cls = NODES[i].cls; }
    if (!kind) {
        fprintf(stderr, "[nvdrv] open %s: no such device here\n", path);
        c->x[0] = 0xFFFFFFFFu;
        return;
    }
    EnterCriticalSection(&lock);
    u32 fd = 1;
    while (fd < 256 && files[fd].kind) fd++;
    if (fd == 256) rt_fatal("nvdrv: too many open device files");
    files[fd].kind = kind;
    files[fd].cls = cls;
    if (kind == DEV_CHANNEL || kind == DEV_HOST1X) files[fd].syncpoint = next_syncpoint++;
    LeaveCriticalSection(&lock);
    if (rt_trace) fprintf(stderr, "[nvdrv] open %s -> %u\n", path, fd);
    c->x[0] = fd;
}

HLE(NvOsDrvClose) { if (c->x[0] < 256) files[c->x[0]].kind = 0; c->x[0] = NV_OK; }
HLE(NvOsDrvIoctl) { c->x[0] = ioctl((u32)c->x[0], (u32)c->x[1], c->x[2], 0, 0, 0, 0); }
HLE(NvOsDrvIoctl2) { c->x[0] = ioctl((u32)c->x[0], (u32)c->x[1], c->x[2], c->x[4], c->x[5], 0, 0); }
HLE(NvOsDrvIoctl3) { c->x[0] = ioctl((u32)c->x[0], (u32)c->x[1], c->x[2], 0, 0, c->x[4], c->x[5]); }
HLE(NvOsDrvMapSharedMem) { c->x[0] = NV_OK; }

/* NvOsDrvQueryEvent(fd, event id, SystemEventType*): a kernel event the driver waits on for
 * device notifications (sync-point events, channel errors). Work completes synchronously
 * here, so these are never signalled. */
HLE(_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE) {
    rt_attach_system_event(c, c->x[2], 0);
    c->x[0] = NV_OK;
}
