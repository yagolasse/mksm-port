/* Stand-in for QEMU's exec/memory.h: complete types only, no address spaces.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

struct MemoryRegion {
    uint64_t size;
    uint8_t *ptr;
};

typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t data, unsigned size);
} MemoryRegionOps;

uint64_t memory_region_size(MemoryRegion *mr);
