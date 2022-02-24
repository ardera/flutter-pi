// SPDX-License-Identifier: MIT
/*
 * Surface
 *
 * - rendering / scanout surface interface
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_SURFACE_H
#define _FLUTTERPI_INCLUDE_SURFACE_H

#include <collection.h>

struct surface;
struct compositor;
struct fl_layer_props;
struct kms_req_builder;
struct fbdev_commit_builder;

#define CAST_SURFACE_UNCHECKED(ptr) ((struct surface*) (ptr))
#ifdef DEBUG
#   define CAST_SURFACE(ptr) __checked_cast_surface(ptr)
ATTR_PURE struct surface *__checked_cast_surface(void *ptr);
#else
#   define CAST_SURFACE(ptr) CAST_SURFACE_UNCHECKED(ptr)
#endif

void surface_destroy(struct surface *s);

DECLARE_LOCK_OPS(surface)

DECLARE_REF_OPS(surface)

static inline void surface_unref_void(void *ptr) {
    DEBUG_ASSERT_NOT_NULL(ptr);
    return surface_unref(CAST_SURFACE(ptr));
}

int surface_register(struct surface *s);

int surface_unregister(struct surface *s);

ATTR_PURE bool surface_is_registered(struct surface *s);

ATTR_CONST static inline int64_t surface_get_view_id(struct surface *s) {
    DEBUG_ASSERT_NOT_NULL(s);
    return ptr_to_int64(s);
}

ATTR_PURE static inline int64_t surface_get_registered_view_id(struct surface *s) {
    DEBUG_ASSERT_NOT_NULL(s);
    DEBUG_ASSERT(surface_is_registered(s));
    return ptr_to_int64(s);
}

ATTR_PURE static inline struct surface *surface_from_id(int64_t id) {
    return CAST_SURFACE(int64_to_ptr(id));
}

ATTR_PURE int64_t surface_get_revision(struct surface *s);

void surface_increase_revision(struct surface *s);

int surface_swap_buffers(struct surface *s);

int surface_present_kms(
    struct surface *s,
    const struct fl_layer_props *props,
    struct kms_req_builder *builder
);

int surface_present_fbdev(
    struct surface *s,
    const struct fl_layer_props *props,
    struct fbdev_commit_builder *builder
);

#endif // _FLUTTERPI_INCLUDE_SURFACE_H
