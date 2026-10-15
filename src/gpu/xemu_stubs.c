/*
 * Definitions the vendored xemu PGRAPH files need from parts of xemu/QEMU that are not ported
 * (nv2a.c, pfifo.c, QEMU core, xemu UI). The nv_dma_* / nv_clip_gpu_tile_blit bodies follow
 * xemu's hw/xbox/nv2a/nv2a.c (36334d9).
 *
 * Plan 2.H.3b decides the sync-relevant ones (pfifo_kick, nv2a_update_irq): for now they only
 * count calls, so the link check can see them.
 *
 * Copyright (c) 2012 espes, (c) 2015 Jannik Vogel, (c) 2018-2025 Matt Borgerson (nv_dma_*)
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "xemu_stubs.h"
#include "xxhash.h"

#include <direct.h>

/* xemu's settings, only the fields PGRAPH and the GL renderer read. No on-disk shader cache:
 * shaders are generated and compiled at run time (a cache can come with 2.H.8). */
struct config g_config = {
    .display = { .renderer = CONFIG_DISPLAY_RENDERER_NULL, .quality = { .surface_scale = 1 } },
    .perf = { .cache_shaders = false },
};

/* epoxy/gl.h shim: glGetError reads the driver only when set (RECOMP_PB_GPU_GLCHECK=1). */
int mksm_gl_check_errors;

/* xemu-version.h: the GL shader cache keys its files on it. */
const char *xemu_version = "mksm-pc";

/* The GL renderer creates "<base>shaders" at init even with the cache off, so point it at a
 * per-user folder rather than the working directory. */
const char *xemu_settings_get_base_path(void)
{
    static char path[MAX_PATH];
    if (!path[0]) {
        const char *base = getenv("LOCALAPPDATA");
        if (base && snprintf(path, sizeof(path), "%s\\mksm-pc\\", base) < (int)sizeof(path)) {
            _mkdir(path);
        } else {
            strcpy(path, ".\\");
        }
    }
    return path;
}

/* nv2a.c: register block names for nv2a_reg_log_*; the MMIO ops are not ported. */
const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
#define ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { .name = #NAME, .offset = OFFSET, .size = SIZE }
    ENTRY(PMC,      0x000000, 0x001000),
    ENTRY(PBUS,     0x001000, 0x001000),
    ENTRY(PFIFO,    0x002000, 0x002000),
    ENTRY(PRMA,     0x007000, 0x001000),
    ENTRY(PVIDEO,   0x008000, 0x001000),
    ENTRY(PTIMER,   0x009000, 0x001000),
    ENTRY(PCOUNTER, 0x00a000, 0x001000),
    ENTRY(PVPE,     0x00b000, 0x001000),
    ENTRY(PTV,      0x00d000, 0x001000),
    ENTRY(PRMFB,    0x0a0000, 0x020000),
    ENTRY(PRMVIO,   0x0c0000, 0x001000),
    ENTRY(PFB,      0x100000, 0x001000),
    ENTRY(PSTRAPS,  0x101000, 0x001000),
    ENTRY(PGRAPH,   0x400000, 0x002000),
    ENTRY(PCRTC,    0x600000, 0x001000),
    ENTRY(PRMCIO,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  0x680000, 0x001000),
    ENTRY(PRMDIO,   0x681000, 0x001000),
    ENTRY(USER,     0x800000, 0x800000),
#undef ENTRY
};

XemuStubCounts xemu_stub_counts;

/* pfifo.c: wakes xemu's FIFO thread. There is none here (the toolkit's ack thread feeds
 * methods); 2.H.3b decides what a kick means. */
void pfifo_kick(NV2AState *d)
{
    (void)d;
    xemu_stub_counts.pfifo_kick++;
}

/* nv2a.c: raises the PCI interrupt from the pending/enabled masks. The toolkit does not model
 * GPU interrupts yet; 2.H.3b decides. */
void nv2a_update_irq(NV2AState *d)
{
    (void)d;
    xemu_stub_counts.update_irq++;
}

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);

    dma.address &= 0x07FFFFFF;
    assert(dma.address < memory_region_size(d->vram));
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

hwaddr nv_clip_gpu_tile_blit(NV2AState *d, hwaddr blit_base_address, hwaddr len)
{
    const uint32_t *regs = d->pfb.regs;
    hwaddr blit_end = blit_base_address + len;
    for (int i = 0; i < NV_NUM_GPU_TILES; ++i) {
        uint32_t base_and_flags = regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)];
        if (!(base_and_flags & NV_PFB_TILE_FLAGS_VALID)) {
            continue;
        }
        uint32_t limit = regs[NV_PFB_TILE_LIMIT(i)];
        if (blit_base_address < limit && blit_end > limit) {
            return limit + 1 - blit_base_address;
        }
    }
    return len;
}

/* QEMU memory API: regions are plain host buffers here. */
uint64_t memory_region_size(MemoryRegion *mr)
{
    return mr->size;
}

/* Dirty tracking without a dirty bitmap (guest RAM is a file mapping, see
 * port/docs/gpu-renderer.md, "Dirty tracking"). The title can only expect the GPU to see a
 * CPU write once it has kicked the pushbuffer after it (written DMA_PUT), and x86 makes the
 * data visible before the PUT that follows it. So a page reads as dirty at most once per
 * kick: the first check after DMA_PUT moves reports every page of the range as dirty, later
 * checks in the same kick only the pages not checked since. xemu then confirms texture
 * changes by content hash and re-uploads vertex data. Without a DMA_PUT register
 * (xemu_stubs_set_dma_put not called) every range is dirty, as before.
 * Texture pages also keep a content hash: a page checked again in a later kick reads dirty
 * only if its bytes changed. Every texture page that reads dirty costs xemu a walk of its
 * whole texture cache (pgraph_gl_mark_textures_possibly_dirty), and most textures never
 * change, so this is what keeps the walks rare (plan 2.H.8c2a).
 * Ranges xemu writes itself (surface downloads, blits) are marked dirty for the clients
 * (texture pages: re-hashed on their next check). */
