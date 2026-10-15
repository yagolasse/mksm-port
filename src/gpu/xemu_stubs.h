/*
 * Port-side glue for the vendored xemu PGRAPH files (see xemu_stubs.c).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

/* How often the stubbed sync hooks were called (plan 2.H.3b turns them into real ones). */
typedef struct XemuStubCounts {
    unsigned long pfifo_kick;
    unsigned long update_irq;
    unsigned long dirty_epochs;     /* DMA_PUT changes seen by the dirty query */
} XemuStubCounts;

extern XemuStubCounts xemu_stub_counts;

/* The title's DMA_PUT register: the dirty query reads a page as dirty once per kick. */
void xemu_stubs_set_dma_put(const volatile uint32_t *dma_put);
