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
 * Scanout (OpenGL renderer): the title never writes PCRTC_START (D3D's VBlank handler would,
 * and there is none), so the flip shows the surface the title drew into last, as the software
 * executor does. That address goes into pcrtc.start and the VGA hooks report its pitch and
 * size, then xemu's own pgraph_gl_sync renders it into the display buffer, which is read back
 * and handed to the present callback. In xemu the UI thread asks for this and waits; here it
 * runs on the method thread at the flip, before the VBlank wait.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"
#include "mksm_gpu.h"
#include "xemu_stubs.h"
#include "ui/xemu-settings.h"

static NV2AState *s_dev;
static MemoryRegion s_vram;
static const volatile uint32_t *s_pfifo;
static MksmGpuPresentFn s_present;
static double s_vblank_hz = 60000.0 / 1001.0;

static struct {
    unsigned long methods, objects, draws, flips, nops, semaphores, bad_handles;
    unsigned long presents, no_surface, masked_stages;
    unsigned long frame_draws;      /* draws since the last flip */
    unsigned long last_nonblack;    /* non-black pixels of the last presented frame */
    unsigned last_w, last_h;
    int thread_up;
} s_stats;

/* The colour surface the title drew into last (the frame the next flip shows). */
static struct {
    hwaddr addr;
    unsigned pitch, width, height;
} s_scanout;

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

/* xemu's nv2a.c hooks, answered from the scanout surface instead of the VGA CRTC registers
 * (AvSetDisplayMode is high-level here and never programs them). */
static void scanout_get_resolution(VGACommonState *s, int *pwidth, int *pheight)
{
    (void)s;
    *pwidth = (int)s_scanout.width;
    *pheight = (int)s_scanout.height;
}

static void scanout_get_params(VGACommonState *s, VGADisplayParams *params)
{
    (void)s;
    memset(params, 0, sizeof(*params));
    params->line_offset = s_scanout.pitch;
}

void mksm_gpu_set_present(MksmGpuPresentFn fn)
{
    s_present = fn;
}

void mksm_gpu_set_vblank_hz(double hz)
{
    s_vblank_hz = hz;
}

int mksm_gpu_init(const MksmGpuMemory *mem, int opengl)
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
    d->vga.get_resolution = scanout_get_resolution;
    d->vga.get_params = scanout_get_params;
    d->vga.cr[NV_PRMCIO_INTERLACE_MODE] = NV_PRMCIO_INTERLACE_MODE_DISABLED;

    g_config.display.renderer =
        opengl ? CONFIG_DISPLAY_RENDERER_OPENGL : CONFIG_DISPLAY_RENDERER_NULL;
    pgraph_init(d);
    nv2a_context_init();
    /* early_context_init leaves the GL display context current on this thread; the method
     * thread needs it at each flip, and a context is current on one thread at a time. */
    glo_set_current(NULL);
    /* One channel, always current: pgraph_method asserts the channel is valid. */
    pgraph_reg_w(&d->pgraph, NV_PGRAPH_CTX_CONTROL, NV_PGRAPH_CTX_CONTROL_CHID);

    s_dev = d;
    fprintf(stderr, "[XGPU] PGRAPH set up: vram %zu MiB, ramin %zu KiB, %s renderer, "
            "vblank %.2f Hz\n", mem->vram_size >> 20, mem->ramin_size >> 10,
            opengl ? "OpenGL" : "null", s_vblank_hz);
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
            "semaphores %lu, bad handles %lu, irq updates %lu, masked stages %lu, "
            "presents %lu (no surface %lu, last %ux%u, %lu non-black px)\n",
            s_stats.methods, s_stats.objects, s_stats.draws, s_stats.flips, s_stats.nops,
            s_stats.semaphores, s_stats.bad_handles, xemu_stub_counts.update_irq,
            s_stats.masked_stages, s_stats.presents, s_stats.no_surface, s_stats.last_w, s_stats.last_h,
            s_stats.last_nonblack);
}

/* Remembers the colour surface a draw or clear writes, as xemu's surface binding computes
 * its address (colour DMA object base + surface offset). */
