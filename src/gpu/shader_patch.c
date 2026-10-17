/*
 * Edits to the GLSL xemu's shader generator emits, applied as the text reaches glShaderSource
 * (the xemu files stay unmodified). Installed by mksm_gpu_init once the GL entry points are
 * loaded; each edit is keyed on a fixed line of xemu's generator and leaves a shader without
 * that line alone.
 *
 * 2D snap (plan 4.1 / B2b), only at a surface scale above 1, so the 2D layer shows at Nx what
 * it shows at 1x, upsampled:
 * - Coverage (vertex shader): a pre-transformed (screen-space) vertex is moved to the 1x pixel
 *   edge that covers the same pixels at 1x. xemu keeps vertex positions in 1x pixels and lets
 *   the viewport scale them, so at 1x the rasterizer's sample at a pixel's centre decides which
 *   whole pixels a sprite covers, while at Nx an edge between two 1x centres lands on a scaled
 *   pixel in between. A 1x pixel is covered when its centre is on or past the leading (top or
 *   left) edge and before the trailing one, so both edges go to ceil(x - 0.5), half rounded
 *   down (most of the game's 2D sits on x.5 ties: floor(x + 0.5) put the HUD's bars a row
 *   low). Screen-space = w of 2^63 (D3D's pass-through program for XYZRHW
 *   vertices with rhw = 0: w = 1/0, which xemu clamps before this point) or exactly 1 (the
 *   game's own screen-space program, `mov oPos, v0`). Other vertices are left alone.
 * - Texture coordinates (fragment shader): moving the vertex would drag the texture with it.
 *   For a stage with a nearest (point) magnification filter they are read where 1x reads
 *   them, at the 1x pixel's centre, so each NxN block shows the texel 1x shows (the game's
 *   HUD sits at x.5: at 1x every centre lands on a texel boundary and shows the texel past it,
 *   which a small bias in texel space repeats; reading at the moved place showed the texel
 *   row before the sprite, lines above the health bar). For a filtered stage they are read
 *   where the unsnapped sprite had them, keeping the Nx detail (menu art, the loading
 *   backdrop). vtxPos0 is a vertex of the primitive before the snap; the sprite is taken to
 *   move as a whole by that vertex's snap, and a 2D primitive's coordinates are affine on
 *   screen, so the derivatives give them at the other place. Window y grows with the game's y
 *   (xemu's NDC mapping), so both axes take the same sign. The nearest stages come from
 *   PGRAPH's filter registers per draw, as the uniform mksmNearest (bit per stage), set at
 *   each draw call.
 * RECOMP_PB_GPU_SNAP2D=0 turns it off (A/B runs), =debug also colours the 2D draws (below).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glad/gl.h>

static PFNGLSHADERSOURCEPROC s_real_shader_source;
static PFNGLUSEPROGRAMPROC s_real_use_program;
static PFNGLLINKPROGRAMPROC s_real_link_program;
static PFNGLDRAWARRAYSPROC s_real_draw_arrays;
static PFNGLDRAWELEMENTSPROC s_real_draw_elements;
static PFNGLMULTIDRAWARRAYSPROC s_real_multi_draw_arrays;
static unsigned (*s_nearest_mask)(void);
static unsigned long s_snapped;
static unsigned s_scale;
static int s_debug;

static const char k_round_fn[] =
    "vec2 roundScreenCoords(vec2 pos) {\n"
    "  return trunc(pos * 16.0f) / 16.0f;\n"
    "}\n";
static const char k_snap_fn[] =
    "vec2 mksmSnap2D(vec2 pos, float w) {\n"
    "  return abs(w) >= 1.0e18 || w == 1.0 ? ceil(pos - 0.5) : pos;\n"
    "}\n";
static const char k_to_ndc[] = "  oPos.xy = (2.0f * oPos.xy - surfaceSize) / surfaceSize;\n";
static const char k_snap_call[] = "  oPos.xy = mksmSnap2D(oPos.xy, oPos.w);\n";

static const char k_main_start[] = "void main() {\n";
static const char k_main_end[] = "}\n";
static const char k_tex_fmt[] = "vec4 pT%d = vtxT%d;\n";
static const char k_tex_new_fmt[] = "vec4 pT%d = mksmTex2D(vtxT%d, %d);\n";
/* %u = the surface scale. */
static const char k_tex_fn_fmt[] =
    "uniform int mksmNearest;\n"
    "vec4 mksmTex2D(vec4 t, int stage) {\n"
    "  const float n = %u.0;\n"
    "  vec4 tx = dFdx(t), ty = dFdy(t);\n"
    "  float w = vtxPos0.w;\n"
    "  if (!(abs(w) >= 1.0e18 || w == 1.0)) return t;\n"
    "  vec2 d = ceil(vtxPos0.xy - 0.5) - vtxPos0.xy;\n"
    "  if (((mksmNearest >> stage) & 1) == 0) return t + (tx * d.x + ty * d.y) * n;\n"
    "  vec2 f = gl_FragCoord.xy / n;\n"
    "  d += floor(f) + 0.5 - f;\n"
    "  return t + (tx * d.x + ty * d.y) * n + (abs(tx) + abs(ty)) * (n / 64.0);\n"
    "}\n";

