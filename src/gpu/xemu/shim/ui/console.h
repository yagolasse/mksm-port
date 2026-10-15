/* Stand-in for QEMU's ui/console.h. SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

typedef struct GraphicHwOps { void (*gfx_update)(void *opaque); } GraphicHwOps;
