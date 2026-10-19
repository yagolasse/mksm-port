/**
 * Widescreen (plan 4.2, B5a-B5b): the aspect and wide-buffer settings, the wide 2D layer.
 * Split out of recomp_manual.c in plan B14; the entry hooks and wrappers stay there and call
 * the functions here.
 */
#include "manual_internal.h"

/* ── Widescreen (plan 4.2, B5a) ────────────────────────────────────────────
 *
 * RECOMP_ASPECT=16:9 (also 16:10, 21:9, W/H or a plain ratio; unset or 4:3 = off) draws the
 * 3D view at that aspect: Hor+, the vertical FOV stays, more shows at the sides. The game
 * builds its projection as D3DXMatrixPerspectiveFovLH(fovy, cam+0x20, ...) in the camera
 * update 0x00124150, and nothing rewrites cam+0x20 per frame, so the hook widens it by
 * target / (4/3) before each update (see the hook for why relative); frustum culling is built from
 * the same matrix and follows. The frame stays 640x480 (anamorphic) and the GPU present
 * stretches it to the aspect, except while a movie plays (0x00142420 sets the movie up,
 * 0x00141F10 tears it down): movies keep 4:3. The 2D layer (HUD, menus, text) is stretched
 * for now. Addresses and the reasoning: ../docs/widescreen.md § 9.
 *
 * RECOMP_ASPECT_WIDE=1 (B5b1) renders into a W x 480 back buffer instead (854 at 16:9): the
 * display init, the device reset and D3D's mode lookup are hooked below, and the present shows
 * the frame at its own size. The game itself keeps seeing a 640-wide display, so its 2D layout
 * (authored for 640) stays as on 4:3; only the code that has to cover the real frame (2D
 * projection, viewports, full-screen quads) sees W, and pixel-space 2D is moved by (W - 640) / 2:
 * the 2D sits centred (pillarboxed, B5b2). Movies keep a 640x480 buffer.
 *
 * The hooks are entry hooks (`sub_X_enter`): the lift has to be made with
 * `--exclude-manual src/recomp_manual.c` so the generated function calls them first. Direct
 * calls in the lifted code don't go through recomp_lookup_manual, so an override there would
 * miss them. */

/* The target aspect, 0 = off (the game's own 4:3). */
float wide_aspect(void)
{
    static int parsed;
    static float aspect;
    if (!parsed) {
        const char *e = getenv("RECOMP_ASPECT");
        double a = 0.0, w, h;
        char sep;
        if (e && *e) {
            if (sscanf(e, "%lf%c%lf", &w, &sep, &h) == 3 && (sep == ':' || sep == '/') && h > 0)
                a = w / h;
            else
                a = atof(e);
            if (a < 1.0 || a > 4.0) {
                fprintf(stderr, "[WIDE] RECOMP_ASPECT=%s ignored (want e.g. 16:9)\n", e);
                a = 0.0;
            } else if (a > 4.0 / 3.0 - 0.001 && a < 4.0 / 3.0 + 0.001) {
                a = 0.0;    /* 4:3 is the game's own */
            }
        }
        aspect = (float)a;
        if (aspect)
            fprintf(stderr, "[WIDE] aspect %.4f: 3D Hor+, frame stretched at present\n", a);
        parsed = 1;
    }
    return aspect;
}

/* RECOMP_ASPECT_WIDE=1 (plan B5b1) with an aspect: the game renders into a wider back buffer
 * (W x 480, W = 480 * aspect rounded to even: 854 at 16:9) instead of stretching 640x480, so
 * the frame has square pixels and the present shows it at its own size. 0 = anamorphic (B5a).
 * W is capped at 1024: wider buffers (pitch above 4096 bytes) get horizontal stripes over the
 * frame from the glow layer (21:9 = 1120 wide; cause not found yet), so a capped frame is
 * stretched the rest of the way to the aspect at present. */
#define WIDE_MAX_WIDTH 1024u
static int wide_capped;

uint32_t wide_buffer_width(void)
{
    static int parsed;
    static uint32_t width;
    if (!parsed) {
        const char *e = getenv("RECOMP_ASPECT_WIDE");
        if (e && *e && *e != '0' && wide_aspect()) {
            width = ((uint32_t)(480.0f * wide_aspect() + 1.0f)) & ~1u;
            if (width > WIDE_MAX_WIDTH) {
                width = WIDE_MAX_WIDTH;
                wide_capped = 1;
            }
            fprintf(stderr, "[WIDE] back buffer %ux480%s\n", width,
                    wide_capped ? " (capped: stretched to the aspect at present)" : "");
        }
        parsed = 1;
    }
    return width;
}

/* What the present shows the frame at: the target aspect, or the frame's own (0) in a movie
 * or when the frame is already as wide as the aspect. */
void wide_set_output(int movie)
{
#ifdef MKSM_HAVE_GPU
    int own = movie || (wide_buffer_width() && !wide_capped);
    mksm_gpu_set_output_aspect(own ? 0.0f : wide_aspect());
#else
    (void)movie;
#endif
}

