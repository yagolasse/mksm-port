/*
 * gloffscreen_wgl.c beyond xemu's gloffscreen.h: a context on a window someone else owns.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "gloffscreen.h"

/* An OpenGL 4.x core context on `hwnd`'s DC (an HWND; the window class should be CS_OWNDC),
 * sharing objects with the context current on the calling thread, swap interval 0. Sets the
 * window's pixel format if it has none. Leaves the caller's context current. NULL on failure
 * (logged); needs a context created by glo_context_create first. The window is not destroyed
 * with the context. */
GloContext *glo_context_create_for_window(void *hwnd);

/* SwapBuffers on the context's DC. */
void glo_swap_buffers(GloContext *context);
