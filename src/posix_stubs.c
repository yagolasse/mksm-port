/* POSIX stand-ins for the Win32-only parts (plan H.5c2a): the RECOMP_PROF sampler (src/prof.c,
 * SuspendThread + GetThreadContext), the first-launch setup (src/setup.c, Win32 dialogs) and its
 * default.xbe check (src/xiso.c, CNG hashes). On Linux the game folder comes from the first
 * argument, MKSM_GAME_DIR or settings.ini's game_dir=; extract a disc with tools/xiso.py. */
#include <stdio.h>
#include <stdlib.h>

#include "prof.h"
#include "setup.h"

void prof_start_from_env(void)
{
    if (getenv("RECOMP_PROF"))
        fprintf(stderr, "[PROF] RECOMP_PROF ignored: no sampler on this platform yet\n");
}

int mksm_setup_run(const char *base_dir, char *game_dir, size_t cap)
{
    (void)base_dir;
    (void)game_dir;
    (void)cap;
    fprintf(stderr, "[SETUP] no first-launch setup on this platform yet: extract the disc "
                    "(tools/xiso.py extract) and pass the folder as the first argument or "
                    "set MKSM_GAME_DIR\n");
    return 0;
}

int mksm_setup_log_xbe(const char *xbe_path)
{
    fprintf(stderr, "[SETUP] %s: version not checked on this platform yet\n", xbe_path);
    return 0;
}
