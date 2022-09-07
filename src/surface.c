// SPDX-License-Identifier: MIT
/*
 * Window Surface
 *
 * - provides an object that can be composited by flutter-pi
 * - (by calling present_kms or present_fbdev on it)
 * - == basically the thing that stores the graphics of a FlutterLayer
 * - render surfaces are special kinds of scanout surfaces that flutter can render into
 * - every scanout surface can be registered as a platform view to display it
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include <stddef.h>
#include <stdlib.h>

#include <collection.h>
#include <compositor_ng.h>
#include <surface.h>
#include <surface_private.h>
#include <tracer.h>

FILE_DESCR("rendering surfaces")

void surface_deinit(struct surface *s);

static const uuid_t uuid = CONST_UUID(0xce, 0x35, 0x87, 0x0c, 0x82, 0x08, 0x46, 0x09, 0xbd, 0xab, 0x80, 0x67, 0x28, 0x15, 0x45, 0xb5);

#ifdef DEBUG
ATTR_PURE struct surface *__checked_cast_surface(void *ptr) {
    struct surface *s;
    
    s = CAST_SURFACE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

int surface_init(struct surface *s, struct tracer *tracer) {
    uuid_copy(&s->uuid, uuid);
    s->n_refs = REFCOUNT_INIT_1;
    pthread_mutex_init(&s->lock, NULL);
    s->tracer = tracer_ref(tracer);
    s->revision = 1;
    s->present_kms = NULL;
    s->present_fbdev = NULL;
    s->deinit = surface_deinit;
    return 0;
}

void surface_deinit(struct surface *s) {
    tracer_unref(s->tracer);
}

struct surface *surface_new(struct tracer *tracer) {
    struct surface *s;
    int ok;
    
    s = malloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }

    ok = surface_init(s, tracer);
    if (ok != 0) {
        free(s);
        return NULL;
    }
    
    return s;
}

void surface_destroy(struct surface *s) {
    DEBUG_ASSERT_NOT_NULL(s->deinit);
    s->deinit(s);
    free(s);
}

DEFINE_LOCK_OPS(surface, lock)

DEFINE_REF_OPS(surface, n_refs)

int64_t surface_get_revision(struct surface *s) {
    DEBUG_ASSERT_NOT_NULL(s);
    return s->revision;
}

int surface_present_kms(
    struct surface *s,
    const struct fl_layer_props *props,
    struct kms_req_builder *builder
) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(s);
    DEBUG_ASSERT_NOT_NULL(props);
    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT_NOT_NULL(s->present_kms);

    TRACER_BEGIN(s->tracer, "surface_present_kms");    
    ok = s->present_kms(s, props, builder);
    TRACER_END(s->tracer, "surface_present_kms");
    
    return ok;
}

int surface_present_fbdev(
    struct surface *s,
    const struct fl_layer_props *props,
    struct fbdev_commit_builder *builder
) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(s);
    DEBUG_ASSERT_NOT_NULL(props);
    DEBUG_ASSERT_NOT_NULL(builder);
    DEBUG_ASSERT_NOT_NULL(s->present_fbdev);

    TRACER_BEGIN(s->tracer, "surface_present_fbdev");
    ok = s->present_fbdev(s, props, builder);
    TRACER_END(s->tracer, "surface_present_fbdev");

    return ok;
}