/* RECOMP_PB_GPU_SNAP2D=debug: screen-space draws are coloured by where their (unsnapped)
 * provoking vertex sits inside a 1x pixel: red = fract(x), green = fract(y), blue = 0.25,
 * + 0.5 for w == 1 (else rhw = 0), + 0.25 if stage 0 is point-sampled. For finding the game's 2D sub-pixel positions in a dump. */
static const char k_debug_out[] =
    "if (abs(vtxPos0.w) >= 1.0e18 || vtxPos0.w == 1.0) {\n"
    "  fragColor = vec4(fract(vtxPos0.xy),\n"
    "                   (vtxPos0.w == 1.0 ? 0.5 : 0.0) + float(mksmNearest & 1) * 0.25 + 0.25,\n"
    "                   1.0);\n"
    "}\n";

/* Copies src with `ins` inserted at offset `at`. */
static char *insert(const char *src, size_t at, const char *ins, size_t ins_len)
{
    size_t len = strlen(src);
    char *out = (char *)malloc(len + ins_len + 1);
    if (out) {
        memcpy(out, src, at);
        memcpy(out + at, ins, ins_len);
        memcpy(out + at + ins_len, src + at, len - at + 1);
    }
    return out;
}

/* Copies src with the first `from` replaced by `to`; NULL if src has no `from`. */
static char *replace(const char *src, const char *from, const char *to)
{
    const char *at = strstr(src, from);
    if (!at) {
        return NULL;
    }
    size_t len = strlen(src), nf = strlen(from), nt = strlen(to), k = (size_t)(at - src);
    char *out = (char *)malloc(len - nf + nt + 1);
    if (out) {
        memcpy(out, src, k);
        memcpy(out + k, to, nt);
        memcpy(out + k + nt, at + nf, len - k - nf + 1);
    }
    return out;
}

/* The source with the 2D snap added, or NULL if it isn't one of xemu's vertex shaders. */
static char *add_vertex_snap(const char *src)
{
    const char *fn = strstr(src, k_round_fn);
    const char *ndc = fn ? strstr(fn, k_to_ndc) : NULL;
    if (!ndc) {
        return NULL;
    }
    size_t at_fn = (size_t)(fn - src) + sizeof(k_round_fn) - 1;
    char *call = insert(src, (size_t)(ndc - src), k_snap_call, sizeof(k_snap_call) - 1);
    char *out = call ? insert(call, at_fn, k_snap_fn, sizeof(k_snap_fn) - 1) : NULL;
    free(call);
    return out;
}

/* The fragment side (texture coordinates, debug colours), or NULL if it isn't one of xemu's
 * pixel shaders (they all start main with the pT0 = vtxT0 line). */
static char *add_fragment_snap(const char *src)
{
    char from[32], to[48];
    snprintf(from, sizeof(from), k_tex_fmt, 0, 0);
    const char *main_at = strstr(src, k_main_start);
    if (!main_at || !strstr(main_at, from)) {
        return NULL;
    }
    char fn[sizeof(k_tex_fn_fmt) + 16];
    snprintf(fn, sizeof(fn), k_tex_fn_fmt, s_scale);
    char *out = insert(src, (size_t)(main_at - src), fn, strlen(fn));
    for (int i = 0; out && i < 4; i++) {
        snprintf(from, sizeof(from), k_tex_fmt, i, i);
        snprintf(to, sizeof(to), k_tex_new_fmt, i, i, i);
        char *next = replace(out, from, to);
        if (next) {
            free(out);
            out = next;
        }
    }
    size_t len = out ? strlen(out) : 0, end = sizeof(k_main_end) - 1;
    if (out && s_debug && len >= end && strcmp(out + len - end, k_main_end) == 0) {
        char *next = insert(out, len - end, k_debug_out, sizeof(k_debug_out) - 1);
        free(out);
        out = next;
    }
    return out;
}

