/* Player settings file (plan 4.6, B7b1): <base dir>\settings.ini, key=value lines.
 *
 * Each key stands for an environment variable the port already reads (RECOMP_PB_GPU_SCALE,
 * RECOMP_ASPECT, ...). At start the file's values are put into the environment for every
 * variable that isn't set already, so a set variable overrides the file and the code that
 * reads the options stays as it is. A missing file is written with defaults that follow the
 * primary monitor (scale and aspect). RECOMP_SETTINGS=0 skips the file (the smoke test and
 * the run scripts: dev runs keep the Xbox look), =1 or unset uses <base dir>\settings.ini,
 * anything else is the file's path. */
#ifndef MKSM_SETTINGS_H
#define MKSM_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MKSM_SET_SCALE,         /* RECOMP_PB_GPU_SCALE    1..4, restart */
    MKSM_SET_ASPECT,        /* RECOMP_ASPECT          4:3, 16:9, W:H, restart */
    MKSM_SET_WIDE,          /* RECOMP_ASPECT_WIDE     0/1, restart */
    MKSM_SET_HUD_EDGES,     /* RECOMP_HUD_EDGES       0/1 */
    MKSM_SET_MOVIE_ZOOM,    /* RECOMP_MOVIE_ZOOM      0/1 */
    MKSM_SET_GAUNTLET_ZOOM, /* RECOMP_MOVIE_ZOOM_GAUNTLET 0/1 */
    MKSM_SET_FILTER,        /* RECOMP_PB_GPU_FILTER   linear / nearest */
    MKSM_SET_VOLUME,        /* RECOMP_APU_VOLUME      0..1 */
    MKSM_SET_FULLSCREEN,    /* RECOMP_FB_FULLSCREEN   0/1 */
    MKSM_SET_SAVE_DIR,      /* MKSM_SAVE_DIR          folder for UDATA/TDATA */
    MKSM_SET_GAME_DIR,      /* MKSM_GAME_DIR          the game folder (first-launch setup, B10b2) */
    MKSM_SET_COUNT
};

#define MKSM_SET_VALUE_MAX 260

#define MKSM_SET_EXTRA_MAX 1024

typedef struct MksmSettings {
    char value[MKSM_SET_COUNT][MKSM_SET_VALUE_MAX];
    unsigned char present[MKSM_SET_COUNT];  /* key seen (or set), even with an empty value */
    char extra[MKSM_SET_EXTRA_MAX];         /* unknown key=value lines, kept on rewrite */
} MksmSettings;

const char *mksm_settings_key(int id);
const char *mksm_settings_env(int id);
int mksm_settings_find(const char *key);    /* id, or -1 */

/* Parses INI text into s (cleared first): key=value per line, spaces around both trimmed,
 * ';' / '#' comments and [sections] skipped, the last of a repeated key wins. Returns the
 * number of lines it couldn't use (unknown keys, no '='); unknown key=value lines are kept in
 * s->extra (as far as they fit) so a rewrite doesn't lose them (a newer build's keys, typos). */
int mksm_settings_parse(MksmSettings *s, const char *text);

/* The file text for s: a comment header, then every present key in table order, then the
 * unknown lines (s->extra).
 * Returns the length (like snprintf: may exceed cap, then the text is cut). */
size_t mksm_settings_format(const MksmSettings *s, char *buf, size_t cap);

/* D3D (2): internal scale = round(monitor height / 480), ties up, clamped 1..4. */
int mksm_settings_scale_for_height(unsigned height);
/* The monitor's aspect as a RECOMP_ASPECT value: "4:3" up to 4:3 (and taller), "16:10" /
 * "16:9" within 1%, else "W:H" in pixels (the wide buffer caps at 1024, B5b3). */
void mksm_settings_aspect_for(unsigned width, unsigned height, char *out, size_t cap);
/* The defaults a new file gets, for a monitor of width x height physical pixels. */
void mksm_settings_defaults(MksmSettings *s, unsigned width, unsigned height);

/* At start, before anything reads the variables: resolves the file under base_dir, writes it
 * with the monitor defaults if it is missing, and exports its values. Logs [SETTINGS]. */
void mksm_settings_load(const char *base_dir);
/* The file in use ("" when skipped), and whether a variable got its value from it (so a
 * relaunch can drop those and the menu can tell them from the user's own, B7b2). */
const char *mksm_settings_path(void);
int mksm_settings_env_from_file(const char *env);
/* The file's contents as read (or written fresh) at start, updated by mksm_settings_save;
 * NULL when the file is skipped. For the menu (B7b2), one thread. */
const MksmSettings *mksm_settings_file(void);
/* Writes s to the file in use (a temp file, then replaced). 0 = written. Logs [SETTINGS]. */
int mksm_settings_save(const MksmSettings *s);
/* A variable the port set itself from a file value (e.g. RECOMP_SAVE_DIR from save_dir=):
 * a relaunch drops it too, so the new process works it out again. */
void mksm_settings_derived_env(const char *env);
/* Starts the exe again (same path, command line, working dir, std handles) without the
 * variables that came from the file, so the new process reads the file afresh, and ends this
 * one. The new process waits for this one to be gone before it opens anything
 * (MKSM_RESTART_PARENT). Returns only if the new process couldn't be started (-1). */
int mksm_settings_restart(void);

#ifdef __cplusplus
}
#endif

#endif
