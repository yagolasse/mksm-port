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
#include "manual/manual_internal.h"

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
 * RECOMP_AUDIO_TICK_STATS=1 (plan 3.3c): the game's 5 ms sound tick, the periodic timeSetEvent
 * callback 0x00319DF0, comes through recomp_lookup_manual (audio.c counts the calls).
 */
extern void sub_00319DF0(void);

static void counted_audio_tick(void)
{
    audio_tick_count();
    sub_00319DF0();
}

/* ── Widescreen (plan 4.2, B5a) ────────────────────────────────────────────
 *
 * RECOMP_ASPECT, the wide buffer and the 2D layer: widescreen.c has the notes and the helpers;
 * the hooks are entry hooks (`sub_X_enter`): the lift has to be made with
 * `--exclude-manual src/recomp_manual.c` so the generated function calls them first. Direct
 * calls in the lifted code don't go through recomp_lookup_manual, so an override there would
 * miss them. */

/* Display init 0x0011B290 (thiscall, ecx = display object *(0x00638988)): see
 * wide_display_init_done. */
extern void sub_0011B290_gen(void);

void sub_0011B290(void)
{
    uint32_t disp = g_ecx;
    sub_0011B290_gen();
    wide_display_init_done(disp);
}

/* D3D's display-mode lookup 0x003D30D3 (cdecl; width, height, flags, ...) */
void sub_003D30D3_enter(void) { wide_mode_lookup_enter(); }

/* D3DDevice_Reset 0x003C99E0 (stdcall, the present parameters) */
void sub_003C99E0_enter(void) { wide_reset_enter(); }

/* Camera update 0x00124150 (thiscall, ecx = camera): Hor+ */
void sub_00124150_enter(void) { wide_camera_enter(); }

/* Movie set-up / tear-down (the blocking movie loop 0x00128DE0 calls both). */
void sub_00142420_enter(void) { movie_start_hook(); }

void sub_00141F10_enter(void) { movie_end_hook(); }

/* ── HUD at the screen edges (plan B5d2) ───────────────────────────────────
 * hud.c has the notes and the state. */

/* HUD layout tables (0x38-byte rows; thiscall: table, layer, flags): remembers the centred ones */
extern void sub_00091240_gen(void);
void sub_00091240(void)
{
    HudLayoutState st;
    if (!hud_layout_begin(&st)) {
        sub_00091240_gen();
        return;
    }
    sub_00091240_gen();
    hud_layout_end(&st);
}

/* the per-frame element callbacks; hud_cur is the element whose callback runs */
extern void sub_001B6900_gen(void);
void sub_001B6900(void)
{
    hud_loop_begin();
    sub_001B6900_gen();
    hud_loop_end();
}

void sub_0011D140_enter(void) { hud_frame_enter(); }

/* sprite draw (cdecl entry): a HUD piece drawn at its edge-anchored x */
extern void sub_0011D000_gen(void);
void sub_0011D000(void)
{
    uint32_t s;
    float x;
    if (!hud_sprite_begin(&s, &x)) {
        sub_0011D000_gen();
        return;
    }
    sub_0011D000_gen();
    hud_sprite_end(s, x);
}

/* text (thiscall; x, y floats, colour, string, align): a HUD element's text follows it */
void sub_001EEF90_enter(void) { hud_text_enter(); }

/* ── Widescreen 2D (plan B5b2): see widescreen.c ───────────── */

void sub_00125E10_enter(void) { hud_log_rect(); wide_shift_rect_args(); }
void sub_001B3B10_enter(void) { wide_shift_rect_args(); }

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

/* Menu sprite 0x00094710: a full-width one covers the whole frame. */
void sub_00094710_enter(void) { wide_menu_sprite_enter(); }

/* Letterbox bars create 0x001CF380: widened to W. */
extern void sub_001CF380_gen(void);

void sub_001CF380(void)
{
    sub_001CF380_gen();
    wide_letterbox_bars_widen();
}

/* Per-frame render dispatch 0x00114CD0: blacks out the sides in the front end. */
void sub_00114CD0_enter(void) { wide_render_dispatch_enter(); }

/* RECOMP_CINE_LOG=1 (plan B5a): the cutscene signals, at the start of each game update. */
void sub_000CB780_enter(void) { cine_log_update(); }

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    hud_note_callback();
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

/* ── D3D's fence wait (plan 2.7c6): see fence.c ────────────── */
extern void sub_003C8490_gen(void);
void sub_003C8490(void) { fence_wait(sub_003C8490_gen); }

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