static void note_scanout(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    if (!pg->dma_color || !pg->surface_color.pitch) {
        return;
    }
    s_scanout.addr = nv_dma_load(d, pg->dma_color).address + pg->surface_color.offset;
    s_scanout.pitch = pg->surface_color.pitch;
}

/* RECOMP_PB_GPU_DUMP=<path.bmp>: one presented frame as a 24-bit BMP, at the flip given by
 * RECOMP_PB_GPU_DUMP_FLIP (default 600). */
static void maybe_dump(const uint32_t *xrgb, unsigned w, unsigned h)
{
    static const char *path = (const char *)-1;
    static unsigned long at;
    if (path == (const char *)-1) {
        path = getenv("RECOMP_PB_GPU_DUMP");
        const char *f = getenv("RECOMP_PB_GPU_DUMP_FLIP");
        at = f ? strtoul(f, NULL, 0) : 600;
    }
    if (!path || s_stats.flips != at) {
        return;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "[XGPU] can't write %s\n", path);
        return;
    }
    uint32_t row = (w * 3 + 3) & ~3u, img = row * h, total = 54 + img;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), fp);
    uint8_t *line = (uint8_t *)g_malloc0(row);
    for (unsigned y = 0; y < h; y++) {
        const uint32_t *src = xrgb + (size_t)(h - 1 - y) * w;   /* BMP rows are bottom-up */
        for (unsigned x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)src[x];
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        fwrite(line, 1, row, fp);
    }
    g_free(line);
    fclose(fp);
    fprintf(stderr, "[XGPU] dumped flip %lu (%ux%u) to %s\n", s_stats.flips, w, h, path);
}

/* The flip's scanout, OpenGL only: render the last-drawn surface into xemu's display buffer
 * (pgraph_gl_sync, as the UI's get_framebuffer_surface would), read it back, present it.
 * Called with pgraph.lock held. */
static void present_scanout(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    if (!r || !s_scanout.addr) {
        s_stats.no_surface++;
        return;
    }
    SurfaceBinding *surface = pgraph_gl_surface_get_within(d, s_scanout.addr);
    if (!surface || !surface->color || !surface->width || !surface->height) {
        s_stats.no_surface++;
        return;
    }
    s_scanout.width = surface->width;
    s_scanout.height = surface->height;
    d->pcrtc.start = s_scanout.addr;
    pgraph_gl_sync(d);

    static uint32_t *pixels;
    static size_t cap;
    unsigned w = (unsigned)r->gl_display_buffer_width, h = (unsigned)r->gl_display_buffer_height;
    size_t n = (size_t)w * h;
    if (n > cap) {
        pixels = (uint32_t *)g_realloc(pixels, n * 8);
        cap = n;
    }
    /* Read into the second half of the buffer, then flip the rows (GL is bottom-up) into
     * the first. GL_BGRA + 8_8_8_8_REV gives 0xAARRGGBB words. */
    uint32_t *gl_rows = pixels + n;
    glo_set_current(g_nv2a_context_display);
    glBindTexture(GL_TEXTURE_2D, r->gl_display_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, gl_rows);
    glBindTexture(GL_TEXTURE_2D, 0);
    glo_set_current(g_nv2a_context_render);

    unsigned long nonblack = 0;
    for (unsigned y = 0; y < h; y++) {
        const uint32_t *src = gl_rows + (size_t)(h - 1 - y) * w;
        uint32_t *dst = pixels + (size_t)y * w;
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = src[x] & 0x00FFFFFFu;
            dst[x] = v;
            nonblack += v != 0;
        }
    }
    s_stats.presents++;
    s_stats.last_w = w;
    s_stats.last_h = h;
    s_stats.last_nonblack = nonblack;
    maybe_dump(pixels, w, h);
    if (s_present) {
        s_present(pixels, w, h, (unsigned)s_stats.frame_draws);
    }
}

/* Waits for the next VBlank on a fixed grid from the first flip. A flip that is already late
 * (a slow frame, a load) goes at once; one more than a few periods late restarts the grid
 * rather than letting the following frames run uncapped to catch up. */
