/**
 * RECOMP_CINE_LOG (plan B5a): the cutscene signals. Split out of recomp_manual.c in plan B14;
 * the entry hook stays there and calls cine_log_update.
 */
#include "manual_internal.h"

/* RECOMP_CINE_LOG=1 (plan B5a): the cutscene signals, sampled at the start of each game
 * update 0x000CB780 (= after the previous one), one line whenever one changes
 * (widescreen.md § 10.4). seq = a scripted sequence is playing (0x006C4198); cincam = the
 * current camera is the cinematic class and not done (FUN_0016CEC0's test, read from the
 * fields); auto = the update's own "bars wanted" state (0x005ACEE6); bars = the letterbox
 * sprites are up (FUN_001CF6C0's test) with their y; fade = fade active / alpha. */
int cine_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CINE_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

void cine_log_update(void)
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
