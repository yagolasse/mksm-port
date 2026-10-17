/*
 * The in-game settings menu (plan 4.6; B7a1: up, toggles, shows the current values).
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
 * B7a1 shows the settings read-only (from the environment variables they are set with today,
 * README Run section); B7b adds the settings file. While open the game is frozen (B7a2):
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
void xbox_InputKeysClear(void);
/* toolkit src/apu/apu.h */
void mcpx_apu_set_host_paused(int on);
/* toolkit src/input/mkb_input.h */
void mkb_set_overlay(int open);
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

const char *env_or(const char *name, const char *fallback)
{
    const char *e = getenv(name);
    return e && *e ? e : fallback;
}

bool env_on(const char *name, bool fallback)
{
    const char *e = getenv(name);
    return e && *e ? *e != '0' : fallback;
}

void row(const char *label, const char *value, const char *source)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", source);
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
    bool open = true;
    if (ImGui::Begin("Settings", &open, flags)) {
        /* What the options are set to, read where the game reads them (the environment). */
        bool aspect_set = getenv("RECOMP_ASPECT") && *getenv("RECOMP_ASPECT");
        SDL_Window *win = static_cast<SDL_Window *>(xbox_FramebufferWindowSdl());
        bool fullscreen = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP);
        char scale[32], vol[32], size[48];
        snprintf(scale, sizeof(scale), "%sx", env_or("RECOMP_PB_GPU_SCALE", "1"));
        const char *v = getenv("RECOMP_APU_VOLUME");
        if (v && *v) {
            snprintf(vol, sizeof(vol), "%.0f%%", atof(v) * 100.0);
        } else {
            snprintf(vol, sizeof(vol), "100%%");
        }
        snprintf(size, sizeof(size), "%ux%u", width, height);

        ImGui::TextDisabled("Read-only for now: set with the environment variables shown");
        ImGui::Spacing();
        if (ImGui::BeginTable("values", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            row("Resolution scale", scale, "RECOMP_PB_GPU_SCALE *");
            row("Aspect", env_or("RECOMP_ASPECT", "4:3"), "RECOMP_ASPECT *");
            row("Wide buffer", aspect_set && env_on("RECOMP_ASPECT_WIDE", false) ? "on" : "off",
                "RECOMP_ASPECT_WIDE *");
            row("HUD at edges", aspect_set && env_on("RECOMP_HUD_EDGES", true) ? "on" : "off",
                "RECOMP_HUD_EDGES");
            row("Movie zoom", aspect_set && env_on("RECOMP_MOVIE_ZOOM", true) ? "on" : "off",
                "RECOMP_MOVIE_ZOOM");
            row("Gauntlet zoom",
                aspect_set && env_on("RECOMP_MOVIE_ZOOM_GAUNTLET", false) ? "on" : "off",
                "RECOMP_MOVIE_ZOOM_GAUNTLET");
            row("Present filter", env_or("RECOMP_PB_GPU_FILTER", "linear"), "RECOMP_PB_GPU_FILTER");
            row("Volume", vol, "RECOMP_APU_VOLUME");
            row("Fullscreen", fullscreen ? "on" : "off", "Alt+Enter");
            row("Window", size, "");
            ImGui::EndTable();
        }
        ImGui::TextDisabled("* applies on restart");
        ImGui::Spacing();
        bool fps = g_show_fps.load();
        if (ImGui::Checkbox("Show FPS", &fps)) {
            g_show_fps.store(fps);
        }
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close")) {
            open = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F1 / Esc / Back+Start / B");
    }
    ImGui::End();
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
