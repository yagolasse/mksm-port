/*
 * The in-game settings menu (plan 4.6; B7a1: up, toggles, shows the current values; B7b2 edits;
 * B7c: the non-restart options apply at once).
 *
 * Dear ImGui (external/imgui, unmodified) with its SDL2 and OpenGL3 backends, drawn by
 * mksm_gpu's present thread over the game frame (mksm_gpu_set_overlay), so it costs nothing
 * while closed. Three threads meet here:
 *
 *   window thread  (toolkit fb_present_sdl.c, "fb window"): every SDL event comes through
 *                  event_hook first. F1 toggles the menu, Esc closes it; while open the
 *                  keyboard/mouse events are queued for ImGui and key-downs are kept from the
 *                  game's keyboard pad (key-ups still go through, so no key stays held).
 *   polling thread (the toolkit's xbox_InputGetState, the USB thread): input_filter sees each
 *                  pad state the game is about to get. Back+Start pressed together toggles the
 *                  menu; the state is kept for ImGui's gamepad navigation; while open (and
 *                  after closing, until the pad is let go) the game gets a neutral pad.
 *   present thread (mksm_gpu.c): overlay() runs ImGui's frame with the present context
 *                  current; while open it asks for a redraw every 16 ms (no new frame needed).
 *
 * The settings come from settings.ini (src/settings.c, B7b1) and are edited here (B7b2, see
 * the Settings section below). While open the game is frozen (B7a2):
 * mksm_gpu holds the method thread at the next flip and the toolkit's APU stops (no frames, the
 * audio device paused where it is).
 */
#include "menu.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

#include "input/xinput_xbox.h"    /* toolkit: XBOX_INPUT_STATE, xbox_InputSetFilter */
#include "mksm_gpu.h"
#include "settings.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
/* toolkit src/video/video_player.h, src/input/keyboard.h */
typedef int (*xbox_fb_event_hook_fn)(const void *sdl_event);
void xbox_FramebufferWindowSetEventHook(xbox_fb_event_hook_fn hook);
void *xbox_FramebufferWindowSdl(void);
void xbox_FramebufferWindowSetFullscreen(int on);
int xbox_FramebufferWindowIsFullscreen(void);
void xbox_InputKeysClear(void);
/* toolkit src/apu/apu.h */
void mcpx_apu_set_host_paused(int on);
void mcpx_apu_set_volume(float vol);
/* toolkit src/input/mkb_input.h */
void mkb_set_overlay(int open);
/* recomp_manual.c (B7c) */
void mksm_manual_set_hud_edges(int on);
void mksm_manual_set_movie_zoom(int on, int gauntlet);
}

namespace {

std::atomic<bool> g_open{false};
std::atomic<unsigned> g_polls{0};    /* pad states filtered (logged at close) */
std::atomic<bool> g_show_fps{false};

/* SDL events for ImGui, window thread -> present thread. */
std::mutex g_events_lock;
std::vector<SDL_Event> g_events;
const size_t kMaxEvents = 256;

void set_open(bool on)
{
    if (g_open.exchange(on) == on) {
        return;
    }
    if (on) {
        xbox_InputKeysClear();    /* a key held when the menu opens would stay held */
    }
    /* Freeze the game while open: the GPU holds at the next flip, the APU stops. */
    mksm_gpu_set_frozen(on);
    mcpx_apu_set_host_paused(on);
    mkb_set_overlay(on ? 1 : 0);  /* the mouse pad lets go of the pointer while open */
    unsigned polls = g_polls.exchange(0);
    if (on) {
        fprintf(stderr, "[MENU] open\n");
    } else {
        fprintf(stderr, "[MENU] closed (%u pad polls while open)\n", polls);
    }
    fflush(stderr);
    mksm_gpu_overlay_wake();
}

/* ── Window thread ─────────────────────────────────────────────── */

int event_hook(const void *p)
{
    const SDL_Event *ev = static_cast<const SDL_Event *>(p);
    if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_F1) {
        if (!ev->key.repeat) {
            set_open(!g_open.load());
        }
        return 1;
    }
    if (!g_open.load()) {
        return 0;
    }
    if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_ESCAPE) {
        if (!ev->key.repeat) {
            set_open(false);
        }
        return 1;
    }
    switch (ev->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_WINDOWEVENT: {
        std::lock_guard<std::mutex> lock(g_events_lock);
        if (g_events.size() < kMaxEvents) {
            g_events.push_back(*ev);
        }
        break;
    }
    default:
        break;
    }
    return ev->type == SDL_KEYDOWN || ev->type == SDL_TEXTINPUT;
}

