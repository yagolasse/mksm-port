/*
 * Port-side glue for the vendored xemu PGRAPH files (see xemu_stubs.c).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

/* How often the stubbed sync hooks were called (plan 2.H.3b turns them into real ones). */
typedef struct XemuStubCounts {
    unsigned long pfifo_kick;
    unsigned long update_irq;
} XemuStubCounts;

extern XemuStubCounts xemu_stub_counts;
