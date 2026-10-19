/*
 * A depth surface sampled as a depth texture stays on the GPU (plan 2.7c9b).
 *
 * xemu never binds a zeta surface as a texture ("FIXME: Support zeta to color"), so when a
 * stage samples one, bind_textures writes the surface back to VRAM (a GPU sync + glReadPixels,
 * 1.2 ms each on the Steam Deck) and uploads it again as a texture. The game's character
 * shadow maps do that 2-3 times a frame: a Z16 surface, 256x256 linear, sampled at its own
 * address as format 0x31 (plan 2.7c9a, 14% of the Deck's method thread).
 *
 * texture.c's two surface-to-texture calls come here (zeta_texture.h, forced into that file
 * only, so the xemu files stay unmodified). For a Z16 surface sampled at its own address as the
 * matching linear depth format with the same shape, the compatibility check says yes, and the
 * render draws the surface into the texture's depth: a full-screen triangle that writes
 * gl_FragDepth from the surface texel (copy and blit refuse different depth formats: the
 * surface is D32F for a float Z16, the texture D16). For the float format the value is rounded
 * to a half float first, as the VRAM round trip did (glReadPixels as GL_HALF_FLOAT, then an
 * upload from half floats), so the texture gets the same depths. Everything else goes to xemu.
 *
 * As for a colour render target bound as a texture, the surface's VRAM is then not written
 * until the surface is evicted (xemu downloads it then); the CPU doesn't read these surfaces
 * in this port (no access callbacks), and the texture cache keys this binding as a surface
 * texture (hash 0), so a later VRAM upload replaces it. RECOMP_PB_GPU_ZETA_TEX=0 = xemu's
 * round trip (A/B runs).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"

/* Called by texture.c in place of xemu's functions of the same name without the mksm_. */
bool mksm_gl_check_surface_to_texture_compatibility(const SurfaceBinding *surface,
                                                   const TextureShape *shape);
void mksm_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface,
                                       TextureBinding *texture, TextureShape *texture_shape,
                                       int texture_unit);

static int s_on = -1;
static GLuint s_prog, s_fbo;
static GLint s_tex_loc, s_to_half_loc;

static bool zeta_tex_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_PB_GPU_ZETA_TEX");
        s_on = !(e && *e == '0');
    }
    return s_on;
}

/* A Z16 surface sampled as the linear depth format of the same kind (float or fixed), with
 * the surface's own shape: the texture's texels are the surface's depths. */
static bool zeta_matches(const SurfaceBinding *s, const TextureShape *shape)
{
    if (s->color || s->swizzle || s->fmt.bytes_per_pixel != 2) {
        return false;
    }
    if (s->pitch != shape->pitch || s->width != shape->width || s->height != shape->height ||
        shape->cubemap || shape->levels > 1 || shape->dimensionality != 2) {
        return false;
    }
    switch (shape->color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FLOAT:
        return s->fmt.gl_internal_format == GL_DEPTH_COMPONENT32F;
    case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED:
        return s->fmt.gl_internal_format == GL_DEPTH_COMPONENT16;
    default:
        return false;
    }
}

bool mksm_gl_check_surface_to_texture_compatibility(const SurfaceBinding *surface,
                                                   const TextureShape *shape)
{
    if (!surface->color && zeta_tex_on() && zeta_matches(surface, shape)) {
        return true;
    }
    return pgraph_gl_check_surface_to_texture_compatibility(surface, shape);
}

static void init_zeta_render(void)
{
    const char *vs =
        "#version 330\n"
        "void main()\n"
        "{\n"
        "    float x = -1.0 + float((gl_VertexID & 1) << 2);\n"
        "    float y = -1.0 + float((gl_VertexID & 2) << 1);\n"
        "    gl_Position = vec4(x, y, 0, 1);\n"
        "}\n";
    /* Half-float rounding as float_to_half in depth_readback.c (nearest even) for depths in
     * [0, 1]: normal halves keep 10 mantissa bits, below 2^-14 the step is 2^-24. */
    const char *fs =
        "#version 330\n"
        "uniform sampler2D tex;\n"
        "uniform bool to_half;\n"
        "void main()\n"
        "{\n"
        "    float z = texelFetch(tex, ivec2(gl_FragCoord.xy), 0).r;\n"
        "    if (to_half) {\n"
        "        if (z < 6.103515625e-05) {\n"
        "            z = roundEven(z * 16777216.0) / 16777216.0;\n"
        "        } else {\n"
        "            uint u = floatBitsToUint(z);\n"
        "            u += 0x0FFFu + ((u >> 13) & 1u);\n"
        "            z = uintBitsToFloat(u & ~0x1FFFu);\n"
        "        }\n"
        "    }\n"
        "    gl_FragDepth = z;\n"
        "}\n";

    s_prog = pgraph_gl_compile_shader(vs, fs);
    s_tex_loc = glGetUniformLocation(s_prog, "tex");
    s_to_half_loc = glGetUniformLocation(s_prog, "to_half");
    glGenFramebuffers(1, &s_fbo);
    fprintf(stderr, "[XGPU] depth surfaces sampled as depth textures stay on the GPU "
                    "(RECOMP_PB_GPU_ZETA_TEX=0: VRAM round trip)\n");
}

/* As xemu's render_surface_to (called before PGRAPH sets the draw's GL state, which
 * pgraph_gl_draw_begin does after bind_textures: depth test/func/mask, colour mask, viewport). */
static void render_zeta_to_texture(NV2AState *d, SurfaceBinding *surface,
                                   TextureBinding *texture, int texture_unit)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!s_prog) {
        init_zeta_render();
    }

    unsigned int width = surface->width, height = surface->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    glActiveTexture(GL_TEXTURE0 + texture_unit);
    /* A binding drawn here before at the current scale already has this size (texture.c sets
     * draw_time and scale after each render); otherwise (re)allocate it. */
    if (texture->draw_time == 0 || texture->scale != pg->surface_scale_factor) {
        glBindTexture(texture->gl_target, texture->gl_texture);
        glTexImage2D(texture->gl_target, 0, GL_DEPTH_COMPONENT16, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, NULL);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture->gl_target,
                           texture->gl_texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindTexture(GL_TEXTURE_2D, surface->gl_buffer);
    glBindVertexArray(r->s2t_rndr.vao);
    glUseProgram(s_prog);
    glProgramUniform1i(s_prog, s_tex_loc, texture_unit);
    glProgramUniform1i(s_prog, s_to_half_loc,
                       surface->fmt.gl_internal_format == GL_DEPTH_COMPONENT32F);

    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texture->gl_target, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, r->gl_framebuffer);
    glBindVertexArray(r->gl_vertex_array);
    glBindTexture(texture->gl_target, texture->gl_texture);
    glUseProgram(r->shader_binding ? r->shader_binding->gl_program : 0);
}

void mksm_gl_render_surface_to_texture(NV2AState *d, SurfaceBinding *surface,
                                       TextureBinding *texture, TextureShape *texture_shape,
                                       int texture_unit)
{
    if (!surface->color) {
        /* Only reached for what the check above accepted (xemu's refuses every zeta). */
        render_zeta_to_texture(d, surface, texture, texture_unit);
        return;
    }
    pgraph_gl_render_surface_to_texture(d, surface, texture, texture_shape, texture_unit);
}