/* ── Polling thread ────────────────────────────────────────────── */

const WORD kCombo = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
bool g_combo[XBOX_MAX_CONTROLLERS];
bool g_hold[XBOX_MAX_CONTROLLERS];      /* the game gets a neutral pad */

std::mutex g_pad_lock;
XBOX_GAMEPAD g_pad[XBOX_MAX_CONTROLLERS];
bool g_pad_seen[XBOX_MAX_CONTROLLERS];

bool pad_idle(const XBOX_GAMEPAD &g)
{
    const int dz = 8000;
    if (g.wButtons) {
        return false;
    }
    for (BYTE b : g.bAnalogButtons) {
        if (b > 30) {
            return false;
        }
    }
    return std::abs(g.sThumbLX) < dz && std::abs(g.sThumbLY) < dz &&
           std::abs(g.sThumbRX) < dz && std::abs(g.sThumbRY) < dz;
}

void input_filter(DWORD port, XBOX_INPUT_STATE *st)
{
    if (port >= XBOX_MAX_CONTROLLERS) {
        return;
    }
    g_polls++;
    bool combo = (st->Gamepad.wButtons & kCombo) == kCombo;
    if (combo && !g_combo[port]) {
        fprintf(stderr, "[MENU] Back+Start on port %lu\n", (unsigned long)port);
        set_open(!g_open.load());
    }
    g_combo[port] = combo;
    {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        g_pad[port] = st->Gamepad;
        g_pad_seen[port] = true;
    }
    /* Neutral while open, and after closing until let go: the buttons that closed the menu
     * (Back+Start, B) must not reach the game. */
    if (g_open.load()) {
        g_hold[port] = true;
    } else if (g_hold[port] && pad_idle(st->Gamepad)) {
        g_hold[port] = false;
    }
    if (g_hold[port]) {
        memset(&st->Gamepad, 0, sizeof(st->Gamepad));
    }
}

/* ── Present thread ────────────────────────────────────────────── */

bool g_inited, g_init_failed, g_was_open;
ImGuiStyle g_base_style;
float g_ui_scale;
const float kFontPx = 48.0f;    /* atlas size; drawn smaller (FontGlobalScale) */

/* Frames presented over the last half second. */
Uint64 g_fps_t0;
unsigned g_fps_frames;
float g_fps;

bool init(void)
{
    SDL_Window *win = static_cast<SDL_Window *>(xbox_FramebufferWindowSdl());
    if (!win) {
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;    /* no imgui.ini next to the exe */
    io.LogFilename = nullptr;
    /* NoMouseCursorChange: SDL_SetCursor from this thread would act on the wrong thread's
     * cursor (Win32 cursors are per thread); the window keeps the arrow. */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    g_base_style = ImGui::GetStyle();

    ImFont *font = nullptr;
#ifdef _WIN32
    char path[MAX_PATH];
    UINT n = GetWindowsDirectoryA(path, MAX_PATH);
    if (n && n < MAX_PATH - 32) {
        strcat(path, "\\Fonts\\segoeui.ttf");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            font = io.Fonts->AddFontFromFileTTF(path, kFontPx);
        }
    }
#endif
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = kFontPx;
        io.Fonts->AddFontDefault(&cfg);
    }

    /* Gamepads: fed from input_filter (the toolkit owns the controllers), not opened again. */
    if (!ImGui_ImplSDL2_InitForOpenGL(win, SDL_GL_GetCurrentContext()) ||
        !ImGui_ImplOpenGL3_Init("#version 330 core")) {
        fprintf(stderr, "[MENU] ImGui set-up failed, no menu\n");
        return false;
    }
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0);
    fprintf(stderr, "[MENU] ImGui %s up (F1 or Back+Start opens the menu)\n", IMGUI_VERSION);
    return true;
}

/* Text and spacing follow the window height: 16 px at 480 lines, 30 at 1080, 60 at 2160. */
void update_scale(unsigned height)
{
    float px = (float)height / 36.0f;
    px = px < 16.0f ? 16.0f : px > 60.0f ? 60.0f : px;
    float scale = px / 16.0f;
    if (std::fabs(scale - g_ui_scale) > 0.01f) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(scale);
        ImGui::GetIO().FontGlobalScale = px / kFontPx;
        g_ui_scale = scale;
    }
}

