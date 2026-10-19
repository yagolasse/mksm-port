/**
 * Movie zoom (plan B5c2) and the movie start/end hooks. Split out of recomp_manual.c in plan
 * B14; the entry hooks stay there and call the functions here.
 */
#include "manual_internal.h"

/* Movie set-up / tear-down (the blocking movie loop 0x00128DE0 calls both). */
/* With the wide buffer a movie keeps 640x480: the set-up resets the device to the movie's size
 * (0x0011B200, same present parameters) and draws a 640-space quad; the present pillarboxes
 * it. The tear-down ends with the display init 0x0011B290, which widens the buffer again. */
/* Movie zoom (plan B5c2): most movies are letterboxed inside their 4:3 frame, so on a wider
 * window the present may zoom them up to the share the picture fills (480 / picture rows, by
 * decoding the files: ../docs/widescreen.md § 14), cropping only black rows. The rest (logo or
 * card on black, full-frame shots) stay pillarboxed. Keyed by the file name the movie player
 * 0x00128DE0 (name, w, h, ...) gets, the only caller of the set-up. Only with RECOMP_ASPECT set
 * (wide or anamorphic); RECOMP_MOVIE_ZOOM=0 turns it off there too; RECOMP_MOVIE_ZOOM_GAUNTLET=1 zooms the Gauntlet trailer too, which cuts 17% of its
 * full-frame shots at 16:9. The settings menu flips both while running (plan B7c,
 * mksm_manual_set_movie_zoom, present thread) and the movie hooks run on the game's thread: a
 * spin lock keeps a menu change computed from a movie that just ended from overwriting the end
 * hook's 0 (the gameplay frame would stay zoomed). */
static volatile long movie_lock;
static int movie_zoom_on = -1, movie_gauntlet_on = -1;    /* -1 = env not read yet */
static float movie_cur_zoom;    /* the playing movie's own zoom, 0 = none or no movie */
static int movie_cur_gauntlet;

static void movie_lock_take(void)
{
    while (_InterlockedExchange(&movie_lock, 1))
        _mm_pause();
}

static void movie_lock_drop(void) { _InterlockedExchange(&movie_lock, 0); }

/* The movie's own zoom, whatever the switches say (movie_zoom_apply applies them). */
static float movie_zoom_for(const char *name, int *gauntlet)
{
    static const struct { const char *name; float zoom; } table[] = {
        { "opening.sfd", 480.0f / 288 }, { "end006.sfd", 480.0f / 340 },
        { "end011.sfd", 480.0f / 352 },  { "final.sfd", 480.0f / 340 },
        { "blitz.sfd", 480.0f / 322 },
    };
    const char *base = name, *p;
    *gauntlet = 0;
    if (!wide_aspect())
        return 0.0f;    /* without RECOMP_ASPECT movies look as on the Xbox */
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (_stricmp(base, table[i].name) == 0)
            return table[i].zoom;
    if (_stricmp(base, "gauntlet.sfd") == 0) {
        *gauntlet = 1;
        return 4.0f / 3.0f;
    }
    return 0.0f;
}

/* Lock held: hands the present the zoom the switches allow for the playing movie; returns it. */
static float movie_zoom_apply(void)
{
    float z;
    if (movie_zoom_on < 0) {
        const char *e = getenv("RECOMP_MOVIE_ZOOM");
        movie_zoom_on = !(e && *e == '0');
    }
    if (movie_gauntlet_on < 0) {
        const char *e = getenv("RECOMP_MOVIE_ZOOM_GAUNTLET");
        movie_gauntlet_on = e && *e && *e != '0';
    }
    z = movie_zoom_on && (!movie_cur_gauntlet || movie_gauntlet_on) ? movie_cur_zoom : 0.0f;
#ifdef MKSM_HAVE_GPU
    mksm_gpu_set_movie_zoom(z);
#endif
    return z;
}

/* The settings menu (plan B7c): RECOMP_MOVIE_ZOOM / _GAUNTLET while running; a playing movie
 * follows at the next present. */
void mksm_manual_set_movie_zoom(int on, int gauntlet)
{
    float z;
    movie_lock_take();
    movie_zoom_on = on != 0;
    movie_gauntlet_on = gauntlet != 0;
    z = movie_zoom_apply();
    movie_lock_drop();
    fprintf(stderr, "[MOVIE] zoom %s, Gauntlet %s (live): now up to %.3f\n", on ? "on" : "off",
            gauntlet ? "on" : "off", z ? z : 1.0f);
}

static void movie_zoom_start(void)
{
    char name[128];
    uint32_t va, w, h;
    size_t i;
    float own, zoom;
    int gauntlet;
    /* The set-up's return address in 0x00128DE0, whose own arguments sit above its 4 saved
     * registers and the 2 set-up arguments. */
    if (GUEST_U32(g_esp) != 0x00128DF3u)
        return;
    va = GUEST_U32(g_esp + 0x20u);
    w = GUEST_U32(g_esp + 0x24u);
    h = GUEST_U32(g_esp + 0x28u);
    for (i = 0; va && i < sizeof(name) - 1 && GUEST_U8(va + i); i++)
        name[i] = (char)GUEST_U8(va + i);
    name[i] = 0;
    own = movie_zoom_for(name, &gauntlet);
    movie_lock_take();
    movie_cur_zoom = own;
    movie_cur_gauntlet = gauntlet;
    zoom = movie_zoom_apply();
    movie_lock_drop();
    fprintf(stderr, "[MOVIE] %s %ux%u: zoom up to %.3f\n", name, w, h, zoom ? zoom : 1.0f);
}

void movie_start_hook(void)
{
    if (cine_log_on())
        fprintf(stderr, "[cine] movie start\n");
    movie_zoom_start();
    wide_in_movie = 1;
    if (wide_aspect())
        wide_set_output(1);
}

void movie_end_hook(void)
{
    if (cine_log_on())
        fprintf(stderr, "[cine] movie end\n");
    movie_lock_take();
    movie_cur_zoom = 0.0f;
    movie_cur_gauntlet = 0;
    movie_zoom_apply();
    movie_lock_drop();
    wide_in_movie = 0;
    if (wide_aspect())
        wide_set_output(0);
}
