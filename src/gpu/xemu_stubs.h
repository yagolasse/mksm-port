/*
 * Port-side glue for the vendored xemu PGRAPH files (see xemu_stubs.c).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How often the stubbed sync hooks were called (plan 2.H.3b turns them into real ones). */
typedef struct XemuStubCounts {
    unsigned long pfifo_kick;
    unsigned long update_irq;
    unsigned long dirty_epochs;     /* DMA_PUT changes seen by the dirty query (per batch) */
    unsigned long tex_pages_hashed; /* texture pages re-hashed in a new epoch */
    unsigned long tex_pages_changed;/* ... of which read dirty (first check or new content) */
} XemuStubCounts;

extern XemuStubCounts xemu_stub_counts;

/* The title's DMA_PUT register: the dirty query reads a page as dirty at most once per kick;
 * with tex_hash, a texture page only when its content changed since its last check. */
void xemu_stubs_set_dma_put(const volatile uint32_t *dma_put, bool tex_hash);

/* A batch of methods (one pushbuffer walk) begins / ends: the dirty query's epoch is read once
 * at the begin and held to the end (xemu_stubs.c). Method thread only. */
void xemu_stubs_dirty_batch(bool begin);
