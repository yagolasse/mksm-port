/* Stand-in for <glib.h>: the parts xemu's PGRAPH, GL renderer and qemu/mstring.h use.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#ifdef _WIN32
#define g_strdup(s) _strdup(s)
#else
#define g_strdup(s) strdup(s)
#endif
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

#define g_malloc_n(n, size) malloc((size_t)(n) * (size_t)(size))
#define G_GNUC_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define g_assert_not_reached() \
    do { fprintf(stderr, "%s:%d: should not be reached\n", __FILE__, __LINE__); abort(); } while (0)

/* g_autoptr(T) calls T's cleanup function, declared with G_DEFINE_AUTOPTR_CLEANUP_FUNC. */
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(T, fn) \
    static inline void glib_autoptr_cleanup_##T(T **p) { if (*p) fn(*p); }
#define g_autoptr(T) __attribute__((cleanup(glib_autoptr_cleanup_##T))) T *

/* GString: a growable NUL-terminated buffer. */
typedef struct GString {
    gchar *str;
    size_t len;
    size_t allocated_len;
} GString;

static inline void g_string_maybe_grow(GString *s, size_t extra)
{
    if (s->len + extra + 1 > s->allocated_len) {
        size_t n = s->allocated_len ? s->allocated_len : 64;
        while (n < s->len + extra + 1) {
            n *= 2;
        }
        s->str = (gchar *)realloc(s->str, n);
        s->allocated_len = n;
    }
}
static inline GString *g_string_new_take(gchar *init)
{
    GString *s = (GString *)calloc(1, sizeof(*s));
    if (init) {
        s->str = init;
        s->len = strlen(init);
        s->allocated_len = s->len + 1;
    } else {
        g_string_maybe_grow(s, 0);
        s->str[0] = 0;
    }
    return s;
}
static inline GString *g_string_new(const gchar *init)
{
    return g_string_new_take(g_strdup(init ? init : ""));
}
static inline GString *g_string_append_len(GString *s, const gchar *val, size_t len)
{
    g_string_maybe_grow(s, len);
    memcpy(s->str + s->len, val, len);
    s->len += len;
    s->str[s->len] = 0;
    return s;
}
static inline GString *g_string_append(GString *s, const gchar *val)
{
    return g_string_append_len(s, val, strlen(val));
}
static inline void g_string_append_vprintf(GString *s, const gchar *fmt, va_list ap)
{
    gchar *t = g_strdup_vprintf(fmt, ap);
    g_string_append(s, t);
    free(t);
}
static inline gchar *g_string_free(GString *s, gboolean free_segment)
{
    gchar *str = s->str;
    if (free_segment) {
        free(str);
        str = NULL;
    }
    free(s);
    return str;
}
