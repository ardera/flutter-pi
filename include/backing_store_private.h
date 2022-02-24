// SPDX-License-Identifier: MIT
/*
 * Backing store implementation
 *
 * - private implementation for backing stores
 * - needed for implementing specific kinds of backing stores
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_BACKING_STORE_PRIVATE_H
#define _FLUTTERPI_INCLUDE_BACKING_STORE_PRIVATE_H

#include <flutter_embedder.h>
#include <collection.h>
#include <surface_private.h>
#include <compositor_ng.h>

struct backing_store {
    struct surface surface;

    uuid_t uuid;
    struct point size;
    int (*fill_opengl)(struct backing_store *store, FlutterOpenGLBackingStore *fl_store);
    int (*fill_software)(struct backing_store *store, FlutterSoftwareBackingStore *fl_store);
    int (*fill_metal)(struct backing_store *store, FlutterMetalBackingStore *fl_store);
    int (*fill_vulkan)(struct backing_store *store, void *fl_store);
};


int backing_store_init(struct backing_store *store, struct compositor *compositor, struct tracer *tracer, struct point size);

void backing_store_deinit(struct surface *s);

#endif // _FLUTTERPI_INCLUDE_BACKING_STORE_PRIVATE_H


