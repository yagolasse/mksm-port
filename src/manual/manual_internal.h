/**
 * Private header of the hand-written overrides (plan B14): what src/recomp_manual.c and the
 * feature files in src/manual/ share. recomp_manual.c keeps every sub_XXXXXXXX definition and
 * wrapper (the lifter's --exclude-manual scans that one file); the helpers, state and env
 * parsing live here by feature:
 *   widescreen.c  RECOMP_ASPECT / _WIDE, the wide 2D layer
 *   hud.c         HUD at the screen edges (RECOMP_HUD_EDGES / _LOG)
 *   movie.c       movie zoom, movie start/end hooks
 *   cine.c        RECOMP_CINE_LOG
 *   fence.c       D3D's fence wait
 *   audio.c       RECOMP_AUDIO_TICK_STATS
 */
#ifndef MKSM_MANUAL_INTERNAL_H
#define MKSM_MANUAL_INTERNAL_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <intrin.h>    /* _InterlockedExchange, _mm_pause (plan B7c live options) */
#define MKSM_THREAD_LOCAL __declspec(thread)
#else
/* POSIX (plan H.5c2a): the same intrinsics from GCC's builtins. */
#include <strings.h>
#include <time.h>
#include <immintrin.h> /* _mm_pause */
#define _stricmp strcasecmp
#define MKSM_THREAD_LOCAL __thread
#define _InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedCompareExchange(p, x, c) __extension__({                         \
        __typeof__(+*(p)) mksm_cmp_ = (c);                                              \
        __atomic_compare_exchange_n((p), &mksm_cmp_, (x), 0, __ATOMIC_SEQ_CST,         \
                                    __ATOMIC_SEQ_CST);                                 \
        mksm_cmp_; })
#endif

#ifdef MKSM_HAVE_GPU
#include "gpu/mksm_gpu.h"
#endif

/* Guest register state and memory (defined by the runtime). */
extern ptrdiff_t g_xbox_mem_offset;
extern MKSM_THREAD_LOCAL uint32_t g_ecx, g_esp;

#define GUEST_U8(va)  (*(volatile uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GUEST_U32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GUEST_F32(va) (*(volatile float *)((uintptr_t)(va) + g_xbox_mem_offset))

/* ── widescreen.c ───────────────────────────────────────────── */
float wide_aspect(void);                    /* the target aspect, 0 = off */
uint32_t wide_buffer_width(void);           /* the wide back buffer's width, 0 = off */
void wide_set_output(int movie);
int wide_2d_shift(void);                    /* how far 640-authored 2D moves, 0 = not wide */
extern int wide_in_movie;
void wide_display_init_done(uint32_t disp); /* after the display init 0x0011B290 */
void wide_mode_lookup_enter(void);          /* entry of 0x003D30D3 */
void wide_reset_enter(void);                /* entry of 0x003C99E0 */
void wide_camera_enter(void);               /* entry of 0x00124150 */
void wide_shift_rect_args(void);            /* entry of the rect quads 0x00125E10 / 0x001B3B10 */
void wide_real_width_call(void (*fn)(void));
void wide_menu_sprite_enter(void);          /* entry of 0x00094710 */
void wide_letterbox_bars_widen(void);       /* after the bars are created (0x001CF380) */
void wide_render_dispatch_enter(void);      /* entry of 0x00114CD0 */

/* ── hud.c ──────────────────────────────────────────────────── */
#define HUD_SLOTS 128u
typedef struct HudLayoutState {
    uint32_t list;
    uint32_t before[HUD_SLOTS];
    int centred_table;
} HudLayoutState;
int hud_layout_begin(HudLayoutState *st);   /* 0 = untracked manager: just run the original */
void hud_layout_end(const HudLayoutState *st);
void hud_loop_begin(void);                  /* around the element callback loop 0x001B6900 */
void hud_loop_end(void);
void hud_note_callback(void);               /* from recomp_lookup_manual, every indirect call */
void hud_frame_enter(void);                 /* entry of the sprite flush 0x0011D140 */
int hud_sprite_begin(uint32_t *s, float *x);/* nonzero: the sprite's x was moved for the draw */
void hud_sprite_end(uint32_t s, float x);
void hud_text_enter(void);                  /* entry of the text helper 0x001EEF90 */
void hud_log_rect(void);

/* ── movie.c ────────────────────────────────────────────────── */
void movie_start_hook(void);                /* entry of the movie set-up 0x00142420 */
void movie_end_hook(void);                  /* entry of the tear-down 0x00141F10 */

/* ── cine.c ─────────────────────────────────────────────────── */
int cine_log_on(void);
void cine_log_update(void);                 /* entry of the game update 0x000CB780 */

/* ── fence.c ────────────────────────────────────────────────── */
void fence_wait(void (*gen)(void));         /* D3D's fence wait 0x003C8490; gen = the lifted body */

/* ── audio.c ────────────────────────────────────────────────── */
int audio_tick_stats(void);
void audio_tick_count(void);                /* one call of the sound driver's tick */

#endif
