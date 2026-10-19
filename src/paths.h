/**
 * Paths and run defaults (plan B14, split out of main.c): the game / base / save directories,
 * the one-time save migration, and the built-in env defaults (plan B10a).
 */
#ifndef MKSM_PATHS_H
#define MKSM_PATHS_H

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#include <limits.h>
#endif
#include <stdbool.h>

#ifdef _WIN32
#define DIRSEP          "\\"
#define mksm_argc       __argc
#define mksm_argv       __argv
#else
#undef MAX_PATH
#define MAX_PATH        PATH_MAX
#define DIRSEP          "/"
extern int mksm_argc;       /* set by main() */
extern char **mksm_argv;
#endif

#define SAVE_DIR_NAME           "mksm_recomp"

typedef enum GameDirFrom {
    GAME_DIR_ARG,       /* argv[1] */
    GAME_DIR_ENV,       /* MKSM_GAME_DIR set by the user */
    GAME_DIR_FILE,      /* settings.ini's game_dir= */
    GAME_DIR_DEFAULT,   /* "game" in the working directory */
} GameDirFrom;

extern char g_game_dir[MAX_PATH];
extern char g_xbe_path[MAX_PATH];
extern char g_base_dir[MAX_PATH];
extern char g_save_dir[MAX_PATH];
extern bool g_save_dir_toolkit;     /* RECOMP_SAVE_DIR = g_save_dir (we set it) */

bool path_exists(const char *p);
void set_game_dir(const char *dir);
GameDirFrom resolve_game_dir(void);
void resolve_base_dir(void);
void resolve_save_dir(void);
void migrate_legacy_saves(void);
void apply_env_defaults(void);

#endif
