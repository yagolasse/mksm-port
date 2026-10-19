/**
 * HUD at the screen edges (plan B5d, B5f): which HUD elements are centred, which side the
 * others sit on, the draw/text hooks that move them. Split out of recomp_manual.c in plan B14;
 * the entry hooks and wrappers stay there and call the functions here.
 */
#include "manual_internal.h"

/* ── HUD at the screen edges (plan B5d2) ───────────────────────────────────
 *
 * With the wide buffer the HUD (authored for 640) sits pillarboxed in the centre 640 columns.
 * RECOMP_HUD_EDGES (default on with RECOMP_ASPECT_WIDE; =0 keeps the 4:3 place) moves it out to
 * the screen edges: pieces on the left by -(W - 640) / 2, on the right by +(W - 640) / 2,
 * centred ones stay (../docs/widescreen.md § 15/16).
 * - The HUD manager (built by 0x000921D0) keeps its elements, one sprite each, in a pointer
 *   array (mgr +0, 128 slots). Elements come from layout tables (0x00091240, thiscall ecx = mgr,
 *   the table first) or per player (0x000914F0); the wrapper below remembers the centred ones:
 *   made from the bottom-bar or popup tables, placed at x 320, or the pause map's pieces
 *   (plan B5f: same manager, flags 0x600B4..0x600B6, placed in the menu's "Map Display" box).
 * - Side of the others: the sprite's x (+0xC0, 640 space) at the draw vs 320. The draw
 *   (0x0011D000, cdecl entry; sprite at entry +8) gets the x moved for the call.
 * - The HUD text is queued by the elements' per-frame callbacks (run by 0x001B6900 through
 *   +0x1FC, with the element as the argument) via the text helper 0x001EEF90: the callback's
 *   element is caught at the indirect call (recomp_lookup_manual) and its text moved alike.
 *
 * RECOMP_HUD_LOG=1 (plan B5d1) logs every 300th sprite flush: the drawn sprites (position,
 * size, anchor flags, HUD side) and the next frame's queued text and rect quads with the
 * element whose callback queued them (`[HUD]` lines). */
static int hud_log_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HUD_LOG");
        on = e && *e && *e != '0';
    }
    return on;
}

/* RECOMP_HUD_EDGES, -1 = not read yet. The settings menu sets it while running (plan B7c,
 * present thread); read at each sprite draw and text queue on the game's thread. Nothing
 * outlives one draw (the sprite's x is put back), so the next frame is laid out the new way. */
static volatile long hud_edges_on = -1;

void mksm_manual_set_hud_edges(int on)
{
    _InterlockedExchange(&hud_edges_on, on != 0);
    fprintf(stderr, "[HUD] edges %s (live)\n", on ? "on" : "off");
}

/* how far HUD pieces move to the edges (0 = off) */
static int hud_edge_shift(void)
{
    long on = hud_edges_on;
    if (on < 0) {
        const char *e = getenv("RECOMP_HUD_EDGES");
        _InterlockedCompareExchange(&hud_edges_on, !(e && *e == '0'), -1);
        on = hud_edges_on;
    }
    return on ? wide_2d_shift() : 0;
}

static uint32_t hud_mgr, hud_cur;
static int hud_in_loop;
static uint32_t hud_centred[HUD_SLOTS];
static unsigned hud_ncentred, hud_frame;

/* RAM, or its 0x80000000 mirror (the HUD's sprites and manager live there) */
static int hud_va_ok(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u);
}

static uint32_t hud_list(void)
{
    uint32_t list = hud_va_ok(hud_mgr) ? GUEST_U32(hud_mgr) : 0;
    return hud_va_ok(list) ? list : 0;
}

/* -1 left, +1 right, 0 centred or not a HUD element */
static int hud_side(uint32_t s)
{
    uint32_t list = hud_list();
    float x;
    unsigned i;
    if (!list || !s)
        return 0;
    for (i = 0; i < HUD_SLOTS && GUEST_U32(list + 4u * i) != s; i++)
        ;
    if (i == HUD_SLOTS)
        return 0;
    for (i = 0; i < hud_ncentred; i++)
        if (hud_centred[i] == s)
            return 0;
    x = GUEST_F32(s + 0xC0u);
    return x < 320.0f ? -1 : x > 320.0f ? 1 : 0;
}

/* Elements from a layout table (0x38-byte rows, x at +8; thiscall: table, layer, flags).
 * Centred: the bottom bar 0x004EC2C8, the popup 0x004EC878, rows at x 320, and the pause
 * map (flags 0x600B5 backdrop, 0x600B6 rooms of area 0x0059E3AC, 0x600B4 the no-map picture;
 * the HUD's own are 0x600B2). The map's rooms are replaced on each area change
 * (0x0008B0F0 frees them), so entries no longer in the list are dropped. */
int hud_layout_begin(HudLayoutState *st)
{
    uint32_t mgr = g_ecx, table = GUEST_U32(g_esp + 4u), list;
    uint32_t flags = GUEST_U32(g_esp + 12u);
    unsigned i;
    st->centred_table = table == 0x004EC2C8u || table == 0x004EC878u ||
                        (flags >= 0x000600B4u && flags <= 0x000600B6u);
    list = hud_va_ok(mgr) ? GUEST_U32(mgr) : 0;
    st->list = list;
    if (!hud_va_ok(list))
        return 0;
    if (mgr != hud_mgr || GUEST_U32(mgr + 4u) == 0)
        hud_ncentred = 0;    /* a new HUD */
    hud_mgr = mgr;
    for (i = 0; i < HUD_SLOTS; i++)
        st->before[i] = GUEST_U32(list + 4u * i);
    return 1;
}

