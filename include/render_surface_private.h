// SPDX-License-Identifier: MIT
/*
 * Render surface implementation
 *
 * - private implementation for render surfaces
 * - needed for implementing specific kinds of render surfaces
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_RENDER_SURFACE_PRIVATE_H
#define _FLUTTERPI_INCLUDE_RENDER_SURFACE_PRIVATE_H

#include <flutter_embedder.h>
#include <collection.h>
#include <surface_private.h>
#include <compositor_ng.h>

struct render_surface {
    struct surface surface;

    uuid_t uuid;
    struct vec2f size;
    int (*fill)(struct render_surface *surface, FlutterBackingStore *fl_store);
    int (*queue_present)(struct render_surface *surface, const FlutterBackingStore *fl_store);
};

int render_surface_init(struct render_surface *surface, struct tracer *tracer, struct vec2f size);

void render_surface_deinit(struct surface *s);

#endif // _FLUTTERPI_INCLUDE_BACKING_STORE_PRIVATE_H


