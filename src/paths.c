/**
 * MK Shaolin Monks - paths and run defaults (plan B14, split out of main.c): the game, base
 * and save directories, the one-time save migration, the built-in env defaults.
 */

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#define _GNU_SOURCE
#include "win32_compat.h"
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "paths.h"
#include "settings.h"

/* Game directory: the extracted disc root, holding default.xbe and the
 * data files. Taken from the first command-line argument, else the
 * MKSM_GAME_DIR environment variable (or the settings file's game_dir=),
 * else "game" in the working directory. Made absolute by resolve_game_dir().
 * Without a default.xbe there (and no argument / variable of the user's),
 * the first-launch setup asks for the disc (src/setup.c, plan B10b2). */
#define DEFAULT_GAME_DIR        "game"

#ifndef _WIN32
int mksm_argc;
char **mksm_argv;
#endif

/* ── Host file system (Win32 calls, POSIX equivalents) ──────── */

#ifdef _WIN32
static void full_path(const char *in, char *out)
{
    if (!GetFullPathNameA(in, MAX_PATH, out, NULL))
        snprintf(out, MAX_PATH, "%s", in);
}
bool path_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static bool path_is_dir(const char *p)
{
    DWORD fa = GetFileAttributesA(p);
    return fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);
}
/* The exe's folder; false if unknown. */
static bool exe_dir(char *out)
{
    DWORD n = GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = n && n < MAX_PATH ? strrchr(out, '\\') : NULL;
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}
static bool move_path(const char *from, const char *to) { return MoveFileExA(from, to, 0) != 0; }
static void make_dirs(const char *dir) { SHCreateDirectoryExA(NULL, dir, NULL); }
static int env_put(const char *name, const char *value) { return _putenv_s(name, value); }
#else
static void full_path(const char *in, char *out)
{
    char cwd[MAX_PATH];
    if (in[0] != '/' && getcwd(cwd, sizeof(cwd)))
        snprintf(out, MAX_PATH, "%s/%s", cwd, in);
    else
        snprintf(out, MAX_PATH, "%s", in);
}
bool path_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static bool path_is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static bool exe_dir(char *out)
{
    ssize_t n = readlink("/proc/self/exe", out, MAX_PATH - 1);
    char *slash;
    if (n <= 0)
        return false;
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return true;
}
static bool move_path(const char *from, const char *to) { return rename(from, to) == 0; }
static void make_dirs(const char *dir)
{
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}
static int env_put(const char *name, const char *value) { return setenv(name, value, 1); }
#endif

char g_game_dir[MAX_PATH];
char g_xbe_path[MAX_PATH];

/* Base directory: the emulated hard disk (partition images, T:/Z: caches) and
 * settings.ini (plan B7b1). %LOCALAPPDATA%\mksm_recomp: not the toolkit default
 * (%LOCALAPPDATA%\xboxrecomp), which every xboxrecomp title shares, and not the
 * game dir, which may be read-only (a Steam install).
 * Save directory: the title's UDATA/TDATA (the saves) alone, <base>\saves, so a
 * player can sync the saves without the settings. MKSM_SAVE_DIR (or the
 * settings file's save_dir=) moves it. It goes to the toolkit as
 * RECOMP_SAVE_DIR, which moves just those two; one already set wins. Both made
 * absolute by resolve_base_dir() / resolve_save_dir(). */

char g_base_dir[MAX_PATH];
char g_save_dir[MAX_PATH];
bool g_save_dir_toolkit = true;  /* RECOMP_SAVE_DIR = g_save_dir (we set it) */

/* ── Game directory ────────────────────────────────────────── */

void set_game_dir(const char *dir)
{
    size_t len;

    full_path(dir, g_game_dir);

    /* Drop trailing separators, but keep a bare drive root like "D:\" (or "/"). */
    len = strlen(g_game_dir);
    while (len > (DIRSEP[0] == '/' ? 1 : 3)
            && (g_game_dir[len - 1] == '\\' || g_game_dir[len - 1] == '/'))
        g_game_dir[--len] = '\0';

    snprintf(g_xbe_path, MAX_PATH, "%s" DIRSEP "default.xbe", g_game_dir);
}

/* After mksm_settings_load: its game_dir= arrives as MKSM_GAME_DIR. */
GameDirFrom resolve_game_dir(void)
{
    const char *env = getenv("MKSM_GAME_DIR");

    if (mksm_argc > 1 && mksm_argv[1][0]) {
        set_game_dir(mksm_argv[1]);
        return GAME_DIR_ARG;
    }
    if (env && env[0]) {
        set_game_dir(env);
        return mksm_settings_env_from_file("MKSM_GAME_DIR") ? GAME_DIR_FILE : GAME_DIR_ENV;
    }
    set_game_dir(DEFAULT_GAME_DIR);
    if (!path_exists(g_xbe_path)) {
        /* game/ beside the exe (where the setup extracts to), when started from elsewhere. */
        char exe_game[MAX_PATH];
        if (exe_dir(exe_game)
                && strlen(exe_game) + sizeof(DIRSEP DEFAULT_GAME_DIR) <= MAX_PATH) {
            strcat(exe_game, DIRSEP DEFAULT_GAME_DIR);
            snprintf(g_xbe_path, MAX_PATH, "%s" DIRSEP "default.xbe", exe_game);
            if (path_exists(g_xbe_path))
                set_game_dir(exe_game);
            else
                set_game_dir(DEFAULT_GAME_DIR);   /* both missing: report the working dir's */
        }
    }
    return GAME_DIR_DEFAULT;
}

