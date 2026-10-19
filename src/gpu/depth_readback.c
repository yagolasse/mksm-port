/*
 * Depth readback as half floats, done on the CPU (plan H.5c2e). xemu downloads a float Z16
 * surface (GL_DEPTH_COMPONENT32F) with glReadPixels(GL_DEPTH_COMPONENT, GL_HALF_FLOAT). On Mesa
 * radeonsi (26.2.4: Steam Deck, RX 9070 XT) that read returns 0x7FFF (a half NaN) for every
 * texel once the same read has run a few times, while a GL_FLOAT read of the same surface right
 * before it is correct. The game's character shadows are such a surface (a 256x256 depth map
 * sampled as texture format 0x31): a map of NaNs reads as 0 in the shadow test, so the whole
 * map footprint is drawn in shadow (hard-edged dark planes). NVIDIA reads it correctly.
 *
 * The hook reads GL_FLOAT and converts to half floats here, so every driver gets the same
 * bytes. Installed by mksm_gpu_init through glad's pointer, as shader_patch.c does it (the xemu
 * files stay unmodified). Reads into a pixel pack buffer, or with skip pixels/rows set, go to
 * the driver unchanged (xemu does neither). RECOMP_PB_GPU_DEPTH_HALF=0 turns the hook off
 * (A/B runs).
 * The conversion runs per texel on the method thread (a 256x256 map ~60 times a second: 4.8% of
 * the Steam Deck's method thread as plain C, plan 2.7c5), so CPUs with F16C convert 8 at a time
 * (vcvtps2ph, round to nearest even: the same bytes as float_to_half for every non-NaN input).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glad/gl.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define HAVE_F16C_PATH 1
#endif

static PFNGLREADPIXELSPROC s_real_read_pixels;
static float *s_buf;
static size_t s_buf_len;

/* IEEE binary32 -> binary16, round to nearest even (what GL's conversion does). */
static uint16_t float_to_half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000);
    uint32_t mag = x & 0x7FFFFFFF;
    if (mag >= 0x7F800000) {
        return sign | (mag > 0x7F800000 ? 0x7E00 : 0x7C00);
    }
    if (mag >= 0x477FF000) { /* rounds to >= 65520: overflow */
        return sign | 0x7C00;
    }
    if (mag < 0x38800000) { /* below the smallest normal half: denormal or zero */
        if (mag < 0x33000000) {
            return sign;
        }
        uint32_t m = (mag & 0x007FFFFF) | 0x00800000;
        int shift = 126 - (int)(mag >> 23); /* 14..24 */
        uint32_t h = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        h += rem > half || (rem == half && (h & 1));
        return sign | (uint16_t)h;
    }
    uint32_t h = ((mag - 0x38000000) >> 13), rem = mag & 0x1FFF;
    h += rem > 0x1000 || (rem == 0x1000 && (h & 1));
    return sign | (uint16_t)h;
}

static void row_to_half(const float *in, uint16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = float_to_half(in[i]);
    }
}

#ifdef HAVE_F16C_PATH
__attribute__((target("avx,f16c")))
static void row_to_half_f16c(const float *in, uint16_t *out, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i h = _mm256_cvtps_ph(_mm256_loadu_ps(in + i), _MM_FROUND_TO_NEAREST_INT);
        _mm_storeu_si128((__m128i *)(out + i), h);
    }
    row_to_half(in + i, out + i, n - i);
}

/* F16C and AVX in CPUID leaf 1, and the OS saving the YMM state (OSXSAVE, XCR0 bits 1-2). */
static int cpu_has_f16c(void)
{
    unsigned ecx;
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 1);
    ecx = (unsigned)r[2];
#else
    unsigned eax, ebx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return 0;
    }
#endif
    const unsigned need = (1u << 27) | (1u << 28) | (1u << 29); /* OSXSAVE, AVX, F16C */
    if ((ecx & need) != need) {
        return 0;
    }
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}
#endif

static void (*s_row_to_half)(const float *, uint16_t *, size_t) = row_to_half;

static void GLAD_API_PTR read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                     GLenum type, void *data)
{
    GLint pack_buffer = 0, skip_px = 0, skip_rows = 0;
    if (format == GL_DEPTH_COMPONENT && type == GL_HALF_FLOAT && w > 0 && h > 0) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_px);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    }
    if (format != GL_DEPTH_COMPONENT || type != GL_HALF_FLOAT || w <= 0 || h <= 0 ||
        pack_buffer || skip_px || skip_rows) {
        s_real_read_pixels(x, y, w, h, format, type, data);
        return;
    }

    /* Destination layout from the caller's pack state (xemu: row length = pitch / 2, alignment
     * 1). */
    GLint row_len = 0, align = 4;
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_len);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    size_t row_px = row_len > 0 ? (size_t)row_len : (size_t)w;
    size_t stride = (row_px * 2 + (size_t)align - 1) / (size_t)align * (size_t)align;

    size_t need = (size_t)w * (size_t)h;
    if (need > s_buf_len) {
        float *nb = realloc(s_buf, need * sizeof(float));
        if (!nb) {
            s_real_read_pixels(x, y, w, h, format, type, data);
            return;
        }
        s_buf = nb;
        s_buf_len = need;
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    s_real_read_pixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, s_buf);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_len);
    glPixelStorei(GL_PACK_ALIGNMENT, align);

    for (GLsizei r = 0; r < h; r++) {
        s_row_to_half(s_buf + (size_t)r * (size_t)w,
                      (uint16_t *)((uint8_t *)data + (size_t)r * stride), (size_t)w);
    }
}

/* 0: off, 1: on, 2: on with the F16C conversion. */
int mksm_depth_readback_install(void);
int mksm_depth_readback_install(void)
{
    const char *env = getenv("RECOMP_PB_GPU_DEPTH_HALF");
    if ((env && *env == '0') || !glad_glReadPixels || s_real_read_pixels) {
        return 0;
    }
    s_real_read_pixels = glad_glReadPixels;
    glad_glReadPixels = read_pixels;
#ifdef HAVE_F16C_PATH
    if (cpu_has_f16c()) {
        s_row_to_half = row_to_half_f16c;
        return 2;
    }
#endif
    return 1;
}
