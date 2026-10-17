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
 *   - NO_OPERATION with a parameter (a software notify): the error interrupt is acked, and
 *     the parameter goes to the notify callback, which does what the title's handler would;
 *   - FLIP_STALL: one VBlank's NV_PGRAPH_INCREMENT (read buffer + 1).
 * PGRAPH's registers live in xemu's state, not in the guest's register page, so the one the
 * title reads back is copied there: NV_PGRAPH_PATT_COLOR0, D3D's pushbuffer fence (see
 * mirror_fence).
 * The software executor never raised either, so the title sees the same behaviour.
 * pfifo_kick / nv2a_update_irq stay call counters (xemu_stubs.c): there is no FIFO thread
 * to wake, and no interrupt line.
 *
 * Scanout (OpenGL renderer): the title never writes PCRTC_START (D3D's VBlank handler would,
 * and there is none), so the flip shows the surface the title drew into last, as the software
 * executor does. That address goes into pcrtc.start and the VGA hooks report its pitch and
 * size. Once the game window exists, the surface is drawn straight into it from a second
 * context (present_window); before that, or with RECOMP_PB_GPU_PRESENT=pbo, it is read back
 * without waiting for the GPU and handed to the present callback one flip later
 * (present_async; =sync = xemu's own display pass, pgraph_gl_sync, as xemu's UI thread asks
 * for it). All run on the method thread at the flip, before the VBlank wait.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"
#include "mksm_gpu.h"
#include "xemu_stubs.h"
#include "gloffscreen_wgl.h"
#include "ui/xemu-settings.h"

/* shader_patch.c */
int mksm_shader_patch_install(unsigned surface_scale, unsigned (*nearest_mask)(void));

static NV2AState *s_dev;
static MemoryRegion s_vram;
static const volatile uint32_t *s_pfifo;
static volatile uint32_t *s_fence;
static MksmGpuPresentFn s_present;
static MksmGpuNotifyFn s_notify;
static double s_vblank_hz = 60000.0 / 1001.0;

static struct {
    unsigned long methods, objects, draws, flips, nops, fences, semaphores, bad_handles;
    unsigned long presents, no_surface, masked_stages;
    unsigned long surf_downloads, surf_uploads, surf_to_tex; /* xemu's counters, summed */
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

void mksm_gpu_set_notify(MksmGpuNotifyFn fn)
{
    s_notify = fn;
}

void mksm_gpu_set_vblank_hz(double hz)
{
    s_vblank_hz = hz;
}

/* For shader_patch.c, at each GL draw (method thread, pg->lock held): the enabled texture
 * stages whose magnification filter is nearest (NV_PGRAPH_TEXFILTER0_MAG = 1, GL_NEAREST in
 * xemu's map), one bit per stage. */
static unsigned nearest_stages(void)
{
    PGRAPHState *pg = &s_dev->pgraph;
    unsigned mask = 0;
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (pgraph_is_texture_enabled(pg, i) &&
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + i * 4),
                     NV_PGRAPH_TEXFILTER0_MAG) == 1) {
            mask |= 1u << i;
        }
    }
    return mask;
}

