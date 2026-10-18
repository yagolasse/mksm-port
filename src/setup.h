/* First-launch setup (plan B10b2): when no game folder is found, ask the player for their disc
 * image or extracted folder, check that default.xbe is the retail build this recompile is tied
 * to, extract an image once (progress window) and save the folder as game_dir= in
 * settings.ini so the next start goes straight in. Win32 only, runs on the main thread before
 * the game starts. */
#ifndef MKSM_SETUP_H
#define MKSM_SETUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* base_dir: %LOCALAPPDATA%\mksm_recomp. An image is extracted into <dir>\game, where <dir> is
 * the settings file's folder (base_dir, or the folder of a RECOMP_SETTINGS=<path> file).
 * Returns 1 with the game folder in game_dir (absolute, ANSI, saved to the settings file when
 * there is one), 0 when the player cancelled or it failed (the player was told why).
 * MKSM_SETUP_PICK=<path> (tests) takes that path instead of the dialogs: no message boxes
 * (the [SETUP] log lines say it), and a refused path returns 0 at once. */
int mksm_setup_run(const char *base_dir, char *game_dir, size_t cap);

/* Logs whether <game_dir>\default.xbe is the known retail build ([SETUP] line); 1 = known. */
int mksm_setup_log_xbe(const char *xbe_path);

#ifdef __cplusplus
}
#endif
#endif
