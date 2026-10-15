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

/* Dirty-memory clients (xemu's list). There is no dirty bitmap here: see xemu_stubs.c. */
#define DIRTY_MEMORY_VGA       0
#define DIRTY_MEMORY_CODE      1
#define DIRTY_MEMORY_MIGRATION 2
#define DIRTY_MEMORY_NV2A      3
#define DIRTY_MEMORY_NV2A_TEX  4
#define DIRTY_MEMORY_NUM       5

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                                        unsigned client);
void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                                    unsigned client);

/* xemu's CPU memory-access callbacks (TCG only): fire when the CPU touches a range. */
typedef void (*MemAccessCallbackFunc)(void *opaque, MemoryRegion *mr, hwaddr offset,
                                      hwaddr len, bool write);
typedef struct MemAccessCallback MemAccessCallback;
MemAccessCallback *mem_access_callback_insert(CPUState *cpu, MemoryRegion *mr, hwaddr offset,
                                              hwaddr len, MemAccessCallbackFunc func,
                                              void *opaque);
void mem_access_callback_remove_by_ref(CPUState *cpu, MemAccessCallback *cb);

/* QEMU's accelerator query (system/tcg.h). */
bool tcg_enabled(void);
