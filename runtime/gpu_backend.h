/* The rendering back end, as seen from the GPU command processor (gpu.c): the 3D engine's
 * register file at the moments that produce pixels, and the frames the display shows. */
#pragma once
#include "gpu.h"
#include "maxwell3d.h"

typedef struct CbBinding { u64 address; u32 size; } CbBinding;   /* a bound constant buffer (GPU VA) */

/* CLEAR_SURFACE with the 3D engine's registers (clear values, render target bindings). */
void gfx_clear(const u32 *regs, u32 arg);

/* A draw has been issued (VERTEX_END_GL), with the 3D engine's state as it stands. */
void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed);

/* Show the frame whose pixels live at this guest address (a block-linear surface). */
void gfx_present(u64 guest, u32 width, u32 height);

/* The 2D engine's scaled copy between surfaces (PIXELS_FROM_MEMORY), with its registers. */
void gfx_blit2d(const u32 *regs);
/* A compute launch: the engine's registers (program region, texture pools) and its launch
 * descriptor (QMD): program offset, grid and workgroup size, shared memory, constant buffers. */
void gfx_dispatch(const u32 *regs, const u32 *qmd);
/* Compute results stay on the GPU (gpu_d3d11.c). The copy engine asks: copy [src, src + bytes)
 * to dst there if the source is such data (1 if done); bring a range back to guest memory
 * before reading it otherwise; and refresh the GPU's copy of a range it wrote in guest memory. */
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes);
void gfx_resident_flush(u64 va, u64 bytes);
void gfx_resident_written(u64 va, u64 bytes);
/* The command stream itself wrote guest memory (constant buffer loads, inline-to-memory). */
void gfx_stream_wrote(u64 va, u64 bytes);
/* Other writes of the GPU to guest memory (semaphores, copies into block-linear surfaces;
 * bytes 0: somewhere unknown). */
void gfx_guest_wrote(u64 va, u64 bytes);
void gfx_stream_end(void);   /* (after each submission) */
/* Memory taken away (hle_nv.c, once the GPU thread's queued work has run): the GPU mapping of
 * [va, va + size) changed or went, or guest memory [addr, addr + size) the GPU may read is
 * about to be decommitted. What the back end made from it and keeps is dropped. */
void gfx_gpu_unmapped(u64 va, u64 size);
void gfx_guest_unmapped(u64 addr, u64 size);
int gfx_draw_logging(void);   /* a frame's draws are being logged (debugging) */
void *gfx_window_handle(void);   /* the game's window (an HWND), to own dialogs */
void gfx_status_lines(void);   /* (host.c's [status]) the back end's own lines, if any */
void gfx_closing(void);        /* the game is about to end (Direct3D: see gpu_d3d11.c's "first draws") */