/* ── Base and save directories ─────────────────────────────── */

static void trim_dir(char *dir)
{
    size_t len = strlen(dir);
    while (len > (DIRSEP[0] == '/' ? 1 : 3) && (dir[len - 1] == '\\' || dir[len - 1] == '/'))
        dir[--len] = '\0';
}

/* The per-user data folder: %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share on
 * POSIX. false if unknown. */
static bool user_data_dir(char *out)
{
#ifdef _WIN32
    return SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, out));
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/')
        snprintf(out, MAX_PATH, "%s", xdg);
    else if (home && home[0])
        snprintf(out, MAX_PATH, "%s/.local/share", home);
    else
        return false;
    return true;
#endif
}

void resolve_base_dir(void)
{
    char base[MAX_PATH];

    if (user_data_dir(base)) {
        snprintf(g_base_dir, MAX_PATH, "%s" DIRSEP SAVE_DIR_NAME, base);
    } else {
        /* Next to the exe (the game dir isn't known yet: settings.ini may name it). */
        if (!exe_dir(base))
            snprintf(base, MAX_PATH, ".");
        snprintf(g_base_dir, MAX_PATH, "%s" DIRSEP SAVE_DIR_NAME, base);
    }
    trim_dir(g_base_dir);
}

/* After mksm_settings_load: its save_dir= arrives as MKSM_SAVE_DIR. */
void resolve_save_dir(void)
{
    const char *env = getenv("MKSM_SAVE_DIR");

    if (env && env[0])
        full_path(env, g_save_dir);
    else
        snprintf(g_save_dir, MAX_PATH, "%s" DIRSEP "saves", g_base_dir);
    trim_dir(g_save_dir);
}

/* Saves from older builds, moved into the save dir once so a player keeps
 * them: <base>\UDATA/TDATA (builds before B7b1 kept them next to the disk
 * images; not when MKSM_SAVE_DIR comes from the environment: a dev run's
 * folder shouldn't take the player's saves), then <game dir>\UDATA/TDATA
 * (before toolkit v0.14's UDATA/TDATA rules). Only into a save dir that has
 * none yet, and not with RECOMP_SAVE_DIR set by hand (it sends them
 * elsewhere). A move across volumes fails for a directory: base saves then
 * stay where they are (logged); game-dir saves too, and the save dir isn't
 * handed to the toolkit, whose own fallback keeps using them there. */
static bool move_saves(const char *from_dir, const char *what)
{
    static const char *const names[] = { "UDATA", "TDATA" };
    bool ok = true;

    for (int i = 0; i < 2; i++) {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, MAX_PATH, "%s" DIRSEP "%s", from_dir, names[i]);
        snprintf(to, MAX_PATH, "%s" DIRSEP "%s", g_save_dir, names[i]);
        if (!path_is_dir(from))
            continue;
        if (path_exists(to))
            continue;
        if (move_path(from, to)) {
            fprintf(stderr, "[SAVE] moved %s to %s\n", from, to);
        } else {
#ifdef _WIN32
            fprintf(stderr, "[SAVE] could not move %s (%s) to %s (error %lu)\n",
                    from, what, to, GetLastError());
#else
            fprintf(stderr, "[SAVE] could not move %s (%s) to %s (%s)\n",
                    from, what, to, strerror(errno));
#endif
            ok = false;
        }
    }
    return ok;
}

