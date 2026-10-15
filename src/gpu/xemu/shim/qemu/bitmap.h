/* Stand-in for QEMU's qemu/bitmap.h + bitops.h (the parts PGRAPH uses).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "qemu/osdep.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)
#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline int test_bit(long nr, const unsigned long *addr)
{
    return 1UL & (addr[BIT_WORD(nr)] >> (nr & (BITS_PER_LONG - 1)));
}

static inline void bitmap_set(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++) {
        map[BIT_WORD(i)] |= 1UL << (i & (BITS_PER_LONG - 1));
    }
}

static inline void bitmap_clear(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++) {
        map[BIT_WORD(i)] &= ~(1UL << (i & (BITS_PER_LONG - 1)));
    }
}

static inline void bitmap_zero(unsigned long *dst, long nbits)
{
    memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}