/* Display init 0x0011B290 (thiscall, ecx = display object *(0x00638988)): fills the present
 * parameters at +0xC (BackBufferWidth +0xC, BackBufferHeight +0x10) with 640x480, the float
 * size +0xC0/+0xC4 and the globals 0x0050E59C/0x0050E5A0, then resets the device (when one
 * exists) or returns to 0x0011B7C0, which creates it from the same parameters. It runs again
 * after every movie. The wide size goes into the back buffer width only; the reset inside sees
 * it through the hook on 0x003C99E0. The float size and the globals stay 640: the game lays its
 * 2D out from them (W/2 for centred text, W - 104 for right-side prompts, B5b2 hand check), and
 * the few readers that must cover the real frame get W from wide_real_width_call. Not while a
 * movie is set up (0x00142420 resets to 640x480 itself). */

int wide_in_movie;

static void wide_patch_display(uint32_t disp)
{
    uint32_t w = wide_buffer_width();
    if (!w || !disp || wide_in_movie)
        return;
    GUEST_U32(disp + 0xCu) = w;
}

/* How far 640-authored 2D moves to sit centred in the wide frame (0 = not wide). */
int wide_2d_shift(void)
{
    uint32_t w = wide_buffer_width();
    return w && !wide_in_movie ? (int)(w - 640u) / 2 : 0;
}

/* After the display init's own body (the wrapper in recomp_manual.c): log it. */
void wide_display_init_done(uint32_t disp)
{
    static int logged;
    if (wide_buffer_width()) {
        wide_patch_display(disp);
        if (logged++ < 8)
            fprintf(stderr, "[WIDE] display 0x%08X init: %ux%u%s\n", disp,
                    GUEST_U32(disp + 0xCu), GUEST_U32(disp + 0x10u),
                    wide_in_movie ? " (movie, left at 640)" : "");
    }
}

/* D3D's display-mode lookup 0x003D30D3 (cdecl; width, height, flags, ...): matches the size
 * against the AV encoder mode table (0x003EB238, 12-byte entries keyed by AV pack, with width
 * and height at +4/+6) and fails CreateDevice for a size that isn't there (B5b1's first wide
 * run: buffers allocated at pitch 0xE00, then all freed, no pushbuffer). The mode only picks
 * the encoder timings; the frame shown is the rendered surface (mksm_gpu.c), so W x 480 asks
 * for the 640x480 mode. */
void wide_mode_lookup_enter(void)
{
    static int logged;
    uint32_t w = wide_buffer_width();
    if (!w || GUEST_U32(g_esp + 4u) != w || GUEST_U32(g_esp + 8u) != 480u)
        return;
    GUEST_U32(g_esp + 4u) = 640u;
    if (logged++ < 4)
        fprintf(stderr, "[WIDE] display mode lookup %ux480 -> 640x480 (from 0x%08X)\n", w,
                GUEST_U32(g_esp));
}

/* D3DDevice_Reset 0x003C99E0 (stdcall, the present parameters): 0x0011B290 calls it with the
 * display's parameters it has just set to 640 wide. */
void wide_reset_enter(void)
{
    uint32_t w = wide_buffer_width(), pp, disp;
    if (!w)
        return;
    pp = GUEST_U32(g_esp + 4u);
    disp = GUEST_U32(0x00638988u);
    if (disp && pp == disp + 0xCu)
        wide_patch_display(disp);
    else
        fprintf(stderr, "[WIDE] reset with other parameters 0x%08X (%ux%u), left alone\n", pp,
                pp ? GUEST_U32(pp) : 0, pp ? GUEST_U32(pp + 4u) : 0);
}

/* Camera update (thiscall, ecx = camera). Only the renderer's view camera: renderer
 * *(0x00639224), current camera +0x304, world camera +0x308.
 *
 * Relative, not absolute: the game draws its 640x480 frame with cam+0x20 = 1.2, not 4/3
 * (B5a run: the camera already holds 1.2 at its first update, and gets it back after
 * movies; the writer isn't known, the constructor writes 640/480), so the 4:3 picture is
 * ~11% wider than square. The hook scales whatever the game set by target / (4/3), which keeps
 * the original proportions and only adds picture at the sides. A value other than the one
 * written last is the game's new base. */
void wide_camera_enter(void)
{
    static int logged, output_set;
    static uint32_t last_cam;
    static float base, written;
    float aspect = wide_aspect(), now;
    uint32_t cam = g_ecx, renderer;
    if (!aspect)
        return;
    renderer = GUEST_U32(0x00639224u);
    if (!renderer || !cam ||
        (cam != GUEST_U32(renderer + 0x304u) && cam != GUEST_U32(renderer + 0x308u)))
        return;
    if (!output_set) {
        wide_set_output(0);
        output_set = 1;
    }
    now = GUEST_F32(cam + 0x20u);
    if (cam == last_cam && now == written)
        return;
    /* The game's value is 0.9 * W/H of the display it sees (1.2 at 640; B5b1 found 1.6012 when
     * it saw 854). It sees 640 in both modes, so it's always the 4:3 base. */
    if (now != base && logged < 8) {
        logged++;
        fprintf(stderr, "[WIDE] camera 0x%08X game aspect %.4f -> %.4f (from 0x%08X)\n", cam,
                now, now * aspect * 0.75f, GUEST_U32(g_esp));
    }
    last_cam = cam;
    base = now;
    written = now * aspect * 0.75f;
    GUEST_F32(cam + 0x20u) = written;
}