static const volatile uint32_t *s_dma_put;
static uint32_t s_last_put, s_epoch = 1;
static uint32_t *s_page_epoch[2];   /* DIRTY_MEMORY_NV2A, _NV2A_TEX: epoch of the last check */
static uint64_t *s_tex_page_hash;   /* _NV2A_TEX: content hash at the last check */
static bool s_tex_hash = true;
static size_t s_pages;

void xemu_stubs_set_dma_put(const volatile uint32_t *dma_put, bool tex_hash)
{
    s_dma_put = dma_put;
    s_tex_hash = tex_hash;
}

static uint32_t *page_epochs(MemoryRegion *mr, unsigned client)
{
    if (!s_dma_put || (client != DIRTY_MEMORY_NV2A && client != DIRTY_MEMORY_NV2A_TEX)) {
        return NULL;
    }
    unsigned i = client - DIRTY_MEMORY_NV2A;
    if (!s_page_epoch[i]) {
        s_pages = mr->size >> TARGET_PAGE_BITS;
        s_page_epoch[i] = (uint32_t *)g_malloc0(s_pages * sizeof(uint32_t));
        if (client == DIRTY_MEMORY_NV2A_TEX && s_tex_hash) {
            s_tex_page_hash = (uint64_t *)g_malloc0(s_pages * sizeof(uint64_t));
        }
    }
    return s_page_epoch[i];
}

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                                        unsigned client)
{
    uint32_t *epochs = page_epochs(mr, client);
    if (!epochs) {
        return true;
    }
    uint32_t put = *s_dma_put;
    if (put != s_last_put) {
        s_last_put = put;
        s_epoch++;
        xemu_stub_counts.dirty_epochs++;
    }
    uint64_t *hashes = client == DIRTY_MEMORY_NV2A_TEX ? s_tex_page_hash : NULL;
    bool dirty = false;
    size_t last = size ? (size_t)((addr + size - 1) >> TARGET_PAGE_BITS) : 0;
    for (size_t p = (size_t)(addr >> TARGET_PAGE_BITS); p <= last && p < s_pages; p++) {
        if (epochs[p] == s_epoch) {
            continue;
        }
        bool seen = epochs[p] != 0;
        epochs[p] = s_epoch;
        if (!hashes) {
            dirty = true;
            continue;
        }
        uint64_t h = XXH3_64bits(mr->ptr + (p << TARGET_PAGE_BITS), TARGET_PAGE_SIZE);
        xemu_stub_counts.tex_pages_hashed++;
        if (!seen || h != hashes[p]) {
            hashes[p] = h;
            xemu_stub_counts.tex_pages_changed++;
            dirty = true;
        }
    }
    return dirty;
}

void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    uint32_t *epochs = page_epochs(mr, client);
    if (!epochs || !size) {
        return;
    }
    /* Texture pages with a hash only need re-hashing (any epoch but the current one): a
     * download that wrote back the bytes already there changes no texture. */
    bool rehash = client == DIRTY_MEMORY_NV2A_TEX && s_tex_page_hash;
    size_t last = (size_t)((addr + size - 1) >> TARGET_PAGE_BITS);
    for (size_t p = (size_t)(addr >> TARGET_PAGE_BITS); p <= last && p < s_pages; p++) {
        epochs[p] = rehash && epochs[p] ? s_epoch - 1 : 0;
    }
}

/* QEMU accelerator: report TCG. In xemu that selects CPU access callbacks for surfaces (below,
 * never fired yet) over the dirty bitmap: with "always dirty", the bitmap path would re-upload
 * every surface from guest RAM over what the GPU drew. Surface downloads on CPU access are
 * plan 2.H.6. */
bool tcg_enabled(void)
{
    return true;
}

MemAccessCallback *mem_access_callback_insert(CPUState *cpu, MemoryRegion *mr, hwaddr offset,
                                              hwaddr len, MemAccessCallbackFunc func,
                                              void *opaque)
{
    (void)cpu; (void)mr; (void)offset; (void)len; (void)func; (void)opaque;
    return NULL;
}

void mem_access_callback_remove_by_ref(CPUState *cpu, MemAccessCallback *cb)
{
    (void)cpu; (void)cb;
}

/* QEMU timers: only QEMU_CLOCK_REALTIME is read (profiling, frame timing). */
int64_t qemu_clock_get_ns(QEMUClockType type)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    (void)type;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

/* QEMU CPUs: only used by the live renderer switch, which this port never triggers (the
 * renderer is fixed at init). */
static CPUState cpu0;

CPUState *qemu_get_cpu(int index)
{
    return index == 0 ? &cpu0 : NULL;
}

void async_safe_run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    func(cpu, data);
}

/* qapi/error.h */
void error_setg(Error **errp, const char *fmt, ...)
{
    if (!errp || *errp) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    Error *err = (Error *)g_malloc0(sizeof(*err));
    err->msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    *errp = err;
}

const char *error_get_pretty(const Error *err)
{
    return err->msg;
}

void error_free(Error *err)
{
    if (err) {
        g_free(err->msg);
        g_free(err);
    }
}

/* xemu UI notifications: logged. */
void xemu_queue_notification(const char *msg)
{
    fprintf(stderr, "[XEMU] %s\n", msg);
}

void xemu_queue_error_message(const char *msg)
{
    fprintf(stderr, "[XEMU] error: %s\n", msg);
}
