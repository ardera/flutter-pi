// SPDX-License-Identifier: MIT
/*
 * Vulkan GBM Backing Store
 *
 * - a render surface that can be used for filling flutter vulkan backing stores
 * - and for scanout using KMS
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "dummy_render_surface.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

#include <unistd.h>

#include "render_surface.h"
#include "render_surface_private.h"
#include "surface.h"
#include "surface_private.h"
#include "tracer.h"
#include "util/geometry.h"
#include "util/uuid.h"

struct dummy_render_surface {
    union {
        struct surface surface;
        struct render_surface render_surface;
    };

#ifdef DEBUG
    uuid_t uuid;
#endif
};

COMPILE_ASSERT(offsetof(struct dummy_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct dummy_render_surface, render_surface.surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0x26, 0xfe, 0x91, 0x53, 0x75, 0xf2, 0x41, 0x90, 0xa1, 0xf5, 0xba, 0xe1, 0x1b, 0x28, 0xd5, 0xe5);
#endif

#define CAST_THIS(ptr) CAST_DUMMY_RENDER_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_DUMMY_RENDER_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct dummy_render_surface *__checked_cast_dummy_render_surface(void *ptr) {
    struct dummy_render_surface *surface;

    surface = CAST_DUMMY_RENDER_SURFACE_UNCHECKED(ptr);
    ASSERT(uuid_equals(surface->uuid, uuid));
    return surface;
}
#endif

void dummy_render_surface_deinit(struct surface *s);
static int dummy_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int dummy_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int dummy_render_surface_fill(struct render_surface *surface, FlutterBackingStore *fl_store);
static int dummy_render_surface_queue_present(struct render_surface *surface, const FlutterBackingStore *fl_store);

int dummy_render_surface_init(struct dummy_render_surface *surface, struct tracer *tracer, struct vec2i size) {
    int ok;

    ok = render_surface_init(CAST_RENDER_SURFACE_UNCHECKED(surface), tracer, size);
    if (ok != 0) {
        return EIO;
    }

    surface->surface.present_kms = dummy_render_surface_present_kms;
    surface->surface.present_fbdev = dummy_render_surface_present_fbdev;
    surface->surface.deinit = dummy_render_surface_deinit;
    surface->render_surface.fill = dummy_render_surface_fill;
    surface->render_surface.queue_present = dummy_render_surface_queue_present;

#ifdef DEBUG
    uuid_copy(&surface->uuid, uuid);
#endif
    return 0;
}

struct dummy_render_surface *dummy_render_surface_new(struct tracer *tracer, struct vec2i size) {
    struct dummy_render_surface *surface;
    int ok;

    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = dummy_render_surface_init(surface, tracer, size);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return surface;

fail_free_surface:
    free(surface);

fail_return_null:
    return NULL;
}

void dummy_render_surface_deinit(struct surface *s) {
    render_surface_deinit(s);
}

static int
dummy_render_surface_present_kms(struct surface *s, UNUSED const struct fl_layer_props *props, UNUSED struct kms_req_builder *builder) {
    (void) props;
    (void) builder;

    TRACER_INSTANT(s->tracer, "dummy_render_surface_present_kms");

    return 0;
}

static int dummy_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    (void) s;
    (void) props;
    (void) builder;

    TRACER_INSTANT(s->tracer, "dummy_render_surface_present_fbdev");

    return 0;
}

static int dummy_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store) {
    (void) fl_store;

    TRACER_INSTANT(s->surface.tracer, "dummy_render_surface_fill");

    return 0;
}

static int dummy_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    (void) fl_store;

    TRACER_INSTANT(s->surface.tracer, "dummy_render_surface_queue_present");

    return 0;
}
