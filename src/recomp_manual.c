/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/*
 * Incremental-link thunk table at the start of .text: one 5-byte
 * `jmp rel32` per function. Function pointers stored in data (vtables,
 * callbacks) point at these thunks, but the lifter only registers the
 * thunks it saw called directly, so an indirect call to any other one
 * fails to resolve and is skipped. Follow the jump instead.
 */
#define ILT_LO 0x00011005u
#define ILT_HI 0x00016CDAu

static recomp_func_t resolve_ilt_thunk(uint32_t xbox_va)
{
    const uint8_t *p = (const uint8_t *)((uintptr_t)xbox_va + g_xbox_mem_offset);
    int32_t rel;

    if (p[0] != 0xE9)
        return (recomp_func_t)0;
    rel = (int32_t)((uint32_t)p[1] | (uint32_t)p[2] << 8 |
                    (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24);
    return recomp_lookup(xbox_va + 5 + (uint32_t)rel);
}

/*
 * RECOMP_AUDIO_TICK_STATS=1 (plan 3.3c): count the game's 5 ms sound tick,
 * the periodic timeSetEvent callback 0x00319DF0 (the XDK timer thread calls
 * it through a function pointer, so it comes through here). One line per
 * second: calls, and how many found the driver idle (busy byte +0xD99 == 1,
 * so the body ran) vs. still busy.
 */
#include <stdlib.h>

extern void sub_00319DF0(void);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);

static void counted_audio_tick(void)
{
    static unsigned long long t0;
    static unsigned calls, ran;
    uint32_t snd = *(volatile uint32_t *)((uintptr_t)0x008B4464u + g_xbox_mem_offset);
    unsigned long long now = GetTickCount64();

    calls++;
    if (snd && *(volatile uint8_t *)((uintptr_t)snd + 0xD99u + g_xbox_mem_offset) == 1)
        ran++;
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000) {
        fprintf(stderr, "[AUDIO-TICK] calls=%u ran=%u state=0x%08X\n", calls, ran, snd);
        fflush(stderr);
        calls = ran = 0;
        t0 = now;
    }
    sub_00319DF0();
}

static int audio_tick_stats(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_AUDIO_TICK_STATS");
        on = e && *e && *e != '0';
    }
    return on;
}

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
extern __declspec(thread) uint32_t g_ecx, g_esp;

#define GUEST_U8(va)  (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GUEST_U32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GUEST_F32(va) (*(volatile float *)((uintptr_t)(va) + g_xbox_mem_offset))

#ifdef MKSM_HAVE_GPU
#include "gpu/mksm_gpu.h"
#endif

/* The target aspect, 0 = off (the game's own 4:3). */
static float wide_aspect(void)
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

static uint32_t wide_buffer_width(void)
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
static void wide_set_output(int movie)
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
extern void sub_0011B290_gen(void);

static int wide_in_movie;

static void wide_patch_display(uint32_t disp)
{
    uint32_t w = wide_buffer_width();
    if (!w || !disp || wide_in_movie)
        return;
    GUEST_U32(disp + 0xCu) = w;
}

/* How far 640-authored 2D moves to sit centred in the wide frame (0 = not wide). */
static int wide_2d_shift(void)
{
    uint32_t w = wide_buffer_width();
    return w && !wide_in_movie ? (int)(w - 640u) / 2 : 0;
}

