/* Stand-in for QEMU's hw/display/vga_int.h: a complete VGACommonState, contents unused.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "hw/hw.h"

typedef struct VGACommonState { uint8_t sr[256]; uint8_t cr[256]; } VGACommonState;