void feed_pad(ImGuiIO &io)
{
    XBOX_GAMEPAD g = {};
    {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        for (int p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
            if (!g_pad_seen[p]) {
                continue;
            }
            const XBOX_GAMEPAD &s = g_pad[p];
            g.wButtons |= s.wButtons;
            for (int b = 0; b < 8; b++) {
                g.bAnalogButtons[b] =
                    s.bAnalogButtons[b] > g.bAnalogButtons[b] ? s.bAnalogButtons[b] : g.bAnalogButtons[b];
            }
            if (std::abs(s.sThumbLX) + std::abs(s.sThumbLY) >
                std::abs(g.sThumbLX) + std::abs(g.sThumbLY)) {
                g.sThumbLX = s.sThumbLX;
                g.sThumbLY = s.sThumbLY;
            }
        }
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (g.wButtons & XBOX_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (g.wButtons & XBOX_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (g.wButtons & XBOX_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (g.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, g.bAnalogButtons[XBOX_BUTTON_A] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, g.bAnalogButtons[XBOX_BUTTON_B] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, g.bAnalogButtons[XBOX_BUTTON_X] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, g.bAnalogButtons[XBOX_BUTTON_Y] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadL1, g.bAnalogButtons[XBOX_BUTTON_BLACK] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadR1, g.bAnalogButtons[XBOX_BUTTON_WHITE] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadL2, g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] > 128);
    io.AddKeyEvent(ImGuiKey_GamepadR2, g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] > 128);
    /* Stick: dead zone 8000 of 32767, full at 30000; Xbox Y points up. */
    auto axis = [&](ImGuiKey key, int v) {
        float f = (v - 8000) / 22000.0f;
        f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        io.AddKeyAnalogEvent(key, f > 0.1f, f);
    };
    axis(ImGuiKey_GamepadLStickLeft, -g.sThumbLX);
    axis(ImGuiKey_GamepadLStickRight, g.sThumbLX);
    axis(ImGuiKey_GamepadLStickUp, g.sThumbLY);
    axis(ImGuiKey_GamepadLStickDown, -g.sThumbLY);
}

/* ── Settings (B7b2; present thread only) ─────────────────────────
 *
 * g_saved is the file as it is on disk (settings.c's copy at the first open), g_edit what the
 * menu shows. A live item's change takes effect at once (apply_live, B7c) and is saved when no
 * widget is held; Alt+Enter saves the fullscreen item too (sync_fullscreen, also while the menu
 * is closed). A restart item's change stays pending in g_edit until "Apply and
 * restart" saves the file and relaunches the exe, or "Revert" drops it. An item whose variable
 * the user set in the environment wins over the file (settings.h), so it is shown, not edited. */
MksmSettings g_saved, g_edit;
bool g_have_file, g_loaded;
char g_restart_error[96];

/* What an empty value means: the variable unset, the built-in default (README Run section). */
const char *const kBuiltin[MKSM_SET_COUNT] = {
    "1", "4:3", "0", "1", "1", "0", "linear", "1", "0", "", "",
};

bool is_restart(int id)
{
    return id == MKSM_SET_SCALE || id == MKSM_SET_ASPECT || id == MKSM_SET_WIDE ||
           id == MKSM_SET_SAVE_DIR || id == MKSM_SET_GAME_DIR;
}

/* The user's own variable for this item, or NULL (one the file set doesn't count). */
const char *user_env(int id)
{
    const char *name = mksm_settings_env(id);
    const char *v = getenv(name);
    return v && !mksm_settings_env_from_file(name) ? v : NULL;
}

/* The value shown: the user's variable, else the edited file value, else the default. */
const char *shown(int id)
{
    const char *u = user_env(id);
    const char *v = u ? u : g_have_file ? g_edit.value[id] : "";
    return *v ? v : kBuiltin[id];
}

bool pending(int id)
{
    return g_have_file && is_restart(id) && strcmp(g_edit.value[id], g_saved.value[id]) != 0;
}

bool any_pending(void)
{
    for (int i = 0; i < MKSM_SET_COUNT; i++) {
        if (pending(i)) {
            return true;
        }
    }
    return false;
}

void store_edit(int id, const char *v)
{
    snprintf(g_edit.value[id], sizeof(g_edit.value[id]), "%s", v);
    g_edit.present[id] = 1;
}

/* B7c: a live item takes effect now, read with its env var reader's own rule. Each setter is
 * safe from this thread: the fullscreen switch is posted to the window's thread, the rest are
 * atomics read where they are used. */
void apply_live(int id)
{
    const char *v = shown(id);
    switch (id) {
    case MKSM_SET_HUD_EDGES:
        mksm_manual_set_hud_edges(v[0] != '0');
        break;
    case MKSM_SET_MOVIE_ZOOM:
    case MKSM_SET_GAUNTLET_ZOOM:
        mksm_manual_set_movie_zoom(shown(MKSM_SET_MOVIE_ZOOM)[0] != '0',
                                   shown(MKSM_SET_GAUNTLET_ZOOM)[0] != '0');
        break;
    case MKSM_SET_FILTER:
        mksm_gpu_set_filter(strcmp(v, "nearest") == 0);
        break;
    case MKSM_SET_VOLUME:
        mcpx_apu_set_volume((float)atof(v));
        break;
    case MKSM_SET_FULLSCREEN:
        xbox_FramebufferWindowSetFullscreen(v[0] != '0');
        break;
    default:
        break;
    }
}

/* From an editable widget (never a user-set item): stored, and a live item applied. */
void set_edit(int id, const char *v)
{
    store_edit(id, v);
    if (!is_restart(id)) {
        apply_live(id);
    }
}

void load_settings(void)
{
    const MksmSettings *f = mksm_settings_file();
    g_have_file = f != nullptr;
    if (f) {
        g_saved = *f;
        g_edit = *f;
    }
    g_loaded = true;
}

/* Live items that changed go to the file (not while a slider is held). */
void save_live(void)
{
    if (!g_have_file || ImGui::IsAnyItemActive()) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < MKSM_SET_COUNT; i++) {
        if (!is_restart(i) && strcmp(g_edit.value[i], g_saved.value[i]) != 0) {
            snprintf(g_saved.value[i], sizeof(g_saved.value[i]), "%s", g_edit.value[i]);
            g_saved.present[i] = 1;
            changed = true;
        }
    }
    if (changed) {
        mksm_settings_save(&g_saved);
    }
}

/* The window's fullscreen state, followed on every present (menu open or not; user,
 * 2026-10-17: Alt+Enter is saved like the checkbox). A change the menu didn't make (Alt+Enter)
 * goes to the menu's copy and the file at once; the checkbox's own change arrives a moment after
 * its request and already matches. Not with a user-set RECOMP_FB_FULLSCREEN or no file. */
int g_fullscreen_seen = -1;

void sync_fullscreen(void)
{
    int fs = xbox_FramebufferWindowIsFullscreen();
    if (fs == g_fullscreen_seen) {
        return;
    }
    g_fullscreen_seen = fs;
    if (!g_loaded) {
        load_settings();
    }
    const char *v = fs ? "1" : "0";
    if (!g_have_file || user_env(MKSM_SET_FULLSCREEN) ||
        (shown(MKSM_SET_FULLSCREEN)[0] != '0') == (fs != 0)) {
        return;
    }
    store_edit(MKSM_SET_FULLSCREEN, v);
    snprintf(g_saved.value[MKSM_SET_FULLSCREEN], sizeof(g_saved.value[0]), "%s", v);
    g_saved.present[MKSM_SET_FULLSCREEN] = 1;
    mksm_settings_save(&g_saved);
    fprintf(stderr, "[MENU] fullscreen=%s saved (Alt+Enter)\n", v);
}

/* Label | widget | note. Returns whether the widget may be edited. */
bool row_begin(int id, const char *label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(170.0f * g_ui_scale);
    bool editable = g_have_file && !user_env(id);
    ImGui::BeginDisabled(!editable);
    return editable;
}

void row_end(int id, bool needs_wide = false)
{
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::TableNextColumn();
    if (user_env(id)) {
        ImGui::TextDisabled("set by %s", mksm_settings_env(id));
    } else if (pending(id)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "pending restart");
    } else if (is_restart(id)) {
        ImGui::TextDisabled("on restart");
    } else if (needs_wide && strcmp(shown(MKSM_SET_ASPECT), "4:3") == 0) {
        ImGui::TextDisabled("needs a wide aspect");
    }
}

