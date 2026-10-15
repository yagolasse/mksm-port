/* Stand-in for QEMU's qemu/main-loop.h + qemu/timer.h (the parts PGRAPH uses).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

typedef struct QEMUTimer { int unused; } QEMUTimer;

typedef enum { QEMU_CLOCK_REALTIME = 0, QEMU_CLOCK_VIRTUAL = 1 } QEMUClockType;

int64_t qemu_clock_get_ns(QEMUClockType type);
static inline int64_t qemu_clock_get_us(QEMUClockType type)
{
    return qemu_clock_get_ns(type) / 1000;
}
static inline int64_t qemu_clock_get_ms(QEMUClockType type)
{
    return qemu_clock_get_ns(type) / 1000000;
}
