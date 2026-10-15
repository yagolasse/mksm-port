/*
 * mksm_gpu_selftest (see mksm_gpu.h): xemu's PGRAPH + the null renderer on a scratch
 * NV2AState. It fails if the null renderer's constructor did not run (the archive was not
 * linked whole) or the Win32 lock/event shims misbehave.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "mksm_gpu.h"
#include "xemu_stubs.h"

#define RAMIN_SIZE (1 * MiB)
#define VRAM_SIZE (64 * MiB)

int mksm_gpu_selftest(void)
{
    NV2AState *d = (NV2AState *)g_malloc0(sizeof(*d));
    MemoryRegion vram = { .size = VRAM_SIZE, .ptr = (uint8_t *)g_malloc0(VRAM_SIZE) };
    d->vram = &vram;
    d->vram_ptr = vram.ptr;
    d->ramin.size = RAMIN_SIZE;
    d->ramin.ptr = (uint8_t *)g_malloc0(RAMIN_SIZE);
    d->ramin_ptr = d->ramin.ptr;
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    /* nv2a_context_init dereferences the configured renderer: a missing registration shows
     * up as the check below or a crash, never as a pass. */
    pgraph_init(d);
    nv2a_context_init();
    pgraph_init_thread(d);

    int ok = g_nv2a == d && d->pgraph.renderer &&
             d->pgraph.renderer->type == CONFIG_DISPLAY_RENDERER_NULL;
    if (!ok) {
        printf("mksm_gpu: FAIL: null renderer not active\n");
        return 1;
    }
    unsigned int scale = nv2a_get_surface_scale_factor();
    printf("mksm_gpu: OK: renderer '%s', surface scale %u, pfifo_kick %lu, update_irq %lu\n",
           d->pgraph.renderer->name, scale, xemu_stub_counts.pfifo_kick,
           xemu_stub_counts.update_irq);

    pgraph_destroy(&d->pgraph);
    g_free(d->ramin.ptr);
    g_free(vram.ptr);
    g_free(d);
    return 0;
}