static void GLAD_API_PTR shader_source(GLuint shader, GLsizei count,
                                       const GLchar *const *string, const GLint *length)
{
    char *patched = NULL;
    if (count == 1 && !length) {
        patched = add_vertex_snap(string[0]);
        if (!patched) {
            patched = add_fragment_snap(string[0]);
        }
    }
    if (!patched) {
        s_real_shader_source(shader, count, string, length);
        return;
    }
    const GLchar *one = patched;
    s_real_shader_source(shader, 1, &one, NULL);
    free(patched);
    if (s_snapped++ == 0) {
        fprintf(stderr, "[XGPU] 2D snap added to the shaders%s\n",
                s_debug ? " (debug colours)" : "");
    }
}

/* mksmNearest per program: its location (UNKNOWN until looked up, -1 = not in it) and the
 * value last set. Program names are small integers; larger ones look the location up at
 * every draw. */
#define PROG_MAX 65536
#define UNKNOWN (-2)
static struct {
    int loc;
    int value;
} s_prog_tab[PROG_MAX];
/* Per thread: the present thread draws with its own context and programs. */
static _Thread_local GLuint s_prog;

static void GLAD_API_PTR use_program(GLuint program)
{
    s_prog = program;
    s_real_use_program(program);
}

static void GLAD_API_PTR link_program(GLuint program)
{
    if (program < PROG_MAX) {
        s_prog_tab[program].loc = UNKNOWN;
    }
    s_real_link_program(program);
}

/* Before each draw: the current program's mksmNearest from the registers of this draw. */
static void set_nearest(void)
{
    if (!s_prog) {
        return;
    }
    int loc, *value = NULL;
    if (s_prog < PROG_MAX) {
        if (s_prog_tab[s_prog].loc == UNKNOWN) {
            s_prog_tab[s_prog].loc = glad_glGetUniformLocation(s_prog, "mksmNearest");
            s_prog_tab[s_prog].value = -1;
        }
        loc = s_prog_tab[s_prog].loc;
        value = &s_prog_tab[s_prog].value;
    } else {
        loc = glad_glGetUniformLocation(s_prog, "mksmNearest");
    }
    if (loc < 0) {
        return;
    }
    int v = (int)s_nearest_mask();
    if (!value || *value != v) {
        glad_glUniform1i(loc, v);
        if (value) {
            *value = v;
        }
    }
}

static void GLAD_API_PTR draw_arrays(GLenum mode, GLint first, GLsizei count)
{
    set_nearest();
    s_real_draw_arrays(mode, first, count);
}

static void GLAD_API_PTR draw_elements(GLenum mode, GLsizei count, GLenum type,
                                       const void *indices)
{
    set_nearest();
    s_real_draw_elements(mode, count, type, indices);
}

static void GLAD_API_PTR multi_draw_arrays(GLenum mode, const GLint *first,
                                           const GLsizei *count, GLsizei drawcount)
{
    set_nearest();
    s_real_multi_draw_arrays(mode, first, count, drawcount);
}

/* Called from mksm_gpu_init after the GL entry points are loaded; nearest_mask gives the
 * texture stages of the draw being issued whose magnification filter is nearest (method
 * thread, PGRAPH's lock held). Returns 1 if installed. */
int mksm_shader_patch_install(unsigned surface_scale, unsigned (*nearest_mask)(void));
int mksm_shader_patch_install(unsigned surface_scale, unsigned (*nearest_mask)(void))
{
    const char *env = getenv("RECOMP_PB_GPU_SNAP2D");
    if (surface_scale <= 1 || (env && *env == '0') || !glad_glShaderSource ||
        s_real_shader_source) {
        return 0;
    }
    s_debug = env && strcmp(env, "debug") == 0;
    s_scale = surface_scale;
    s_nearest_mask = nearest_mask;
    for (int i = 0; i < PROG_MAX; i++) {
        s_prog_tab[i].loc = UNKNOWN;
    }
    s_real_shader_source = glad_glShaderSource;
    glad_glShaderSource = shader_source;
    s_real_use_program = glad_glUseProgram;
    glad_glUseProgram = use_program;
    s_real_link_program = glad_glLinkProgram;
    glad_glLinkProgram = link_program;
    s_real_draw_arrays = glad_glDrawArrays;
    glad_glDrawArrays = draw_arrays;
    s_real_draw_elements = glad_glDrawElements;
    glad_glDrawElements = draw_elements;
    s_real_multi_draw_arrays = glad_glMultiDrawArrays;
    glad_glMultiDrawArrays = multi_draw_arrays;
    return 1;
}
