// SPDX-License-Identifier: MIT
/*
 * render surface
 *
 * - A surface that can be scanned out, and that flutter can render into.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "render_surface.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "compositor_ng.h"
#include "render_surface_private.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"

// just so we can be sure &render_surface->surface is the same as (struct surface*) render_surface
COMPILE_ASSERT(offsetof(struct render_surface, surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0x78, 0x70, 0x45, 0x13, 0xa8, 0xf3, 0x43, 0x34, 0xa0, 0xa3, 0xae, 0x90, 0xf1, 0x11, 0x41, 0xe0);
#endif

void render_surface_deinit(struct surface *s);

int render_surface_init(struct render_surface *surface, struct tracer *tracer, struct vec2i size) {
    int ok;

    ok = surface_init(&surface->surface, tracer);
    if (ok != 0) {
        return ok;
    }

    surface->surface.deinit = render_surface_deinit;
    surface->surface.present_kms = NULL;
    surface->surface.present_fbdev = NULL;

#ifdef DEBUG
    uuid_copy(&surface->uuid, uuid);
#endif

    surface->size = size;
    surface->fill = NULL;
    surface->queue_present = NULL;
    return 0;
}

void render_surface_deinit(struct surface *s) {
    surface_deinit(s);
}

int render_surface_fill(struct render_surface *surface, FlutterBackingStore *fl_store) {
    int ok;

    ASSERT_NOT_NULL(surface);
    ASSERT_NOT_NULL(fl_store);
    ASSERT_NOT_NULL(surface->fill);

    ASSERT_EQUALS(fl_store->user_data, NULL);
    ASSERT_EQUALS(fl_store->did_update, false);

    TRACER_BEGIN(surface->surface.tracer, "render_surface_fill");
    ok = surface->fill(surface, fl_store);
    TRACER_END(surface->surface.tracer, "render_surface_fill");

    ASSERT_EQUALS(fl_store->user_data, NULL);
    ASSERT_EQUALS(fl_store->did_update, false);

    return ok;
}

int render_surface_queue_present(struct render_surface *surface, const FlutterBackingStore *fl_store) {
    int ok;

    ASSERT_NOT_NULL(surface);
    ASSERT_NOT_NULL(fl_store);
    ASSERT_NOT_NULL(surface->queue_present);

    TRACER_BEGIN(surface->surface.tracer, "render_surface_queue_present");
    ok = surface->queue_present(surface, fl_store);
    TRACER_END(surface->surface.tracer, "render_surface_queue_present");

    return ok;
}

#ifdef DEBUG
ATTR_PURE struct render_surface *__checked_cast_render_surface(void *ptr) {
    struct render_surface *surface;

    surface = CAST_RENDER_SURFACE_UNCHECKED(ptr);
    assert(uuid_equals(surface->uuid, uuid));
    return surface;
}
#endif
