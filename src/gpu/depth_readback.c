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
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <glad/gl.h>

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
        uint16_t *out = (uint16_t *)((uint8_t *)data + (size_t)r * stride);
        const float *in = s_buf + (size_t)r * (size_t)w;
        for (GLsizei c = 0; c < w; c++) {
            out[c] = float_to_half(in[c]);
        }
    }
}

int mksm_depth_readback_install(void);
int mksm_depth_readback_install(void)
{
    const char *env = getenv("RECOMP_PB_GPU_DEPTH_HALF");
    if ((env && *env == '0') || !glad_glReadPixels || s_real_read_pixels) {
        return 0;
    }
    s_real_read_pixels = glad_glReadPixels;
    glad_glReadPixels = read_pixels;
    return 1;
}