int mksm_gpu_init(const MksmGpuMemory *mem, int opengl)
{
    /* xemu runs with asserts on; report them on stderr, not in a message box that would
     * block a headless run. */
    _set_error_mode(_OUT_TO_STDERR);

    /* Real glGetError checks (epoxy/gl.h shim) only on request: they stall the driver. */
    const char *glcheck = getenv("RECOMP_PB_GPU_GLCHECK");
    mksm_gl_check_errors = glcheck && *glcheck && *glcheck != '0';

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
    s_fence = mem->pgraph_fence;
    /* RECOMP_PB_GPU_DIRTY=kick: texture pages read dirty once per kick without the content
     * hash (the 2.H.8a answer, for A/B runs). */
    const char *dirty = getenv("RECOMP_PB_GPU_DIRTY");
    xemu_stubs_set_dma_put(mem->dma_put, !(dirty && strcmp(dirty, "kick") == 0));
    d->vga.get_resolution = scanout_get_resolution;
    d->vga.get_params = scanout_get_params;
    d->vga.cr[NV_PRMCIO_INTERLACE_MODE] = NV_PRMCIO_INTERLACE_MODE_DISABLED;

    g_config.display.renderer =
        opengl ? CONFIG_DISPLAY_RENDERER_OPENGL : CONFIG_DISPLAY_RENDERER_NULL;
    /* RECOMP_PB_GPU_SCALE=1..4: xemu's internal resolution scale. The GL renderer reads it
     * when it sets up its surfaces; surfaces are then drawn at width x height x scale, CPU
     * reads (downloads) are downscaled back to 1x, and the present copies the scaled surface. */
    const char *scale = getenv("RECOMP_PB_GPU_SCALE");
    if (scale && *scale) {
        long v = strtol(scale, NULL, 10);
        g_config.display.quality.surface_scale = v < 1 ? 1 : v > 4 ? 4 : (int)v;
    }
    pgraph_init(d);
    nv2a_context_init();
    /* early_context_init leaves the GL display context current on this thread; the method
     * thread needs it at each flip, and a context is current on one thread at a time. */
    glo_set_current(NULL);
    int snap2d = opengl &&
        mksm_shader_patch_install((unsigned)g_config.display.quality.surface_scale,
                                  nearest_stages);
    /* One channel, always current: pgraph_method asserts the channel is valid. */
    pgraph_reg_w(&d->pgraph, NV_PGRAPH_CTX_CONTROL, NV_PGRAPH_CTX_CONTROL_CHID);

    s_dev = d;
    fprintf(stderr, "[XGPU] PGRAPH set up: vram %zu MiB, ramin %zu KiB, %s renderer, "
            "vblank %.2f Hz, surface scale %ux%s\n", mem->vram_size >> 20, mem->ramin_size >> 10,
            opengl ? "OpenGL" : "null", s_vblank_hz,
            (unsigned)g_config.display.quality.surface_scale, snap2d ? ", 2D snap" : "");
    return 0;
}

