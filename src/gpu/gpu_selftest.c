/*
 * mksm_gpu_selftest (see mksm_gpu.h): xemu's PGRAPH + the null or OpenGL renderer on a
 * scratch NV2AState. It fails if the renderer's constructor did not run (the archive was not
 * linked whole), the Win32 lock/event shims misbehave, or (OpenGL) no 4.0 core context with
 * the extensions xemu asserts on can be created.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "mksm_gpu.h"
#include "xemu_stubs.h"
#include "ui/xemu-settings.h"
#include <epoxy/gl.h>

#define RAMIN_SIZE (1 * MiB)
#define VRAM_SIZE (64 * MiB)

int mksm_gpu_selftest(int opengl)
{
    CONFIG_DISPLAY_RENDERER want =
        opengl ? CONFIG_DISPLAY_RENDERER_OPENGL : CONFIG_DISPLAY_RENDERER_NULL;
    g_config.display.renderer = want;

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
     * up as the check below or a crash, never as a pass. It also runs every registered
     * renderer's early_context_init, so the GL contexts are created in both modes. */
    pgraph_init(d);
    nv2a_context_init();
    pgraph_init_thread(d);

    int ok = g_nv2a == d && d->pgraph.renderer && d->pgraph.renderer->type == want;
    if (!ok) {
        printf("mksm_gpu: FAIL: %s renderer not active\n", opengl ? "OpenGL" : "null");
        return 1;
    }
    if (opengl) {
        /* pgraph_gl_init leaves its render context current on this thread. */
        GLenum err = glad_glGetError();
        printf("mksm_gpu: GL %s | %s | %s\n", (const char *)glGetString(GL_VERSION),
               (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR));
        if (err != GL_NO_ERROR) {
            printf("mksm_gpu: FAIL: GL error 0x%x after renderer init\n", err);
            return 1;
        }
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
