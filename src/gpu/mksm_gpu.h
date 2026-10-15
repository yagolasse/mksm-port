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
} MksmGpuMemory;

/* Sets up xemu's PGRAPH on a device that uses these views, with the OpenGL (opengl = 1) or
 * the null renderer. The renderer itself comes up on the thread of the first mksm_gpu_method
 * call, which must be the only thread calling it. Returns 0 on success. */
int mksm_gpu_init(const MksmGpuMemory *mem, int opengl);

/* Receives each finished frame at the flip (OpenGL renderer only): the surface the title
 * drew last, as top-down 0x00RRGGBB rows of `width` pixels. `draws` = draws since the last
 * flip. Runs on the method thread; the pixels are only valid during the call. */
typedef void (*MksmGpuPresentFn)(const uint32_t *xrgb, unsigned width, unsigned height,
                                 unsigned draws);
void mksm_gpu_set_present(MksmGpuPresentFn fn);

/* VBlank pacing: with hz > 0 (the default is 59.94, NTSC) a flip waits for the next
 * VBlank, the way the title's FLIP_STALL waits for the CRTC. 0 turns it off. */
void mksm_gpu_set_vblank_hz(double hz);

/* One decoded pushbuffer method, in stream order (the toolkit's nv2a_pb_method_hook_fn).
 * Stands in for xemu's PFIFO puller: resolves object handles through RAMHT, then calls
 * pgraph_method. */
void mksm_gpu_method(uint32_t subch, uint32_t method, uint32_t param);

#ifdef __cplusplus
}
#endif