void sub_0011B290(void)
{
    static int logged;
    uint32_t disp = g_ecx;
    sub_0011B290_gen();
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
void sub_003D30D3_enter(void)
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
void sub_003C99E0_enter(void)
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
void sub_00124150_enter(void)
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

static int cine_log_on(void);

/* Movie set-up / tear-down (the blocking movie loop 0x00128DE0 calls both). */
/* With the wide buffer a movie keeps 640x480: the set-up resets the device to the movie's size
 * (0x0011B200, same present parameters) and draws a 640-space quad; the present pillarboxes
 * it. The tear-down ends with the display init 0x0011B290, which widens the buffer again. */
/* Movie zoom (plan B5c2): most movies are letterboxed inside their 4:3 frame, so on a wider
 * window the present may zoom them up to the share the picture fills (480 / picture rows, by
 * decoding the files: ../docs/widescreen.md § 14), cropping only black rows. The rest (logo or
 * card on black, full-frame shots) stay pillarboxed. Keyed by the file name the movie player
 * 0x00128DE0 (name, w, h, ...) gets, the only caller of the set-up. RECOMP_MOVIE_ZOOM=0 turns it
 * off; RECOMP_MOVIE_ZOOM_GAUNTLET=1 zooms the Gauntlet trailer too, which cuts 17% of its
 * full-frame shots at 16:9. */
static float movie_zoom_for(const char *name)
{
    static const struct { const char *name; float zoom; } table[] = {
        { "opening.sfd", 480.0f / 288 }, { "end006.sfd", 480.0f / 340 },
        { "end011.sfd", 480.0f / 352 },  { "final.sfd", 480.0f / 340 },
        { "blitz.sfd", 480.0f / 322 },
    };
    const char *e = getenv("RECOMP_MOVIE_ZOOM"), *base = name, *p;
    if (e && *e == '0')
        return 0.0f;
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (_stricmp(base, table[i].name) == 0)
            return table[i].zoom;
    e = getenv("RECOMP_MOVIE_ZOOM_GAUNTLET");
    if (_stricmp(base, "gauntlet.sfd") == 0 && e && *e && *e != '0')
        return 4.0f / 3.0f;
    return 0.0f;
}

static void movie_zoom_start(void)
{
    char name[128];
    uint32_t va, w, h;
    size_t i;
    float zoom;
    /* The set-up's return address in 0x00128DE0, whose own arguments sit above its 4 saved
     * registers and the 2 set-up arguments. */
    if (GUEST_U32(g_esp) != 0x00128DF3u)
        return;
    va = GUEST_U32(g_esp + 0x20u);
    w = GUEST_U32(g_esp + 0x24u);
    h = GUEST_U32(g_esp + 0x28u);
    for (i = 0; va && i < sizeof(name) - 1 && GUEST_U8(va + i); i++)
        name[i] = (char)GUEST_U8(va + i);
    name[i] = 0;
    zoom = movie_zoom_for(name);
    fprintf(stderr, "[MOVIE] %s %ux%u: zoom up to %.3f\n", name, w, h, zoom ? zoom : 1.0f);
#ifdef MKSM_HAVE_GPU
    mksm_gpu_set_movie_zoom(zoom);
#endif
}

void sub_00142420_enter(void)
{
    if (cine_log_on())
        fprintf(stderr, "[cine] movie start\n");
    movie_zoom_start();
    wide_in_movie = 1;
    if (wide_aspect())
        wide_set_output(1);
}

void sub_00141F10_enter(void)
{
    if (cine_log_on())
        fprintf(stderr, "[cine] movie end\n");
#ifdef MKSM_HAVE_GPU
    mksm_gpu_set_movie_zoom(0.0f);
#endif
    wide_in_movie = 0;
    if (wide_aspect())
        wide_set_output(0);
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
static void wide_shift_rect_args(void)
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

void sub_00125E10_enter(void) { wide_shift_rect_args(); }
void sub_001B3B10_enter(void) { wide_shift_rect_args(); }

/* Runs fn with the display's real width in display +0xC0 and 0x0050E59C. */
static void wide_real_width_call(void (*fn)(void))
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

/* sprite pass set-up: ortho W x H into vertex constant 0x66 */
extern void sub_0011C730_gen(void);
void sub_0011C730(void) { wide_real_width_call(sub_0011C730_gen); }

/* the other 2D ortho (0x0050E59C) */
extern void sub_0012F800_gen(void);
void sub_0012F800(void) { wide_real_width_call(sub_0012F800_gen); }

/* render pass viewport W x H */
extern void sub_00136E80_gen(void);
void sub_00136E80(void) { wide_real_width_call(sub_00136E80_gen); }

/* fade/transition loop: viewport + full-screen rect 0x00130680 */
extern void sub_00130770_gen(void);
void sub_00130770(void) { wide_real_width_call(sub_00130770_gen); }

/* full-screen rect */
extern void sub_0010E530_gen(void);
void sub_0010E530(void) { wide_real_width_call(sub_0010E530_gen); }

/* full-screen quad, built once */
extern void sub_00119740_gen(void);
void sub_00119740(void) { wide_real_width_call(sub_00119740_gen); }

/* loading bar: x = (W - 640) / 2 + x */
extern void sub_00125B30_gen(void);
void sub_00125B30(void) { wide_real_width_call(sub_00125B30_gen); }

/* blur targets W/2 x H/2, W/4 x H/4 */
extern void sub_00137E10_gen(void);
void sub_00137E10(void) { wide_real_width_call(sub_00137E10_gen); }


/* 0x0013FFB0 (quads from a 640-space layout into the quad batch) has its own centring by
 * (W - 640) / 2, gated on display +0xA, which retail never sets. */
extern void sub_0013FFB0_gen(void);

static void sub_0013FFB0_centred(void)
{
    uint32_t disp = GUEST_U32(0x00638988u);
    uint8_t was = GUEST_U8(disp + 0xAu);
    GUEST_U8(disp + 0xAu) = 1;
    sub_0013FFB0_gen();
    GUEST_U8(disp + 0xAu) = was;
}

void sub_0013FFB0(void)
{
    if (!wide_2d_shift() || !GUEST_U32(0x00638988u))
        sub_0013FFB0_gen();
    else
        wide_real_width_call(sub_0013FFB0_centred);
}

/* Menu sprite 0x00094710 (thiscall; x, y, w, h at +0x14..+0x20): a full-width one (x 0,
 * w 640: the menu backgrounds of 0x00097180 / 0x00097260) covers the whole frame. */
void sub_00094710_enter(void)
{
    int dx = wide_2d_shift();
    if (dx && GUEST_U32(g_esp + 0x14u) == 0 && GUEST_U32(g_esp + 0x1Cu) == 640u) {
        GUEST_U32(g_esp + 0x14u) = (uint32_t)-dx;
        GUEST_U32(g_esp + 0x1Cu) = wide_buffer_width();
    }
}

/* Letterbox bars create 0x001CF380: two sprites centred at x = 320, as wide as the display it
 * sees (640): widened to W. */
extern void sub_001CF380_gen(void);

void sub_001CF380(void)
{
    uint32_t bar, i;
    sub_001CF380_gen();
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
void sub_00114CD0_enter(void)
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

/* RECOMP_CINE_LOG=1 (plan B5a): the cutscene signals, sampled at the start of each game
 * update 0x000CB780 (= after the previous one), one line whenever one changes
 * (widescreen.md § 10.4). seq = a scripted sequence is playing (0x006C4198); cincam = the
 * current camera is the cinematic class and not done (FUN_0016CEC0's test, read from the
 * fields); auto = the update's own "bars wanted" state (0x005ACEE6); bars = the letterbox
 * sprites are up (FUN_001CF6C0's test) with their y; fade = fade active / alpha. */
static int cine_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CINE_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

void sub_000CB780_enter(void)
{
    static unsigned frame;
    static uint32_t last_key = 0xFFFFFFFFu;
    uint32_t mgr, cam = 0, top, bot, key;
    unsigned seq, cincam = 0, mode = 0, done = 0, autob, bars, fade;
    float top_y = 0.0f, bot_y = 0.0f;

    frame++;
    if (!cine_log_on())
        return;
    seq = GUEST_U8(0x006C4198u);
    mgr = GUEST_U32(0x00599844u);
    if (mgr) {
        uint32_t slot = GUEST_U32(mgr + 0x1Cu) - GUEST_U32(mgr + 0x120u);
        if (slot < 64)
            cam = GUEST_U32(mgr + 0x20u + slot * 4u);
    }
    if (cam && GUEST_U32(cam) == 0x0049E3A0u) {    /* cinematic camera vtable */
        mode = GUEST_U32(cam + 0x2CCu);
        done = GUEST_U8(cam + 0x2D9u);
        cincam = !done || GUEST_U8(0x005144F8u);
    }
    autob = GUEST_U8(0x005ACEE6u);
    top = GUEST_U32(0x006CE8A0u);
    bot = GUEST_U32(0x006CE8A4u);
    bars = top && !GUEST_U8(top + 0x70u);
    if (top)
        top_y = GUEST_F32(top + 0xC4u);
    if (bot)
        bot_y = GUEST_F32(bot + 0xC4u);
    fade = GUEST_U8(0x005146E4u);
    /* Bar y only counts as "at rest" or "moving", or a slide prints 30 lines. */
    key = seq | cincam << 1 | autob << 2 | bars << 3 | (fade != 0) << 4 |
          (top_y != 0.0f) << 5 | (cam && GUEST_U32(cam) == 0x0049E3A0u) << 6;
    if (key != last_key) {
        last_key = key;
        fprintf(stderr,
                "[cine] f=%u seq=%u cincam=%u (cam 0x%08X vt 0x%08X mode %u done %u) auto=%u "
                "bars=%u topY=%.1f botY=%.1f fade=%u/%.0f\n",
                frame, seq, cincam, cam, cam ? GUEST_U32(cam) : 0u, mode, done, autob, bars,
                top_y, bot_y, fade, GUEST_F32(0x005146ECu));
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x00319DF0u && audio_tick_stats())
        return counted_audio_tick;

    /*
     * Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    if (xbox_va - ILT_LO < ILT_HI - ILT_LO)
        return resolve_ilt_thunk(xbox_va);
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
