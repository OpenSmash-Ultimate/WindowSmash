/* The Metal renderer's own header (runtime/macos/gpu_metal.m and gpu_metal_overlay.m): the
 * runtime's headers, which bring the Win32 shim (runtime/linux/include/windows.h), and Apple's
 * frameworks, in one Objective-C file. Some names clash between them: the shim's BOOL (an int)
 * is not Objective-C's, its "#define interface struct" breaks @interface, and Carbon (through
 * Foundation and QuartzCore) has a GetCurrentThread and a GetCurrentProcess of its own. So the
 * shim's BOOL is spelt WINDOWS_BOOL here (callbacks it calls, such as InitOnceExecuteOnce's,
 * return one), and Objective-C's is BOOL. */
#pragma once
#define BOOL WINDOWS_BOOL
#include "debug_profile.h"
#include "gpu_backend.h"
#include "gpu_shader.h"
#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include "net.h"
#include "pad.h"
#undef BOOL
#undef interface
#define GetCurrentThread carbon_GetCurrentThread
#define GetCurrentProcess carbon_GetCurrentProcess
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#undef GetCurrentThread
#undef GetCurrentProcess
#include "display.h"

/* The debug overlay (gpu_metal_overlay.m), as gpu_overlay.c's for Direct3D: called once per
 * presented frame, after the picture is drawn by enc into a target of `format` (at x, y, w x h):
 * notes the frame's time and, if show, draws the overlay over the picture; detail (lines separated
 * by '\n', or NULL) goes under the graph. The settings menu, while open, is drawn instead of the
 * overlay (menu_draw). Leaves the encoder's state changed. */
void overlay_present_metal(id<MTLDevice> dev, id<MTLRenderCommandEncoder> enc, MTLPixelFormat format, unsigned x, unsigned y, unsigned w,
                           unsigned h, int show, const char *detail);

/* For tools/mac/gfxtest.m (and debugging): a render target's pixels as the GPU has them now, read
 * back (waits for the GPU) into out (rows of width * bytes per pixel, at most `size` bytes). Its
 * size and Metal pixel format go to *w, *h, *format. 0 if there is no such surface. */
int gfx_metal_read_surface(u64 guest, int depth, void *out, u32 size, u32 *w, u32 *h, u32 *format);
