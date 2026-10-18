/* Forced include for xemu's pgraph/gl/texture.c only (CMakeLists.txt): xemu's file stays
 * unmodified, its lru_visit_active calls come here.
 *
 * qemu/lru.h's lru_visit_active walks every hash bin (LRU_NUM_BINS = 65536 list heads, ~1 MB)
 * to reach the cache's at most 512 used nodes. texture.c runs it once per texture range that
 * reads dirty (pgraph_gl_mark_textures_possibly_dirty), ~2k times/s in a level, which made it
 * the method thread's top symbol (plan 2.7c1). Every node, used or free, is also on the
 * cache's `global` list, so walking that one and skipping the free nodes visits the same
 * nodes. Only the order differs, and texture.c's visitor only sets a flag on each. */
#ifndef MKSM_LRU_VISIT_FAST_H
#define MKSM_LRU_VISIT_FAST_H

#include <stdbool.h>
#include <stddef.h>
#include "qemu/osdep.h"   /* first, as in xemu's files: the shim's typeof (clang-cl) */
#include "qemu/lru.h"

static inline void mksm_lru_visit_active(Lru *lru, LruNodeVisitorFunc visitor_func,
                                         void *opaque)
{
    LruNode *iter, *iter_next;

    QTAILQ_FOREACH_SAFE(iter, &lru->global, next_global, iter_next) {
        if (lru_is_node_in_use(lru, iter)) {
            visitor_func(lru, iter, opaque);
        }
    }
}

#define lru_visit_active mksm_lru_visit_active

#endif
