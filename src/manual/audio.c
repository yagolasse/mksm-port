/**
 * RECOMP_AUDIO_TICK_STATS (plan 3.3c): the sound driver's tick counter. Split out of
 * recomp_manual.c in plan B14; the wrapper that reaches the game's tick stays there.
 */
#include "manual_internal.h"

/*
 * RECOMP_AUDIO_TICK_STATS=1 (plan 3.3c): count the game's 5 ms sound tick,
 * the periodic timeSetEvent callback 0x00319DF0 (the XDK timer thread calls
 * it through a function pointer, so it comes through here). One line per
 * second: calls, and how many found the driver idle (busy byte +0xD99 == 1,
 * so the body ran) vs. still busy.
 */
#ifdef _WIN32
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#else
static unsigned long long GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}
#endif

void audio_tick_count(void)
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
}

int audio_tick_stats(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_AUDIO_TICK_STATS");
        on = e && *e && *e != '0';
    }
    return on;
}
