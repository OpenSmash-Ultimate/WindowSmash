/* The display service (vi), natively: displays, layers, the vsync event, and the buffer
 * queue behind each layer.
 *
 * The SDK's display library talks to vi:u / vi:s / vi:m; a layer comes back as a "native
 * window" parcel naming a binder object. From then on the graphics driver drives that
 * window through Android's IGraphicBufferProducer protocol, sent as binder transactions to
 * the relay service (IHOSBinderDriver). The queue's consumer is a native vsync thread: 60
 * times a second it presents the oldest queued buffer and returns the previous one, which
 * paces the game exactly as the console's compositor does. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "ipc.h"
#include "present.h"
#include "display.h"
#include "gpu.h"   /* (gpu_queue_mark / gpu_queue_wait) */

#define DISPLAY_ID 1
#define LAYER_ID 1
#define BINDER_ID 1
#define WIDTH 1920
#define HEIGHT 1080
#define SLOTS 64

enum { FREE, DEQUEUED, QUEUED, ACQUIRED };
enum {   /* IGraphicBufferProducer transactions */
    REQUEST_BUFFER = 1, SET_BUFFER_COUNT = 2, DEQUEUE_BUFFER = 3, DETACH_BUFFER = 4, DETACH_NEXT_BUFFER = 5,
    ATTACH_BUFFER = 6, QUEUE_BUFFER = 7, CANCEL_BUFFER = 8, QUERY = 9, CONNECT = 10, DISCONNECT = 11,
    ALLOCATE_BUFFERS = 13, SET_PREALLOCATED_BUFFER = 14, GET_BUFFER_HISTORY = 17
};

typedef struct Slot { int state, has_buffer; u8 buffer[0x400]; u32 buffer_size; u64 queued_at, gpu_mark; } Slot;
static struct {
    Slot slots[SLOTS];
    u32 buffer_event, vsync_event;
    CONDITION_VARIABLE freed;
    SRWLOCK lock;
    int started;
    u64 frames;
} queue = {.lock = SRWLOCK_INIT};

u64 present_frames(void) { return queue.frames; }

/* Who holds the queue's lock, and since when (freeze reports: an SRW lock has no owner field). */
static volatile DWORD queue_holder;
static volatile u64 queue_held_since;
static void queue_lock(void) {
    AcquireSRWLockExclusive(&queue.lock);
    queue_held_since = GetTickCount64();
    queue_holder = GetCurrentThreadId();
}
static void queue_unlock(void) {
    queue_holder = 0;
    ReleaseSRWLockExclusive(&queue.lock);
}
DWORD present_queue_holder(u64 *since_ms) { DWORD t = queue_holder; *since_ms = queue_held_since; return t; }

/* ---- the consumer: vsync -------------------------------------------------------- */
static DWORD WINAPI vsync_thread(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"vsync");
    timeBeginPeriod(1);
    static u8 shown[sizeof queue.slots[0].buffer];
    u64 gpu_mark = 0;
    for (;;) {
        display_wait_tick();   /* the next refresh (display.ini's pacing) */
        queue_lock();
        int oldest = -1;
        u32 shown_size = 0;
        for (int i = 0; i < SLOTS; i++)
            if (queue.slots[i].state == QUEUED && (oldest < 0 || queue.slots[i].queued_at < queue.slots[oldest].queued_at)) oldest = i;
        if (oldest >= 0) {
            for (int i = 0; i < SLOTS; i++) if (queue.slots[i].state == ACQUIRED) queue.slots[i].state = FREE;
            queue.slots[oldest].state = ACQUIRED;
            queue.frames++;
            shown_size = queue.slots[oldest].buffer_size;
            memcpy(shown, queue.slots[oldest].buffer, shown_size);
            gpu_mark = queue.slots[oldest].gpu_mark;
        }
        queue_unlock();
        /* Shown outside the queue's lock: a present can wait for the graphics card (frames
         * behind), and meanwhile the game queues its next frame and goes on with the one after. */
        if (oldest >= 0) {
            gpu_queue_wait(gpu_mark);   /* the frame drawn: what was submitted before it was queued has run */
            present_frame(shown, shown_size);
            kernel_signal_event(queue.buffer_event);
            WakeAllConditionVariable(&queue.freed);
        }
        kernel_signal_event(queue.vsync_event);
    }
}

static void start(void) {
    if (queue.started) return;
    queue.started = 1;
    InitializeConditionVariable(&queue.freed);
    queue.buffer_event = kernel_create_event();
    queue.vsync_event = kernel_create_event();
    CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
}

/* ---- parcels --------------------------------------------------------------------- */
/* A flattened parcel: { u32 data size, data offset, objects size, objects offset } + data. */
typedef struct In { const u8 *d; u32 size, pos; } In;
typedef struct Out { u8 d[0x800]; u32 size; } Out;

