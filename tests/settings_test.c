/* Unit test for src/settings.c (plan B7b1): INI parse/write round trip and the monitor ->
 * scale/aspect defaults. CTest `settings`; tests/test_settings.py runs the built exe too.
 * Prints one line per failure and exits 1 if any. */
#include "settings.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

static void test_parse(void)
{
    MksmSettings s;
    const char *text =
        "; comment\r\n"
        "# also a comment\n"
        "[section]\n"
        "  scale = 3  \r\n"
        "aspect=16:9\n"
        "volume=0.5\n"
        "volume=0.25\n"            /* last wins */
        "save_dir = D:\\My Saves\\mksm \n"
        "game_dir=C:\\Games\\mksm\\game\n"
        "fullscreen=\n"             /* present, empty */
        "bogus=1\n"                 /* unknown */
        "no equals sign\n"
        "filter=nearest";           /* no newline at the end */
    int bad = mksm_settings_parse(&s, text);
    CHECK(bad == 2, "bad lines %d, want 2", bad);
    CHECK(strcmp(s.value[MKSM_SET_SCALE], "3") == 0, "scale '%s'", s.value[MKSM_SET_SCALE]);
    CHECK(strcmp(s.value[MKSM_SET_ASPECT], "16:9") == 0, "aspect '%s'", s.value[MKSM_SET_ASPECT]);
    CHECK(strcmp(s.value[MKSM_SET_VOLUME], "0.25") == 0, "volume '%s'", s.value[MKSM_SET_VOLUME]);
    CHECK(strcmp(s.value[MKSM_SET_SAVE_DIR], "D:\\My Saves\\mksm") == 0, "save_dir '%s'",
          s.value[MKSM_SET_SAVE_DIR]);
    CHECK(strcmp(s.value[MKSM_SET_GAME_DIR], "C:\\Games\\mksm\\game") == 0, "game_dir '%s'",
          s.value[MKSM_SET_GAME_DIR]);
    CHECK(strcmp(s.value[MKSM_SET_FILTER], "nearest") == 0, "filter '%s'", s.value[MKSM_SET_FILTER]);
    CHECK(s.present[MKSM_SET_FULLSCREEN] && !s.value[MKSM_SET_FULLSCREEN][0], "fullscreen empty");
    CHECK(!s.present[MKSM_SET_WIDE], "wide not present");
    CHECK(strcmp(s.extra, "bogus=1\n") == 0, "extra '%s'", s.extra);
    /* Unknown lines survive a rewrite (B7b2: the menu saves the file). */
    {
        MksmSettings t;
        char buf[4096];
        mksm_settings_format(&s, buf, sizeof(buf));
        CHECK(mksm_settings_parse(&t, buf) == 1, "rewrite bad lines");
        CHECK(strcmp(t.extra, "bogus=1\n") == 0, "rewritten extra '%s'", t.extra);
        CHECK(strcmp(t.value[MKSM_SET_SAVE_DIR], s.value[MKSM_SET_SAVE_DIR]) == 0, "rewrite save_dir");
    }
    CHECK(mksm_settings_parse(&s, "") == 0 && !s.present[MKSM_SET_SCALE], "empty text");
    CHECK(mksm_settings_parse(&s, NULL) == 0, "NULL text");
}

static void test_round_trip(void)
{
    MksmSettings a, b;
    char buf[4096];
    mksm_settings_defaults(&a, 2560, 1440);
    size_t n = mksm_settings_format(&a, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf), "format length %zu", n);
    CHECK(mksm_settings_parse(&b, buf) == 0, "defaults re-parse with bad lines");
    for (int i = 0; i < MKSM_SET_COUNT; i++) {
        CHECK(a.present[i] == b.present[i], "%s presence", mksm_settings_key(i));
        CHECK(strcmp(a.value[i], b.value[i]) == 0, "%s '%s' vs '%s'", mksm_settings_key(i),
              a.value[i], b.value[i]);
        CHECK(mksm_settings_find(mksm_settings_key(i)) == i, "find %s", mksm_settings_key(i));
    }
    /* Cut output: snprintf-like length, still terminated. */
    char small[16];
    size_t m = mksm_settings_format(&a, small, sizeof(small));
    CHECK(m == n && strlen(small) == sizeof(small) - 1, "cut format %zu/%zu", m, strlen(small));
}

static void test_monitor(void)
{
    static const struct { unsigned w, h; int scale; const char *aspect; } cases[] = {
        { 640, 480, 1, "4:3" },      { 1280, 720, 2, "16:9" },     { 1366, 768, 2, "16:9" },
        { 1920, 1080, 2, "16:9" },   { 1920, 1200, 3, "16:10" },   { 2560, 1440, 3, "16:9" },
        { 2560, 1600, 3, "16:10" },  { 3840, 2160, 4, "16:9" },    { 2560, 1080, 2, "2560:1080" },
        { 3440, 1440, 3, "3440:1440" }, { 1280, 1024, 2, "4:3" },  { 1280, 800, 2, "16:10" },
        { 7680, 4320, 4, "16:9" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char a[32];
        MksmSettings s;
        int sc = mksm_settings_scale_for_height(cases[i].h);
        mksm_settings_aspect_for(cases[i].w, cases[i].h, a, sizeof(a));
        CHECK(sc == cases[i].scale, "%ux%u scale %d, want %d", cases[i].w, cases[i].h, sc,
              cases[i].scale);
        CHECK(strcmp(a, cases[i].aspect) == 0, "%ux%u aspect %s, want %s", cases[i].w,
              cases[i].h, a, cases[i].aspect);
        mksm_settings_defaults(&s, cases[i].w, cases[i].h);
        CHECK(strcmp(s.value[MKSM_SET_ASPECT], cases[i].aspect) == 0, "defaults aspect");
    }
    CHECK(mksm_settings_scale_for_height(0) == 1, "height 0");

    /* The other defaults (D3 (2)): wide buffer, HUD at the edges, movie zoom on, Gauntlet
     * zoom off, borderless fullscreen. */
    MksmSettings s;
    mksm_settings_defaults(&s, 1920, 1080);
    CHECK(strcmp(s.value[MKSM_SET_WIDE], "1") == 0, "wide");
    CHECK(strcmp(s.value[MKSM_SET_HUD_EDGES], "1") == 0, "hud_edges");
    CHECK(strcmp(s.value[MKSM_SET_MOVIE_ZOOM], "1") == 0, "movie_zoom");
    CHECK(strcmp(s.value[MKSM_SET_GAUNTLET_ZOOM], "0") == 0, "gauntlet_zoom");
    CHECK(strcmp(s.value[MKSM_SET_FULLSCREEN], "1") == 0, "fullscreen");
    CHECK(s.present[MKSM_SET_SAVE_DIR] && !s.value[MKSM_SET_SAVE_DIR][0], "save_dir empty");
}

int main(void)
{
    test_parse();
    test_round_trip();
    test_monitor();
    printf(failures ? "settings_test: %d failure(s)\n" : "settings_test: OK\n", failures);
    return failures ? 1 : 0;
}
