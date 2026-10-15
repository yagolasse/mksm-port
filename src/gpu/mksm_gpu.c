/*
 * mksm_gpu_init / mksm_gpu_method (see mksm_gpu.h): xemu's PGRAPH fed by the toolkit's
 * pushbuffer decoder instead of xemu's PFIFO.
 *
 * What xemu's pfifo.c puller does around pgraph_method, done here per method:
 *   - SET_OBJECT (method 0) and the object methods 0x180-0x1FC carry a handle; the puller
 *     looks it up in RAMHT and hands PGRAPH the object's instance address;
 *   - pgraph_method runs under pgraph.lock.
 * Left out: the context switch (one channel; CTX_CONTROL is marked valid at init) and the
 * puller's stalls. Nothing here delivers NV2A interrupts to the title, and the stream can't
 * be held back, so the two waits PGRAPH can enter are completed at once, the way the
 * title's interrupt handler and the VBlank would:
 *   - NO_OPERATION with a parameter (a software notify): the error interrupt is acked;
 *   - FLIP_STALL: one VBlank's NV_PGRAPH_INCREMENT (read buffer + 1).
 * The software executor never raised either, so the title sees the same behaviour.
 * pfifo_kick / nv2a_update_irq stay call counters (xemu_stubs.c): there is no FIFO thread
 * to wake, and no interrupt line.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "mksm_gpu.h"
#include "xemu_stubs.h"

static NV2AState *s_dev;
static MemoryRegion s_vram;
static const volatile uint32_t *s_pfifo;

static struct {
    unsigned long methods, objects, draws, flips, nops, semaphores, bad_handles;
    int thread_up;
} s_stats;

#define PFIFO_REG(off) (s_pfifo[(off) / 4])

/* xemu's ramht_hash + ramht_lookup (pfifo.c), reading the PFIFO registers the title wrote. */
static bool ramht_lookup(uint32_t handle, uint32_t *instance)
{
    uint32_t ramht = PFIFO_REG(NV_PFIFO_RAMHT);
    uint32_t ramht_size = 1u << (GET_MASK(ramht, NV_PFIFO_RAMHT_SIZE) + 12);
    uint32_t ramht_base = GET_MASK(ramht, NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    unsigned int bits = ctz32(ramht_size) - 1;
    unsigned int chid = GET_MASK(PFIFO_REG(NV_PFIFO_CACHE1_PUSH1), NV_PFIFO_CACHE1_PUSH1_CHID);

    uint32_t hash = 0;
    for (uint32_t h = handle; h; h >>= bits) {
        hash ^= h & ((1u << bits) - 1);
    }
    hash ^= chid << (bits - 4);

    if (ramht_base + hash * 8 + 8 > s_dev->ramin.size) {
        return false;
    }
    const uint8_t *e = s_dev->ramin_ptr + ramht_base + hash * 8;
    uint32_t ctx = ldl_le_p(e + 4);
    if (ldl_le_p(e) != handle || !(ctx & NV_RAMHT_STATUS)) {
        return false;
    }
    *instance = (ctx & NV_RAMHT_INSTANCE) << 4;
    return true;
}

int mksm_gpu_init(const MksmGpuMemory *mem)
{
    /* xemu runs with asserts on; report them on stderr, not in a message box that would
     * block a headless run. */
    _set_error_mode(_OUT_TO_STDERR);

    NV2AState *d = (NV2AState *)g_malloc0(sizeof(*d));
    s_vram.size = mem->vram_size;
    s_vram.ptr = (uint8_t *)mem->vram;
    d->vram = &s_vram;
    d->vram_ptr = s_vram.ptr;
    d->ramin.size = mem->ramin_size;
    d->ramin.ptr = (uint8_t *)mem->ramin;
    d->ramin_ptr = d->ramin.ptr;
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);
    s_pfifo = mem->pfifo;

    pgraph_init(d);
    nv2a_context_init();
    /* One channel, always current: pgraph_method asserts the channel is valid. */
    pgraph_reg_w(&d->pgraph, NV_PGRAPH_CTX_CONTROL, NV_PGRAPH_CTX_CONTROL_CHID);

    s_dev = d;
    fprintf(stderr, "[XGPU] PGRAPH set up: vram %zu MiB, ramin %zu KiB\n",
            mem->vram_size >> 20, mem->ramin_size >> 10);
    return 0;
}

static void report(void)
{
    static int64_t last_ns;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - last_ns < 5000000000LL) {
        return;
    }
    last_ns = now;
    fprintf(stderr,
            "[XGPU] methods %lu, objects %lu, draws %lu, flips %lu, nop notifies %lu, "
            "semaphores %lu, bad handles %lu, irq updates %lu\n",
            s_stats.methods, s_stats.objects, s_stats.draws, s_stats.flips, s_stats.nops,
            s_stats.semaphores, s_stats.bad_handles, xemu_stub_counts.update_irq);
}

void mksm_gpu_method(uint32_t subch, uint32_t method, uint32_t param)
{
    NV2AState *d = s_dev;
    PGRAPHState *pg = &d->pgraph;

    /* xemu brings the renderer up on the thread that will run PGRAPH. */
    if (!s_stats.thread_up) {
        s_stats.thread_up = 1;
        pgraph_init_thread(d);
        fprintf(stderr, "[XGPU] renderer '%s' up on thread %lu\n",
                pg->renderer ? pg->renderer->name : "none", GetCurrentThreadId());
    }
    s_stats.methods++;

    if (method == NV_SET_OBJECT || (method >= 0x180 && method < 0x200)) {
        uint32_t instance;
        if (!ramht_lookup(param, &instance)) {
            if (s_stats.bad_handles++ < 8) {
                fprintf(stderr, "[XGPU] subch %u method 0x%04X: handle 0x%08X not in RAMHT, "
                        "dropped\n", subch, method, param);
            }
            return;
        }
        if (method == NV_SET_OBJECT) {
            s_stats.objects++;
        }
        param = instance;
    }

    qemu_mutex_lock(&pg->lock);
    pgraph_method(d, subch, method, param, &param, 1, 1, true);
    qemu_mutex_unlock(&pg->lock);

    if (subch == 0) {
        switch (method) {
        case NV097_SET_BEGIN_END:
            if (param != NV097_SET_BEGIN_END_OP_END) {
                s_stats.draws++;
            }
            break;
        case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
            s_stats.semaphores++;
            break;
        default:
            break;
        }
    }

    /* The title's interrupt handler acks the notify (it writes NV_PGRAPH_INTR). */
    if (pg->waiting_for_nop) {
        s_stats.nops++;
        pgraph_write(d, NV_PGRAPH_INTR, NV_PGRAPH_INTR_ERROR, 4);
    }
    /* The VBlank moves the read buffer on (NV_PGRAPH_INCREMENT), which ends the stall. */
    if (pg->waiting_for_flip) {
        s_stats.flips++;
        if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SURFACE), NV_PGRAPH_SURFACE_MODULO_3D)) {
            pgraph_write(d, NV_PGRAPH_INCREMENT, NV_PGRAPH_INCREMENT_READ_3D, 4);
        }
        pg->waiting_for_flip = false;
    }
    report();
}