static u32 rd32(In *p) { u32 v = 0; if (p->pos + 4 <= p->size) memcpy(&v, p->d + p->pos, 4); p->pos += 4; return v; }
static void skip_token(In *p) {   /* strict-mode policy, then the interface name as String16 */
    rd32(p);
    i32 len = (i32)rd32(p);
    if (len > 0) p->pos += ((u32)(len + 1) * 2 + 3) & ~3u;
}
static void wr(Out *o, const void *v, u32 n) { memcpy(o->d + o->size, v, n); o->size += (n + 3) & ~3u; }
static void w32(Out *o, u32 v) { wr(o, &v, 4); }

static u32 parcel_out(Out *o, u64 dst, u64 cap) {
    u32 total = 16 + o->size;
    if (!dst || total > cap) return 0;
    u32 h[4] = {o->size, 16, 0, 16 + o->size};
    memcpy(P(void, dst), h, 16);
    memcpy(P(void, dst + 16), o->d, o->size);
    return total;
}

static void queue_output(Out *o) {   /* QueueBufferOutput: width, height, transform hint, pending */
    u32 pending = 0;
    for (int i = 0; i < SLOTS; i++) pending += queue.slots[i].state == QUEUED;
    w32(o, WIDTH); w32(o, HEIGHT); w32(o, 0); w32(o, pending);
}

static void transact(u32 code, In *in, Out *out, Cpu *c) {
    (void)c;
    skip_token(in);
    queue_lock();
    switch (code) {
    case CONNECT:
        queue_output(out);
        w32(out, 0);
        kernel_signal_event(queue.buffer_event);
        break;
    case DISCONNECT: case SET_BUFFER_COUNT: case ALLOCATE_BUFFERS:
        w32(out, 0);
        break;
    case SET_PREALLOCATED_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) {
            Slot *s = &queue.slots[slot];
            s->buffer_size = in->size - in->pos < sizeof s->buffer ? in->size - in->pos : (u32)sizeof s->buffer;
            memcpy(s->buffer, in->d + in->pos, s->buffer_size);
            s->has_buffer = s->buffer_size >= 4 && *(u32 *)s->buffer != 0;
            s->state = FREE;
            kernel_signal_event(queue.buffer_event);   /* a buffer is available */
        }
        w32(out, 0);
        break;
    }
    case REQUEST_BUFFER: {   /* the buffer exactly as it was registered */
        u32 slot = rd32(in);
        if (slot < SLOTS && queue.slots[slot].has_buffer) wr(out, queue.slots[slot].buffer, queue.slots[slot].buffer_size);
        else w32(out, 0);
        w32(out, 0);
        break;
    }
    case DEQUEUE_BUFFER: {   /* never blocks: "would block" sends the client back to the event */
        int slot = -1;
        for (int i = 0; i < SLOTS && slot < 0; i++)
            if (queue.slots[i].has_buffer && queue.slots[i].state == FREE) slot = i;
        if (slot < 0) {
            w32(out, 0xFFFFFFFF);
            w32(out, 0);
            w32(out, (u32)-11);
            break;
        }
        queue.slots[slot].state = DEQUEUED;
        w32(out, (u32)slot);
        w32(out, 1);                       /* a fence follows: flattened, 0x24 bytes, no file descriptors */
        w32(out, 0x24); w32(out, 0);
        u8 fence[0x24] = {0};              /* no fences: the buffer is free now */
        wr(out, fence, sizeof fence);
        w32(out, 0);
        break;
    }
    case QUEUE_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) {
            queue.slots[slot].state = QUEUED;
            queue.slots[slot].queued_at = rt_now_ns();
            queue.slots[slot].gpu_mark = gpu_queue_mark();
        }
        queue_output(out);
        w32(out, 0);
        break;
    }
    case CANCEL_BUFFER: {
        u32 slot = rd32(in);
        if (slot < SLOTS) queue.slots[slot].state = FREE;
        kernel_signal_event(queue.buffer_event);
        w32(out, 0);
        break;
    }
    case QUERY: {
        u32 what = rd32(in), value = 0;
        switch (what) {
        case 0: case 6: value = WIDTH; break;
        case 1: case 7: value = HEIGHT; break;
        case 2: value = 1; break;      /* RGBA 8888 */
        case 3: value = 1; break;      /* minimum undequeued buffers */
        case 5: value = 1; break;      /* a surface */
        }
        w32(out, value);
        w32(out, 0);
        break;
    }
    case GET_BUFFER_HISTORY:
        w32(out, 0);
        w32(out, 0);
        break;
    default:
        fprintf(stderr, "[vi] buffer queue: transaction %u not implemented\n", code);
        w32(out, 0);
    }
    queue_unlock();
}

/* ---- IHOSBinderDriver ------------------------------------------------------------ */
static void binder(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 0: case 3: {   /* TransactParcel(id, code, flags) in -> out, and its "auto" variant */
        u32 code = ipc_u32(ipc, 4);
        u64 in_size, out_size;
        u64 in_addr = ipc_in_buffer(ipc, 0, &in_size), out_addr = ipc_out_buffer(ipc, 0, &out_size);
        In in = {0};
        if (in_addr && in_size >= 16) {
            const u32 *h = P(u32, in_addr);
            in.d = P(u8, in_addr + h[1]);
            in.size = h[0];
        }
        Out out = {0};
        transact(code, &in, &out, ipc->cpu);
        ipc_wrote(ipc, 0, parcel_out(&out, out_addr, out_size));
        return;
    }
    case 1: return;   /* AdjustRefcount */
    case 2:           /* GetNativeHandle(id, type): the "buffer released" event */
        ipc_copy_handle(ipc, queue.buffer_event);
        return;
    }
    ipc_unknown(ipc, self);
}

