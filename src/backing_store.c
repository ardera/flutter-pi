// SPDX-License-Identifier: MIT
/*
 * backing stores
 *
 * - simple flutter backing store implementation
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>
#include <stdatomic.h>
#include <stdint.h>

#include <collection.h>
#include <surface.h>
#include <backing_store.h>
#include <backing_store_private.h>
#include <compositor_ng.h>
#include <tracer.h>

FILE_DESCR("flutter backing store")

// just so we can be sure &backing_store->surface is the same as (struct surface*) backing_store
COMPILE_ASSERT(offsetof(struct backing_store, surface) == 0);

static const uuid_t uuid = CONST_UUID(0x78, 0x70, 0x45, 0x13, 0xa8, 0xf3, 0x43, 0x34, 0xa0, 0xa3, 0xae, 0x90, 0xf1, 0x11, 0x41, 0xe0);

void backing_store_deinit(struct surface *s);

int backing_store_init(struct backing_store *store, struct compositor *compositor, struct tracer *tracer, struct point size) {
    int ok;

    ok = surface_init(&store->surface, compositor, tracer);
    if (ok != 0) {
        return ok;
    }

    store->surface.deinit = backing_store_deinit;
    store->surface.present_kms = NULL;
    store->surface.present_fbdev = NULL;
    uuid_copy(&store->uuid, uuid);
    store->size = size;
    store->fill_opengl = NULL;
    store->fill_software = NULL;
    store->fill_metal = NULL;
    store->fill_vulkan = NULL;
    return 0;
}

void backing_store_deinit(struct surface *s) {
    surface_deinit(s);
}

int backing_store_fill_opengl(struct backing_store *store, FlutterOpenGLBackingStore *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->fill_opengl);
    
    TRACER_BEGIN(store->surface.tracer, "backing_store_fill_opengl");
    ok = store->fill_opengl(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_fill_opengl");

    return ok;
}

int backing_store_fill_software(struct backing_store *store, FlutterSoftwareBackingStore *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->fill_software);

    TRACER_BEGIN(store->surface.tracer, "backing_store_fill_software");
    ok = store->fill_software(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_fill_software");

    return ok;
}

int backing_store_fill_metal(struct backing_store *store, FlutterMetalBackingStore *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->fill_metal);

    TRACER_BEGIN(store->surface.tracer, "backing_store_fill_metal");
    ok = store->fill_metal(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_fill_metal");

    return ok;
}

int backing_store_fill_vulkan(struct backing_store *store, void *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->fill_vulkan);

    TRACER_BEGIN(store->surface.tracer, "backing_store_fill_vulkan");
    ok = store->fill_vulkan(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_fill_vulkan");

    return ok;
}

#ifdef DEBUG
ATTR_PURE struct backing_store *__checked_cast_backing_store(void *ptr) {
    struct backing_store *store;
    
    store = CAST_BACKING_STORE_UNCHECKED(ptr);
    DEBUG_ASSERT(uuid_equals(store->uuid, uuid));
    return store;
}
#endif
