// SPDX-License-Identifier: MIT
/*
 * Surface Implementation details
 *
 * - should be included for expanding the surface (i.e. for a backing store or platform view surface type)
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#ifndef _FLUTTERPI_SRC_SURFACE_PRIVATE_H
#define _FLUTTERPI_SRC_SURFACE_PRIVATE_H

#include <stdint.h>

#include <pthread.h>

#include "util/collection.h"
#include "util/refcounting.h"
#include "util/uuid.h"

struct fl_layer_props;
struct kms_req_builder;
struct fbdev_commit_builder;
struct tracer;

struct surface {
#ifdef DEBUG
    uuid_t uuid;
#endif
    refcount_t n_refs;
    pthread_mutex_t lock;
    struct tracer *tracer;
    int64_t revision;

    int (*present_kms)(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
    int (*present_fbdev)(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
    void (*deinit)(struct surface *s);
};

int surface_init(struct surface *s, struct tracer *tracer);

void surface_deinit(struct surface *s);

#endif  // _FLUTTERPI_SRC_SURFACE_PRIVATE_H
