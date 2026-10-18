/*
 * Minimal stand-in for QEMU's qemu/osdep.h (+ the glib bits it pulls in), enough to build
 * xemu's PGRAPH files unmodified outside QEMU. Built with clang-cl on Windows, gcc/clang on POSIX
 * (GNU extensions).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#pragma once

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* qemu/typedefs.h */
typedef struct Error Error;
typedef struct MemoryRegion MemoryRegion;
typedef struct PCIBus PCIBus;
typedef struct PCIDevice PCIDevice;
typedef struct CPUState CPUState;

typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define HWADDR_PRId PRId64

/* qemu/compiler.h, qemu/osdep.h */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define QEMU_ALIGN_DOWN(n, m) ((n) / (m) * (m))
#define QEMU_ALIGN_UP(n, m) QEMU_ALIGN_DOWN((n) + (m) - 1, (m))
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define tostring(s) #s
#define stringify(s) tostring(s)
#define QEMU_BUILD_BUG_MSG(x, msg) _Static_assert(!(x), msg)
#define QEMU_BUILD_BUG_ON(x) QEMU_BUILD_BUG_MSG(x, "not expecting: " #x)
/* QEMU builds as gnu11; the vendored files spell __typeof__ as typeof. */
#ifndef typeof
#define typeof __typeof__
#endif

/* qemu/host-utils.h */
static inline int ctz32(uint32_t val) { return val ? __builtin_ctz(val) : 32; }
static inline int clz32(uint32_t val) { return val ? __builtin_clz(val) : 32; }

/* qemu/bswap.h (the host is little-endian) */
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* qemu/atomic.h */
#define qatomic_read(ptr) __atomic_load_n(ptr, __ATOMIC_RELAXED)
#define qatomic_set(ptr, i) __atomic_store_n(ptr, i, __ATOMIC_RELAXED)
#define qatomic_inc(ptr) ((void)__atomic_fetch_add(ptr, 1, __ATOMIC_SEQ_CST))
#define qatomic_dec(ptr) ((void)__atomic_fetch_sub(ptr, 1, __ATOMIC_SEQ_CST))
#define qatomic_xchg(ptr, i) __atomic_exchange_n(ptr, i, __ATOMIC_SEQ_CST)
#define qatomic_cmpxchg(ptr, old, new) ({                                  \
    __typeof__(*(ptr)) _old = (old);                                       \
    __atomic_compare_exchange_n(ptr, &_old, new, false, __ATOMIC_SEQ_CST,   \
                                __ATOMIC_SEQ_CST);                         \
    _old; })

#include "glib.h"

/* qemu/osdep.h file helpers (the GL shader cache) */
#include "host_win32.h"
#define qemu_fopen(path, mode) fopen((path), (mode))
#define qemu_mkdir(path) mksm_mkdir(path)
#define qemu_unlink(path) _unlink(path)

/* The big QEMU lock: there is no QEMU here, PGRAPH runs on the pushbuffer thread. */
static inline void bql_lock(void) {}
static inline void bql_unlock(void) {}