static void report(void)
{
    static int64_t last_ns;
    /* Runs after every method: read the clock only every 16k of them (a QPC per method was
     * 6% of the method thread in a level). */
    if (s_stats.methods & 0x3FFF) {
        return;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (now - last_ns < 5000000000LL) {
        return;
    }
    /* With few methods per second (movies) the line comes well after 5 s: it ends with the
     * real interval, which a rate from two lines must divide by. */
    double interval_s = last_ns ? (now - last_ns) / 1e9 : 0.0;
    last_ns = now;
    fprintf(stderr,
            "[XGPU] methods %lu, objects %lu, draws %lu, flips %lu, nop notifies %lu, "
            "fences %lu, semaphores %lu, bad handles %lu, irq updates %lu, dirty epochs %lu, "
            "tex pages hashed %lu changed %lu, surface downloads %lu uploads %lu to-tex %lu, "
            "masked stages %lu, presents %lu (no surface %lu, last %ux%u, %lu non-black px), "
            "%.2f s\n",
            s_stats.methods, s_stats.objects, s_stats.draws, s_stats.flips, s_stats.nops,
            s_stats.fences, s_stats.semaphores, s_stats.bad_handles, xemu_stub_counts.update_irq,
            xemu_stub_counts.dirty_epochs, xemu_stub_counts.tex_pages_hashed,
            xemu_stub_counts.tex_pages_changed,
            s_stats.surf_downloads, s_stats.surf_uploads, s_stats.surf_to_tex,
            s_stats.masked_stages, s_stats.presents, s_stats.no_surface, s_stats.last_w, s_stats.last_h,
            s_stats.last_nonblack, interval_s);
}

/* D3D's pushbuffer fence: it sends a context-pattern object's SET_MONOCHROME_COLOR0 with a
 * (pushbuffer position, 5-bit counter) word, which PGRAPH keeps in NV_PGRAPH_PATT_COLOR0, and
 * reads that register to learn how far the GPU has got (free pushbuffer space, and the wait
 * after a kick for the counter it just sent). Copied to the guest's register page whenever
 * it changes; called under pgraph.lock. */
static void mirror_fence(PGRAPHState *pg)
{
    uint32_t v = pgraph_reg_r(pg, NV_PGRAPH_PATT_COLOR0);
    if (s_fence && *s_fence != v) {
        *s_fence = v;
        s_stats.fences++;
    }
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

/* RECOMP_PB_GPU_DUMP=<path.bmp>: presented frames as 24-bit BMPs, at the flips listed in
 * RECOMP_PB_GPU_DUMP_FLIP (comma-separated, up to 16; default 600). With more than one flip
 * each file is <path minus .bmp>_<flip>.bmp. */
#define DUMP_MAX 16
static struct {
    int parsed;
    const char *path;
    unsigned long at[DUMP_MAX];
    int n;
} s_dump;

static const char *dump_path(void)
{
    if (!s_dump.parsed) {
        s_dump.parsed = 1;
        s_dump.path = getenv("RECOMP_PB_GPU_DUMP");
        const char *f = getenv("RECOMP_PB_GPU_DUMP_FLIP");
        char *end;
        for (const char *p = f; p && *p && s_dump.n < DUMP_MAX; p = *end ? end + 1 : end) {
            unsigned long v = strtoul(p, &end, 0);
            if (end == p) {
                break;
            }
            s_dump.at[s_dump.n++] = v;
        }
        if (!s_dump.n) {
            s_dump.at[s_dump.n++] = 600;
        }
    }
    return s_dump.path;
}

/* Is flip f + ahead one to dump? */
static int dump_wanted(unsigned long f, unsigned long ahead)
{
    if (!dump_path()) {
        return 0;
    }
    for (int i = 0; i < s_dump.n; i++) {
        if (f + ahead == s_dump.at[i]) {
            return 1;
        }
    }
    return 0;
}

static void maybe_dump(const uint32_t *xrgb, unsigned w, unsigned h)
{
    if (!dump_wanted(s_stats.flips, 0)) {
        return;
    }
    char named[1024];
    const char *path = s_dump.path;
    if (s_dump.n > 1) {
        size_t len = strlen(path);
        if (len > 4 && _stricmp(path + len - 4, ".bmp") == 0) {
            len -= 4;
        }
        snprintf(named, sizeof(named), "%.*s_%lu.bmp", (int)len, path, s_stats.flips);
        path = named;
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

/* Hands one frame (0xAARRGGBB words, rows `src_step` words apart, the first row at `src`)
 * to the present callback as top-down 0x00RRGGBB rows. hand_over = false: a frame read back
 * only for the stats and the dump (the window present already showed it). */
static void present_pixels(const uint32_t *src, ptrdiff_t src_step, unsigned w, unsigned h,
                           unsigned draws, bool hand_over)
{
    static uint32_t *pixels;
    static size_t cap;
    size_t n = (size_t)w * h;
    if (n > cap) {
        pixels = (uint32_t *)g_realloc(pixels, n * 4);
        cap = n;
    }
    unsigned long nonblack = 0;
    for (unsigned y = 0; y < h; y++, src += src_step) {
        uint32_t *dst = pixels + (size_t)y * w;
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = src[x] & 0x00FFFFFFu;
            dst[x] = v;
            nonblack += v != 0;
        }
    }
    s_stats.last_nonblack = nonblack;
    maybe_dump(pixels, w, h);
    if (hand_over) {
        s_stats.presents++;
        s_stats.last_w = w;
        s_stats.last_h = h;
        if (s_present) {
            s_present(pixels, w, h, draws);
        }
    }
}

/* The scanout surface: the one the title drew into last, as xemu's pgraph_gl_sync finds it. */
static SurfaceBinding *scanout_surface(NV2AState *d)
{
    if (!d->pgraph.gl_renderer_state || !s_scanout.addr) {
        return NULL;
    }
    SurfaceBinding *surface = pgraph_gl_surface_get_within(d, s_scanout.addr);
    if (!surface || !surface->color || !surface->width || !surface->height) {
        return NULL;
    }
    s_scanout.width = surface->width;
    s_scanout.height = surface->height;
    return surface;
}

/* RECOMP_PB_GPU_PRESENT=sync: xemu's own display path, as the UI's get_framebuffer_surface
 * would use it: pgraph_gl_sync renders the surface into the display buffer in the display
 * context (waiting for the GPU twice), then a synchronous glGetTexImage reads it back. */
static void present_sync(NV2AState *d, SurfaceBinding *surface)
{
    (void)surface;
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    d->pcrtc.start = s_scanout.addr;
    pgraph_gl_sync(d);

    static uint32_t *gl_rows;
    static size_t cap;
    unsigned w = (unsigned)r->gl_display_buffer_width, h = (unsigned)r->gl_display_buffer_height;
    size_t n = (size_t)w * h;
    if (n > cap) {
        gl_rows = (uint32_t *)g_realloc(gl_rows, n * 4);
        cap = n;
    }
    /* GL_BGRA + 8_8_8_8_REV gives 0xAARRGGBB words; the display buffer is bottom-up. */
    glo_set_current(g_nv2a_context_display);
    glBindTexture(GL_TEXTURE_2D, r->gl_display_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, gl_rows);
    glBindTexture(GL_TEXTURE_2D, 0);
    glo_set_current(g_nv2a_context_render);
    present_pixels(gl_rows + (size_t)(h - 1) * w, -(ptrdiff_t)w, w, h,
                   (unsigned)s_stats.frame_draws, true);
}

/* The default present: the scanout surface is read straight from its texture in the render
 * context into a pixel-pack buffer, and that buffer is mapped one flip later, when the GPU
 * has long finished it. No context switch, no wait for the GPU; the window shows each frame
 * one flip late. xemu's display pass only adds the PVIDEO overlay (its registers are never
 * written here) and the scale to the CRTC mode (which is the surface size here), so a plain
 * read of the surface gives the same pixels. The surface texture's first row is the top one.
 * A surface format GL can't convert to BGRA8 (none seen) would need the display pass.
 * hand_over: see present_pixels. */
static void present_async(NV2AState *d, SurfaceBinding *surface, bool hand_over)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    static GLuint fbo, pbo[2];
    static struct {
        GLsync fence;
        unsigned w, h, draws;
    } slot[2];
    static unsigned cur;

    if (!fbo) {
        glGenFramebuffers(1, &fbo);
        glGenBuffers(2, pbo);
    }
    if (surface->upload_pending) {
        pgraph_gl_upload_surface_data(d, surface, !tcg_enabled());
    }

    /* Start this frame's read. */
    unsigned scale = pg->surface_scale_factor ? pg->surface_scale_factor : 1;
    unsigned w = surface->width * scale, h = surface->height * scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           surface->gl_buffer, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[cur]);
    if (slot[cur].w * slot[cur].h != w * h) {
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->gl_framebuffer);
    if (slot[cur].fence) {
        glDeleteSync(slot[cur].fence);
    }
    slot[cur].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot[cur].w = w;
    slot[cur].h = h;
    slot[cur].draws = (unsigned)s_stats.frame_draws;
    glFlush();

    /* Present the previous frame's. */
    unsigned prev = cur ^ 1;
    cur = prev;
    if (slot[prev].fence) {
        glClientWaitSync(slot[prev].fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
        glDeleteSync(slot[prev].fence);
        slot[prev].fence = NULL;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[prev]);
        const uint32_t *px = (const uint32_t *)glMapBufferRange(
            GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)slot[prev].w * slot[prev].h * 4,
            GL_MAP_READ_BIT);
        if (px) {
            present_pixels(px, slot[prev].w, slot[prev].w, slot[prev].h, slot[prev].draws,
                           hand_over);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

/* The window present (plan 2.6b): the scanout surface goes straight into the game window, no
 * readback and no GDI, and shows at its own flip.
 *
 * The method thread only copies the surface on the GPU (glCopyImageSubData) into one of three
 * textures, fences the copy and hands the texture to a present thread. That thread owns a
 * second context on the window's DC (sharing objects with the render context), waits for the
 * fence on the GPU (glWaitSync), draws the texture as one triangle into an aspect-fit viewport
 * (black bars) and swaps. Drawing and swapping on the method thread itself (switching contexts
 * twice per flip) turned NVIDIA's driver thread off and put SwapBuffers' waits on the method
 * thread: 74% -> 92% busy, level dips to 52 fps (plan 2.6b).
 *
 * The three textures are a mailbox: one the present thread shows, at most one waiting
 * (`ready`; a newer frame replaces it), and the method thread fills one that is neither. The
 * window's own thread keeps messages, focus and keys and draws nothing. */
enum { WIN_SLOTS = 3 };

static struct {
    MksmGpuWindow fns;
    bool have_fns;
    volatile LONG failed, up;     /* set by the present thread */
    GloContext *ctx;
    CRITICAL_SECTION lock;
    HANDLE wake;                  /* auto-reset: a frame is ready */
    int ready, showing;           /* slot indices or -1; under lock */
    struct {
        GLuint tex;
        GLenum fmt;
        unsigned w, h;            /* texture size (scaled surface) */
        unsigned aw, ah;          /* the surface's size, for the aspect */
        GLsync fence;
    } slot[WIN_SLOTS];
} s_win;

void mksm_gpu_set_window(const MksmGpuWindow *window)
{
    s_win.fns = *window;
    s_win.have_fns = true;
}

/* The output aspect as float bits, 0 = the surface's own (mksm_gpu_set_output_aspect). */
static volatile LONG s_output_aspect;

void mksm_gpu_set_output_aspect(float aspect)
{
    LONG bits;
    memcpy(&bits, &aspect, sizeof(bits));
    InterlockedExchange(&s_output_aspect, aspect > 0.0f ? bits : 0);
}

static volatile LONG s_side_mask;

void mksm_gpu_set_side_mask(int on)
{
    InterlockedExchange(&s_side_mask, on != 0);
}

/* mksm_gpu_set_side_mask: clears the columns of a wider-than-640 surface outside its centred
 * 640 to black, through a framebuffer of our own; xemu's GL state is put back. */
static void mask_sides(NV2AState *d, SurfaceBinding *surface)
{
    static GLuint fbo;
    if (!s_side_mask || surface->width <= 640) {
        return;
    }
    if (surface->upload_pending) {
        pgraph_gl_upload_surface_data(d, surface, !tcg_enabled());
    }
    unsigned scale = d->pgraph.surface_scale_factor ? d->pgraph.surface_scale_factor : 1;
    GLint side = (GLint)((surface->width - 640) / 2 * scale);
    GLint w = (GLint)(surface->width * scale), h = (GLint)(surface->height * scale);
    GLint old_fbo, box[4];
    GLfloat color[4];
    GLboolean mask[4], scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    if (!fbo) {
        glGenFramebuffers(1, &fbo);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           surface->gl_buffer, 0);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glScissor(0, 0, side, h);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(w - side, 0, side, h);
    glClear(GL_COLOR_BUFFER_BIT);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_fbo);
    glScissor(box[0], box[1], box[2], box[3]);
    glClearColor(color[0], color[1], color[2], color[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (!scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "[XGPU] window present: shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* The present thread's program: a triangle covering the viewport, no vertex data. The
 * surface's first texture row is its top line, so t runs downwards. 0 on failure. */
static GLuint window_program(void)
{
    static const char vs_src[] =
        "#version 400 core\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    uv = vec2(p.x, 1.0 - p.y);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fs_src[] =
        "#version 400 core\n"
        "in vec2 uv;\n"
        "uniform sampler2D tex;\n"
        "layout(location = 0) out vec4 color;\n"
        "void main() {\n"
        "    color = vec4(texture(tex, uv).rgb, 1.0);\n"
        "}\n";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = 0;
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            fprintf(stderr, "[XGPU] window present: program link failed\n");
            glDeleteProgram(prog);
            prog = 0;
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static DWORD WINAPI present_thread(LPVOID unused)
{
    (void)unused;
    glo_set_current(s_win.ctx);
    GLuint prog = window_program();
    if (!prog) {
        fprintf(stderr, "[XGPU] window present unavailable, reading frames back instead\n");
        InterlockedExchange(&s_win.failed, 1);
        glo_set_current(NULL);
        return 0;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    GLuint vao, sampler;
    glGenVertexArrays(1, &vao);    /* core profile draws need one bound */
    glBindVertexArray(vao);
    /* A sampler object, so the textures' own filter state doesn't matter. */
    const char *f = getenv("RECOMP_PB_GPU_FILTER");
    GLint filter = f && strcmp(f, "nearest") == 0 ? GL_NEAREST : GL_LINEAR;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindSampler(0, sampler);
    glActiveTexture(GL_TEXTURE0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    s_win.fns.gpu_present(1);
    InterlockedExchange(&s_win.up, 1);
    fprintf(stderr, "[XGPU] presenting into the game window from thread %lu (%s filter)\n",
            GetCurrentThreadId(), filter == GL_NEAREST ? "nearest" : "linear");

    for (;;) {
        WaitForSingleObject(s_win.wake, INFINITE);
        EnterCriticalSection(&s_win.lock);
        int i = s_win.ready;
        if (i >= 0) {
            s_win.showing = i;
            s_win.ready = -1;
        }
        LeaveCriticalSection(&s_win.lock);
        if (i < 0) {
            continue;
        }
        if (s_win.slot[i].fence) {
            glWaitSync(s_win.slot[i].fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_win.slot[i].fence);
            s_win.slot[i].fence = NULL;
        }
        uint32_t cw = 0, ch = 0;
        s_win.fns.client_size(&cw, &ch);
        if (!cw || !ch) {    /* minimised */
            continue;
        }
        glViewport(0, 0, (GLsizei)cw, (GLsizei)ch);
        glClear(GL_COLOR_BUFFER_BIT);
        /* The largest rect of the surface's aspect (or the output aspect) that fits, centred. */
        LONG bits = s_output_aspect;
        float out_aspect;
        memcpy(&out_aspect, &bits, sizeof(out_aspect));
        double aspect = bits ? out_aspect : (double)s_win.slot[i].aw / s_win.slot[i].ah;
        uint32_t w = cw, h = ch;
        if (cw > ch * aspect) {
            w = (uint32_t)(ch * aspect + 0.5);
        } else {
            h = (uint32_t)(cw / aspect + 0.5);
        }
        glViewport((GLint)(cw - w) / 2, (GLint)(ch - h) / 2, (GLsizei)w, (GLsizei)h);
        glBindTexture(GL_TEXTURE_2D, s_win.slot[i].tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glo_swap_buffers(s_win.ctx);
    }
}

/* Returns false while the window can't be drawn into (not open yet, or set-up failed): the
 * caller presents through the present callback instead. Method thread, render context. */
static bool present_window(NV2AState *d, SurfaceBinding *surface)
{
    if (s_win.failed) {
        return false;
    }
    if (!s_win.ctx) {
        void *hwnd = s_win.have_fns ? s_win.fns.hwnd() : NULL;
        if (!hwnd) {
            return false;
        }
        /* Created here, where the render context is current, so the two share objects. */
        s_win.ctx = glo_context_create_for_window(hwnd);
        InitializeCriticalSection(&s_win.lock);
        s_win.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_win.ready = s_win.showing = -1;
        HANDLE th = s_win.ctx && s_win.wake ? CreateThread(NULL, 0, present_thread, NULL, 0, NULL)
                                            : NULL;
        if (!th) {
            fprintf(stderr, "[XGPU] window present unavailable, reading frames back instead\n");
            s_win.failed = 1;
            return false;
        }
        SetThreadDescription(th, L"gpu present");
        CloseHandle(th);
    }
    if (!s_win.up) {
        return false;    /* the present thread is still setting up */
    }
    if (surface->upload_pending) {
        pgraph_gl_upload_surface_data(d, surface, !tcg_enabled());
    }

    EnterCriticalSection(&s_win.lock);
    int i = 0;
    while (i == s_win.ready || i == s_win.showing) {
        i++;
    }
    LeaveCriticalSection(&s_win.lock);

    /* Slot i is neither shown nor waiting: the method thread has it to itself. */
    unsigned scale = d->pgraph.surface_scale_factor ? d->pgraph.surface_scale_factor : 1;
    unsigned w = surface->width * scale, h = surface->height * scale;
    GLenum fmt = surface->fmt.gl_internal_format;
    if (s_win.slot[i].fence) {    /* a frame that was replaced before it was shown */
        glDeleteSync(s_win.slot[i].fence);
        s_win.slot[i].fence = NULL;
    }
    if (!s_win.slot[i].tex || s_win.slot[i].w != w || s_win.slot[i].h != h ||
        s_win.slot[i].fmt != fmt) {
        /* Same internal format as the surface: glCopyImageSubData copies raw texels. */
        glDeleteTextures(1, &s_win.slot[i].tex);
        glGenTextures(1, &s_win.slot[i].tex);
        glBindTexture(GL_TEXTURE_2D, s_win.slot[i].tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, fmt, (GLsizei)w, (GLsizei)h);
        glBindTexture(GL_TEXTURE_2D, 0);
        s_win.slot[i].w = w;
        s_win.slot[i].h = h;
        s_win.slot[i].fmt = fmt;
    }
    glCopyImageSubData(surface->gl_buffer, GL_TEXTURE_2D, 0, 0, 0, 0, s_win.slot[i].tex,
                       GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)w, (GLsizei)h, 1);
    s_win.slot[i].aw = surface->width;
    s_win.slot[i].ah = surface->height;
    s_win.slot[i].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    EnterCriticalSection(&s_win.lock);
    s_win.ready = i;
    LeaveCriticalSection(&s_win.lock);
    SetEvent(s_win.wake);

    s_stats.presents++;
    s_stats.last_w = surface->width;
    s_stats.last_h = surface->height;
    if (s_present) {
        s_present(NULL, surface->width, surface->height, (unsigned)s_stats.frame_draws);
    }
    return true;
}

/* The flip's scanout, OpenGL only. Called with pgraph.lock held.
 * RECOMP_PB_GPU_PRESENT: window (default) / pbo (present_async) / sync (present_sync). */
static void present_scanout(NV2AState *d)
{
    enum { MODE_WINDOW, MODE_PBO, MODE_SYNC };
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_PB_GPU_PRESENT");
        mode = !e || !*e || strcmp(e, "window") == 0 ? MODE_WINDOW
               : strcmp(e, "sync") == 0             ? MODE_SYNC
                                                    : MODE_PBO;
    }
    SurfaceBinding *surface = scanout_surface(d);
    if (!surface) {
        s_stats.no_surface++;
        return;
    }
    mask_sides(d, surface);
    if (mode == MODE_SYNC) {
        present_sync(d, surface);
    } else if (mode == MODE_PBO || !present_window(d, surface)) {
        present_async(d, surface, true);
    } else {
        /* The window shows the frame on the GPU; for the [XGPU] non-black count and
         * RECOMP_PB_GPU_DUMP, two flips in every 256 (and the two up to the dump flip) are also
         * read back. A read is handed out one flip later, so a pair gives one fresh frame. */
        unsigned long f = s_stats.flips;
        if ((f & 0xFF) <= 1 || dump_wanted(f, 0) || dump_wanted(f, 1)) {
            present_async(d, surface, false);
        }
    }
}

/* VBlanks from the host's VBlank clock (mksm_gpu_vblank_tick), if it runs. */
static volatile LONG s_ticks;
static HANDLE s_tick_event;

void mksm_gpu_vblank_tick(void)
{
    if (!s_tick_event) {
        HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (InterlockedCompareExchangePointer(&s_tick_event, e, NULL) != NULL) {
            CloseHandle(e);
        }
    }
    InterlockedIncrement(&s_ticks);
    SetEvent(s_tick_event);
}

/* Waits for the next VBlank. With the host VBlank clock running, at most one flip per tick,
 * on the same ticks the title's VBlank waits see (two free-running 59.94 Hz grids beat: a
 * frame that waits for one and then the other loses a period now and then; the intro movie
 * flipped at ~52/s). A flip with a tick passed since the previous one is late and goes at once.
 * Without the clock: a fixed grid from the first flip. A flip that is already late (a slow
 * frame, a load) goes at once; one more than a few periods late restarts the grid rather than
 * letting the following frames run uncapped to catch up. */
static void wait_vblank(void)
{
    static HANDLE timer;
    static int64_t next;
    static LONG last_tick;
    if (s_vblank_hz <= 0) {
        return;
    }
    if (s_ticks) {
        LONG cur = s_ticks;
        if (cur != last_tick) {
            last_tick = cur;
            return;
        }
        while (s_ticks == cur) {
            WaitForSingleObject(s_tick_event, 50);   /* the timeout only guards a stopped clock */
        }
        last_tick = s_ticks;
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
     * inline buffer at END) and restored after (counted, the first few logged).
     * Every case seen so far came from commands the toolkit dropped at the pushbuffer
     * wrap (fixed in patch 0011), so this should stay at 0; it is kept as a guard. */
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
    mirror_fence(pg);
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

    /* The title's interrupt handler services the notify and acks it (it writes
     * NV_PGRAPH_INTR). */
    if (pg->waiting_for_nop) {
        s_stats.nops++;
        if (s_notify) {
            s_notify(param);
        }
        pgraph_write(d, NV_PGRAPH_INTR, NV_PGRAPH_INTR_ERROR, 4);
    }
    /* The VBlank moves the read buffer on (NV_PGRAPH_INCREMENT), which ends the stall. */
    if (pg->waiting_for_flip) {
        s_stats.flips++;
        /* FLIP_STALL just closed xemu's frame counters (nv2a_profile_flip_stall). */
        const int *c = g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                                                  NV2A_PROF_NUM_FRAMES].counters;
        s_stats.surf_downloads += (unsigned long)c[NV2A_PROF_SURF_DOWNLOAD];
        s_stats.surf_uploads += (unsigned long)c[NV2A_PROF_SURF_UPLOAD];
        s_stats.surf_to_tex += (unsigned long)c[NV2A_PROF_SURF_TO_TEX];
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
