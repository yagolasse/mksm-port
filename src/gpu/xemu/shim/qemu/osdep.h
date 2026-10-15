/*
 * Minimal stand-in for QEMU's qemu/osdep.h (+ the glib bits it pulls in), enough to build
 * xemu's PGRAPH files unmodified outside QEMU. Built with clang-cl only (GNU extensions).
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

/* glib */
typedef char gchar;
typedef int gboolean;
typedef void *gpointer;
typedef unsigned int guint;
#define g_malloc(n) malloc(n)
#define g_malloc0(n) calloc(1, (n))
#define g_realloc(p, n) realloc((p), (n))
#define g_free(p) free(p)
#define g_new(type, n) ((type *)malloc(sizeof(type) * (n)))
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_strdup(s) _strdup(s)
static inline void g_autoptr_cleanup_free(void *pp) { free(*(void **)pp); }
#define g_autofree __attribute__((cleanup(g_autoptr_cleanup_free)))
static inline gchar *g_strdup_vprintf(const gchar *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    gchar *s = (gchar *)malloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}
static inline gchar *g_strdup_printf(const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return s;
}

/* The big QEMU lock: there is no QEMU here, PGRAPH runs on the pushbuffer thread. */
static inline void bql_lock(void) {}
static inline void bql_unlock(void) {}