/* ---- the display services -------------------------------------------------------- */
/* The parcel naming the layer's binder object: a 0x28-byte flat binder (type, process,
 * 64-bit binder id, the relay service's name) at offset 0, listed in the object table so
 * Parcel::readStrongBinder accepts it. */
static u32 native_window(u64 dst, u64 cap) {
    u32 w[10] = {2, 1, BINDER_ID, 0, 0, 0, 0, 0, 0, 0};
    memcpy(&w[6], "dispdrv", 8);
    u32 total = 16 + sizeof w + 4;
    if (!dst || cap < total) return 0;
    u32 h[4] = {sizeof w, 16, 4, 16 + sizeof w};
    memcpy(P(void, dst), h, 16);
    memcpy(P(void, dst + 16), w, sizeof w);
    st32(dst + 16 + sizeof w, 0);   /* object table: the binder is at data offset 0 */
    return total;
}

static void display_service(IpcObject *self, Ipc *ipc);

static void root(IpcObject *self, Ipc *ipc) {
    if (ipc->command <= 2) {   /* GetDisplayService, whichever root this is */
        ipc_object(ipc, ipc_new_object("vi:IApplicationDisplayService", display_service, NULL));
        return;
    }
    ipc_unknown(ipc, self);
}

static u32 native_window(u64 dst, u64 cap);

static void sub_service(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 2010: IPC_PUSH(ipc, (u64)LAYER_ID); return;   /* CreateManagedLayer */
    case 2012: {   /* CreateStrayLayer(flags, display) -> layer, native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)LAYER_ID);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 3200: {   /* GetDisplayMode: width, height, refresh rate, mode */
        u32 mode[4] = {WIDTH, HEIGHT, 0, 0};
        f32 hz = 60.0f;
        memcpy(&mode[2], &hz, 4);
        ipc_push(ipc, mode, 16);
        return;
    }
    }
    if (ipc->command < 2000 || ipc->command >= 9000) ipc_unknown(ipc, self);
    /* the rest are layer and display property setters: accepted */
}

static void display_service(IpcObject *self, Ipc *ipc) {
    start();
    switch (ipc->command) {
    case 100: case 103:   /* GetRelayService / GetIndirectDisplayTransactionService */
        ipc_object(ipc, ipc_new_object("vi:IHOSBinderDriver", binder, NULL));
        return;
    case 101: ipc_object(ipc, ipc_new_object("vi:ISystemDisplayService", sub_service, NULL)); return;
    case 102: ipc_object(ipc, ipc_new_object("vi:IManagerDisplayService", sub_service, NULL)); return;
    case 1000: {   /* ListDisplays -> count; DisplayInfo { name[0x40]; bool; pad; u64 layers, width, height } */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x60) {
            memset(P(void, b), 0, 0x60);
            memcpy(P(void, b), "Default", 8);
            st8(b + 0x40, 1);
            st64(b + 0x48, 1); st64(b + 0x50, WIDTH); st64(b + 0x58, HEIGHT);
            ipc_wrote(ipc, 0, 0x60);
        }
        IPC_PUSH(ipc, (u64)1);
        return;
    }
    case 1010: case 1011: IPC_PUSH(ipc, (u64)DISPLAY_ID); return;   /* OpenDisplay / OpenDefaultDisplay */
    case 1020: case 1101: case 2021: case 2031: case 2101: return;   /* close / enable / close layer ... */
    case 1102: IPC_PUSH(ipc, (u64)WIDTH); IPC_PUSH(ipc, (u64)HEIGHT); return;   /* GetDisplayResolution */
    case 2020: {   /* OpenLayer(name, layer, applet) -> native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 2030: {   /* CreateStrayLayer(flags, display) -> layer, native window parcel */
        u64 size, b = ipc_out_buffer(ipc, 0, &size);
        u32 n = native_window(b, size);
        ipc_wrote(ipc, 0, n);
        IPC_PUSH(ipc, (u64)LAYER_ID);
        IPC_PUSH(ipc, (u64)n);
        return;
    }
    case 2102: IPC_PUSH(ipc, (u64)ipc_u32(ipc, 0)); return;   /* ConvertScalingMode */
    case 5202: case 5203: ipc_copy_handle(ipc, queue.vsync_event); return;   /* GetDisplayVsyncEvent */
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_vi(const char *name) {
    if (!strcmp(name, "vi:u") || !strcmp(name, "vi:s") || !strcmp(name, "vi:m"))
        return ipc_new_object(_strdup(name), root, NULL);
    return NULL;
}
