/* The game's movie player (its own code in main, around the SDK's movie library): a race that
 * the Switch's scheduling hides.
 *
 * The player's audio renderer takes the audio decoder's outputs from the player's
 * "MovieDecoderEventHandler" thread, copies them into PCM buffers and hands those to the audio
 * out callback. Its Close frees `pending` (the decoder output it is part-way through) and zeroes
 * that pointer, under the renderer's lock; the lists of PCM buffers are emptied by the call
 * after it (main+0x36f6080), without the lock. The renderer's output step
 * (main+0x36f5720) returns at once while it has no free buffer, and otherwise reads `pending`.
 *
 * Stop ends the event thread before those two calls. Starting a movie over (the player's Start
 * with the movie at its end, main+0x36f26f0) makes them while the event thread keeps running,
 * so a decoder output that arrives between them meets free buffers and no `pending`: the event
 * thread reads through the null pointer ("reading address 0x28", or 0x4, in the output step).
 * It shows a few seconds after the game starts, as the title screen's movie begins. On
 * the Switch the two calls follow each other within a microsecond; here Windows can preempt the
 * player's thread between them for a whole time slice, which happens often on a busy PC.
 *
 * The fix: Close first empties those lists itself, under the lock - what the next call does
 * anyway. Then no free buffer is listed while `pending` is gone: only Open lists buffers again,
 * after it has made a new `pending`. */
#include "host.h"

#define RENDERER_FREE 0x70   /* std::vector<movie::Buffer>: PCM buffers to fill */
#define RENDERER_FULL 0x88   /* std::vector<movie::Buffer>: PCM buffers filled, for the audio out callback */
#define RENDERER_LOCK 0xB8   /* std::mutex */
#define BUFFER_SIZE 0x40     /* sizeof(movie::Buffer) */

static void empty_buffers(Cpu *c, u64 vector) {
    u64 begin = ld64(vector), end = ld64(vector + 8);
    while (end != begin) {
        end -= BUFFER_SIZE;
        rt_callsym(c, "_ZN5movie6BufferD1Ev", 1, end);
        st64(vector + 8, end);
    }
}

/* The audio renderer's Close. */
/* signature: f81b0ff9 a9015ff8 a90257f6 a9034ff4 a9047bfd 910103fd 320003e8 9102e014 aa0003f3 39004008 aa1403e0 9....... a945a668 cb080129 d343fd29 5295556a */
HLE_WRAP(71036f6400) {
    u64 renderer = c->x[0], lr = c->x[30];
    rt_callsym(c, "_ZNSt3__15mutex4lockEv", 1, renderer + RENDERER_LOCK);
    empty_buffers(c, renderer + RENDERER_FREE);
    empty_buffers(c, renderer + RENDERER_FULL);
    rt_callsym(c, "_ZNSt3__15mutex6unlockEv", 1, renderer + RENDERER_LOCK);
    c->x[0] = renderer;
    c->x[30] = lr;
    f_71036f6400_orig(c);
}
