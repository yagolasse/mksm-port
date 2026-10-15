/* Stand-in for xemu's generated xemu-config.h: only the renderer selection.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL,
    CONFIG_DISPLAY_RENDERER_OPENGL,
    CONFIG_DISPLAY_RENDERER_VULKAN,
    CONFIG_DISPLAY_RENDERER__COUNT
} CONFIG_DISPLAY_RENDERER;

struct config {
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
    } display;
};