/* ── Widescreen 2D (plan B5b2) ─────────────────────────────────────────────
 *
 * With the wide buffer the game still sees a 640x480 display (display +0xC0, 0x0050E59C), so
 * its 2D layout is the 4:3 one (../docs/widescreen.md § 13):
 * - sprites (flush 0x0011D140) and text (0x001EF210, origin 0x0071AF48 = -320) are placed in
 *   640 space and drawn through an ortho projection of the display's size: with that
 *   projection built from W (below) they come out centred;
 * - the quad batch 0x0063F5E0 and the rect list 0x00639760 take pre-transformed pixels: the
 *   640-space rect quads 0x00125E10 / 0x001B3B10 (menu frames, message boxes) are moved by
 *   (W - 640) / 2, full-width ones stretched to W;
 * - code that covers the real frame gets W for the length of the call: the two 2D projections,
 *   the viewports, full-screen rects and quads, the loading bar, the blur targets;
 * - full-screen sprites (menu backgrounds) and the letterbox bars are widened to W. */

/* Rect quad (cdecl x0, y0, x1, y1, colours, flag), 640 space: moved to the centre, or stretched
 * to the whole frame when it spans the 640 (the message-box dim). */
void wide_shift_rect_args(void)
{
    int dx = wide_2d_shift();
    int32_t x0, x1;
    if (!dx)
        return;
    x0 = (int32_t)GUEST_U32(g_esp + 4u);
    x1 = (int32_t)GUEST_U32(g_esp + 12u);
    if (x0 <= 0 && x1 >= 640)
        x1 += 2 * dx;
    else
        x0 += dx, x1 += dx;
    GUEST_U32(g_esp + 4u) = (uint32_t)x0;
    GUEST_U32(g_esp + 12u) = (uint32_t)x1;
}

/* Runs fn with the display's real width in display +0xC0 and 0x0050E59C. */
void wide_real_width_call(void (*fn)(void))
{
    uint32_t disp = GUEST_U32(0x00638988u);
    float d, g;
    if (!wide_2d_shift() || !disp) {
        fn();
        return;
    }
    d = GUEST_F32(disp + 0xC0u);
    g = GUEST_F32(0x0050E59Cu);
    GUEST_F32(disp + 0xC0u) = (float)wide_buffer_width();
    GUEST_F32(0x0050E59Cu) = (float)wide_buffer_width();
    fn();
    GUEST_F32(disp + 0xC0u) = d;
    GUEST_F32(0x0050E59Cu) = g;
}

/* Menu sprite 0x00094710 (thiscall; x, y, w, h at +0x14..+0x20): a full-width one (x 0,
 * w 640: the menu backgrounds of 0x00097180 / 0x00097260) covers the whole frame. */
void wide_menu_sprite_enter(void)
{
    int dx = wide_2d_shift();
    if (dx && GUEST_U32(g_esp + 0x14u) == 0 && GUEST_U32(g_esp + 0x1Cu) == 640u) {
        GUEST_U32(g_esp + 0x14u) = (uint32_t)-dx;
        GUEST_U32(g_esp + 0x1Cu) = wide_buffer_width();
    }
}

/* Letterbox bars create 0x001CF380: two sprites centred at x = 320, as wide as the display it
 * sees (640): widened to W. */
void wide_letterbox_bars_widen(void)
{
    uint32_t bar, i;
    if (!wide_2d_shift())
        return;
    for (i = 0; i < 2; i++) {
        bar = GUEST_U32(0x006CE8A0u + 4u * i);
        if (bar && GUEST_U32(bar + 0x1D8u))
            *(volatile int16_t *)((uintptr_t)GUEST_U32(bar + 0x1D8u) + 4u + g_xbox_mem_offset) =
                (int16_t)wide_buffer_width();
    }
}

/* Per-frame render dispatch 0x00114CD0: the in-game pass, or the front end's while 0x006C92F8
 * (front-end mode) is set. The front end's backdrops have art outside the 640 columns (a 4:3
 * screen hid it; the game's own dead 720-wide mode masked it too, in 0x0013FD70), so while
 * it's up the GPU glue blacks out the sides. In game the sides are the wider 3D view. */
void wide_render_dispatch_enter(void)
{
#ifdef MKSM_HAVE_GPU
    static int last = -1;
    int on = wide_2d_shift() && GUEST_U32(0x006C92F8u) != 0;
    if (on != last) {
        mksm_gpu_set_side_mask(on);
        last = on;
    }
#endif
}
