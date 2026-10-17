/*
 * The context layer beyond xemu's gloffscreen.h: a context on a window someone else owns, for
 * the present. Two implementations: gloffscreen_sdl.c (SDL window, the default) and
 * gloffscreen_wgl.c (the GDI window's HWND, MKSM_SDL_WINDOW=OFF).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "gloffscreen.h"

/* An OpenGL 4.x core context on `window` (an SDL_Window * made with SDL_WINDOW_OPENGL, or with
 * WGL an HWND whose class should be CS_OWNDC), sharing objects with the context current on the
 * calling thread, swap interval 0. Leaves the caller's context current. NULL on failure
 * (logged); needs a context created by glo_context_create first. The window is not destroyed
 * with the context. */
GloContext *glo_context_create_for_window(void *window);

/* Shows the back buffer of the context's window (the context must be current). */
void glo_swap_buffers(GloContext *context);
