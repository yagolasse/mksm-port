/* Player settings file (plan 4.6, B7b1); see settings.h. */
#include "settings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *key, *env;
} defs[MKSM_SET_COUNT] = {
    [MKSM_SET_SCALE]         = { "scale",         "RECOMP_PB_GPU_SCALE" },
    [MKSM_SET_ASPECT]        = { "aspect",        "RECOMP_ASPECT" },
    [MKSM_SET_WIDE]          = { "wide",          "RECOMP_ASPECT_WIDE" },
    [MKSM_SET_HUD_EDGES]     = { "hud_edges",     "RECOMP_HUD_EDGES" },
    [MKSM_SET_MOVIE_ZOOM]    = { "movie_zoom",    "RECOMP_MOVIE_ZOOM" },
    [MKSM_SET_GAUNTLET_ZOOM] = { "gauntlet_zoom", "RECOMP_MOVIE_ZOOM_GAUNTLET" },
    [MKSM_SET_FILTER]        = { "filter",        "RECOMP_PB_GPU_FILTER" },
    [MKSM_SET_VOLUME]        = { "volume",        "RECOMP_APU_VOLUME" },
    [MKSM_SET_FULLSCREEN]    = { "fullscreen",    "RECOMP_FB_FULLSCREEN" },
    [MKSM_SET_SAVE_DIR]      = { "save_dir",      "MKSM_SAVE_DIR" },
};

const char *mksm_settings_key(int id) { return id >= 0 && id < MKSM_SET_COUNT ? defs[id].key : NULL; }
const char *mksm_settings_env(int id) { return id >= 0 && id < MKSM_SET_COUNT ? defs[id].env : NULL; }

int mksm_settings_find(const char *key)
{
    for (int i = 0; i < MKSM_SET_COUNT; i++)
        if (strcmp(defs[i].key, key) == 0)
            return i;
    return -1;
}

static void set_value(MksmSettings *s, int id, const char *v)
{
    snprintf(s->value[id], MKSM_SET_VALUE_MAX, "%s", v);
    s->present[id] = 1;
}

int mksm_settings_parse(MksmSettings *s, const char *text)
{
    int bad = 0;
    memset(s, 0, sizeof(*s));
    while (text && *text) {
        const char *end = strchr(text, '\n');
        size_t n = end ? (size_t)(end - text) : strlen(text);
        char line[2 * MKSM_SET_VALUE_MAX], *k, *v, *eq, *p;
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, text, n);
        line[n] = '\0';
        text = end ? end + 1 : text + strlen(text);

        for (k = line; isspace((unsigned char)*k); k++)
            ;
        if (!*k || *k == ';' || *k == '#' || *k == '[')
            continue;
        eq = strchr(k, '=');
        if (!eq) {
            bad++;
            continue;
        }
        for (p = eq; p > k && isspace((unsigned char)p[-1]); p--)
            ;
        *p = '\0';
        for (v = eq + 1; isspace((unsigned char)*v); v++)
            ;
        for (p = v + strlen(v); p > v && isspace((unsigned char)p[-1]); p--)
            ;
        *p = '\0';
        int id = mksm_settings_find(k);
        if (id < 0) {
            bad++;
            continue;
        }
        set_value(s, id, v);
    }
    return bad;
}

size_t mksm_settings_format(const MksmSettings *s, char *buf, size_t cap)
{
    static const char header[] =
        "; Mortal Kombat: Shaolin Monks (recomp) settings. One key=value per line.\n"
        "; An environment variable that is set (RECOMP_..., MKSM_SAVE_DIR) overrides its key.\n"
        "; scale, aspect and wide apply on restart. Empty value = the built-in default.\n";
    size_t len = 0;
    int n;
    if (cap)
        buf[0] = '\0';
    n = snprintf(buf, cap, "%s", header);
    len += n > 0 ? (size_t)n : 0;
    for (int i = 0; i < MKSM_SET_COUNT; i++) {
        if (!s->present[i])
            continue;
        n = snprintf(len < cap ? buf + len : NULL, len < cap ? cap - len : 0, "%s=%s\n",
                     defs[i].key, s->value[i]);
        len += n > 0 ? (size_t)n : 0;
    }
    return len;
}

int mksm_settings_scale_for_height(unsigned height)
{
    unsigned s = (height + 240) / 480;
    return s < 1 ? 1 : s > 4 ? 4 : (int)s;
}

