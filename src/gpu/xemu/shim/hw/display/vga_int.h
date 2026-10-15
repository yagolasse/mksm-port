/* Stand-in for QEMU's hw/display/vga_int.h: the VGACommonState fields xemu's NV2A uses
 * (CRTC registers, and the resolution/params hooks nv2a.c fills in).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "hw/hw.h"

typedef struct VGADisplayParams {
    uint32_t line_offset;
    uint32_t start;
    uint32_t line_compare;
    uint8_t hpel;
    bool hpel_split;
} VGADisplayParams;

typedef struct VGACommonState {
    uint8_t sr[256];
    uint8_t cr[256];
    void (*get_resolution)(struct VGACommonState *s, int *pwidth, int *pheight);
    void (*get_params)(struct VGACommonState *s, VGADisplayParams *params);
} VGACommonState;
