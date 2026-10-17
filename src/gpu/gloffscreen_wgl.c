/*
 * gloffscreen on WGL: the context layer xemu's GL renderer uses (thirdparty/gloffscreen/
 * gloffscreen.h), in place of xemu's SDL version. Each context gets its own hidden window
 * (a pixel format can be set once per window) and an OpenGL 4.x core context that shares
 * objects with whatever context is current on the calling thread, as xemu's
 * SDL_GL_SHARE_WITH_CURRENT_CONTEXT does.
 *
 * The WGL extension entry points (wglCreateContextAttribsARB, wglChoosePixelFormatARB) need a
 * current context to be queried, so the first call creates a throwaway legacy context once.
 * GL entry points are loaded with glad when the first real context is current.
 *
 * Also (gloffscreen_window.h, not part of xemu's interface): a context on an existing window, the
 * game window the method thread presents into, and its SwapBuffers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glad/wgl.h>
#include "gloffscreen_window.h"

struct _GloContext {
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;
    int own_window;     /* 0: a context on someone else's window (glo_context_create_for_window) */
};

static const wchar_t kClassName[] = L"mksm_gloffscreen";
static int s_wgl_loaded, s_gl_loaded;

static int set_pixel_format(HDC hdc);
static HGLRC create_core_context(HDC hdc);

static void fatal(const char *what)
{
    fprintf(stderr, "gloffscreen: %s failed (error %lu)\n", what, GetLastError());
    exit(1);
}

static HWND create_hidden_window(void)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc = { 0 };
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = kClassName;
        cls = RegisterClassW(&wc);
        if (!cls) {
            fatal("RegisterClass");
        }
    }
    HWND hwnd = CreateWindowExW(0, kClassName, L"gloffscreen", WS_OVERLAPPEDWINDOW, 0, 0, 640,
                                480, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        fatal("CreateWindow");
    }
    return hwnd;
}

/* The descriptor xemu asks SDL for: RGBA8, depth 24, stencil 8, double-buffered. */
static PIXELFORMATDESCRIPTOR legacy_pfd(void)
{
    PIXELFORMATDESCRIPTOR pfd = { 0 };
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    return pfd;
}

/* Loads the WGL extensions through a temporary legacy context, restoring the caller's. */
static void load_wgl(void)
{
    HGLRC prev_rc = wglGetCurrentContext();
    HDC prev_dc = wglGetCurrentDC();

    HWND hwnd = create_hidden_window();
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = legacy_pfd();
    int fmt = ChoosePixelFormat(hdc, &pfd);
    if (!fmt || !SetPixelFormat(hdc, fmt, &pfd)) {
        fatal("SetPixelFormat (bootstrap)");
    }
    HGLRC rc = wglCreateContext(hdc);
    if (!rc || !wglMakeCurrent(hdc, rc)) {
        fatal("wglCreateContext (bootstrap)");
    }
    if (!gladLoaderLoadWGL(hdc) || !GLAD_WGL_ARB_create_context ||
        !GLAD_WGL_ARB_create_context_profile || !GLAD_WGL_ARB_pixel_format) {
        fprintf(stderr, "gloffscreen: WGL_ARB_create_context/_profile/pixel_format missing\n");
        exit(1);
    }
    wglMakeCurrent(prev_dc, prev_rc);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    s_wgl_loaded = 1;
}

GloContext *glo_context_create(void)
{
    if (!s_wgl_loaded) {
        load_wgl();
    }

    GloContext *context = (GloContext *)calloc(1, sizeof(*context));
    assert(context != NULL);
    context->hwnd = create_hidden_window();
    context->hdc = GetDC(context->hwnd);
    context->own_window = 1;

    if (!set_pixel_format(context->hdc)) {
        fatal("wglChoosePixelFormatARB / SetPixelFormat");
    }
    context->hglrc = create_core_context(context->hdc);
    if (!context->hglrc) {
        fatal("wglCreateContextAttribsARB (OpenGL 4.0+ core)");
    }

    glo_set_current(context);
    if (!s_gl_loaded) {
        if (!gladLoaderLoadGL()) {
            fprintf(stderr, "gloffscreen: loading the OpenGL entry points failed\n");
            exit(1);
        }
        s_gl_loaded = 1;
    }
    return context;
}