void row_combo(int id, const char *label, const char *const *values, const char *const *names,
               int count)
{
    row_begin(id, label);
    const char *cur = shown(id);
    const char *cur_name = cur;
    for (int i = 0; i < count; i++) {
        if (strcmp(cur, values[i]) == 0) {
            cur_name = names[i];
        }
    }
    if (ImGui::BeginCombo("##v", cur_name)) {
        for (int i = 0; i < count; i++) {
            bool sel = strcmp(cur, values[i]) == 0;
            if (ImGui::Selectable(names[i], sel)) {
                set_edit(id, values[i]);
            }
            if (sel) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    row_end(id);
}

void row_check(int id, const char *label, bool needs_wide = false)
{
    row_begin(id, label);
    bool on = shown(id)[0] != '0';
    if (ImGui::Checkbox("##v", &on)) {
        set_edit(id, on ? "1" : "0");
    }
    row_end(id, needs_wide);
}

/* A slider in 5% steps (user, 2026-10-17): the slider runs over the step index, its text is the
 * percentage. Left/right (keys or d-pad) step it while it has the focus, without activating it
 * first as ImGui's own slider nav wants; nothing else sits left or right of it in the table. */
void row_volume(int id, const char *label)
{
    row_begin(id, label);
    int idx = (int)std::lround(atof(shown(id)) * 20.0);
    idx = idx < 0 ? 0 : idx > 20 ? 20 : idx;
    const int before = idx;
    char text[16];
    snprintf(text, sizeof(text), idx ? "%d%%%%" : "Off", idx * 5);
    ImGui::SliderInt("##v", &idx, 0, 20, text, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {    /* disabled = never focused */
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) {
            idx = idx > 0 ? idx - 1 : 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) {
            idx = idx < 20 ? idx + 1 : 20;
        }
    }
    if (idx != before) {
        char v[16];
        snprintf(v, sizeof(v), "%g", idx / 20.0);
        set_edit(id, v);
    }
    row_end(id);
}

void row_text(const char *label, const char *value, const char *note)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", note);
}

void draw_restart_popup(void)
{
    if (!ImGui::BeginPopupModal("Apply and restart?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextUnformatted("The game restarts with the new settings.");
    ImGui::TextUnformatted("Progress since the last save is lost.");
    ImGui::Spacing();
    if (ImGui::Button("Restart")) {
        /* The live changes are in g_edit too. */
        if (mksm_settings_save(&g_edit) != 0) {
            snprintf(g_restart_error, sizeof(g_restart_error), "Could not write the settings file");
        } else {
            g_saved = g_edit;
            fprintf(stderr, "[MENU] apply and restart\n");
            mksm_settings_restart();    /* returns only on failure */
            snprintf(g_restart_error, sizeof(g_restart_error),
                     "Saved, but could not restart: start the game again");
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_quit_popup(void)
{
    if (!ImGui::BeginPopupModal("Quit the game?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextUnformatted("The game closes now.");
    ImGui::TextUnformatted("Progress since the last save is lost.");
    ImGui::Spacing();
    if (ImGui::Button("Quit")) {
        /* The same exit as closing the window: the window thread's loop ends on SDL_QUIT. */
        fprintf(stderr, "[MENU] quit\n");
        SDL_Event ev = {};
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    /* Cancel has the focus: a stray A press doesn't quit. */
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}

void draw_menu(unsigned width, unsigned height)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    if (!g_loaded) {
        load_settings();
    }
    bool open = true;
    if (ImGui::Begin("Settings", &open, flags)) {
        static const char *const scales[] = { "1", "2", "3", "4" };
        static const char *const scale_names[] = { "1x (640x480)", "2x", "3x", "4x" };
        static const char *const aspects[] = { "4:3", "16:10", "16:9", "21:9" };
        static const char *const aspect_names[] = { "4:3 (Xbox)", "16:10", "16:9", "21:9" };
        static const char *const filters[] = { "linear", "nearest" };
        static const char *const filter_names[] = { "Smooth", "Sharp (nearest)" };
        bool wide = strcmp(shown(MKSM_SET_ASPECT), "4:3") != 0;
        bool fullscreen = xbox_FramebufferWindowIsFullscreen() != 0;
        char size[64];
        snprintf(size, sizeof(size), "%ux%u%s", width, height, fullscreen ? ", fullscreen" : "");

        if (!g_have_file) {
            ImGui::TextDisabled("No settings file (RECOMP_SETTINGS=0): shown, not editable");
            ImGui::Spacing();
        }
        if (ImGui::BeginTable("values", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            row_combo(MKSM_SET_SCALE, "Resolution scale", scales, scale_names, 4);
            row_combo(MKSM_SET_ASPECT, "Aspect", aspects, aspect_names, 4);
            ImGui::BeginDisabled(!wide);
            row_check(MKSM_SET_WIDE, "Wide picture", true);
            row_check(MKSM_SET_HUD_EDGES, "HUD at the edges", true);
            row_check(MKSM_SET_MOVIE_ZOOM, "Zoom letterboxed movies", true);
            row_check(MKSM_SET_GAUNTLET_ZOOM, "Zoom the Gauntlet trailer", true);
            ImGui::EndDisabled();
            row_combo(MKSM_SET_FILTER, "Scaling filter", filters, filter_names, 2);
            row_volume(MKSM_SET_VOLUME, "Volume");
            row_check(MKSM_SET_FULLSCREEN, "Fullscreen");
            const char *sd = shown(MKSM_SET_SAVE_DIR);
            row_text("Save folder", *sd ? sd : "default", "edit settings.ini");
            const char *gd = shown(MKSM_SET_GAME_DIR);
            row_text("Game folder", *gd ? gd : "default", "edit settings.ini");
            row_text("Window", size, "Alt+Enter");
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Other changes apply now and are saved.");
        ImGui::Spacing();
        bool fps = g_show_fps.load();
        if (ImGui::Checkbox("Show FPS", &fps)) {
            g_show_fps.store(fps);
        }
        ImGui::Spacing();
        ImGui::Separator();
        bool pend = any_pending();
        ImGui::BeginDisabled(!pend);
        if (ImGui::Button("Apply and restart")) {
            ImGui::OpenPopup("Apply and restart?");
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            for (int i = 0; i < MKSM_SET_COUNT; i++) {
                if (is_restart(i)) {
                    set_edit(i, g_saved.value[i]);
                    g_edit.present[i] = g_saved.present[i];
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit game")) {
            ImGui::OpenPopup("Quit the game?");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F1 / Esc / Back+Start / B");
        if (g_restart_error[0]) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", g_restart_error);
        }
        draw_restart_popup();
        draw_quit_popup();
    }
    ImGui::End();
    save_live();
    /* B closes it too, unless a popup or an active widget takes it first. */
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        open = false;
    }
    if (!open) {
        set_open(false);
    }
}

void draw_fps(void)
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    float pad = 8.0f * g_ui_scale;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##fps", nullptr, flags)) {
        ImGui::Text("%.0f fps", g_fps);
    }
    ImGui::End();
}

unsigned overlay(unsigned width, unsigned height, int new_frame)
{
    if (new_frame) {
        Uint64 now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
        if (!g_fps_t0) {
            g_fps_t0 = now;
        }
        g_fps_frames++;
        if (now - g_fps_t0 >= hz / 2) {
            g_fps = (float)((double)g_fps_frames * (double)hz / (double)(now - g_fps_t0));
            g_fps_t0 = now;
            g_fps_frames = 0;
        }
    }
    sync_fullscreen();
    bool open = g_open.load(), fps = g_show_fps.load();
    if (!open && !fps) {
        if (g_was_open) {
            std::lock_guard<std::mutex> lock(g_events_lock);
            g_events.clear();
            g_was_open = false;
        }
        return 0;
    }
    if (g_init_failed) {
        return 0;
    }
    if (!g_inited) {
        if (!init()) {
            g_init_failed = true;
            return 0;
        }
        g_inited = true;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (open && !g_was_open) {
        io.ClearInputKeys();    /* key-ups that came while closed never reached ImGui */
    }
    g_was_open = open;
    std::vector<SDL_Event> events;
    {
        std::lock_guard<std::mutex> lock(g_events_lock);
        events.swap(g_events);
    }
    for (const SDL_Event &ev : events) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
    }
    update_scale(height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    if (open) {
        feed_pad(io);
    }
    ImGui::NewFrame();
    if (fps) {
        draw_fps();
    }
    if (open) {
        draw_menu(width, height);
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    /* Open: redraw at ~60 Hz. Closed during this frame: once more, without it. */
    return g_open.load() ? 16 : open ? 1 : 0;
}

} // namespace

extern "C" void mksm_menu_install(void)
{
    xbox_FramebufferWindowSetEventHook(event_hook);
    xbox_InputSetFilter(input_filter);
    mksm_gpu_set_overlay(overlay);
}
