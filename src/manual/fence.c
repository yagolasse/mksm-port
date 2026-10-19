/**
 * D3D's fence wait (plan 2.7c6). Split out of recomp_manual.c in plan B14; the wrapper for
 * 0x003C8490 stays there and passes the lifted body in.
 */
#include "manual_internal.h"

/* ── D3D's fence wait (plan 2.7c6) ──────────────────────────────────────
 *
 * 0x003C8490 (stdcall: fence count, flags; Swap waits here for the previous frame) ends in a
 * loop that polls the semaphore word the GPU writes at each fence ([[device+0x30]]) until it
 * reaches the count: a core spinning whenever the title is ahead of the GPU (~half the main
 * thread at the 60 cap since plan 2.7c3). A count other than the device's current one
 * (device+0x2C) belongs to a fence D3D has already sent and kicked (0x003C83D0 kicks), so it
 * is waited for here, blocking (mksm_gpu_wait_semaphore), and the body then finds it reached
 * and returns at once. The current count is the body's own: it sends that fence first.
 * RECOMP_FENCE_WAIT=0: the body alone, as before (the [XGPU] fence line then counts only the
 * waits it ended short of the count: 0 in a pad run, so waiting first changes nothing D3D does). */
void fence_wait(void (*gen)(void))
{
#ifdef MKSM_HAVE_GPU
    static int on = -1;
    uint32_t dev = GUEST_U32(0x003F0C08u), target = GUEST_U32(g_esp + 4u), now;
    const volatile uint32_t *sem;

    if (on < 0) {
        const char *e = getenv("RECOMP_FENCE_WAIT");
        on = !e || !*e || *e != '0';
    }
    now = dev ? GUEST_U32(dev + 0x2Cu) : target;
    sem = &GUEST_U32(dev ? GUEST_U32(dev + 0x30u) : 0u);
    if (!dev || target == now) {
        gen();
        return;
    }
    if (on) {
        mksm_gpu_wait_semaphore(sem, now, target);
        gen();
        return;
    }
    gen();
    if (now - target < now - *sem)
        mksm_gpu_note_semaphore_unreached();
#else
    gen();
#endif
}
