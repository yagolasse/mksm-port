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

/* Brings up xemu's PGRAPH with the null renderer on a scratch NV2AState, prints one line to
 * stdout, tears it down. Returns 0 on success. Used by the gpu_link check (plan 2.H.3a). */
int mksm_gpu_selftest(void);

/* Host views of the guest memory PGRAPH reads and writes. */
typedef struct MksmGpuMemory {
    void *vram;              /* physical address 0 as the GPU sees it (xemu's vram_ptr) */
    size_t vram_size;
    void *ramin;             /* instance memory (PRAMIN): RAMHT, DMA and graphics objects */
    size_t ramin_size;
    const volatile uint32_t *pfifo; /* the PFIFO register block as the title wrote it
                                       (RAMHT location/size, CACHE1 channel id) */
} MksmGpuMemory;

/* Sets up xemu's PGRAPH on a device that uses these views. The renderer itself comes up on
 * the thread of the first mksm_gpu_method call, which must be the only thread calling it.
 * Returns 0 on success. */
int mksm_gpu_init(const MksmGpuMemory *mem);

/* One decoded pushbuffer method, in stream order (the toolkit's nv2a_pb_method_hook_fn).
 * Stands in for xemu's PFIFO puller: resolves object handles through RAMHT, then calls
 * pgraph_method. */
void mksm_gpu_method(uint32_t subch, uint32_t method, uint32_t param);

#ifdef __cplusplus
}
#endif
