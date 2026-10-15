/* Stand-in for libepoxy's <epoxy/gl.h>: the GL API comes from glad (src/gpu/glad, GL 4.0 core
 * + the extensions xemu checks for), loaded by gloffscreen_wgl.c once the first context is up.
 * SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <glad/gl.h>

/* libepoxy: whether the current context exposes an extension. */
bool epoxy_has_gl_extension(const char *ext_name);
