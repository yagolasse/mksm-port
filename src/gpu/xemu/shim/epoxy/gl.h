/* Stand-in for libepoxy's <epoxy/gl.h>: the GL API comes from glad (src/gpu/glad, GL 4.0 core
 * + the extensions xemu checks for), loaded by gloffscreen_wgl.c once the first context is up.
 * SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <glad/gl.h>

/* libepoxy: whether the current context exposes an extension. */
bool epoxy_has_gl_extension(const char *ext_name);

/* xemu asserts glGetError() == GL_NO_ERROR per draw (asserts stay on in this build). With
 * NVIDIA's threaded driver a glGetError may wait for the driver's worker thread (a guess; not
 * measured on its own). So it reads GL_NO_ERROR unless
 * RECOMP_PB_GPU_GLCHECK=1 (xemu_stubs.c); code that needs the real error calls
 * glad_glGetError. */
extern int mksm_gl_check_errors;
static inline GLenum mksm_gl_get_error(void)
{
    return mksm_gl_check_errors ? glad_glGetError() : GL_NO_ERROR;
}
#undef glGetError
#define glGetError mksm_gl_get_error