void migrate_legacy_saves(void)
{
    const char *ov = getenv("RECOMP_SAVE_DIR");

    if (ov && ov[0]) {
        g_save_dir_toolkit = false;
        snprintf(g_save_dir, MAX_PATH, "%s", ov);   /* for the log line */
        return;
    }
    make_dirs(g_save_dir);  /* absolute, any depth */
#ifdef _WIN32
    if (_stricmp(g_save_dir, g_base_dir) != 0
#else
    if (strcmp(g_save_dir, g_base_dir) != 0
#endif
            && (!getenv("MKSM_SAVE_DIR") || mksm_settings_env_from_file("MKSM_SAVE_DIR")))
        move_saves(g_base_dir, "old save location");
    if (!move_saves(g_game_dir, "saves in the game folder")) {
        g_save_dir_toolkit = false;
        fprintf(stderr, "[SAVE] the game folder's saves stay in use there\n");
    }
    if (g_save_dir_toolkit) {
        env_put("RECOMP_SAVE_DIR", g_save_dir);
        mksm_settings_derived_env("RECOMP_SAVE_DIR");  /* worked out again after a restart */
    }
}

/* ── Built-in run defaults (plan B10a) ─────────────────────── */

/* What the run scripts set for the game to run, as the exe's own defaults, so a
 * double-clicked exe runs as tools/run.sh does. Applied after the settings file
 * (environment > settings.ini > this table), only where the variable isn't set at
 * all: an empty value counts as set (RECOMP_APU_EP_ALIVE= / RECOMP_PB_WALK= are the
 * A/B "off"). presence: the toolkit only checks that the variable exists, so 0 or
 * empty must remove it. MKSM_DEFAULTS=0 skips the table (the bare toolkit, e.g. for
 * a capture replay). */
typedef struct EnvDefault {
    const char *name;
    const char *value;
    bool presence;
} EnvDefault;

static const EnvDefault k_env_defaults[] = {
    { "RECOMP_PB_EXEC",         "1",          true  },  /* D3D init waits for the GPU */
    { "RECOMP_PB_GPU",          "1",          false },  /* xemu's PGRAPH draws */
    { "RECOMP_FB_WINDOW",       "1",          true  },  /* the game window */
    { "RECOMP_USB",             "1",          true  },  /* emulated OHCI: the pads */
    { "RECOMP_KEYBOARD",        "1",          false },  /* keyboard on pad 1 */
    { "RECOMP_AC97_READY",      "1",          true  },  /* codec ready + emulated APU */
    { "RECOMP_APU_DSP_ACK",     "auto",       false },  /* (the toolkit's default too) */
    { "RECOMP_APU_EP_ALIVE",    "0xFE85A018", false },  /* plan 2.7a6 */
    { "RECOMP_HEAP_RECLAIM",    "1",          true  },  /* contiguous arena reuse */
    { "RECOMP_TITLE_KEVENTS",   "1",          false },  /* host events for title events */
    { "RECOMP_PB_WALK",         "dma",        false },  /* plan H.5a */
    { "RECOMP_AUDIO_BUF_COUNT", "8",          false },  /* 85 ms, plan H.5a */
};

#ifndef _WIN32
/* POSIX has one environment, so a presence variable turned off must be unset for the toolkit
 * to see it off, and a settings relaunch (execv) would then find it unset and turn it on
 * again. The names turned off travel in MKSM_ENV_OFF (comma-separated) instead, as Windows'
 * process block keeps their 0. */
static bool env_off_listed(const char *name)
{
    const char *p = getenv("MKSM_ENV_OFF");
    size_t len = strlen(name);
    while (p && *p) {
        if (strncmp(p, name, len) == 0 && (p[len] == ',' || p[len] == '\0'))
            return true;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return false;
}

static void env_off_add(const char *name)
{
    const char *list = getenv("MKSM_ENV_OFF");
    char buf[1024];
    if (env_off_listed(name))
        return;
    if (list && *list)
        snprintf(buf, sizeof(buf), "%s,%s", list, name);
    else
        snprintf(buf, sizeof(buf), "%s", name);
    setenv("MKSM_ENV_OFF", buf, 1);
}

/* 0 = unset, 1 = set (value in buf, may be empty; "0" for a name in MKSM_ENV_OFF). */
static int env_state(const char *name, char *buf, size_t cap)
{
    const char *v = getenv(name);
    if (!v) {
        int off = env_off_listed(name);
        snprintf(buf, cap, "%s", off ? "0" : "");
        return off;
    }
    if (strlen(v) >= cap)
        buf[0] = '\0';          /* too long for us: only "set" matters then */
    else
        snprintf(buf, cap, "%s", v);
    return 1;
}
#else
/* 0 = unset, 1 = set (value in buf, may be empty). Reads the process environment
 * block, which tells an empty variable from a missing one. */
static int env_state(const char *name, char *buf, DWORD cap)
{
    DWORD n;
    SetLastError(0);
    n = GetEnvironmentVariableA(name, buf, cap);
    if (n == 0) {
        buf[0] = '\0';
        return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : 1;
    }
    if (n >= cap)
        buf[0] = '\0';          /* too long for us: only "set" matters then */
    return 1;
}
#endif

void apply_env_defaults(void)
{
    const char *skip = getenv("MKSM_DEFAULTS");
    char val[64];

    if (skip && strcmp(skip, "0") == 0) {
        fprintf(stderr, "[ENV] built-in defaults skipped (MKSM_DEFAULTS=0)\n");
        return;
    }
    for (size_t i = 0; i < sizeof(k_env_defaults) / sizeof(k_env_defaults[0]); i++) {
        const EnvDefault *d = &k_env_defaults[i];
        if (!env_state(d->name, val, sizeof(val))) {
            env_put(d->name, d->value);
            /* A relaunch (settings menu) works it out again, like a file value. */
            mksm_settings_derived_env(d->name);
            fprintf(stderr, "[ENV] %s=%s (default)\n", d->name, d->value);
        } else if (d->presence && (!val[0] || strcmp(val, "0") == 0)) {
            /* getenv (the CRT's copy) no longer has it, so the toolkit sees off; the
             * process block keeps 0, so a relaunched child is off too. */
#ifdef _WIN32
            _putenv_s(d->name, "");
            SetEnvironmentVariableA(d->name, "0");
#else
            unsetenv(d->name);
            env_off_add(d->name);
#endif
            fprintf(stderr, "[ENV] %s off\n", d->name);
        }
    }
}
