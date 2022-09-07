// SPDX-License-Identifier: MIT
/*
 * GBM Surface backing store
 *
 * - public interface for backing stores which are based on GBM surfaces
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#ifndef _FLUTTERPI_INCLUDE_GBM_SURFACE_BACKING_STORE_H
#define _FLUTTERPI_INCLUDE_GBM_SURFACE_BACKING_STORE_H

#include <collection.h>
#include <pixel_format.h>
#include <compositor_ng.h>

struct backing_store;
struct gbm_device;
struct gbm_surface_backing_store;

#define CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(ptr) ((struct gbm_surface_backing_store*) (ptr))
#ifdef DEBUG
#   define CAST_GBM_SURFACE_BACKING_STORE(ptr) __checked_cast_gbm_surface_backing_store(ptr)
ATTR_PURE struct gbm_surface_backing_store *__checked_cast_gbm_surface_backing_store(void *ptr);
#else
#   define CAST_GBM_SURFACE_BACKING_STORE(ptr) CAST_GBM_SURFACE_BACKING_STORE_UNCHECKED(ptr)
#endif

ATTR_MALLOC struct gbm_surface_backing_store *gbm_surface_backing_store_new_with_egl_config(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format,
    EGLConfig egl_config
);

ATTR_MALLOC struct gbm_surface_backing_store *gbm_surface_backing_store_new(
    struct tracer *tracer,
    struct point size,
    struct gbm_device *device,
    struct gl_renderer *renderer,
    enum pixfmt pixel_format
);

ATTR_PURE EGLSurface gbm_surface_backing_store_get_egl_surface(struct gbm_surface_backing_store *s);

ATTR_PURE EGLConfig gbm_surface_backing_store_get_egl_config(struct gbm_surface_backing_store *s);

#endif // _FLUTTERPI_INCLUDE_GBM_SURFACE_BACKING_STORE_H
