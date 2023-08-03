// SPDX-License-Identifier: MIT
/*
 * Refcounting - Defines functions and macros for reference keeping.
 *
 * Copyright (c) 2023, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_UTIL_REFCOUNTING_H
#define _FLUTTERPI_SRC_UTIL_REFCOUNTING_H

#include <stdatomic.h>
#include <stdbool.h>

#include "macros.h"

typedef _Atomic(int) refcount_t;

static inline int refcount_inc_n(refcount_t *refcount, int n) {
    return atomic_fetch_add_explicit(refcount, n, memory_order_relaxed);
}

/// Increments the reference count and returns the previous value.
static inline int refcount_inc(refcount_t *refcount) {
    return refcount_inc_n(refcount, 1);
}

/// Decrement the reference count, return true if the refcount afterwards
/// is still non-zero.
static inline bool refcount_dec(refcount_t *refcount) {
    return atomic_fetch_sub_explicit(refcount, 1, memory_order_acq_rel) != 1;
}

/// Returns true if the reference count is one.
/// If this is the case you that means this thread has exclusive access
/// to the object.
static inline bool refcount_is_one(refcount_t *refcount) {
    return atomic_load_explicit(refcount, memory_order_acquire) == 1;
}

/// Returns true if the reference count is zero. Should never be true
/// in practice because that'd only be the case for a destroyed object.
/// So this is only really useful for debugging.
static inline bool refcount_is_zero(refcount_t *refcount) {
    return atomic_load_explicit(refcount, memory_order_acquire) == 0;
}

/// Get the current reference count, without any memory ordering restrictions.
/// Not strictly correct, should only be used for debugging.
static inline int refcount_get_for_debug(refcount_t *refcount) {
    return atomic_load_explicit(refcount, memory_order_relaxed);
}

#define REFCOUNT_INIT_0 (0)
#define REFCOUNT_INIT_1 (1)
#define REFCOUNT_INIT_N(n) (n)

#define DECLARE_REF_OPS(obj_name)                                                   \
    UNUSED struct obj_name *obj_name##_ref(struct obj_name *obj);                   \
    UNUSED void obj_name##_unref(struct obj_name *obj);                             \
    UNUSED void obj_name##_unrefp(struct obj_name **obj);                           \
    UNUSED void obj_name##_swap_ptrs(struct obj_name **objp, struct obj_name *obj); \
    UNUSED void obj_name##_unref_void(void *obj);

#define DEFINE_REF_OPS(obj_name, refcount_member_name)                               \
    UNUSED struct obj_name *obj_name##_ref(struct obj_name *obj) {                   \
        refcount_inc(&obj->refcount_member_name);                                    \
        return obj;                                                                  \
    }                                                                                \
    UNUSED void obj_name##_unref(struct obj_name *obj) {                             \
        if (refcount_dec(&obj->refcount_member_name) == false) {                     \
            obj_name##_destroy(obj);                                                 \
        }                                                                            \
    }                                                                                \
    UNUSED void obj_name##_unrefp(struct obj_name **obj) {                           \
        obj_name##_unref(*obj);                                                      \
        *obj = NULL;                                                                 \
    }                                                                                \
    UNUSED void obj_name##_swap_ptrs(struct obj_name **objp, struct obj_name *obj) { \
        if (obj != NULL) {                                                           \
            obj_name##_ref(obj);                                                     \
        }                                                                            \
        if (*objp != NULL) {                                                         \
            obj_name##_unrefp(objp);                                                 \
        }                                                                            \
        *objp = obj;                                                                 \
    }                                                                                \
    UNUSED void obj_name##_unref_void(void *obj) { obj_name##_unref((struct obj_name *) obj); }

#define DEFINE_STATIC_REF_OPS(obj_name, refcount_member_name)                               \
    UNUSED static struct obj_name *obj_name##_ref(struct obj_name *obj) {                   \
        refcount_inc(&obj->refcount_member_name);                                           \
        return obj;                                                                         \
    }                                                                                       \
    UNUSED static void obj_name##_unref(struct obj_name *obj) {                             \
        if (refcount_dec(&obj->refcount_member_name) == false) {                            \
            obj_name##_destroy(obj);                                                        \
        }                                                                                   \
    }                                                                                       \
    UNUSED static void obj_name##_unrefp(struct obj_name **obj) {                           \
        obj_name##_unref(*obj);                                                             \
        *obj = NULL;                                                                        \
    }                                                                                       \
    UNUSED static void obj_name##_swap_ptrs(struct obj_name **objp, struct obj_name *obj) { \
        if (obj != NULL) {                                                                  \
            obj_name##_ref(obj);                                                            \
        }                                                                                   \
        if (*objp != NULL) {                                                                \
            obj_name##_unrefp(objp);                                                        \
        }                                                                                   \
        *objp = obj;                                                                        \
    }                                                                                       \
    UNUSED static void obj_name##_unref_void(void *obj) { obj_name##_unref((struct obj_name *) obj); }

#endif  // _FLUTTERPI_SRC_UTIL_REFCOUNTING_H