void hud_layout_end(const HudLayoutState *st)
{
    uint32_t list = st->list;
    int centred_table = st->centred_table;
    unsigned i, j;
    for (j = 0; j < hud_ncentred;) {    /* forget freed elements */
        for (i = 0; i < HUD_SLOTS && GUEST_U32(list + 4u * i) != hud_centred[j]; i++)
            ;
        if (i == HUD_SLOTS)
            hud_centred[j] = hud_centred[--hud_ncentred];
        else
            j++;
    }
    for (i = 0; i < HUD_SLOTS; i++) {
        uint32_t s = GUEST_U32(list + 4u * i);
        if (!s || s == st->before[i])
            continue;
        for (j = 0; j < hud_ncentred && hud_centred[j] != s; j++)
            ;
        if (centred_table || GUEST_F32(s + 0xC0u) == 320.0f) {
            if (j == hud_ncentred && hud_ncentred < HUD_SLOTS)
                hud_centred[hud_ncentred++] = s;
        } else if (j < hud_ncentred) {
            hud_centred[j] = hud_centred[--hud_ncentred];
        }
    }
}

/* the per-frame element callbacks; hud_cur is the element whose callback runs */
void hud_loop_begin(void)
{
    hud_cur = 0;
    hud_in_loop = 1;
}

void hud_loop_end(void)
{
    hud_in_loop = 0;
    hud_cur = 0;
}

/* called from recomp_lookup_manual for every indirect call: the loop's call returns to
 * 0x001B691D with the element as the only argument */
void hud_note_callback(void)
{
    if (hud_in_loop && GUEST_U32(g_esp) == 0x001B691Du)
        hud_cur = GUEST_U32(g_esp + 4u);
}

static int hud_log_frame(void)
{
    return hud_log_on() && hud_frame % 300u == 299u;
}

/* logged on the frame after a sprite log, so a whole frame's queue shows */
static int hud_log_queue_frame(void)
{
    return hud_log_on() && hud_frame % 300u == 0u && hud_frame;
}

void hud_frame_enter(void)
{
    hud_frame++;
    if (hud_log_frame())
        fprintf(stderr, "[HUD] ---- frame %u (sprites, then the next frame's text/rects)\n",
                hud_frame);
}

static void hud_log_sprite(uint32_t e)
{
    uint32_t rect = e ? GUEST_U32(e) : 0, s = e ? GUEST_U32(e + 8u) : 0;
    if (!rect || !s || GUEST_U8(s + 0x70u) || !GUEST_U32(s + 0x128u))
        return;
    fprintf(stderr, "[HUD] sprite 0x%08X side=%d x=%.1f y=%.1f flags=0x%02X w=%d h=%d "
            "tex=0x%08X\n", s, hud_side(s), GUEST_F32(s + 0xC0u), GUEST_F32(s + 0xC4u),
            GUEST_U32(s + 0x78u) & 0xFFu,
            *(volatile int16_t *)((uintptr_t)rect + 4u + g_xbox_mem_offset),
            *(volatile int16_t *)((uintptr_t)rect + 6u + g_xbox_mem_offset),
            GUEST_U32(s + 0x128u));
}

/* sprite draw (cdecl entry): a HUD piece drawn at its edge-anchored x. Returns nonzero when the
 * sprite's x was moved for the draw (*s the sprite, *x its own x); hud_sprite_end puts it back. */
int hud_sprite_begin(uint32_t *s_out, float *x_out)
{
    uint32_t e = GUEST_U32(g_esp + 4u), s = e ? GUEST_U32(e + 8u) : 0;
    int dx = hud_edge_shift(), side = dx && s ? hud_side(s) : 0;
    float x;
    if (hud_log_frame())
        hud_log_sprite(e);
    if (!side)
        return 0;
    x = GUEST_F32(s + 0xC0u);
    GUEST_F32(s + 0xC0u) = x + (float)(side * dx);
    *s_out = s;
    *x_out = x;
    return 1;
}

void hud_sprite_end(uint32_t s, float x)
{
    GUEST_F32(s + 0xC0u) = x;
}

/* text (thiscall; x, y floats, colour, string, align): a HUD element's text follows it */
void hud_text_enter(void)
{
    int dx = hud_edge_shift(), side = dx && hud_cur ? hud_side(hud_cur) : 0;
    if (hud_log_queue_frame()) {
        char str[48];
        uint32_t va = GUEST_U32(g_esp + 0x10u);
        size_t i;
        for (i = 0; va && i < sizeof(str) - 1 && GUEST_U8(va + i); i++) {
            uint8_t c = GUEST_U8(va + i);
            str[i] = c >= 0x20 && c < 0x7F ? (char)c : '.';
        }
        str[i] = 0;
        fprintf(stderr, "[HUD] text elem=0x%08X side=%d ra=0x%08X x=%.1f y=%.1f align=%d "
                "\"%s\"\n", hud_cur, side, GUEST_U32(g_esp), GUEST_F32(g_esp + 4u),
                GUEST_F32(g_esp + 8u), (int)GUEST_U32(g_esp + 0x14u), str);
    }
    if (side)
        GUEST_F32(g_esp + 4u) += (float)(side * dx);
}

void hud_log_rect(void)
{
    if (!hud_log_queue_frame())
        return;
    fprintf(stderr, "[HUD] rect elem=0x%08X ra=0x%08X %d,%d .. %d,%d\n", hud_cur,
            GUEST_U32(g_esp), (int)GUEST_U32(g_esp + 4u), (int)GUEST_U32(g_esp + 8u),
            (int)GUEST_U32(g_esp + 12u), (int)GUEST_U32(g_esp + 16u));
}
