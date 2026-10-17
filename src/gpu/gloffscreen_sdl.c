/*
 * gloffscreen on SDL2: the context layer xemu's GL renderer uses (thirdparty/gloffscreen/
 * gloffscreen.h), as xemu's own sdl.c (SDL3) does it: each context gets a hidden
 * SDL_WINDOW_OPENGL window and an OpenGL 4.x core context that shares objects with whatever
 * context is current on the calling thread (SDL_GL_SHARE_WITH_CURRENT_CONTEXT). GL entry points
 * are loaded with glad through SDL_GL_GetProcAddress when the first context is current.
 *
 * SDL's video subsystem is brought up here (the first glo_context_create, at mksm_gpu_init on
 * the main thread, before the game window's thread opens it); SDL counts the init, so the
 * window's own SDL_Init/SDL_QuitSubSystem don't take it down.
 *
 * Also (gloffscreen_window.h): a context on the SDL game window for the present, and its swap.
 *
 * SPDX-License-Identifier: MIT
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <glad/gl.h>
#include "gloffscreen_window.h"

struct _GloContext {
    SDL_Window *window;
    SDL_GLContext gl_context;
    int own_window;     /* 0: a context on someone else's window (glo_context_create_for_window) */
};

static int s_gl_loaded;

static void fatal(const char *what)
{
    fprintf(stderr, "gloffscreen: %s failed (%s)\n", what, SDL_GetError());
    exit(1);
}

/* SDL video up once, with the attributes every context and the game window's pixel format
 * use: RGBA8, depth 24, stencil 8, double-buffered (what xemu asks for), core profile, shared
 * with the current context. The video init enumerates the monitors: per-monitor DPI aware
 * there, as the game window's thread is, or SDL keeps scaled display sizes for fullscreen. */
static void init_sdl(void)
{
    static int done;
    if (done) {
        return;
    }
    typedef DPI_AWARENESS_CONTEXT (WINAPI *set_thread_dpi_fn)(DPI_AWARENESS_CONTEXT);
    set_thread_dpi_fn set_dpi = (set_thread_dpi_fn)(void *)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    DPI_AWARENESS_CONTEXT prev_dpi =
        set_dpi ? set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : NULL;
    SDL_SetMainReady();
    int rc = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (set_dpi && prev_dpi) {
        set_dpi(prev_dpi);
    }
    if (rc != 0) {
        fatal("SDL_InitSubSystem(SDL_INIT_VIDEO)");
    }
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    done = 1;
}

/* An OpenGL 4.x core context on `window`, sharing objects with the current one, left current;
 * NULL if the driver has none. xemu asks for 4.0 core but calls later entry points
 * (glProgramUniform*, 4.1) that libepoxy resolves whatever the version. glad only loads up to
 * the version the context reports, and NVIDIA reports exactly what was asked, so ask for the
 * newest core version the driver has (core profiles are backward compatible) and go down to
 * 4.0. */
static SDL_GLContext create_core_context(SDL_Window *window)
{
    static const int minors[] = { 6, 5, 4, 3, 2, 1, 0 };
    SDL_GLContext ctx = NULL;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    for (size_t i = 0; i < sizeof(minors) / sizeof(minors[0]) && !ctx; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minors[i]);
        ctx = SDL_GL_CreateContext(window);
    }
    return ctx;
}

GloContext *glo_context_create(void)
{
    init_sdl();

    GloContext *context = (GloContext *)calloc(1, sizeof(*context));
    assert(context != NULL);
    context->own_window = 1;
    context->window = SDL_CreateWindow("gloffscreen", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!context->window) {
        fatal("SDL_CreateWindow (hidden, OpenGL)");
    }
    context->gl_context = create_core_context(context->window);
    if (!context->gl_context) {
        fatal("SDL_GL_CreateContext (OpenGL 4.0+ core)");
    }

    glo_set_current(context);
    if (!s_gl_loaded) {
        if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
            fprintf(stderr, "gloffscreen: loading the OpenGL entry points failed\n");
            exit(1);
        }
        s_gl_loaded = 1;
    }
    return context;
}

GloContext *glo_context_create_for_window(void *window)
{
    if (!s_gl_loaded || !window) {
        return NULL;
    }
    SDL_Window *prev_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext prev_ctx = SDL_GL_GetCurrentContext();

    GloContext *context = (GloContext *)calloc(1, sizeof(*context));
    if (!context) {
        return NULL;
    }
    context->window = (SDL_Window *)window;
    context->gl_context = create_core_context(context->window);
    if (!context->gl_context) {
        fprintf(stderr, "gloffscreen: context on the window failed (%s)\n", SDL_GetError());
        free(context);
        /* A failed attempt leaves SDL's temporary context released behind its back: SDL
         * still thinks the caller's is current, so go through NULL to really restore it. */
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_MakeCurrent(prev_window, prev_ctx);
        return NULL;
    }
    /* Presents never wait for the monitor: the flip pacing does the waiting. */
    SDL_GL_SetSwapInterval(0);
    SDL_GL_MakeCurrent(prev_window, prev_ctx);
    return context;
}

void glo_swap_buffers(GloContext *context)
{
    SDL_GL_SwapWindow(context->window);
}

void glo_set_current(GloContext *context)
{
    int rc = context ? SDL_GL_MakeCurrent(context->window, context->gl_context)
                     : SDL_GL_MakeCurrent(NULL, NULL);
    if (rc != 0) {
        fatal("SDL_GL_MakeCurrent");
    }
}

void glo_context_destroy(GloContext *context)
{
    if (!context) {
        return;
    }
    glo_set_current(NULL);
    SDL_GL_DeleteContext(context->gl_context);
    if (context->own_window) {
        SDL_DestroyWindow(context->window);
    }
    free(context);
}
