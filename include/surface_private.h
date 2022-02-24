// SPDX-License-Identifier: MIT
/*
 * Surface Implementation details
 *
 * - should be included for expanding the surface (i.e. for a backing store or platform view surface type)
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_SURFACE_PRIVATE_H
#define _FLUTTERPI_INCLUDE_SURFACE_PRIVATE_H

#include <stdint.h>
#include <pthread.h>
#include <collection.h>

struct fl_layer_props;
struct kms_req_builder;
struct fbdev_commit_builder;
struct compositor;
struct tracer;

struct surface {
    uuid_t uuid;
    refcount_t n_refs;
    pthread_mutex_t lock;
    struct compositor *compositor;
    struct tracer *tracer;
    bool registered;
    int64_t id;
    int64_t revision;

    int (*swap_buffers)(struct surface *s);
    int (*present_kms)(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
    int (*present_fbdev)(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
    void (*deinit)(struct surface *s);
};

int surface_init(struct surface *s, struct compositor *compositor, struct tracer *tracer);

void surface_deinit(struct surface *s);

#endif // _FLUTTERPI_INCLUDE_SURFACE_PRIVATE_H
