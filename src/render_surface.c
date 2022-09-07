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

int backing_store_init(struct backing_store *store, struct tracer *tracer, struct point size) {
    int ok;

    ok = surface_init(&store->surface, tracer);
    if (ok != 0) {
        return ok;
    }

    store->surface.deinit = backing_store_deinit;
    store->surface.present_kms = NULL;
    store->surface.present_fbdev = NULL;
    uuid_copy(&store->uuid, uuid);
    store->size = size;
    store->fill = NULL;
    store->queue_present = NULL;
    return 0;
}

void backing_store_deinit(struct surface *s) {
    surface_deinit(s);
}

int backing_store_fill(struct backing_store *store, FlutterBackingStore *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->fill);

    DEBUG_ASSERT_EQUALS(fl_store->user_data, NULL);
    DEBUG_ASSERT_EQUALS(fl_store->did_update, false);

    TRACER_BEGIN(store->surface.tracer, "backing_store_fill");
    ok = store->fill(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_fill");

    DEBUG_ASSERT_EQUALS(fl_store->user_data, NULL);
    DEBUG_ASSERT_EQUALS(fl_store->did_update, false);

    return ok;
}

int backing_store_queue_present(struct backing_store *store, const FlutterBackingStore *fl_store) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(store);
    DEBUG_ASSERT_NOT_NULL(fl_store);
    DEBUG_ASSERT_NOT_NULL(store->queue_present);

    TRACER_BEGIN(store->surface.tracer, "backing_store_queue_present");
    ok = store->queue_present(store, fl_store);
    TRACER_END(store->surface.tracer, "backing_store_queue_present");

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