void mksm_settings_aspect_for(unsigned width, unsigned height, char *out, size_t cap)
{
    double r = height ? (double)width / height : 0.0;
    if (r <= 4.0 / 3.0 * 1.01)
        snprintf(out, cap, "4:3");
    else if (fabs(r / 1.6 - 1.0) < 0.01)
        snprintf(out, cap, "16:10");
    else if (fabs(r / (16.0 / 9.0) - 1.0) < 0.01)
        snprintf(out, cap, "16:9");
    else
        snprintf(out, cap, "%u:%u", width, height);
}

void mksm_settings_defaults(MksmSettings *s, unsigned width, unsigned height)
{
    char v[32];
    memset(s, 0, sizeof(*s));
    snprintf(v, sizeof(v), "%d", mksm_settings_scale_for_height(height));
    set_value(s, MKSM_SET_SCALE, v);
    mksm_settings_aspect_for(width, height, v, sizeof(v));
    set_value(s, MKSM_SET_ASPECT, v);
    set_value(s, MKSM_SET_WIDE, "1");
    set_value(s, MKSM_SET_HUD_EDGES, "1");
    set_value(s, MKSM_SET_MOVIE_ZOOM, "1");
    set_value(s, MKSM_SET_GAUNTLET_ZOOM, "0");
    set_value(s, MKSM_SET_FILTER, "linear");
    set_value(s, MKSM_SET_VOLUME, "1");
    set_value(s, MKSM_SET_FULLSCREEN, "1");
    set_value(s, MKSM_SET_SAVE_DIR, "");
}

#ifdef _WIN32
#include <windows.h>

static char g_path[MAX_PATH];
static unsigned char g_from_file[MKSM_SET_COUNT];

const char *mksm_settings_path(void) { return g_path; }

int mksm_settings_env_from_file(const char *env)
{
    for (int i = 0; i < MKSM_SET_COUNT; i++)
        if (g_from_file[i] && _stricmp(defs[i].env, env) == 0)
            return 1;
    return 0;
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = size >= 0 && size < (1 << 20) ? malloc((size_t)size + 1) : NULL;
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

void mksm_settings_load(const char *base_dir)
{
    const char *e = getenv("RECOMP_SETTINGS");
    MksmSettings s;
    char *text;

    g_path[0] = '\0';
    if (e && strcmp(e, "0") == 0) {
        fprintf(stderr, "[SETTINGS] skipped (RECOMP_SETTINGS=0)\n");
        return;
    }
    if (e && *e && strcmp(e, "1") != 0) {
        if (!GetFullPathNameA(e, MAX_PATH, g_path, NULL))
            snprintf(g_path, MAX_PATH, "%s", e);
    } else {
        snprintf(g_path, MAX_PATH, "%s\\settings.ini", base_dir);
    }

    text = read_text(g_path);
    if (text) {
        int bad = mksm_settings_parse(&s, text);
        free(text);
        fprintf(stderr, "[SETTINGS] read %s%s\n", g_path,
                bad ? " (some lines not understood, ignored)" : "");
    } else {
        /* First start: defaults that follow the primary monitor. ENUM_CURRENT_SETTINGS
         * reports its mode in physical pixels, whatever the DPI awareness. */
        DEVMODEA dm = { .dmSize = sizeof(dm) };
        unsigned w = 640, h = 480;
        char out[2048];
        FILE *f;
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight) {
            w = dm.dmPelsWidth;
            h = dm.dmPelsHeight;
        }
        mksm_settings_defaults(&s, w, h);
        mksm_settings_format(&s, out, sizeof(out));
        CreateDirectoryA(base_dir, NULL);
        f = fopen(g_path, "wb");
        if (f) {
            fputs(out, f);
            fclose(f);
        }
        fprintf(stderr, "[SETTINGS] %s %s (monitor %ux%u: scale %s, aspect %s)\n",
                f ? "wrote" : "could not write", g_path, w, h,
                s.value[MKSM_SET_SCALE], s.value[MKSM_SET_ASPECT]);
    }

    for (int i = 0; i < MKSM_SET_COUNT; i++) {
        if (!s.present[i] || !s.value[i][0])
            continue;
        if (getenv(defs[i].env)) {
            fprintf(stderr, "[SETTINGS] %s: %s is set, file value %s not used\n",
                    defs[i].key, defs[i].env, s.value[i]);
            continue;
        }
        if (_putenv_s(defs[i].env, s.value[i]) == 0) {
            g_from_file[i] = 1;
            fprintf(stderr, "[SETTINGS] %s=%s\n", defs[i].key, s.value[i]);
        }
    }
}
#endif
