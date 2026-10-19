/*
 * RECOMP_PB_GPU_DOWNLOAD_LOG=1: which surfaces xemu reads back into VRAM, and why (plan
 * 2.7c9a). Each read is a glReadPixels that waits for the GPU (1.2 ms each on the Steam Deck,
 * ~2 per frame: 14% of the method thread, 2.7c7). The trace shim (xemu/shim/trace) calls in at
 * xemu's surface_download / evict / invalidated events, so the xemu files stay unmodified.
 *
 * A download's cause, from what came just before it on the method thread:
 *   - an eviction of that surface ("overlap": a new surface overlaps it; "old": unused for
 *     5 frames; "incompatible": re-bound with another shape), traced before its download;
 *   - "fallback": xemu's surface-to-texture fallback (render_surface_to) read it to a buffer;
 *   - "texture": an enabled texture stage overlaps it (bind_textures writes back every dirty
 *     surface a texture it can't bind as that surface may index; the stage is logged with the
 *     texture's address, format and shape);
 *   - "other": none of these (a blit, or a CPU-side request).
 * Counts are kept per (cause, surface, texture) and printed with the [XGPU] report, every 5 s:
 * the interval's entries, most frequent first. Method thread only (under pgraph.lock).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"

int mksm_surface_log_on;

static NV2AState *s_dev;

#define LOG_MAX 128

typedef struct {
    const char *cause;
    uint32_t addr, width, height, pitch, bpp;
    bool zeta, swizzled;
    int stage;                      /* texture stage, -1 if none */
    uint32_t tex_addr, tex_fmt, tex_w, tex_h, tex_levels, tex_pitch;
    bool tex_cubemap;
    unsigned long n, total;
} Entry;

static Entry s_entries[LOG_MAX];
static int s_count;
static unsigned long s_dropped;

/* The eviction reason traced for a surface whose download may follow; cleared when the
 * surface is invalidated (right after its download, if any). */
static const char *s_reason;
static uint32_t s_reason_addr;

/* xemu's fallback counter at the last download (it counts into the current frame). */
static int s_fallbacks;

void mksm_surface_log_init(NV2AState *d)
{
    const char *e = getenv("RECOMP_PB_GPU_DOWNLOAD_LOG");
    s_dev = d;
    mksm_surface_log_on = e && *e && *e != '0';
}

void mksm_surface_log_reason(const char *reason, uint32_t addr)
{
    s_reason = reason;
    s_reason_addr = addr;
}

static bool overlaps(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len)
{
    return a < b + b_len && b < a + a_len;
}

void mksm_surface_log_download(const char *binding, const char *layout, uint32_t addr,
                               uint32_t width, uint32_t height, uint32_t pitch,
                               uint32_t bytes_per_pixel)
{
    PGRAPHState *pg = &s_dev->pgraph;
    Entry k;
    memset(&k, 0, sizeof(k));
    k.addr = addr;
    k.width = width;
    k.height = height;
    k.pitch = pitch;
    k.bpp = bytes_per_pixel;
    k.zeta = strcmp(binding, "ZETA") == 0;
    k.swizzled = strcmp(layout, "sz") == 0;
    k.stage = -1;

    int fallbacks = g_nv2a_stats.frame_working.counters[NV2A_PROF_SURF_TO_TEX_FALLBACK];
    if (s_reason && s_reason_addr == addr) {
        k.cause = s_reason;
    } else if (fallbacks != s_fallbacks) {
        k.cause = "fallback";
    } else {
        k.cause = "other";
        for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
            if (!pgraph_is_texture_enabled(pg, i)) {
                continue;
            }
            TextureShape shape = pgraph_get_texture_shape(pg, i);
            uint32_t tex_addr = (uint32_t)pgraph_get_texture_phys_addr(pg, i);
            uint32_t len = (uint32_t)pgraph_get_texture_length(pg, &shape);
            if (overlaps(tex_addr, len, addr, pitch * height)) {
                k.cause = "texture";
                k.stage = i;
                k.tex_addr = tex_addr;
                k.tex_fmt = shape.color_format;
                k.tex_w = shape.width;
                k.tex_h = shape.height;
                k.tex_levels = shape.levels;
                k.tex_pitch = shape.pitch;
                k.tex_cubemap = shape.cubemap;
                break;
            }
        }
    }
    s_fallbacks = fallbacks;

    for (int i = 0; i < s_count; i++) {
        Entry *e = &s_entries[i];
        if (e->cause == k.cause && e->addr == k.addr && e->width == k.width &&
            e->height == k.height && e->pitch == k.pitch && e->zeta == k.zeta &&
            e->swizzled == k.swizzled && e->stage == k.stage && e->tex_addr == k.tex_addr &&
            e->tex_fmt == k.tex_fmt && e->tex_w == k.tex_w && e->tex_h == k.tex_h) {
            e->n++;
            e->total++;
            return;
        }
    }
    if (s_count == LOG_MAX) {
        s_dropped++;
        return;
    }
    k.n = k.total = 1;
    s_entries[s_count++] = k;
}

/* At each flip, after xemu closed the frame (its working counters start again from 0). */
void mksm_surface_log_flip(void)
{
    s_fallbacks = 0;
}

static int by_count(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

void mksm_surface_log_report(void)
{
    if (!mksm_surface_log_on) {
        return;
    }
    qsort(s_entries, s_count, sizeof(Entry), by_count);
    for (int i = 0; i < s_count && s_entries[i].n; i++) {
        const Entry *e = &s_entries[i];
        fprintf(stderr, "[XGPU] download %lu (total %lu) %-12s %s %s 0x%08X %ux%u pitch %u bpp %u",
                e->n, e->total, e->cause, e->zeta ? "ZETA " : "COLOR", e->swizzled ? "sz " : "lin",
                e->addr, e->width, e->height, e->pitch, e->bpp);
        if (e->stage >= 0) {
            fprintf(stderr, " | stage %d tex 0x%08X fmt 0x%02X %ux%u pitch %u levels %u%s",
                    e->stage, e->tex_addr, e->tex_fmt, e->tex_w, e->tex_h, e->tex_pitch,
                    e->tex_levels, e->tex_cubemap ? " cube" : "");
        }
        fputc('\n', stderr);
    }
    if (s_dropped) {
        fprintf(stderr, "[XGPU] download log full: %lu downloads not counted\n", s_dropped);
    }
    for (int i = 0; i < s_count; i++) {
        s_entries[i].n = 0;
    }
}