static void wait_vblank(void)
{
    static HANDLE timer;
    static int64_t next;
    if (s_vblank_hz <= 0) {
        return;
    }
    int64_t period = (int64_t)(1e9 / s_vblank_hz);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!next || now - next > 4 * period) {
        next = now;
    }
    if (now < next) {
        if (!timer) {
            timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due = { .QuadPart = -(next - now) / 100 };   /* relative, 100 ns */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep((DWORD)((next - now) / 1000000));
        }
    }
    next += period;
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

    /* A draw whose shader stage samples a texture that is disabled or has no dimensionality
     * would abort in xemu's pixel shader generator (get_sampler_type asserts): the title
     * leaves PROJECT2D in the stage program with the texture disabled (TEXCTL0 = 0). On
     * hardware a disabled texture unit reads as zero, so the stage is drawn as "no texture":
     * SHADERPROG is masked from BEGIN to END (xemu binds shaders again when it flushes an
     * inline buffer at END) and restored after (counted, the first few logged). */
    static uint32_t restore_prog;
    static bool restore_pending;
    uint32_t prog = 0, masked = 0;
    if (subch == 0 && method == NV097_SET_BEGIN_END && param != NV097_SET_BEGIN_END_OP_END) {
        prog = masked = pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG);
        for (int i = 0; i < 4; i++) {
            uint32_t mode = (prog >> (i * 5)) & 0x1F;
            uint32_t ctl = pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + i * 4);
            uint32_t fmt = pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + i * 4);
            bool samples = mode == PS_TEXTUREMODES_PROJECT2D || mode == PS_TEXTUREMODES_PROJECT3D ||
                           mode == PS_TEXTUREMODES_CUBEMAP || mode == PS_TEXTUREMODES_BUMPENVMAP ||
                           mode == PS_TEXTUREMODES_BUMPENVMAP_LUM;
            if (samples && (!(ctl & NV_PGRAPH_TEXCTL0_0_ENABLE) ||
                            !GET_MASK(fmt, NV_PGRAPH_TEXFMT0_DIMENSIONALITY))) {
                masked &= ~(0x1Fu << (i * 5));
                if (s_stats.masked_stages++ < 8) {
                    fprintf(stderr, "[XGPU] draw %lu: stage %d mode %u samples an unusable "
                            "texture (texctl0 0x%08X, texfmt 0x%08X), drawn untextured\n",
                            s_stats.draws, i, mode, ctl, fmt);
                }
            }
        }
    }

    qemu_mutex_lock(&pg->lock);
    if (masked != prog) {
        pgraph_reg_w(pg, NV_PGRAPH_SHADERPROG, masked);
        restore_prog = prog;
        restore_pending = true;
    }
    pgraph_method(d, subch, method, param, &param, 1, 1, true);
    if (restore_pending && subch == 0 && method == NV097_SET_BEGIN_END &&
        param == NV097_SET_BEGIN_END_OP_END) {
        pgraph_reg_w(pg, NV_PGRAPH_SHADERPROG, restore_prog);
        restore_pending = false;
    }
    qemu_mutex_unlock(&pg->lock);

    if (subch == 0) {
        switch (method) {
        case NV097_SET_BEGIN_END:
            if (param != NV097_SET_BEGIN_END_OP_END) {
                s_stats.draws++;
                s_stats.frame_draws++;
                note_scanout(d);
            }
            break;
        case NV097_CLEAR_SURFACE:
            note_scanout(d);
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
        qemu_mutex_lock(&pg->lock);
        present_scanout(d);
        qemu_mutex_unlock(&pg->lock);
        s_stats.frame_draws = 0;
        wait_vblank();
        if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SURFACE), NV_PGRAPH_SURFACE_MODULO_3D)) {
            pgraph_write(d, NV_PGRAPH_INCREMENT, NV_PGRAPH_INCREMENT_READ_3D, 4);
        }
        pg->waiting_for_flip = false;
    }
    report();
}