GloContext *glo_context_create_for_window(void *hwnd)
{
    if (!s_wgl_loaded || !s_gl_loaded || !hwnd) {
        return NULL;
    }
    HGLRC prev_rc = wglGetCurrentContext();
    HDC prev_dc = wglGetCurrentDC();

    GloContext *context = (GloContext *)calloc(1, sizeof(*context));
    if (!context) {
        return NULL;
    }
    context->hwnd = (HWND)hwnd;
    context->hdc = GetDC(context->hwnd);
    /* A pixel format can be set once per window; one already there (a second context on the
     * same window) is kept. */
    if (!context->hdc || (!GetPixelFormat(context->hdc) && !set_pixel_format(context->hdc))) {
        fprintf(stderr, "gloffscreen: pixel format on the window failed (error %lu)\n",
                GetLastError());
        goto fail;
    }
    context->hglrc = create_core_context(context->hdc);
    if (!context->hglrc || !wglMakeCurrent(context->hdc, context->hglrc)) {
        fprintf(stderr, "gloffscreen: context on the window failed (error %lu)\n",
                GetLastError());
        goto fail;
    }
    /* Presents never wait for the monitor: the flip pacing does the waiting. */
    if (GLAD_WGL_EXT_swap_control) {
        wglSwapIntervalEXT(0);
    }
    wglMakeCurrent(prev_dc, prev_rc);
    return context;

fail:
    if (context->hglrc) {
        wglDeleteContext(context->hglrc);
    }
    if (context->hdc) {
        ReleaseDC(context->hwnd, context->hdc);
    }
    free(context);
    wglMakeCurrent(prev_dc, prev_rc);
    return NULL;
}

void glo_swap_buffers(GloContext *context)
{
    SwapBuffers(context->hdc);
}

/* RGBA8, depth 24, stencil 8, double-buffered: the descriptor xemu asks SDL for. */
static int set_pixel_format(HDC hdc)
{
    const int fmt_attribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
        WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
        WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_RED_BITS_ARB, 8,
        WGL_GREEN_BITS_ARB, 8,
        WGL_BLUE_BITS_ARB, 8,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_STENCIL_BITS_ARB, 8,
        0,
    };
    int fmt;
    UINT nfmt = 0;
    PIXELFORMATDESCRIPTOR pfd;
    return wglChoosePixelFormatARB(hdc, fmt_attribs, NULL, 1, &fmt, &nfmt) && nfmt &&
           DescribePixelFormat(hdc, fmt, sizeof(pfd), &pfd) && SetPixelFormat(hdc, fmt, &pfd);
}

/* An OpenGL 4.x core context on `hdc` sharing objects with the current one, or NULL.
 * xemu asks SDL for 4.0 core but calls later entry points (glProgramUniform*, 4.1) that
 * libepoxy resolves whatever the version. glad only loads up to the version the context
 * reports, and NVIDIA reports exactly what was asked, so ask for the newest core version
 * the driver has (core profiles are backward compatible) and go down to 4.0. */
static HGLRC create_core_context(HDC hdc)
{
    static const int minors[] = { 6, 5, 4, 3, 2, 1, 0 };
    HGLRC rc = NULL;
    for (size_t i = 0; i < sizeof(minors) / sizeof(minors[0]) && !rc; i++) {
        const int ctx_attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, minors[i],
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0,
        };
        rc = wglCreateContextAttribsARB(hdc, wglGetCurrentContext(), ctx_attribs);
    }
    return rc;
}

void glo_set_current(GloContext *context)
{
    BOOL ok = context ? wglMakeCurrent(context->hdc, context->hglrc)
                      : wglMakeCurrent(NULL, NULL);
    if (!ok) {
        fatal("wglMakeCurrent");
    }
}

void glo_context_destroy(GloContext *context)
{
    if (!context) {
        return;
    }
    glo_set_current(NULL);
    wglDeleteContext(context->hglrc);
    ReleaseDC(context->hwnd, context->hdc);
    if (context->own_window) {
        DestroyWindow(context->hwnd);
    }
    free(context);
}
