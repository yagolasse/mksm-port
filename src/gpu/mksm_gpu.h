/*
 * Interface of mksm_gpu (xemu's PGRAPH, built with clang-cl) for code built with MSVC cl.
 * Plain C only: the xemu headers need GNU C, so cl-built code includes this, not them.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up xemu's PGRAPH with the null (opengl = 0) or OpenGL renderer on a scratch
 * NV2AState, prints a status line to stdout, tears it down. Returns 0 on success. Used by the
 * gpu_link checks (plan 2.H.3a, 2.H.4a). */
int mksm_gpu_selftest(int opengl);

/* Host views of the guest memory PGRAPH reads and writes. */
typedef struct MksmGpuMemory {
    void *vram;              /* physical address 0 as the GPU sees it (xemu's vram_ptr) */
    size_t vram_size;
    void *ramin;             /* instance memory (PRAMIN): RAMHT, DMA and graphics objects */
    size_t ramin_size;
    const volatile uint32_t *pfifo; /* the PFIFO register block as the title wrote it
                                       (RAMHT location/size, CACHE1 channel id) */
    const volatile uint32_t *dma_put; /* the channel's DMA_PUT: texture and vertex memory is
                                         re-checked once per kick (NULL: on every use) */
    volatile uint32_t *pgraph_fence; /* the title's view of NV_PGRAPH_PATT_COLOR0, which D3D
                                        polls for its pushbuffer fence (NULL: not kept) */
} MksmGpuMemory;

/* Sets up xemu's PGRAPH on a device that uses these views, with the OpenGL (opengl = 1) or
 * the null renderer. The renderer itself comes up on the thread of the first mksm_gpu_method
 * call, which must be the only thread calling it. Returns 0 on success. */
int mksm_gpu_init(const MksmGpuMemory *mem, int opengl);

/* Receives each finished frame at the flip (OpenGL renderer only): the surface the title
 * drew last, as top-down 0x00RRGGBB rows of `width` pixels. `draws` = draws since the last
 * flip. Runs on the method thread; the pixels are only valid during the call. xrgb = NULL:
 * the frame went straight into the window (mksm_gpu_set_window), only the stats are new. */
typedef void (*MksmGpuPresentFn)(const uint32_t *xrgb, unsigned width, unsigned height,
                                 unsigned draws);
void mksm_gpu_set_present(MksmGpuPresentFn fn);

/* The game window, for presenting into it with OpenGL from the method thread (the toolkit's
 * fb_present.c; plan 2.6b). Called on the method thread; none of them may block on the window
 * thread (the method thread holds pgraph.lock). */
typedef struct MksmGpuWindow {
    void *(*hwnd)(void);                  /* the HWND once the window exists, else NULL */
    void (*gpu_present)(int on);          /* 1: the window stops drawing frames itself */
    int (*client_size)(uint32_t *width, uint32_t *height); /* physical pixels; 0 = not known */
} MksmGpuWindow;

/* With a window set and RECOMP_PB_GPU_PRESENT unset or "window", each flip draws the scanout
 * surface into it (aspect-fit, black bars, RECOMP_PB_GPU_FILTER=nearest|linear) and swaps;
 * until the window exists, frames go to the present callback as before. */
void mksm_gpu_set_window(const MksmGpuWindow *window);

/* The aspect the window present shows each frame at (plan 4.2, B5a: an anamorphic widescreen
 * frame is stretched to it); 0 = the frame's own size (the default). Any thread; the next
 * presented frame uses it. */
void mksm_gpu_set_output_aspect(float aspect);

/* on = 1: a frame wider than 640 gets the columns outside its centred 640 cleared to black at
 * the flip, in the surface itself (plan 4.2, B5b2: the game's menus have art beyond the 640
 * they were made for, which a 4:3 screen never showed). Any thread; the next flip uses it. */
void mksm_gpu_set_side_mask(int on);

/* Receives the parameter of each software notify (NO_OPERATION with a non-zero parameter),
 * which on hardware interrupts the title's GPU handler. Runs on the method thread. */
typedef void (*MksmGpuNotifyFn)(uint32_t param);
void mksm_gpu_set_notify(MksmGpuNotifyFn fn);

/* VBlank pacing: with hz > 0 (the default is 59.94, NTSC) a flip waits for the next
 * VBlank, the way the title's FLIP_STALL waits for the CRTC. 0 turns it off. */
void mksm_gpu_set_vblank_hz(double hz);

/* One VBlank of the host's VBlank clock (main.c's vblank thread; any thread). Once it ticks,
 * flips are paced on its ticks instead of the renderer's own grid, so they line up with the
 * title's VBlank waits. */
void mksm_gpu_vblank_tick(void);

/* One decoded pushbuffer method, in stream order (the toolkit's nv2a_pb_method_hook_fn).
 * Stands in for xemu's PFIFO puller: resolves object handles through RAMHT, then calls
 * pgraph_method. */
void mksm_gpu_method(uint32_t subch, uint32_t method, uint32_t param);

#ifdef __cplusplus
}
#endif
